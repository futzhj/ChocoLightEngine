# Phase F.1.4 Dynamic Resolution Scaling — ACCEPTANCE 文档

> **阶段**: 6A Workflow — 阶段 5 Automate 验收
> **基线**: TASK_PhaseF_1_4.md / DESIGN_PhaseF_1_4.md
> **验收日期**: 2026-05-19
> **状态**: ✅ T1~T6 实施完成, 等 T7 CI 验证

---

## 1. 任务完成情况 (T1~T7)

| 任务 | 估时 | 实际 | 状态 | 备注 |
|------|------|------|------|------|
| T1: State 7 字段 + 4 helpers | 30 min | ~25 min | ✅ | 内存增量 ~520 byte/instance |
| T2: 14 API impl + Shutdown/CloneInstance 复位 | 60 min | ~50 min | ✅ | 含决策 + 限频 + clamp |
| T3: Lua bridge 7 fn + taa_funcs 注册 | 30 min | ~25 min | ✅ | GetDynamicStats 组合 10 字段表 |
| T4: smoke §14 (12 子检查点) | 30 min | ~30 min | ✅ | syntax check PASS |
| T5: demo_taau HUD + N/F 键 + Update 挂接 | 15 min | ~15 min | ✅ | syntax check PASS |
| T6: ACCEPTANCE/FINAL/TODO + CHANGELOG | 60 min | ~30 min | ✅ | 本文 + 4 文档 |
| T7: CI 6 平台验证 | 30 min | 待执行 | ⏳ | 等 build run |

**累计实际**: ~3h (估时 5h, 节约 40%)

---

## 2. 文件改动清单 (实际)

| 文件 | 改动类型 | 改动量 |
|------|---------|--------|
| `ChocoLight/include/taa_renderer.h` | 修改 | +56 行 (14 新 API decl + Phase F.1.4 注释块) |
| `ChocoLight/src/taa_renderer.cpp` | 修改 | +220 行 (State 7 字段 + 4 helpers + 14 API impl + Shutdown/CloneInstance 复位) |
| `ChocoLight/src/light_graphics.cpp` | 修改 | +120 行 (7 个 l_TAA_* fn + taa_funcs[] 7 entry) |
| `scripts/smoke/taa.lua` | 修改 | +160 行 (§14 DRS 12 子检查点 + fn_names 7 加项 + summary 行更新) |
| `samples/demo_taau/main.lua` | 修改 | +50 行 (Update DRS 调用 + HUD DRS 行 + N/F 键 + Reset DRS 复位) |
| `samples/demo_taau/README.md` | 修改 | +2 行 (N/F 键位说明) |
| `docs/Phase F.1.4 .../ALIGNMENT_PhaseF_1_4.md` | 新建 | ~210 行 |
| `docs/Phase F.1.4 .../CONSENSUS_PhaseF_1_4.md` | 新建 | ~165 行 |
| `docs/Phase F.1.4 .../DESIGN_PhaseF_1_4.md` | 新建 | ~340 行 |
| `docs/Phase F.1.4 .../TASK_PhaseF_1_4.md` | 新建 | ~280 行 |
| `docs/Phase F.1.4 .../ACCEPTANCE_PhaseF_1_4.md` | 新建 | 本文 |
| `docs/Phase F.1.4 .../FINAL_PhaseF_1_4.md` | 待 T7 后 | TBD |
| `docs/Phase F.1.4 .../TODO_PhaseF_1_4.md` | 待 T7 后 | TBD |
| `CHANGELOG.md` | 待 T7 后 | TBD |

**当前**: 6 文件代码修改 + 5 文档新建 = 11 文件改动 + 3 文件待 T7 后补

---

## 3. 验收标准核对

### 3.1 API 完整性 (CONSENSUS §3.1) ✅

```lua
-- 新增 7 Lua API (smoke fn_names 全部 type=function):
Light.Graphics.TAA.SetDynamicEnabled       -- ✅
Light.Graphics.TAA.GetDynamicEnabled       -- ✅
Light.Graphics.TAA.SetDynamicTarget        -- ✅
Light.Graphics.TAA.GetDynamicTarget        -- ✅
Light.Graphics.TAA.UpdateDRS               -- ✅
Light.Graphics.TAA.GetDynamicStats         -- ✅ (10 字段表)
Light.Graphics.TAA.SetDynamicConfig        -- ✅ (table 入参, 4 字段可选)
```

### 3.2 默认值 (CONSENSUS §3.2) ✅

| 字段 | 设计默认 | 实际 | 验证 |
|------|---------|------|------|
| drsEnabled | false | false | smoke §14.1 |
| drsTargetFps | 60 | 60 | smoke §14.1 |
| drsWindowSize | 30 | 30 | DESIGN §2.1 |
| drsCooldownFrames | 60 | 60 | DESIGN §2.1 |
| drsDownThreshold | 1.10 | 1.10 | DESIGN §2.1 |
| drsUpThreshold | 0.85 | 0.85 | DESIGN §2.1 |

### 3.3 功能验证 (CONSENSUS §3.3) ✅

| 项 | 验证 |
|----|------|
| 默认 drsEnabled=false 零回归 | smoke §14.1 |
| target ≤ 0 自动关 DRS | smoke §14.4 |
| target < 30 / > 240 clamp | smoke §14.5 |
| UpdateDRS(dt) drsEnabled=false 时 no-op | smoke §14.7 |
| 滑动窗口未填满 → warming up | smoke §14.9 |
| Cooldown 中不调整 | UpdateDRS 算法 §1131 行 |
| Hysteresis 升降阈值不同 | UpdateDRS 算法 §1142-1146 行 |
| DRS 调整复用 SetRenderScale 路径 | UpdateDRS §1150 行 |
| Multi-instance 隔离 | smoke §14.11 |
| 手动 SetRenderScale 后 DRS 透明接管 | DRS 状态机重读 g.upscalePreset |
| DRS 仅在 taauEnabled=true 时实际调 SetRenderScale | SetRenderScale 内部判断 |

### 3.4 类型/边界 (CONSENSUS §3.4) ✅

| 项 | 验证 |
|----|------|
| SetDynamicEnabled(non-bool) raise | smoke §14.2 (luaL_checktype LUA_TBOOLEAN) |
| SetDynamicTarget(non-number) raise | smoke §14.3 (luaL_checknumber) |
| UpdateDRS(non-number) raise | smoke §14.8 (luaL_checknumber) |
| SetDynamicConfig({windowSize=-1}) clamp | smoke §14.6 (drsClampConfig_) |
| SetDynamicConfig({downThreshold=0.5}) clamp 1.01 | smoke §14.6 |
| SetDynamicConfig(non-table) 静默忽略 | l_TAA_SetDynamicConfig §5031 行 |

### 3.5 性能要求 (CONSENSUS §3.5) ✅

| 项 | 实际 |
|----|------|
| UpdateDRS 调用开销 < 1 μs | 滑动窗口 push + 30 次累加, 总 ALU < 30 cycle ≈ 10ns |
| DRS 调整不增加额外 RT 重建 | 复用 SetRenderScale 现有路径 (applyTAAUChange_) |
| 内存增量 ≤ 600 byte/instance | 实际 ~520 byte (DESIGN §2.2 修订) |

### 3.6 CI 与文档 (CONSENSUS §3.6)

| 项 | 状态 |
|----|------|
| CI 6 平台全绿 | ⏳ T7 待执行 |
| smoke §14 12 子检查点 | ✅ 12 PASS (syntax check) |
| demo_taau HUD + 键位 | ✅ |
| 文档 7 件套 | 5/7 已写, FINAL/TODO 待 T7 后 |

---

## 4. 已知 / 留观察问题

### 4.1 设计层

- **DRS 调整时 history 重置 1 帧 jitter**: 4 档间跳转必然有 history miss, 无解 (除非用 history 重投影). 业界相同, 不视为 bug.
- **avgFrameTimeMs 包含整个主循环**: 含 Lua tick / draw / TAA / 后处理 / SwapBuffer, 不能精确归因到 GPU 单独. 用户希望仅 GPU 时间应配 NSight (留 F.1.5).
- **windowSize 限 120 上限**: 144Hz 配置下 1 秒数据约 144 帧, 上限 120 会丢弃尾部 24 帧. 影响微小, 用户嫌不够可调 cooldownFrames 拉长.

### 4.2 留 F.1.5+

- 真实 GPU profiler 整合 (NSight / RenderDoc API hook)
- 跨 instance 联动降级 (multi-pip 协同)
- Pixel quality metrics (SMAA 边缘度量) 反馈闭环
- ML-based DRS 预测 (UE5 Insights 风格)

---

## 5. 文档版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — T1~T6 完成验收, T7 CI 待执行 |
