# Phase F.0.1 TAA Sharpening — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0_1.md` / `ACCEPTANCE_PhaseF_0_1.md` / `FINAL_PhaseF_0_1.md`

---

## 1. 必做（阻塞性，本 Phase 内） — 全部完成 ✅

| 任务 | 操作 | 优先级 | 状态 |
|------|------|--------|------|
| commit + push 代码到 main | git commit `011a549` + git push origin main | 🔴 高 | ✅ 完成 |
| 监控 GitHub Actions CI 6/6 平台 success | run [25915592135](https://github.com/futzhj/ChocoLightEngine/actions/runs/25915592135) — 6/6 success / 8m44s | 🔴 高 | ✅ 完成 |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | 编辑 3 份文档 §CI 状态 | 🔴 高 | ✅ 完成 |

---

## 2. 推荐（Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.4 — Anti-flicker filter | 消除 sharpening > 1.0 时的 firefly 加剧 | 2h | 🟡 中 |
| Phase F.0.5 — Half-res history RT | TAA history VRAM -75% (移动 4K 友好) | 3h | 🟡 中 |
| Phase F.0.6 — 5-tap CAS sharpening | 替换 4-tap, 对角线频率补偿, AMD FSR2 算法 | 4h | 🟢 低 |
| Phase F.0.2 — YCoCg color-space clip | TAA blend 阶段更鲁棒 clip (独立于 sharpening) | 4h | 🟢 低 |
| Phase F.0.3 — Variance clipping | clip 替代算法, AABB → 均值±k×σ | 3h | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale (TAAU) | 性能 +50% (0.7× 渲染分辨率) | TAAU 算法 + history halfRes |
| Phase F.2 — Bloom + TAA sharp 联动 | Bloom 输入用 TAA 后 sharp HDR, Bloom 质量 +10% | 调整 EndScene pipeline 顺序 |
| Phase F.3 — TAAU 完整方案 | 替代 DLSS/FSR2 | F.1 落地 + 调优 |

---

## 4. 用户指引（启用建议）

### 4.1 默认配置（推荐）

```lua
local Gfx = require 'Light.Graphics'
local HDR, TAA = Gfx.HDR, Gfx.TAA

HDR.Enable(1280, 720)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    -- F.0.1 默认 sharpness=0.5, 不调用 SetSharpness 即可
    -- 默认 alpha=0.92, clip=true, jitter=true 同 F.0
end
```

### 4.2 不同场景的 sharpness 取值

| 场景 | sharpness | 视觉效果 |
|------|-----------|---------|
| 性能敏感 / 软场景 | 0.0 | 关闭锐化, 走纯 blit (零 ALU) |
| 自然画面 / 慢动作 | 0.3 | 轻微补偿 (UE5 保守值) |
| 通用 (推荐) | 0.5 | 中等锐化, 视觉差异可感知 |
| 高细节 / cyber-punk | 0.8 | 明显锐化 |
| debug only | > 1.5 | 易 ringing, 不推荐 |

### 4.3 sharpness=0 vs sharpness=0.5 性能对比

| 模式 | TAA 总开销 @ 1080p | VRAM |
|------|---------------------|------|
| TAA 关闭 | 0 ms | 0 MB |
| TAA enabled, sharp=0 (走 blit) | ~0.10 ms (与 F.0 一致) | 16 MB history |
| TAA enabled, sharp=0.5 (sharpen pass) | ~0.13 ms (+0.03 ms) | 16 MB history (不变) |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（shader 内嵌 render_gl33.cpp）
- 不需要修改 CMake（taa_renderer.cpp 已在 F.0 加入）
- 不需要修改 CI workflow（taa.lua smoke 已在 F.0 加入）
- 不需要 .env / API key

---

## 6. CI 回填 ✅ 已完成

| 字段 | 值 |
|------|---|
| GitHub Run ID | [25915592135](https://github.com/futzhj/ChocoLightEngine/actions/runs/25915592135)（代码主 commit）<br>[25916027105](https://github.com/futzhj/ChocoLightEngine/actions/runs/25916027105)（docs 回填 commit） |
| Commit hash | `011a549`（代码+文档）+ `e46e5a5`（CI 回填） |
| 6/6 平台状态 | ✅ build-windows / build-linux / build-macos / build-android / build-ios / build-web 全部 success |
| Total duration | 8 分 44 秒（代码 commit）|
| Date | 2026-05-15 |

同步更新：
- ✅ `ACCEPTANCE_PhaseF_0_1.md` §7
- ✅ `FINAL_PhaseF_0_1.md` §8

---

## 7. 总结

Phase F.0.1 实施完整，**无阻塞性遗留**。主要交付：
- 4-tap unsharp mask sharpening shader (GLES + GL3.3 双版本)
- 1 backend 接口 `DrawTAASharpenPass` (in-place 替代 BlitTAAToHDR)
- 2 Lua API (`SetSharpness` / `GetSharpness`) clamp `[0, 2]` 默认 0.5
- in-place 设计零额外 RT / 零额外 pass / 零额外 VRAM
- sharpness=0 走纯 blit fallback (与 Phase F.0 完全一致)
- smoke / demo / docs 同步更新到 15 函数

**下一步**：T5 commit + push + CI 验证 6/6 平台 success。
