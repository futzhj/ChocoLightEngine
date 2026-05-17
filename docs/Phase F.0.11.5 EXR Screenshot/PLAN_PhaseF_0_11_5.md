# Phase F.0.11.5 EXR Screenshot — PLAN 文档

> **阶段**：6A Workflow 合并版 (ALIGNMENT + DESIGN + TASK)
> **目标**：在现有 ScreenshotHDR (Radiance RGBE) 基础上增加 OpenEXR 截图能力 (浮点 HDR 16-bit half / 32-bit float)
> **基线**：Phase F.1.1 完结 (commit pending, 2026-05-17)
> **创建日期**：2026-05-17
> **配套**：FINAL_PhaseF_0_11_5.md (实施记录) + ACCEPTANCE_PhaseF_0_11_5.md (验收)

---

## 1. 背景

`Light.Graphics.ScreenshotHDR(path)` 已能输出 Radiance `.hdr` (RGBE 32-bit). 但 RGBE 仅有 ~8 bit per channel 精度 (公共指数), 不足以满足:

- **影视后期工作流**: Nuke / DaVinci Resolve / After Effects 等行业工具读 EXR, 不读 .hdr
- **HDR 工作流精度需求**: 半精度 float (16-bit half) 是行业标准
- **Z-depth / 多通道导出**: EXR 支持任意通道命名 (RGB / RGBA / RGBA + Z), 留扩展空间
- **更好的压缩**: EXR ZIP / ZIPS / PIZ 多种压缩算法, 无损但比 .hdr 体积小很多

`tinyexr` 是单文件 header-only 库, 与 stb_image / stb_image_write 同模式. 集成成本低, 增 ~10 行 CMake + ~50 行 Lua bridge.

## 2. 设计

### 2.1 集成 tinyexr

```
ChocoLight/third_party/
    tinyexr.h           ← 单文件 header (~12000 行, 含 TINYEXR_IMPLEMENTATION)
    tinyexr_impl.c      ← 1 行 `#define TINYEXR_IMPLEMENTATION` + `#include "tinyexr.h"`
```

CMake 添加 `tinyexr_impl.c` 到 sources, 与现有 stb_impl.c / miniaudio_impl.c 同模式. tinyexr 内部依赖 miniz (zip 压缩), 但 stb 已经间接引入 zlib-compatible 接口; tinyexr 默认走自带 miniz, 不冲突.

**精简编译选项** (在 tinyexr_impl.c 顶部):
```c
#define TINYEXR_IMPLEMENTATION
// 关闭 ZFP / OpenMP (减少依赖)
#define TINYEXR_USE_MINIZ 1   // 默认; 不需链 zlib
#define TINYEXR_USE_THREAD 0  // 单线程, 截图频率低不必并行
#define TINYEXR_USE_OPENMP 0
#include "tinyexr.h"
```

### 2.2 Lua API: `ScreenshotEXR`

```lua
-- 默认 half-float (16-bit, 体积小); compress 默认 ZIP (中速)
local ok = Light.Graphics.ScreenshotEXR("out.exr")

-- 显式版: 控制 float bit 与压缩
-- bit_depth: 16 (half, 默认) / 32 (float, 双倍体积)
-- compression: "none" / "zip" (默认) / "zips" / "rle" / "piz" (有损 wavelet, 最快)
local ok = Light.Graphics.ScreenshotEXR("out.exr", { bit_depth = 32, compression = "zip" })
```

**与 ScreenshotHDR 共享路径**:
- 都读 HDR active instance 的 sceneTex (RGBA16F)
- 都用 `g_render->ReadbackTextureRGBAFloat(tex, w, h, rgba)` 拿 float 数据
- 都需要 Y 翻转 (OpenGL bottom-left → 图像格式 top-left)

**与 ScreenshotHDR 差异**:
- EXR 保留 4 channels (R/G/B/A), 不像 .hdr 丢 alpha
- EXR 写 half (16) 比 float (32) 体积小一半, 精度 ~3 位小数足够 HDR 调色
- tinyexr API 比 stb 复杂, 需手写 EXRHeader + EXRImage 结构

### 2.3 tinyexr 调用骨架

```cpp
#include "tinyexr.h"

bool SaveEXR_HDR16(const char* path, int w, int h, const float* rgba) {
    EXRHeader header;  InitEXRHeader(&header);
    EXRImage  image;   InitEXRImage(&image);

    image.num_channels = 4;
    std::vector<float> chR(w*h), chG(w*h), chB(w*h), chA(w*h);
    for (int i = 0; i < w * h; ++i) {
        const int srcY = h - 1 - (i / w);   // Y flip (OpenGL bottom-left -> EXR top-left)
        const int srcX = i % w;
        const int s = (srcY * w + srcX) * 4;
        chR[i] = rgba[s+0]; chG[i] = rgba[s+1];
        chB[i] = rgba[s+2]; chA[i] = rgba[s+3];
    }
    float* channels[4] = { chB.data(), chG.data(), chR.data(), chA.data() };   // EXR 内部 BGRA 排序
    image.images = (unsigned char**)channels;
    image.width = w; image.height = h;

    header.num_channels = 4;
    EXRChannelInfo channels_info[4];
    header.channels = channels_info;
    // tinyexr 用 ABGR/A-first 排序; channel 名按字母序读, 用习惯 "A" "B" "G" "R"
    strcpy(channels_info[0].name, "A");
    strcpy(channels_info[1].name, "B");
    strcpy(channels_info[2].name, "G");
    strcpy(channels_info[3].name, "R");

    int pixel_types[4]            = { TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF };
    int requested_pixel_types[4]  = { TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF, TINYEXR_PIXELTYPE_HALF };
    // 源数据是 float (32-bit); requested 是 half (16-bit) 让 tinyexr 内部转换
    header.pixel_types           = pixel_types;
    header.requested_pixel_types = requested_pixel_types;
    header.compression_type      = TINYEXR_COMPRESSIONTYPE_ZIP;

    const char* err = nullptr;
    int ret = SaveEXRImageToFile(&image, &header, path, &err);
    if (ret != TINYEXR_SUCCESS) {
        if (err) { /* log err; FreeEXRErrorMessage(err); */ }
        return false;
    }
    return true;
}
```

### 2.4 失败模式

| 触发条件 | 行为 |
|---|---|
| HDR 未启用 | `nil, "ScreenshotEXR: HDR not enabled"` |
| sceneTex 无效 | `nil, "ScreenshotEXR: invalid HDR scene"` |
| ReadbackTextureRGBAFloat 失败 | `nil, "ScreenshotEXR: readback failed"` |
| 不支持的 bit_depth (≠ 16/32) | `nil, "ScreenshotEXR: bit_depth must be 16 or 32"` |
| 不支持的 compression | `nil, "ScreenshotEXR: invalid compression mode"` |
| 写盘失败 (磁盘满 / 权限) | `nil, "ScreenshotEXR: SaveEXR failed: <err>"` |

### 2.5 测试覆盖

**Smoke 检查**:
1. Surface: `Light.Graphics.ScreenshotEXR` 是 function
2. Headless: HDR 未启用时返 `nil + err` (不 crash)
3. 错误参数: 非 string path / 无效 options 表 → 拒绝

**Demo 集成** (可选): `demo_taau` 或 `demo_multi_hdr_pip` 增 X 键截图 EXR 演示.

## 3. 风险与缓解

| 风险 | 缓解 |
|---|---|
| tinyexr.h ~12000 行影响编译时间 | impl 隔离在 tinyexr_impl.c, 头文件只在用到的 cpp 单文件 include |
| miniz 与现有 stb / sqlite zlib 符号冲突 | tinyexr 默认 static linkage, 符号 prefix `mz_`/`tinyexr_`, 无冲突历史 |
| Windows MSVC `strcpy` deprecation 警告 | 用 `strncpy_s` 或 `#pragma warning(disable: 4996)` 包裹 |
| OpenGL Y 翻转 | 与 ScreenshotHDR 同模式 (stbi_flip_vertically_on_write), 这里手动翻转 chR/G/B/A 索引 |

## 4. 任务拆分

| 任务 | 内容 | 估时 |
|---|---|---|
| T1 | 下载 tinyexr.h 到 third_party + 创建 tinyexr_impl.c + CMake 集成 | 30 min |
| T2 | l_Graphics_ScreenshotEXR 实现 (含 options table 解析 + Y flip + 错误处理) | 1 h |
| T3 | smoke 增 3 检查点 + 注册到 graphics_funcs[] | 20 min |
| T4 | demo_taau X 键演示截图 + README 更新 + 文档收尾 | 30 min |

**总预计**: 2.5 小时

## 5. 验收门槛

- ✅ Release build 通过 (含 tinyexr_impl.c 编译)
- ✅ smoke 3 检查点过
- ✅ 4 demo 启动零回归
- ⏳ 真机功能: 用户在 demo_taau / demo_multi_hdr_pip 内调 ScreenshotEXR, 生成的 .exr 用 Nuke/Krita/Photoshop 能正确读取

## 6. Commit 拆分建议

1. **deps**: tinyexr.h + tinyexr_impl.c + CMake 集成
2. **api**: l_Graphics_ScreenshotEXR 实现 + 注册
3. **test+docs**: smoke + demo 集成 + 文档
