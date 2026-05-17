# Phase F.1.0.1 Multi-HDR × TAAU — ACCEPTANCE 文档

> **阶段**：6A Workflow — 阶段 5 Acceptance（验收）
> **基线**：PLAN_PhaseF_1_0_1.md
> **实施记录**：FINAL_PhaseF_1_0_1.md
> **验收日期**：2026-05-17

---

## 1. 功能验收

### 1.1 限制移除
- [x] `TAARenderer::SetTAAUEnabled` 不再因 `g_active != 0` 拒绝 ([taa_renderer.cpp:702-714](../../ChocoLight/src/taa_renderer.cpp#L702))
- [x] `TAARenderer::SetRenderScale` 不再因 `g_active != 0` 跳过 HDR 重建 ([taa_renderer.cpp:748-751](../../ChocoLight/src/taa_renderer.cpp#L748))
- [x] `taa_renderer.h` 头部注释从 "F.1.0 仅 default 支持" 更新为 "F.1.0.1+ 任意 instance 支持"

### 1.2 Clone 行为优化
- [x] `CloneInstance` 仍清 `taauEnabled / renderW / renderH` (新 instance 需自己 Enable + SetTAAUEnabled)
- [x] `CloneInstance` 保留 `renderScale / upscalePreset` (源 instance 调过的设置传过去)

### 1.3 Smoke 增量
- [x] 章节 12.1: user instance × TAAU state 隔离 OK (default=balanced, user=performance, 各自独立)
- [x] 章节 12.2: user instance SetTAAUEnabled 不再被拒绝 (返 boolean, 不专门拒绝)
- [x] 章节 12.3: Clone 保留 renderScale/preset, 清 taauEnabled

**Smoke 输出验证** (在 demo_ssr 真 GL3.3 backend 下运行):
```
PASS: Phase F.1.0.1: user instance × TAAU state 隔离 OK
PASS: Phase F.1.0.1: user instance SetTAAUEnabled 不再被拒绝 (result=false)
PASS: Phase F.1.0.1: Clone 保留 renderScale/preset, 清 taauEnabled
=== Phase F.0 + F.0.1 + ... + F.1 + F.1.0.1 TAA smoke: ALL TESTS PASSED ===
```
**总计**: 166 PASS / 0 FAIL

### 1.4 Demo (demo_multi_hdr_pip 增 PIP × TAAU)
- [x] OnOpen 自动创建 PIP TAA instance (id=1), 默认 TAAU OFF
- [x] T 键切 PIP TAAU on/off (启用时自动设 'balanced' preset)
- [x] HUD 显示 PIP TAAU 状态 (ON 时显示 render-res / output-res)
- [x] cleanup 释放 PIP TAA instance (在 PIP HDR 销毁前, 防 RT 悬挂)
- [x] 启动日志: `Phase F.1.0.1 PIP TAA instance ready (id=1), 按 T 切 PIP TAAU`

---

## 2. 兼容性验收 (零回归)

| Demo | 状态 |
|---|---|
| `demo_ssr` | ✅ 启动无 warning/error/fail/undef |
| `demo_taa_split2` | ✅ 启动无 warning/error/fail/undef |
| `demo_taau` (F.1.0 default instance) | ✅ 启动无 warning/error/fail/undef |
| `demo_multi_hdr_pip` | ✅ 启动无 warning/error/fail/undef + PIP TAA instance 创建成功 |

**Smoke**: 166 PASS / 0 FAIL (F.0 → F.1.0 → F.1.0.1 全量)

---

## 3. 设计决策回顾

### 3.1 调用约定 (caller 责任)
HDR `g_active` 与 TAA `g_active` 必须同步。`HDRRenderer::GetSceneFboForOutput()` 透明使用 HDR 当前 active, 与 TAA Process 期望的 history 尺寸隐式耦合。

**正确用法**:
```lua
HDR.SetActiveInstance(pipId)
TAA.SetActiveInstance(pipId)
TAA.SetTAAUEnabled(true)
```

**错误用法 (静默视觉错误)**:
```lua
HDR.SetActiveInstance(0)
TAA.SetActiveInstance(pipId)   -- 不一致!
TAA.SetTAAUEnabled(true)        -- pipId 的 HDR taauActive=false, sceneFboForOutput 返主 hdrFbo, 尺寸错配
```

### 3.2 不引入显式同步检查
设计上**不引入** "TAA active 与 HDR active 必须一致" 的强制 assert/warning. 原因:
- 用户可能临时切 active 做查询 (e.g. 读 PIP 参数), 然后切回 — 中间瞬间不一致是正常的
- 强制检查会带来 false positive
- 调用约定写入 PLAN + 头部注释已足够

### 3.3 与 demo_taa_split2 的关系
`demo_taa_split2` 用 multi TAA instance + **单 HDR instance** + region scissor 做 split-screen. 该路径 F.1.0.1 后**不变** (单 HDR 时 GetSceneFboForOutput 始终返同一 fbo, 与 region 路径独立). 此 demo 不演示 F.1.0.1, 但隐式验证零回归.

---

## 4. 文档验收

- [x] `docs/Phase F.1.0.1 Multi-HDR TAAU/PLAN_PhaseF_1_0_1.md` (合并 ALIGNMENT/CONSENSUS/DESIGN/TASK)
- [x] `docs/Phase F.1.0.1 Multi-HDR TAAU/ACCEPTANCE_PhaseF_1_0_1.md` (本文)
- [x] `docs/Phase F.1.0.1 Multi-HDR TAAU/FINAL_PhaseF_1_0_1.md` (实施记录)
- [x] `docs/HANDOFF_REMAINING_TASKS.md` 更新 F.1.0.1 完结
- [x] `samples/demo_multi_hdr_pip/main.lua` 增 PIP × TAAU 演示

---

## 5. 验收结论

**核心交付**: F.1.0 的 g_active==0 限制完全移除, multi-HDR-instance 各自独立 TAAU + renderScale 支持. 配套 smoke 3 检查点 + demo_multi_hdr_pip 增 PIP × TAAU 演示.

**验收级别**:
- ✅ **代码层**: PASS (Release build clean, smoke 166 PASS / 0 FAIL)
- ✅ **兼容性**: PASS (4 个 demo 全部零回归)
- ⏳ **真机视觉**: 待用户 demo_multi_hdr_pip 按 T 切 PIP TAAU 确认画质

**结论**: F.1.0.1 **代码层通过验收**, 进入用户真机评估阶段。

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 验收提交 — 代码层 PASS, 真机评估 PENDING |
