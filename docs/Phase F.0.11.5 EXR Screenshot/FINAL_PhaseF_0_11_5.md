# Phase F.0.11.5 EXR Screenshot — FINAL 文档（实施记录）

> **阶段**：6A Workflow — 阶段 4 Approve / Apply（实施）
> **基线**：PLAN_PhaseF_0_11_5.md
> **实施日期**：2026-05-17
> **完成度**：F.0.11.5 全量交付

---

## 1. 实施时间线

| 任务 | 实际产出 | 耗时 |
|---|---|---|
| T1 | tinyexr v1.0.7 下载 + tinyexr_impl.cpp + CMake 集成 | ~30 min (含 v1.0.7 vs release 选型) |
| T2 | l_Graphics_ScreenshotEXR + parse_exr_compression_ + 注册 graphics_funcs[] | ~45 min |
| T3 | screenshot smoke +4 检查点 + demo_taau E 键 + 文档收尾 | ~30 min |

**总计**: ~1.75 小时 (PLAN 估时 2.5 小时, 提前完成因 v1.0.7 选型避开多文件依赖)

---

## 2. 文件改动清单

| 文件 | 改动类型 | 改动量 |
|---|---|---|
| `ChocoLight/third_party/tinyexr.h` | 新建 (从 v1.0.7) | ~12000 行 (vendored) |
| `ChocoLight/third_party/tinyexr_impl.cpp` | 新建 | ~30 行 |
| `ChocoLight/CMakeLists.txt` | 修改 | +1 行 |
| `ChocoLight/src/light_graphics.cpp` | 修改 | +130 行 (include + parse_exr_compression_ + ScreenshotEXR + funcs[] 注册) |
| `scripts/smoke/screenshot.lua` | 修改 | +12 行 (4 新检查点) |
| `samples/demo_taau/main.lua` | 修改 | +12 行 (E 键 + HUD 提示更新) |
| `samples/demo_taau/README.md` | 修改 | +1 行 |
| `docs/Phase F.0.11.5 EXR Screenshot/PLAN_PhaseF_0_11_5.md` | 新建 | ~150 行 |
| `docs/Phase F.0.11.5 EXR Screenshot/ACCEPTANCE_PhaseF_0_11_5.md` | 新建 | ~100 行 |
| `docs/Phase F.0.11.5 EXR Screenshot/FINAL_PhaseF_0_11_5.md` | 新建 | 本文 |
| `docs/HANDOFF_REMAINING_TASKS.md` | 修改 | F.0.11.5 状态更新 |

---

## 3. 关键实现细节

### 3.1 tinyexr 集成关键点

**v1.0.7 选型**: 最新 `release` 分支拆为多文件 (含 `exr_reader.hh` / `exr_writer.hh`), 不再是单文件库. 选稳定 v1.0.7 单文件版本保持集成简单.

**TINYEXR_USE_STB_ZLIB=1**: 复用现有 stb 的 zlib 实现 (`stbi_zlib_decode_buffer` / `stbi_zlib_compress`). 避免再带一份 miniz 引入 ~3000 行 + 符号冲突风险.

**统一编译选项**: tinyexr_impl.cpp 与 light_graphics.cpp 必须用相同的 macro define, 否则 inline 函数 ODR 违规:
```cpp
// 双方都写
#define TINYEXR_USE_MINIZ     0
#define TINYEXR_USE_STB_ZLIB  1
#define TINYEXR_USE_THREAD    0
#define TINYEXR_USE_OPENMP    0
```

**MSVC 警告抑制**: tinyexr v1.0.7 内大量使用 `strcpy` / `strncpy` 等被 MSVC 标记 deprecated. impl 文件 `#pragma warning push/pop` 局部禁用 C4996/C4244/C4267/C4505.

### 3.2 channel 顺序: A B G R 字母序

OpenEXR 规范要求按字母序. tinyexr 内部按 `header.channels[i].name` 排序拿对应 `image.images[i]` 数据. 实现时:
```cpp
strncpy(chans[0].name, "A", 255);   // image.images[0] -> chA
strncpy(chans[1].name, "B", 255);   // image.images[1] -> chB
strncpy(chans[2].name, "G", 255);   // image.images[2] -> chG
strncpy(chans[3].name, "R", 255);   // image.images[3] -> chR
```

### 3.3 Y 翻转 + 拆 channel 一次性完成

OpenGL 纹理原点在左下, EXR 文件原点在左上. 在拆 RGBA → 4 通道时同时反转 Y:
```cpp
for (int y = 0; y < h; ++y) {
    const int srcY = h - 1 - y;
    for (int x = 0; x < w; ++x) {
        const size_t dst = (size_t)y * w + x;
        const size_t src = ((size_t)srcY * w + x) * 4;
        chR[dst] = rgba[src + 0]; ...
    }
}
```

### 3.4 bit_depth 16/32 通过 requested_pixel_types 控制

```cpp
pixel_types[i]           = TINYEXR_PIXELTYPE_FLOAT;   // 源数据是 32-bit float
requested_pixel_types[i] = (bit_depth == 16) ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
```
tinyexr 内部根据两者差异自动转换 (float32 → float16).

### 3.5 默认 compression = ZIP

业界经验:
- ZIP: 中等压缩比, 中等速度 (默认, 大多数场景最佳)
- ZIPS: 单行 ZIP, 速度快但压缩比低
- PIZ: wavelet, 最快但有损 (动画帧/序列优先)
- RLE: 旧机器兼容, 体积大
- NONE: 调试用

---

## 4. 测试覆盖

### 4.1 Smoke (4 新检查点全过)
```
PASS ScreenshotEXR exists
PASS ScreenshotEXR no HDR → nil+err
PASS ScreenshotEXR bit_depth=24 → nil+err
PASS ScreenshotEXR compression=lzma → nil+err
```
**总计**: 34 PASS / 0 FAIL

### 4.2 Zero-Regression
- ✅ demo_ssr / demo_taa_split2 / demo_taau / demo_multi_hdr_pip 全部启动无错
- ✅ TAA smoke 171/0 不受影响 (F.0.11.5 不动 TAA 路径)

### 4.3 真机验证 (待用户)
- ⏳ demo_taau 按 E → 生成 `taau_screenshot.exr` → 用 Nuke / Krita / Photoshop / DaVinci Resolve 读取
- ⏳ 验证 bit_depth=32 时文件体积约 16-bit 两倍
- ⏳ 验证 compression=zip 比 compression=none 文件小约 50-70%

---

## 5. 已知 / 留观察问题

### 5.1 设计层
- **多实例截图**: 当前 ScreenshotEXR 永远读 HDR active instance, 与 ScreenshotHDR 同模式. F.0.11.7 将增 instance_id 参数支持显式截 PIP / split-screen 子实例.
- **PIZ 压缩有损警告**: PIZ 是 wavelet 编码, 对 EXR 标准是有损的 (但对极高动态范围数据影响小). 文档未单独警告; 用户自负责选择.

### 5.2 性能 (待真机测)
- tinyexr v1.0.7 单线程编码; 1080p RGBA half ZIP 写盘预计 ~30-80 ms (与 .hdr 同档)
- 极大分辨率 (4K+) 时可考虑后续启用 OpenMP (TINYEXR_USE_OPENMP=1)

### 5.3 EXR 元数据
- 当前未写入 chromaticities / displayWindow / dataWindow 等高级元数据 (默认值即可)
- 后续可扩展 opts 表加入 colorspace="sRGB"|"linear" 等字段

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 实施完结 — F.0.11.5 全量代码 + 文档交付 |
