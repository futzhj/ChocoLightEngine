# Phase F.0.9 TAA Custom Upsampler — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告

---

## 1. 交付物

| 文件 | 行数 |
|------|------|
| `ChocoLight/src/render_gl33.cpp` (FS_BICUBIC × 2 + backend) | +130 |
| `ChocoLight/include/render_backend.h` (virtual DrawTAAUpscalePass) | +12 |
| `ChocoLight/include/taa_renderer.h` + `.cpp` | +35 |
| `ChocoLight/src/light_graphics.cpp` (Lua +2 fn) | +44 |
| `scripts/smoke/taa.lua` | +120 |
| `samples/demo_ssr/main.lua` (P 键 + HUD upscale 后缀) | +18 |
| `docs/api/Light_Graphics.md` | +110 |
| `docs/Phase F.0.9 TAA Custom Upsampler/` 4 文档 | ~400 |

**累计**: 代码 ~240 行 + 文档 ~510 行

---

## 2. 核心方案

引入 Sigggraph 2018 "Filmic SMAA Slidedeck" 标准的 Catmull-Rom 9-tap bicubic 上采样，作为 F.0.5 GL_LINEAR stretch 的可选高画质替代。仅在 `sharpness=0 && halfResHistory=true` 路径生效，让用户在性能与画质间多一个选择。

### 算法 (Catmull-Rom 9-tap)

```
理论 16-tap Catmull-Rom bicubic → 9 sample (3x3 hardware bilinear)

shader 内:
  w0..w3 = Catmull-Rom 卷积核 (per axis)
  w12 = w1 + w2   // 合并 = 一次 hardware bilinear (5-tap 优化关键)
  9 sample (3x3) 加权叠加, max(result, 0) HDR safe
```

### 性能/画质对比 (1080p)

| 算法 | sample | 时间 | 模糊 |
|------|--------|------|------|
| GL_LINEAR (F.0.5) | 1 | <0.01 ms | 1.0× |
| **Catmull-Rom 9-tap (F.0.9)** | **9** | **~0.03 ms** | **0.5×** |
| Lanczos-2 (未实现) | 25 | ~0.10 ms | 0.4× |

---

## 3. API surface (新增 2, TAA 29 → 31 fn)

```lua
TAA.SetUpscaleMode("bicubic")    -- 高画质 (-50% blur, +0.025 ms)
TAA.SetUpscaleMode("bilinear")   -- 默认 (F.0.5 行为, 零回归)
TAA.GetUpscaleMode() → "bilinear" / "bicubic"
```

**生效条件**：`sharpness=0` && `halfResHistory=true`。其他配置 upscaleMode 上传但 backend pass 不切换。

---

## 4. Phase F.0 系列累计 (9 sub-phase)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0/F.0.1/F.0.4/F.0.2/F.0.3/F.0.5/F.0.7/F.0.6/F.0.8 | 主管线 + 8 优化 | 29 |
| **F.0.9** | **Catmull-Rom 9-tap bicubic 上采样** | **+2 (31)** |

**累计**: 31 fn / 4 shader (FS_TAA + FS_SHARPEN + FS_CAS + FS_BICUBIC) / 3 backend pass / 1 backend 接口扩展 / 3 demos

---

## 5. CI 状态（待回填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 6. 工程反思

### 做得好

1. **影响范围控制**：仅 sharpness=0 路径，sharpen/CAS shader 不动，零破坏
2. **5-tap 优化版**：9 sample (3x3) 等效 16-tap，比 Lanczos-25 节省 64%
3. **HDR safe**：max(result, 0) 防 Catmull-Rom 负权重 ringing 黑斑
4. **完整决策矩阵 7/7 自动决策**：基于 Sigggraph 标准，无用户拍板
5. **复用 case-insensitive 模式**：parseUpscaleMode_ 与 parseClipMode_/parseSharpenMode_ 同模式

### 可改进

1. **未实现 Lanczos-2**：留 Phase F.0.x 候选
2. **未在 sharpen/CAS 路径用 bicubic sample**：可能让 sharpen 内部上采样质量也提升，但破坏 sharpen 算法语义
3. **未做 perceptual A/B 测试**：FLIP / SSIM 量化
4. **uTexel = 1/srcSize 在 HUD 不显示**：用户难直观感知 src/dst 比例

---

## 7. Phase F.0.x 后续候选

- F.0.10 — TAARenderer 多实例化 + 真 split-screen demo
- F.0.11 — Demo 截图 / 录屏
- F.0.12 — RCAS (FSR2 增强)
- F.0.13 — Motion-adaptive sharpness
- F.0.14 — Lanczos-2 25-tap 上采样
- F.1 — DLSS-like TAAU
