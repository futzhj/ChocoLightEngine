# Phase F.1.0.1 Multi-HDR × TAAU — PLAN 文档

> **阶段**：6A Workflow 合并版 (ALIGNMENT + CONSENSUS + DESIGN + TASK)
> **目标**：F.1.0 单 instance 限制 → 多 HDR instance 各自独立 TAAU
> **基线**：Phase F.1.0 (commit pending, 2026-05-17)
> **创建日期**：2026-05-17
> **配套**：FINAL_PhaseF_1_0_1.md (实施记录) + ACCEPTANCE_PhaseF_1_0_1.md (验收)

---

## 1. 背景

Phase F.1.0 单 instance TAAU 已交付, 但 `TAARenderer::SetTAAUEnabled` 显式拒绝 `g_active != 0` (即 user instance 1..3 不能启用 TAAU). 这阻碍了:

- **Multi-HDR PIP** (Phase F.0.10.9.2 演示) 的 PIP 区域不能用 TAAU 提速 — 主屏 1600×900 native + PIP 480×270 native 同时跑, 总像素 ~1.59M, 若 PIP 改 0.5 scale 渲染 (240×135) 节省 70% PIP raster 时间.
- **Split-screen 4 player** (Phase F.0.10.7 demo_taa_split2 模式) 每 player 独立画质策略 — 例如 Player1 高画质 (native), Player2 性能模式 (TAAU 0.5).

F.1.0.1 移除此限制, 让 user instance (1..3) 也能启用 TAAU.

## 2. 用户拍板的场景

**Per-instance HDR (PIP 多 HDR)** — Phase F.0.10.9 的多 HDR instance 各自独立 TAAU + 各自 renderScale.

每 HDR instance 已经独立持有:
- `outputW/H` (用户 Enable 入参)
- `width/height` (内部 sceneTex 尺寸, TAAU 模式 == renderW/H)
- `taauActive` / `outputSceneFbo` / `outputSceneTex`

每 TAA instance 独立持有:
- `taauEnabled` / `renderScale` / `upscalePreset` / `renderW/H`
- history RT × 2 (output-res, TAAU 模式下)

**关键不变量** (F.1.0.1 仍保持):
- HDR instance ID 与 TAA instance ID **必须一致** (caller 责任): 当 `HDR::SetActiveInstance(i) + TAA::SetActiveInstance(i)` 时, 整套 TAAU 链路作用于 instance i.
- HDR g_active 与 TAA g_active 错位时, `HDRRenderer::GetSceneFboForOutput()` 返当前 HDR active 的 outputSceneFbo, 与 TAA active 的 history 尺寸可能不匹配 → 视觉错误.

## 3. 设计

### 3.1 改动范围 (最小)

| 文件 | 改动 |
|---|---|
| `ChocoLight/src/taa_renderer.cpp` | 移除 `SetTAAUEnabled` 中 `g_active != 0` 限制 + 放开 `applyTAAUChange_` 中 `g_active == 0` 检查 |
| `ChocoLight/src/taa_renderer.cpp` | `CloneInstance` 不再强制 `taauEnabled=false` (允许从 src 继承) — 但仍清 renderW/H (待新 instance 自己 Enable+SetTAAUEnabled 重算) |
| `scripts/smoke/taa.lua` | 新增章节 11.9: multi-instance × TAAU smoke 检查 |
| `samples/demo_multi_hdr_pip/main.lua` | 增 PIP × TAAU 演示 (PIP 启用 TAAU balanced 0.667) |
| `docs/Phase F.1.0.1 Multi-HDR TAAU/{PLAN,ACCEPTANCE,FINAL}_PhaseF_1_0_1.md` | 3 件套文档 |

**预估代码改动**: ~30 行 + smoke 60 行 + demo 调整 ~40 行

### 3.2 关键代码改动

**taa_renderer.cpp::SetTAAUEnabled** (移除 `g_active==0` 限制):
```cpp
bool SetTAAUEnabled(bool flag) {
    if (g.taauEnabled == flag) return true;

    // Phase F.1.0.1: 移除 F.1.0 的 `g_active != 0` 限制, 让 user instance 也能启用 TAAU.
    //                调用方需保证 HDR g_active 与 TAA g_active 一致.

    if (!g.supported) {
        CC::Log(CC::LOG_WARN, "TAARenderer::SetTAAUEnabled: backend 不支持 TAA, 忽略");
        return false;
    }
    // ... 后续不变 (Q5 仲裁 + applyTAAUChange_)
}
```

**taa_renderer.cpp::SetRenderScale** (放开 g_active 检查):
```cpp
// 仅 taauEnabled 时触发 HDR 重建 (F.1.0.1: 移除 g_active==0 限制)
if (g.taauEnabled && g.enabled) {
    applyTAAUChange_();
}
```

**taa_renderer.cpp::CloneInstance** (保留 src 的 renderScale + upscalePreset, 但 RT 清零):
```cpp
// Phase F.1.0.1: clone 不再强制 taauEnabled=false; 但 RT 清零让新 instance 自己 Enable
g_states[i].taauEnabled   = false;   // 仍清: 新 instance 走自己 Enable + SetTAAUEnabled
g_states[i].renderW       = 0;
g_states[i].renderH       = 0;
// renderScale / upscalePreset 保留 (源 instance 调过的设置, 复制过来)
```

### 3.3 调用约定 (caller 责任)

```lua
-- 同步切 HDR + TAA active id, 让 GetSceneFboForOutput 返该 instance 的 outputSceneFbo
HDR.SetActiveInstance(pipId)
TAA.SetActiveInstance(pipId)
TAA.SetTAAUEnabled(true)
TAA.SetUpscalePreset('balanced')

-- 帧循环时, EndScene 内部 TAARenderer::Process 走 active=pipId, 整条链路对该 instance
HDR.BeginScene()  -- active=pipId 的 fbo
-- draw...
HDR.EndScene()    -- 内部 TAA.Process 作用于 pipId

HDR.SetActiveInstance(0)  -- 切回 main, 后续 default instance 路径
```

### 3.4 验收门槛

- ✅ smoke 章节 11.9 通过
- ✅ demo_multi_hdr_pip 启用 PIP × TAAU 后启动无 warning/error
- ✅ demo_ssr / demo_taa_split2 / demo_taau (F.1.0 default instance) 行为零回归

## 4. 风险

| 风险 | 缓解 |
|---|---|
| HDR / TAA g_active 错位导致视觉错误 | 文档明示 caller 责任; 加 debug log when GetSceneFboForOutput 在 TAA active 时 HDR active != TAA active |
| user instance 与 default instance 共享 backend TAA program (programTAA) | 这是已有架构, 不受 F.1.0.1 影响; 每 instance 独立的是 RT 和 state, 不是 program |
| outputSceneFbo 在 user instance HDR 未启用 TAAU 时为 0 → TAA Process 用 hdrFbo fallback | 已通过 `(g.taauEnabled ? HDRRenderer::GetSceneFboForOutput() : hdrFbo)` 解决, 行为正确 |

## 5. 任务拆分

| 任务 | 内容 | 估时 |
|---|---|---|
| T1 | 移除 SetTAAUEnabled / SetRenderScale 中 g_active==0 限制 + 更新注释 | 15 min |
| T2 | smoke 增 multi-instance × TAAU 章节 | 30 min |
| T3 | demo_multi_hdr_pip 增 PIP × TAAU 演示 | 45 min |
| T4 | 构建 + 验证 + 填 ACCEPTANCE/FINAL | 30 min |

**总预计**: 2 小时 (小型 phase, 不分 commit)
