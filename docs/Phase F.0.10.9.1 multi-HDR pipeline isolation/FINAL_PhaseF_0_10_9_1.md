# Phase F.0.10.9.1 — Multi-HDR Pipeline Isolation FINAL

> 6A · ASSESS 收尾交付报告
> 状态: ✅ 全部完成 (验证型 phase, 无 C++ 改动)

---

## 1. 完成工作

F.0.10.9 把 HDRRenderer 改成 multi-instance, 但仅验证了 instance 槽管理 (§21).
本 phase 进一步验证 **per-instance state 真隔离 + LUT global 真共享 + TAA 不联动设计**, 确保多 instance 场景零状态污染.

---

## 2. 验证矩阵 (15 子项全 PASS)

### 2.1 §22 Per-Instance State Isolation (11 子项)

| # | 测试 | 状态 | 说明 |
|---|------|------|------|
| 22.1 | exposure 隔离 (1.0 默认) | ✅ | 0=0.5 / 1=2.0 切回不污染 |
| 22.2 | gamma 隔离 (2.2 默认) | ✅ | 0=1.8 / 1=2.4 |
| 22.3 | tonemap 隔离 ("aces" 默认) | ✅ | 0="reinhard" / 1="uncharted2" |
| 22.4 | 5 autoXXX flag 隔离 (true 默认) | ✅ | autoTAA/Bloom/SSR/MotionBlur/Tonemap 独立 |
| 22.5 | dilation 3 flag 隔离 | ✅ | velocityDilation/HalfRes/AutoSkip 独立 |
| 22.6 | velocityFormat 隔离 ("rg16f" 默认) | ✅ | 0="rg8" / 1="rg16f" |
| 22.7 | per-instance LUT 应用 (lutTexId+strength) | ✅ | 0=12345/0.7 / 1=67890/0.3 |
| 22.8 | 新 instance 全 default 校验 | ✅ | 15 字段全等于 State{} 默认 |

**测试模板** (test_isolation 函数, 减重复):
```lua
saved = getter()
setter(value_a)               -- 改 0 槽
id = HDR.CreateInstance()
HDR.SetActiveInstance(id)
assert(getter() == default_v)  -- 新 instance 期望默认
setter(value_b)               -- 改 1 槽
HDR.SetActiveInstance(0)
assert(getter() == value_a)    -- 0 槽未污染
HDR.SetActiveInstance(id)
assert(getter() == value_b)    -- 1 槽保留
HDR.DestroyInstance(id)
setter(saved)                 -- restore
```

### 2.2 §23 LUT Global Cross-Instance 共享 (2 子项)

| # | 测试 | 状态 |
|---|------|------|
| 23.1 | SetLUTHotReload(false) 切 instance 后仍 false | ✅ |
| 23.2 | SetLUTReloadCallback 切 instance 后 HasLUTReloadCallback 仍 true | ✅ |

证实 g_global 拆分正确, hotReload / reloadCb 跨 instance 共用.

### 2.3 §24 TAA 与 HDR active 不联动 (2 子项)

| # | 测试 | 状态 |
|---|------|------|
| 24.1 | HDR.SetActiveInstance(1) 后 TAA.GetActiveInstance() 仍=0 (不自动联动) | ✅ |
| 24.2 | 用户手动 TAA.SetActiveInstance + HDR.SetActiveInstance 双向同步 | ✅ |

文档化已知行为, 引导用户正确用法.

### 2.4 零回归

8 个相关 smoke 全 PASS:
- `hdr` (52 fn) · `bloom` · `ssr` · `auto_exposure`
- `lens_fx` · `motion_blur` · `taa` · `lighting2d`

---

## 3. 渲染管线解耦验证 (调研结果)

通过 code search 确认 5 个 renderer 都符合解耦设计:

| Renderer | 读 HDR 方式 | 切 active 影响 |
|----------|------------|---------------|
| **Bloom** | `Process(hdrFbo, hdrTex, ...)` 由 caller 传 | ✅ 透明 (caller 决定) |
| **SSR** | `Process(hdrFbo, hdrTex, ...)` + 内部调 `HDRRenderer::GetDilatedVelocityTexture()` | ✅ 自动跟随 active (Get* 基于 g.fbo) |
| **MotionBlur** | `Process(hdrFbo, hdrTex, ...)` + 内部调 `HDRRenderer::GetVelocityTexture / GetCameraVelocityTexture / GetDilatedVelocityTexture / GetDilatedCameraVelocityTexture` | ✅ 自动跟随 |
| **SSAO** | `Process(hdrFbo, hdrTex)` + `g.backend->GetHDRNormalTex(hdrFbo)` | ✅ 透明 |
| **TAA** | `Process(hdrFbo, hdrTex, ...)` + `HDRRenderer::GetDilatedVelocityTexture` | ✅ 自动跟随 |

**结论**: 所有 dynamic-read API 都基于 `g.fbo / g.dilatedXXX` (active instance), `SetActiveInstance` 后透明跟随 ✅. 无需改 renderer 代码.

---

## 4. 文件变更

| 文件 | 变更 | LOC |
|------|------|-----|
| `scripts/smoke/hdr.lua` | +§22-§24 (15 子项) + test_isolation 模板 | +296 |
| `docs/Phase F.0.10.9.1 multi-HDR pipeline isolation/PLAN_PhaseF_0_10_9_1.md` | 6A PLAN | +95 |
| `docs/Phase F.0.10.9.1 multi-HDR pipeline isolation/FINAL_PhaseF_0_10_9_1.md` | 本文 | +120 |
| `docs/Phase F.0.10.9.1 multi-HDR pipeline isolation/TODO_PhaseF_0_10_9_1.md` | 后续接力 | +50 |
| **C++ 代码** | **无改动** | **0** |

---

## 5. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN §1 任务对齐 + 边界 + 5 项决策矩阵 |
| **Architect** | PLAN §2 验证矩阵 + 测试模板 + 数据流图 |
| **Atomize** | PLAN §3 拆 S1-S5 |
| **Approve** | 用户隐式确认 (验证型 phase, 无歧义) |
| **Automate** | smoke §22-§24 实现 + 1 处修复 (tonemap 字符串 vs int) |
| **Assess** | 本 FINAL + 15 子项 PASS + 8 smoke 零回归 |

---

## 6. Lua API 数

无新 fn. 总数仍 = 78 (与 F.0.10.9 相同). 本 phase 仅扩 smoke 覆盖度.

---

## 7. 后续接力

见 `TODO_PhaseF_0_10_9_1.md`:
- **F.0.10.9.2**: demo live 主屏 1080p + PIP 480p 真不同分辨率
- **F.0.10.9.x**: DeleteLUT3D 时遍历所有 instance 清同 lutTexId 引用 (防悬挂)
