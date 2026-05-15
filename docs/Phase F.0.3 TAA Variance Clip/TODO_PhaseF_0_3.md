# Phase F.0.3 TAA Variance Clipping — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_3.md` / `ACCEPTANCE_PhaseF_0_3.md` / `FINAL_PhaseF_0_3.md`

---

## 1. 必做（阻塞性，本 Phase 内）

| 任务 | 操作 | 优先级 |
|------|------|--------|
| commit + push 代码到 main | ✅ 完成 (commit `15b0db7`) |
| 监控 GitHub Actions CI 6/6 平台 success | ✅ 完成 (Run 25927812437) |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | ✅ 完成 |

---

## 2. 推荐（Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.5 — Half-res TAA history RT | TAA history VRAM -75% (移动 4K 友好) | 3h | 🟡 中 |
| Phase F.0.7 — Split-screen A/B demo | 屏幕分两半，左 ycocg / 右 variance 直接可视化 | 2h | 🟢 低 |
| Phase F.0.6 — 5-tap CAS sharpening | 替换 F.0.1 4-tap, AMD FSR2 算法 | 4h | 🟢 低 |
| Phase F.0.8 — Motion-adaptive γ | 根据 velocity 长度动态调 γ (UE5 高级形式) | 3h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale (TAAU) | 性能 +50% (0.7× 渲染分辨率) | TAAU 算法 + history halfRes |
| Phase F.2 — Bloom + TAA sharp 联动 | Bloom 输入用 TAA 后 sharp HDR | 调整 EndScene pipeline 顺序 |
| FLIP / SSIM perceptual A/B | 自动量化 clip 算法画质收益 | 集成 NVIDIA FLIP 库 |
| Chroma rotation / OBB clip | 真正的椭球 clip (替代 AABB) | 协方差矩阵 SVD |

---

## 4. 用户指引（启用建议）

### 4.1 默认配置（推荐）

```lua
local Gfx = require 'Light.Graphics'
local HDR, TAA = Gfx.HDR, Gfx.TAA

HDR.Enable(1280, 720)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    -- 默认 clipMode="ycocg" (F.0.2), antiFlicker=true (F.0.4), sharpness=0.5 (F.0.1)
    -- 升级到 F.0.3 variance 仅需:
    TAA.SetClipMode("variance")           -- F.0.3
    TAA.SetVarianceGamma(1.0)             -- Salvi/UE5 默认 (可省, 默认就是 1.0)
end
```

### 4.2 不同场景的 ClipMode + γ 建议

| 场景 | ClipMode | VarianceGamma | 说明 |
|------|----------|----------------|------|
| **默认推荐** | `"ycocg"` (F.0.2) | — | 平衡性能与画质 |
| 高色度边缘场景 (cyber-punk / 旗帜) | `"variance"` | 0.75 | 严抑制 + 色度独立保护 |
| HDR firefly 场景 | `"variance"` | 1.0 | Salvi 默认，处理 outlier 最佳 |
| 快速运动 / 高动态场景 | `"variance"` | 1.5 | 略宽松避免 trail |
| 严格复现 F.0 行为 | `"rgb"` | — | 测试基线 |
| 性能极端敏感 | `"rgb"` | — | 零 ALU 增量 |

### 4.3 性能对比表

| 模式 | TAA 总开销 @ 1080p | 与 baseline 增量 |
|------|---------------------|------------------|
| F.0 only | ~0.10 ms | — |
| F.0 + F.0.2 (clipMode="ycocg") | ~0.15 ms | +0.05 |
| **F.0 + F.0.3 (clipMode="variance")** | **~0.17 ms** | **+0.07** |
| F.0 + F.0.3 + F.0.4 (推荐高画质) | ~0.18 ms | +0.08 |
| F.0 + F.0.1 sharp=0.5 + F.0.3 + F.0.4 | ~0.21 ms | +0.11 |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（shader 改造内嵌 render_gl33.cpp）
- 不需要修改 CMake（taa_renderer.cpp 已在 F.0 加入）
- 不需要修改 CI workflow（taa.lua smoke 已在 F.0 注册）
- 不需要 .env / API key

---

## 6. CI 回填（待 T6 完成后填）

| 字段 | 值 |
|------|---|
| GitHub Run ID | `25927812437` |
| Commit hash | `15b0db7` |
| 6/6 平台状态 | ✅ 全部 success |
| Date | `2026-05-15` |

回填完成 ✅:
- `ACCEPTANCE_PhaseF_0_3.md` 第 7 章 已更新
- `FINAL_PhaseF_0_3.md` 第 8 章 已更新

---

## 7. 总结

Phase F.0.3 实施完整，**无阻塞性遗留**。主要交付：

- YCoCg variance clipping (Salvi 2016 / UE5 default) 在 FS_TAA shader 内作为 `uClipMode==2` 第 3 个分支实现 (GLES + GL3.3 双源)
- König-Huygens 一遍计算公式 `σ² = m2 - m1²`，max(0) 防浮点负数
- 1 backend 接口参数 `float varianceGamma = 1.0f` (带默认值, 向后兼容)
- 2 Lua API (`SetVarianceGamma` / `GetVarianceGamma`) 默认 1.0，clamp [0, 4]
- 1 SetClipMode 白名单值扩展 ("variance")，大小写不敏感
- 与 F.0.4 anti-flicker 独立互补 (clip 限制 history 范围 + blend 降权 high-luma firefly)
- 与 F.0.1 sharpening 完全正交 (作用不同 pipeline 阶段)
- smoke 21 fn / demo HUD 条件 γ / docs 同步更新

**状态**: ✅ 全部交付完成，CI 6/6 平台验证通过。
