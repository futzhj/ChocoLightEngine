# Phase F.0.10.9.1 — Multi-HDR Pipeline Isolation 验证 PLAN

> 6A · ALIGN + DESIGN + TASK 合并 (验证型 phase, 无 C++ 改动)
> 工作量 ~1.5h

---

## 1. ALIGN — 任务对齐

### 1.1 目标

F.0.10.9 把 HDRRenderer 单例改成 multi-instance, 但仅验证了 instance 槽管理 (Create/Destroy/SetActive). 本 phase 验证:
- **per-instance state 真隔离** — exposure / gamma / tonemap / 5 autoXXX flag / velocity dilation 三 flag / LUT 应用 (lutTexId/lutStrength) 切 instance 不污染
- **LUT 系统 global 真共享** — hotReload / reload callback / watch list / LUT id 跨 instance 共用
- **Bloom/SSR/MB/SSAO/TAA 解耦正确** — 这 5 个 renderer 的 `Process(hdrFbo, hdrTex, ...)` 由 caller 传入 fbo/tex, 不直接耦合 active instance
- **TAA 与 HDR active 不自动联动** (设计) — 用户负责手动 TAA.SetActiveInstance + HDR.SetActiveInstance 同步; 测错配场景退化合理

### 1.2 边界

**In** (本 phase):
- smoke `hdr.lua` §22: per-instance state 隔离 (8 子项)
- smoke `hdr.lua` §23: LUT global 跨 instance 共享 (3 子项)
- smoke `hdr.lua` §24: TAA 与 HDR active 不联动 (2 子项)
- 验证 8 相关 smoke 仍零回归
- 文档 PLAN / FINAL / TODO

**Out** (本 phase 不做):
- C++ 代码改动 (F.0.10.9 已完成隔离, 本 phase 仅验证)
- demo live 演示 (留 F.0.10.9.2)
- 已知限制修复 (LUT id 全局共享时 DeleteLUT3D 不清其他 instance lutTexId — 留 F.0.10.9.x)
- Bloom/SSR/MB pyramid 跟随多 HDR instance (架构限制, 留后续)

### 1.3 决策矩阵

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | smoke 文件 | 加 §22-§24 到 `hdr.lua` (不新建文件) | 与 §21 同位, KISS |
| 2 | per-instance 测试范围 | 8 字段类组 (exposure/gamma/tonemap/auto5/dilation3/LUT2) | 覆盖所有 user-facing per-instance API |
| 3 | TAA 联动测试 | 仅验证不自动联动 (设计) | 文档化已知行为, 引导用户正确用法 |
| 4 | headless 验证 | API 行为可观察 (state field 切换), 不验证 GL RT | smoke 总是 headless, 与 §21 一致 |
| 5 | 是否改 demo | 不改 (round-trip 已在 F.0.10.9 demo probe 测) | 本 phase 纯 smoke 加固 |

### 1.4 风险

- **低**: per-instance 测试矩阵漏字段 — 缓解: 对照 State struct 8 类 50+ 字段逐项 review
- **低**: LUT global 拆分错误 (write 到 g 而非 g_global) — F.0.10.9 commit 已编译验证 + §21 跑通, 风险已落地

---

## 2. DESIGN — 验证矩阵

### 2.1 §22 Per-Instance State Isolation (8 子项)

| 子项 | 测试 | API |
|------|------|-----|
| 22.1 | exposure 隔离 | SetExposure / GetExposure |
| 22.2 | gamma 隔离 | SetGamma / GetGamma |
| 22.3 | tonemap operator 隔离 | SetTonemapper / GetTonemapper |
| 22.4 | autoTAA + autoBloom + autoSSR + autoMotionBlur + autoTonemap 5 flag 隔离 | SetAutoXXX / GetAutoXXX |
| 22.5 | velocityDilation + dilationHalfRes + dilationAutoSkip 3 flag 隔离 | SetVelocityDilation* / Get* |
| 22.6 | velocityFormat 隔离 | SetVelocityFormat / GetVelocityFormat |
| 22.7 | per-instance LUT 应用 (lutTexId + lutStrength) 隔离 | SetGradingLUT / GetGradingLUT* |
| 22.8 | 新 instance 默认值 = State{} 默认 (不继承 active 当前值) | CreateInstance + Get* 检查默认 |

**测试模板** (统一模式):
```lua
local default_v = HDR.GetX()           -- 0 槽默认
HDR.SetX(value_a)                       -- 改 0 槽
local id = HDR.CreateInstance()
HDR.SetActiveInstance(id)
local new_v = HDR.GetX()                -- 期望 = State{} 默认 (不继承)
HDR.SetX(value_b)                       -- 改 1 槽
HDR.SetActiveInstance(0)
assert(HDR.GetX() == value_a)           -- 0 槽未受 1 槽影响
HDR.SetActiveInstance(id)
assert(HDR.GetX() == value_b)           -- 1 槽保留自己的值
HDR.DestroyInstance(id)                 -- cleanup
HDR.SetX(default_v)                     -- restore 0 槽
```

### 2.2 §23 LUT Global Cross-Instance 共享 (3 子项)

| 子项 | 测试 |
|------|------|
| 23.1 | SetLUTHotReload(false) 切 instance 后仍 = false |
| 23.2 | SetLUTReloadCallback 切 instance 后 HasLUTReloadCallback 仍 = true |
| 23.3 | (不测 watch list — 需要真 LUT 文件 + 已在 §17/18 覆盖) |

### 2.3 §24 TAA 与 HDR active 不联动 (2 子项)

| 子项 | 测试 |
|------|------|
| 24.1 | HDR.SetActiveInstance(1) 后 TAA.GetActiveInstance() 仍 = 0 |
| 24.2 | 用户手动 TAA.SetActiveInstance + HDR.SetActiveInstance 双向同步 |

### 2.4 数据流图

```
Lua: HDR.SetExposure(0.5)              0 instance state.exposure = 0.5
   └─ g.exposure = 0.5  (g_states[0])

Lua: HDR.CreateInstance() → 1
Lua: HDR.SetActiveInstance(1)          切到 g_states[1]
   └─ g_active = 1
   └─ macro g 透明展开 → g_states[1] (默认 exposure=1.0)

Lua: HDR.GetExposure()                 读 g_states[1].exposure = 1.0 (新 instance)
Lua: HDR.SetExposure(2.0)              g_states[1].exposure = 2.0

Lua: HDR.SetActiveInstance(0)          切回 g_states[0]
Lua: HDR.GetExposure()                 应仍 = 0.5 (隔离 ✅)
```

---

## 3. TASK — 拆 sub-step

| Step | 内容 | 工作量 |
|------|------|------|
| **S1** | 写 PLAN (本文) | ~15min |
| **S2** | 加 §22 + §23 + §24 到 `scripts/smoke/hdr.lua` | ~30min |
| **S3** | 跑 smoke 验证 + 8 相关 smoke 零回归 | ~15min |
| **S4** | 写 FINAL + TODO | ~15min |
| **S5** | commit + push + CI 6/6 | ~15min |
| **合计** | | **~1.5h** |

---

## 4. 验收标准

| 类型 | 标准 |
|------|------|
| smoke `hdr.lua` | §22 8 子项 + §23 2 子项 + §24 2 子项 全 PASS |
| 8 相关 smoke 零回归 | hdr/bloom/ssr/auto_exposure/lens_fx/motion_blur/taa/lighting2d 全 PASS |
| CI | 6/6 绿 |
| 无 C++ 改动 | 本 phase 仅加 smoke + 文档 |

---

## 5. 后续接力

- **F.0.10.9.2**: demo live 主屏 1080p + PIP 480p 真不同分辨率
- **F.0.10.9.x**: 已知限制修 — DeleteLUT3D 时遍历所有 instance 清同 lutTexId 引用 (防悬挂)
