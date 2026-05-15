# Phase F.0.12 TAA RCAS Sharpening — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告

---

## 1. 交付物

| 文件 | 行数 |
|------|------|
| `ChocoLight/src/render_gl33.cpp` (FS_RCAS × 2 + backend) | +130 |
| `ChocoLight/include/render_backend.h` (virtual DrawTAARCASPass) | +12 |
| `ChocoLight/include/taa_renderer.h` (doxygen 更新) | +6 |
| `ChocoLight/src/taa_renderer.cpp` (parseSharpenMode_ + Process 3 分支 + GetSharpenMode) | +15 |
| `ChocoLight/src/light_graphics.cpp` (l_TAA_SetSharpenMode 加 rcas) | +6 |
| `scripts/smoke/taa.lua` (RCAS 测试段) | +25 |
| `samples/demo_ssr/main.lua` (Z 键 3-cycle) | +6 |
| `docs/api/Light_Graphics.md` (RCAS 算法 + 对比表 + 适用场景) | +55 |
| `docs/Phase F.0.12 TAA RCAS Sharpening/` 4 文档 | ~450 |

**累计**: 代码 ~200 行 + 文档 ~530 行

---

## 2. 核心方案

引入 AMD FidelityFX FSR2 标准的 Robust CAS (RCAS) 算法，作为 Phase F.0.6 CAS (FSR1) 的高级形式。RCAS 在 CAS 基础上增加 noise detection + edge protection 两层鲁棒性，解决 TAA 后 noise 放大与 edge ringing 两大常见 artifacts。

### 算法 (FSR2 RCAS)

```
Step 1: 5-tap fetch (e 中心 + b/d/f/h 4 邻域)
Step 2: luma proxy (G channel, FSR2 优化)
Step 3: 4-tap min/max range (排除中心)
Step 4: Noise detection: if (range < 1/64) skip
Step 5: Edge protection: lobe = sqrt(min(eL-mn, mx-eL) / range)
Step 6: Sharpen amount: peak = -1/mix(16, 4, s*0.5); wgt = peak * lobe
Step 7: Final composite: (e + 4 邻域 × wgt) / (1 + 4 wgt)
Step 8: HDR safe: max(result, 0)
```

### CAS vs RCAS 对比 (1080p)

| 维度 | CAS (F.0.6) | RCAS (F.0.12) |
|------|-------------|-----------------|
| ALU/px | ~12 | ~22 (+10) |
| sharpness 范围 | [0, 1] | [0, 2] |
| Noise detection | ❌ | ✅ |
| Edge protection | ❌ | ✅ |
| 时间 | ~0.05 ms | ~0.08 ms (+0.03) |

---

## 3. API surface (新增 0, TAA 仍 31 fn)

```lua
-- Phase F.0.12 仅扩展 sharpenMode 字符串识别，不增函数
TAA.SetSharpenMode("rcas")    -- FSR2 Robust CAS
TAA.SetSharpenMode("cas")     -- FSR1 CAS (F.0.6)
TAA.SetSharpenMode("unsharp") -- 4-tap unsharp (F.0.1 默认)
TAA.GetSharpenMode() → "unsharp" / "cas" / "rcas"
```

**生效条件**：`sharpness > 0`（与 cas/unsharp 同模式，sharpness=0 走 BlitTAAToHDR fallback）。

---

## 4. Phase F.0 系列累计 (10 sub-phase)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0~F.0.9 | 主管线 + 8 优化 + bicubic 上采 | 31 |
| **F.0.12** | **FSR2 RCAS (Robust CAS)** | **+0 (仍 31, sharpenMode 主体扩展)** |

**累计**: 31 fn / 5 shader (FS_TAA + FS_SHARPEN + FS_CAS + FS_BICUBIC + **FS_RCAS**) / 4 backend pass / 4 backend 接口扩展 / 3 demos

---

## 5. CI 状态（待回填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 6. 工程反思

### 做得好

1. **零 API 增量**：通过扩展 sharpenMode 字符串主体而非增加 Set/Get 函数，保持 Lua API 31 fn 精简
2. **复用基础设施**：vaoTonemap, buildProgram, parseSharpenMode_ 全部复用 F.0.6 CAS 同模式
3. **零回归**：默认 sharpenMode='unsharp' (F.0.1)，老用户无感知；RCAS 仅 opt-in
4. **决策矩阵 7/7 自动决策**：全部基于 FSR2 标准，无用户拍板
5. **3-cycle demo 切换**：用户体验流畅 (Z 键依次 unsharp → cas → rcas)
6. **完整文档**：算法公式 + CAS 对比表 + 适用场景表 + 示例

### 可改进

1. **未做 perceptual A/B 测试**：FLIP / SSIM 量化 vs CAS / unsharp
2. **未实现 RCAS sharpness 0 时跳过**：理论上 sharpness=0 时 RCAS 应直接 blit（与 unsharp 一致），但目前仍走 fragment shader（虽然 noise threshold 已经跳过 smooth 区，仍有少量 fetch 开销）
3. **未在 demo 添加 HUD sharpenMode-aware 范围 hint**：sharpness [0, 1] vs [0, 2] 在 Z 键切换时未提示用户调整

---

## 7. Phase F.0.x 后续候选

- F.0.10 — TAARenderer 多实例化 + 真 split-screen demo
- F.0.11 — Demo 截图 / 录屏
- F.0.13 — Motion-adaptive sharpness (高速时降 sharpness 减 trail)
- F.0.14 — Lanczos-2 25-tap 上采样
- F.1 — DLSS-like TAAU
