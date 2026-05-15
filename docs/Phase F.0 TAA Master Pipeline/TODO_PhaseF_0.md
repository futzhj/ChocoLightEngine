# Phase F.0 TAA 主管线 — TODO 待办清单

> 6A 工作流 · 阶段 6 (Assess) · TODO 收尾文档
> 关联：`PLAN_PhaseF_0.md` / `ACCEPTANCE_PhaseF_0.md` / `FINAL_PhaseF_0.md`

---

## 1. 必做（阻塞性，本 Phase 内）— ✅ 全部完成

| 任务 | 操作 | 状态 |
|------|------|--------|
| commit + push 代码到 main | `bc82376` pushed to `origin/main` | ✅ 完成 |
| 监控 GitHub Actions CI 6/6 平台 success | Run `25914279471` 11m01s, 6/6 PASS | ✅ 完成 |
| CI 状态回填 ACCEPTANCE/FINAL/TODO | 三份文档均已更新 | ✅ 完成 |

---

## 2. 推荐（短期，Phase F.0.x 候选）

| 候选 | 价值 | 工作量 | 优先级 |
|------|------|--------|--------|
| Phase F.0.1 — TAA Sharpening (Filmic 1-tap) | 弥补 super-sampling 模糊 | 0.5 天 | 🟡 中 |
| Phase F.0.4 — Anti-flicker filter | 消除高 luminance firefly 闪烁 | 0.5 天 | 🟡 中 |
| Phase F.0.5 — Half-res history RT | VRAM -75% (移动端 4K 友好) | 1 天 | 🟡 中 |
| Phase F.0.2 — YCoCg color-space clip | clip 鲁棒性 +20% | 1 天 | 🟢 低 |
| Phase F.0.3 — Variance clipping | 比 AABB 更鲁棒 | 1 天 | 🟢 低 |

---

## 3. 长期 / 未来候选

| 候选 | 价值 | 依赖 |
|------|------|------|
| Phase F.1 — DLSS-like upscale | 性能 +50% 渲染分辨率降至 0.7x | TAAU 算法落地 |
| Phase F.2 — Bloom 输入用 TAA 后 sharp HDR | Bloom 质量 +10% | 调整 EndScene pipeline 顺序 |
| Phase F.3 — TAAU 完整方案 | 替代 DLSS/FSR2 路径 | 视厂商授权情况 |

---

## 4. 用户指引（此次 Phase 后建议操作）

### 4.1 启用 TAA 的标准流程

```lua
local Gfx = require 'Light.Graphics'
local HDR, SSR, TAA = Gfx.HDR, Gfx.SSR, Gfx.TAA

-- 1. HDR 必须先启用 (TAA 依赖 HDR sceneTex + velocity buffer)
HDR.Enable(1280, 720)

-- 2. 推荐：启用 TAA 时关 SSR Temporal (避免双 temporal 模糊反射)
if SSR.IsEnabled() then SSR.SetTemporalEnabled(false) end

-- 3. 启用 TAA 主管线 (默认 OFF, 用户主动 Enable)
if TAA.IsSupported() then
    TAA.Enable(1280, 720)
    TAA.SetBlendAlpha(0.92)         -- 0.85=响应快/0.95=累积稳/0.99=几乎不更新
    TAA.SetNeighborhoodClip(true)   -- 默认即开, 防 ghosting (业界共识)
    TAA.SetJitterEnabled(true)      -- 默认即开, super-sampling 必开
end
```

### 4.2 TAA 与 SSR Temporal 共存的取舍

| 配置 | 视觉 | 性能 | 推荐 |
|------|------|------|------|
| 仅 TAA | 整体抗锯齿 + 反射继承场景 temporal | 0.10 ms | ⭐⭐⭐⭐⭐ 推荐 |
| 仅 SSR Temporal | 反射稳定; 几何边缘有锯齿 | 0.10 ms | ⭐⭐⭐ |
| TAA + SSR Temporal | 反射被 temporal 两次 (略过 blur) | 0.20 ms | ⭐⭐ 不推荐 (过度 ghosting 风险) |
| 都关 | 几何/反射均锯齿 | 0 | ⭐ baseline |

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源（TAA shader 内嵌 render_gl33.cpp）
- 不需要修改 CMake 第三方库依赖（仅加 1 行 `taa_renderer.cpp`）
- 不需要修改 .env / 任何运行时配置文件
- 不需要 API key

### 已修改的 build / CI 配置

- `@e:/jinyiNew/Light/ChocoLight/CMakeLists.txt`：新增 1 行 `${CHOCO_SRC}/taa_renderer.cpp`
- `@e:/jinyiNew/Light/.github/workflows/build-templates.yml`：新增 phaseF0Smoke 行 + runtime smoke 调用 1 行

---

## 6. CI 回填

| 字段 | 值 |
|------|---|
| GitHub Run ID | `25914279471` |
| Commit hash | `bc823760c2c2fec9a00c65effde2078679ecafa2` (short: `bc82376`) |
| 6/6 平台状态 | ✅ build-windows / build-linux / build-macos / build-android / build-ios / build-web 全 success |
| Total duration | **11m01s** (2026-05-15T10:59:14Z → 2026-05-15T11:10:15Z) |
| Date | 2026-05-15 |

同步更新状态：
- ✅ `ACCEPTANCE_PhaseF_0.md` 第 7 章
- ✅ `FINAL_PhaseF_0.md` 第 8 章
- ✅ `TODO_PhaseF_0.md` 本章

---

## 7. 总结

Phase F.0 实施完整，**无阻塞性遗留**。主要交付：
- backend 双 projection 架构（raster jittered + sample unjittered）
- 完整 TAA shader (GLES + GL3.3 双版本)
- 13 Lua API 子表 (`Light.Graphics.TAA.*`)
- HDR EndScene 集成 + light_ui ApplyJitter hook
- smoke / demo / docs 全套
- CMake + CI workflow 集成

Phase E velocity 链路在 F.0 后呈完美闭环：
- **VRAM 三层优化**：E.14 RG8 + E.17 MB halfRes + E.18.1 dilation halfRes
- **性能三层优化**：E.18 共享 dilation pass + E.18.1 dilation halfRes + E.18.2 single-consumer skip
- **应用层集大成**：F.0 TAA 主管线（Halton + history + clip + blend）

下一步候选：
1. Phase F.0.x 系列优化（Sharpening / anti-flicker / halfRes history 等）
2. Phase F.1 DLSS-like upscale (TAAU)
3. 切到非渲染方向（音频空间化、UI、Editor、ECS 网络、物理等）
