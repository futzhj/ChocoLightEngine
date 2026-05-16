# Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 ALIGNMENT

> 6A · 阶段 1 (Align) · 模糊需求 → 精确规范

---

## 1. 项目上下文

- **基础**: F.0.10.8 提供 `CreateLUT3D` byte API, F.0.10.8.1 加 `.cube` 文本格式. 但 Photoshop / GIMP / ImageMagick 工作流主流输出是 **HALD CLUT** (PNG/JPG image LUT).
- **目标**: 加 `HDR.LoadHaldLUT(path)` 一行调用, 让 PNG/JPG 形式的 LUT 直接落地. 拓展 LUT 来源 (Photoshop "Color Lookup" + ImageMagick + GIMP 全部走 HALD).

---

## 2. HALD CLUT 标准

### 2.1 定义 (ImageMagick / GIMP / Photoshop 通用)

- **HALD level N**: 一个二维方阵图像, 内部按 raster scan 存储 3D LUT 数据
- 图像分辨率: **N³ × N³** 像素 (N² × N² × N² 立方体展开成 (N³)² 方阵)
- LUT 立方体大小: `size = N²`
- LUT entry 数: `size^3 = N^6 = (N³)²` = 像素数 ✓

### 2.2 主流尺寸

| Level N | 图像 (像素) | LUT size | LUT entries | PNG 文件 ~ |
|---------|------------|----------|-------------|-----------|
| 2 | 8 × 8 | 4 | 64 | < 1KB |
| 3 | 27 × 27 | 9 | 729 | ~5KB |
| 4 | 64 × 64 | 16 | 4096 | ~15KB |
| 6 | 216 × 216 | 36 | 46656 | ~120KB |
| **8** | **512 × 512** | **64** | **262144** | **~600KB** |
| 12 | 1728 × 1728 | 144 | 2985984 | ~5MB (out of range) |

**N=8 是工业标准** (Photoshop / DaVinci HALD / GIMP CLUT)

### 2.3 像素 → LUT 映射

```
pixel(x, y), 其中 x ∈ [0, N³), y ∈ [0, N³)
LUT entry (R, G, B) idx = y * N³ + x

按 R 最快变 (与 GL 3D texture / .cube 数据布局**完全一致**):
LUT entry idx = ((B * size + G) * size + R)

→ 像素 raster scan 顺序 = LUT byte 数据顺序, **零 reshape**, 直接 memcpy.
```

### 2.4 典型 HALD identity 文件 (level=2, 8×8 像素)

像素 (x, y) 对应 LUT (r, g, b):
- r = (y * 8 + x) % 4
- g = ((y * 8 + x) / 4) % 4
- b = (y * 8 + x) / 16

输入像素颜色 = identity LUT 时 = 该 LUT entry 的 RGB:
- pixel (0,0) = (0, 0, 0) (LUT[0,0,0])
- pixel (1,0) = (85, 0, 0) (LUT[1,0,0], r=1/3)
- ...
- pixel (7,7) = (255, 255, 255) (LUT[3,3,3])

---

## 3. 边界确认 (Scope)

### 3.1 In Scope (本 phase)

- 解析 PNG / JPG / BMP / TGA (stb_image 全支持)
- HALD level N ∈ [2, 8] → LUT size N² ∈ [4, 64]
- 强制 4 通道 RGBA decode (drop alpha → RGB byte)
- 像素 → byte 直接拷 (R 最快变)
- C++ 内部 `LoadHaldLUTFile(path)` + Lua wrap `HDR.LoadHaldLUT(path)`
- 错误情况完整覆盖 (file I/O / 非方阵 / 非 N³ / size 越界)

### 3.2 Out of Scope (后续 phase)

- ❌ HDR HALD (16-bit PNG / EXR) - 本 phase 仅 8-bit
- ❌ sRGB → linear 转换 - 本 phase 假设 sRGB
- ❌ Stripe LUT (1×N³ 长条版本, ImageMagick 也支持但少见)
- ❌ LUT 热重载 (留 F.0.10.8.3)

---

## 4. 用户故事

```lua
-- 用户从 Photoshop / GIMP 导出 my_lut.png (Hald level 8)
local lut_id = HDR.LoadHaldLUT("assets/luts/my_lut.png")
HDR.SetGradingLUT(lut_id, 1.0)
-- 自动应用 64³ LUT 到所有 HDR.Tonemap 输出
```

---

## 5. 错误处理

| 错误 | 返回 |
|------|------|
| 文件不存在 | `nil, "stbi_load failed: <reason>"` |
| 解码失败 (PNG 损坏) | `nil, "stbi_load failed: <reason>"` |
| 非方阵 (w ≠ h) | `nil, "image not square: WxH"` |
| width 不是完美立方 (∛width ∉ ℤ) | `nil, "image size <W> is not N³ for any integer N (HALD format)"` |
| LUT size = N² 越界 [4, 64] | `nil, "HALD level <N> → LUT size <S> out of range [4, 64]"` |
| backend 失败 | `nil, "backend CreateLUT3D failed"` |

---

## 6. 技术决策矩阵

| # | 决策 | 选项 | 选择 | 理由 |
|---|------|------|------|------|
| 1 | 实现位置 | (a) hdr_renderer.cpp / (b) 新模块 | **(a) hdr_renderer** | 与 .cube parser 同模块, 一致性 |
| 2 | 图像库 | (a) stb_image (项目已用) / (b) 自写 PNG decoder | **(a) stb_image** | 已在项目, header-only, 零新依赖 |
| 3 | 解码通道 | (a) 强制 4 (RGBA) / (b) 跟原图 | **(a) 强制 4** | 简化逻辑, 性能差异忽略 |
| 4 | alpha 通道 | (a) drop / (b) 转 alpha | **(a) drop** | LUT 无 alpha 概念 |
| 5 | 颜色空间 | (a) 直接 byte / (b) sRGB → linear | **(a) byte** | 与 .cube + F.0.10.8 一致 |
| 6 | level N 范围 | (a) [2, 8] / (b) [2, 16] | **(a) [2, 8]** | size N² ∈ [4, 64] 与 F.0.10.8 一致 |
| 7 | level 求解 | (a) ∛width 整数 / (b) lookup table | **(a) ∛** | 简洁; size 仅 7 种值, lookup 也 OK 但不必要 |
| 8 | API 命名 | (a) LoadHaldLUT / (b) LoadHALDLUT / (c) LoadCLUT | **(a) LoadHaldLUT** | "Hald" 是人名 (Hald & Aas), 同 .cube → LoadCubeLUT 命名一致 |
| 9 | 内存管理 | (a) stbi_image_free / (b) bytes vector copy | **(a) 直接用 + free** | 像素数据已是 RGBA byte stream, 取 RGB stride 直接传 backend, 之后 free |
| 10 | RGBA → RGB 提取 | (a) memcpy with stride / (b) std::vector copy | **(b) vector** | 简洁, 性能差异忽略 (一次性启动加载) |
| 11 | 错误信息携带尺寸 | (a) yes / (b) no | **(a) yes** | 用户调试 "image not square 511×512" |
| 12 | 测试 fixture 来源 | (a) 在线生成 / (b) 静态文件 | **(a) 启动时生成** | smoke 不依赖 disk 静态资源, 自包含 |

---

## 7. 验收标准

### 7.1 功能

- ✅ `HDR.LoadHaldLUT(path)` 加载合法 HALD PNG → tex_id > 0
- ✅ 不存在文件 → nil + err
- ✅ 非方阵 → nil + err
- ✅ width 非 N³ → nil + err
- ✅ level 越界 (N>8 或 N<2) → nil + err

### 7.2 性能

- HALD level 8 (512×512 PNG, 64³ LUT) decode + reshape < 100ms
- HALD level 4 (64×64 PNG, 16³ LUT) < 10ms

### 7.3 测试

- HDR smoke §16 加 5+ PASS:
  - 不存在文件
  - 文本文件 (.txt) decode 失败
  - 非方阵 image (写 1×1 BMP 测试)
  - HALD level=2 (8×8 identity) → tex_id (or backend err)
  - HALD level=4 (64×64 identity) → tex_id (or backend err)
- demo headless probe 加 1 PASS

### 7.4 demo

- (可选) 加 `samples/demo_taa_split2/luts/sepia_hald8.png` 真 HALD 样本
- demo 主循环可加载切换演示 (留 TODO)

---

## 8. 工作量预算

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN/DESIGN/TASK | 调研 + 决策 + 文档 | 0.5h |
| SP1 (parser) | hdr_renderer.h/cpp 加 LoadHaldLUT* | 1.5h |
| SP2 (Lua + smoke + demo) | 注册 + smoke 5 PASS + demo probe | 1.5h |
| 6A Assess | FINAL + TODO | 0.3h |
| **合计** | | **~3.8h** |

vs 估 4h, 与预算一致.

---

## 9. 共识

实现 **HALD CLUT 标准 PNG/JPG → 3D LUT 加载**, 让 F.0.10.8 接入 Photoshop / GIMP / ImageMagick 工作流. 工作量 ~3.8h, 风险低 (stb_image 已在项目稳定使用).
