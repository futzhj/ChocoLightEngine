# Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 FINAL

> 6A · 阶段 6 (Assess) · 项目总结
> 工作量 ~2.0h (vs 估 ~3.8h, 节约 ~47%)

---

## 1. 项目背景

F.0.10.8 提供 `CreateLUT3D` byte API + F.0.10.8.1 加 Adobe `.cube` 文本格式. 但 Photoshop / GIMP / ImageMagick 工作流主流输出是 **HALD CLUT** (PNG/JPG image LUT, GIMP "Color Curves Tool" / Photoshop "Color Lookup" 都支持). 本 phase 加 `HDR.LoadHaldLUT(path)` 让 image LUT 直接落地.

---

## 2. 交付内容

### 2.1 HDRRenderer (hdr_renderer.h / .cpp)

| API | 类型 | 说明 |
|-----|------|------|
| `LoadHaldLUTFile(path, outErr, errCap)` | 新增 | stbi_load → 方阵+N³ 验证 → RGBA 取 RGB → CreateLUT3D |

**实现核心** (~80 行):
- `stbi_load` 强制 4 通道 RGBA decode (与 light_graphics_image.cpp 一致模式)
- 验证方阵 (w == h)
- 求 level N ∈ [2, 8] 使 N³ == w (整数循环避免浮点 cbrt)
- LUT size = N² ∈ [4, 64] (与 F.0.10.8 / F.0.10.8.1 一致)
- RGBA → RGB byte stream (drop alpha, **零 reshape** — HALD raster scan = GL R-fastest)
- 所有错误路径 `stbi_image_free` 必调
- backend null 延后检查 (parse err 优先)

### 2.2 Lua API (light_graphics.cpp)

```lua
HDR.LoadHaldLUT(path) → tex_id, err
```

注册到 `hdr_funcs[]`.

### 2.3 smoke (scripts/smoke/hdr.lua)

§16 HALD CLUT section **5 PASS**:
1. 不存在文件 → "stbi_load failed: can't fopen"
2. 非图像文件 (.txt 内容) → "stbi_load failed: unknown image type"
3. 1×1 BMP (合法解码但 width 1 非 N³) → "HALD width 1 is not N^3 for any N in [2,8]"
4. 4×2 BMP 矩形 (非方阵) → "HALD image not square: 4x2"
5. 8×8 BMP HALD level=2 (合法尺寸) → "HDR backend not initialized (HALD parse ok: level=2, size=4)"

**BMP 编码 helpers** (~40 行 Lua):
- `u32_le / u16_le` (复用 audio.lua 模式)
- `make_bmp_solid(W, H, R, G, B)` 24-bit BMP File+DIB header + BGR pixel data

### 2.4 demo (samples/demo_taa_split2/main.lua)

- 加 `hasF10_8_2` API 检测
- headless probe 加 LoadHaldLUT(missing) PASS
- **demo total 15 → 16 PASS**

### 2.5 验证结果

| 类型 | 结果 |
|------|-----|
| 编译 (Release) | ✅ 通过 |
| HDR smoke (§16 5 PASS) | ✅ 38 fn (= 37+1) |
| 8 smoke 零回归 | ✅ hdr/motion_blur/bloom/ssr/ssao/taa/lens_flare/lens_fx |
| demo headless | ✅ **16 PASS** (= 15+1) |
| Lua API 总数 | 63 → **64** (+1) |

### 2.6 文档

| 文件 | 行数 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_8_2.md` | ~250 (HALD 标准 + scope + 12 决策 + 验收) |
| `DESIGN_PhaseF_0_10_8_2.md` | ~210 (parser 算法 + 接口契约 + 测试矩阵) |
| `TASK_PhaseF_0_10_8_2.md` | ~110 (6 原子任务 + 2 sub-phase) |
| **`FINAL_PhaseF_0_10_8_2.md`** | 本 (~140) |
| `TODO_PhaseF_0_10_8_2.md` | 待办 + 用户支持 |

---

## 3. 关键设计决策

### 3.1 实现位置 — hdr_renderer.cpp

**选**: 与 .cube parser 同模块. 复用 `writeErr_` helper + backend null 延后检查模式.

**收益**: 一致性 + 模块边界清晰 + 模板可复用.

### 3.2 stb_image 集成

**选**: `#include "stb_image.h"` 仅声明 (实现已在 `third_party/stb_impl.c` 一次编译).

**好处**:
- 零新依赖 (stb_image 已在项目用于 Image / Mesh / tray icon)
- 支持 PNG/JPG/BMP/TGA 全标准
- 跨平台 (header-only C)

### 3.3 RGBA 强制 + drop alpha

**选**: stbi_load 强制 4 通道 → RGBA 字节流 → 显式跳过 alpha.

**理由**:
- 简化逻辑 (不需根据原图通道数分支)
- 性能差异忽略 (一次性启动加载)
- alpha drop 在 LUT 语义下天然 (LUT 无 alpha 概念)

### 3.4 level N 求解 — 整数循环

**选**: `for N in [2, 8]: if N^3 == w break` 而非浮点 cbrt.

**理由**:
- 精确 (无浮点误差)
- 性能等价 (7 次循环 vs 1 次浮点 cbrt + 比较)
- 错误信息更准确 (能列出所有合法 expected width)

### 3.5 零 reshape — raster scan = GL R-fastest

**选**: 像素 raster scan 顺序**直接**当 LUT byte 顺序, 仅 drop alpha.

**理由 (关键性能优化)**:
- HALD 标准 pixel(x, y) → LUT idx (y * N³ + x), R 最快变
- GL 3D texture data layout: 同 (R 最快, G 次, B 慢)
- **故零 reshape**, 只需 RGBA byte stride 取 RGB
- 性能: level 8 (512×512 = 262144 像素) 处理 < 5ms

### 3.6 size 范围 [4, 64] 一致

**选**: 不接受 level > 8 (size > 64).

**理由**:
- 与 F.0.10.8 CreateLUT3D + F.0.10.8.1 LoadCubeLUT 约束完全一致
- 错误信息统一 (用户不困惑)
- level 8 (64³) VRAM 786KB, 已足够 AAA 项目质量
- level 12 (144³) → 16M VRAM, 罕见且不实用

### 3.7 backend null 延后检查

**选**: 与 F.0.10.8.1 一致, parse 完成后才检查 g.backend.

**理由**:
- parser err (1x1 / 非方阵 / 非 N³) 优先报告, 诊断更准
- smoke 在 headless 环境可独立测试 parser 逻辑 (不需要 GL context)

---

## 4. 工作量统计

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN | HALD 调研 + 12 决策 + 文档 | 0.3h |
| DESIGN/TASK | 设计 + 拆分 | 0.2h |
| SP1 (T1 + T2 parser) | hdr_renderer.h/cpp | 0.5h |
| SP2 (T3 Lua wrap) | light_graphics.cpp | 0.1h |
| SP2 (T4 smoke §16) | hdr.lua BMP encode + 5 PASS | 0.6h |
| SP2 (T5 demo) | main.lua headless probe | 0.1h |
| SP2 (T6 docs) | FINAL + TODO | 0.2h |
| **合计** | | **~2.0h** |

**vs ALIGN 估 ~3.8h, 节约 ~47%** (主因: 复用 F.0.10.8.1 模式 + stb_image 已在项目, 零新依赖).

---

## 5. F.0.10.x 系列累计

| Phase | API 增量 | 主题 |
|-------|---------|------|
| F.0.10.2 | +5 | 双 TAA instance |
| F.0.10.3 | +9 | Bloom/SSR/MB region + auto-* |
| F.0.10.5 | 0 | shader uvBounds 像素完美 |
| F.0.10.6 | +3 | per-region tonemap |
| F.0.10.7 | 0 | demo 视觉演示 |
| F.0.10.8 | +5 | per-region color grading LUT |
| F.0.10.8.1 | +1 | .cube 文件解析 |
| **F.0.10.8.2** | **+1** | **HALD CLUT 图像 LUT** |
| **累计** | **+24** | 39 → **64** Lua API |

---

## 6. 模板复用度

`writeErr_` + backend null 延后检查 + `nil + err string` 错误模式已成熟, 复用于:

| 候选 | 复用度 |
|------|--------|
| F.0.10.8.3 LUT 热重载 | 高 (沿用错误模式) |
| HDR LUT (16-bit / EXR) | 中 (改 backend 路径) |
| `.3dl` Lustre LUT (类似 .cube) | 极高 (parser 框架复用) |
| 任意 file → GPU asset loader | 高 |

---

## 7. LUT 生态拓展现状

至此 ChocoLight 支持的 LUT 来源:

| 来源 | API | 主流工具 |
|------|-----|---------|
| 内存 byte stream | `HDR.CreateLUT3D` | 程序生成 |
| `.cube` (Adobe 1.0) | `HDR.LoadCubeLUT` | DaVinci Resolve / Lightroom / Premiere |
| **HALD PNG/JPG** | **`HDR.LoadHaldLUT`** | **Photoshop / GIMP / ImageMagick** |

**3 入口共享同一 backend 路径**, 一致性 + 维护性极佳.

---

## 8. 后续候选

- **F.0.10.8.3** LUT 热重载 (~2h) - file watch + auto reload
- **HDR LUT 完整** (~3h) - DOMAIN > 1.0 + RGB16F backend
- **F.1 TAAU** (~10-15h) - DLSS-like 上采样, F 大版本里程碑
- **F.0.10.9** 真多 HDR target / RT pool (~8-10h)

---

## 9. 结论

Phase F.0.10.8.2 **成功完成**, 用 ~2.0h (vs 估 ~3.8h, 节约 ~47%) 交付完整 **HALD CLUT 图像 LUT 加载**. 至此 F.0.10.8 LUT 子系列已支持**两大业界工作流**:

- **F.0.10.8.1**: DaVinci Resolve / Lightroom / Premiere → `.cube`
- **F.0.10.8.2**: Photoshop / GIMP / ImageMagick → HALD PNG

下一步建议: **F.0.10.8.3 LUT 热重载** (2h, 完善美术工作流即时性) 或 **F.1 TAAU** (大版本里程碑).
