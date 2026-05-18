# Phase F.1.4 Dynamic Resolution Scaling — CONSENSUS 文档

> **阶段**: 6A Workflow — 阶段 1 Align (锁共识)
> **基线**: ALIGNMENT_PhaseF_1_4.md (5 决策点全 A 默认)
> **共识日期**: 2026-05-19
> **状态**: ✅ 用户确认全部默认决策, 进入阶段 2 Architect

---

## 1. 锁定的决策结果

| Q | 主题 | 决策 |
|---|------|------|
| Q1 | DRS 触发时机 | **用户每帧调 `TAA.UpdateDRS(dt)`** (explicit, 与 SpriteAnim 一致) |
| Q2 | DRS 算法 | **限频 + 离散 4 档预设跳转** (复用 Performance/Balanced/Quality/Native) |
| Q3 | 作用范围 | **per-instance state** (与 F.1.0.1 multi-instance 对齐) |
| Q4 | 阈值参数 | target=60 / window=30 / cooldown=60 / down=1.10× / up=0.85× (全部可调) |
| Q5 | 手动 vs 自动 | **手动调用立即生效**, DRS 状态机透明接管 (无 abort) |

---

## 2. 最终需求描述

`Light.Graphics.TAA` 子表新增 6 个公开 API + 1 个配置 setter, 实现帧时间监控 → 4 档预设自动切换的闭环系统:

```
+-----------------------------+
| User: TAA.UpdateDRS(dt)     |  每帧调用
+-----------------------------+
              |
              v
+-----------------------------+
| Sliding Window (30 frames)  |  累积帧时间
+-----------------------------+
              |
              v
+-----------------------------+
| Cooldown >= 60 frames?      |  限频, 防 ping-pong
+-----------------------------+
       N ────┐       Y────────┐
              v                v
       (no-op)    +--------------------+
                  | avg > target × 1.10 |  超预算 → 降一档
                  | avg < target × 0.85 |  富余 → 升一档
                  +--------------------+
                            |
                            v
                  SetRenderScale(presetIdx ± 1)
                            |
                            v
                  applyTAAUChange_() (HDR 重建)
```

---

## 3. 验收标准 (锁定)

### 3.1 API 完整性

```lua
-- 新增 6 个 API:
Light.Graphics.TAA.SetDynamicEnabled(boolean)       -- 总开关
Light.Graphics.TAA.GetDynamicEnabled() : boolean
Light.Graphics.TAA.SetDynamicTarget(fps)            -- 目标 FPS, 0 = 关 DRS
Light.Graphics.TAA.GetDynamicTarget() : number
Light.Graphics.TAA.UpdateDRS(deltaTimeSec)          -- 用户每帧调
Light.Graphics.TAA.GetDynamicStats() : table        -- {avgFrameTimeMs, avgFps, currentScale, adjustments, framesSinceLastAdjust, warmingUp}

-- 1 个配置 setter (高级):
Light.Graphics.TAA.SetDynamicConfig(table)          -- {windowSize, cooldownFrames, downThreshold, upThreshold}
```

### 3.2 默认值

| 字段 | 默认 |
|------|------|
| dynamicEnabled | `false` (零回归) |
| dynamicTargetFps | `60` |
| dynamicWindowSize | `30` 帧 |
| dynamicCooldownFrames | `60` 帧 |
| dynamicDownThreshold | `1.10` (frameTime > target × 1.10 → 降) |
| dynamicUpThreshold | `0.85` (frameTime < target × 0.85 → 升) |

### 3.3 功能验证

- [ ] 默认 dynamicEnabled=false, 不影响 F.1 行为 (零回归)
- [ ] target ≤ 0 → 自动关 DRS (`dynamicEnabled = false`)
- [ ] target < 30 / target > 240 → clamp + warning
- [ ] UpdateDRS(dt) 在 `dynamicEnabled=false` 时立即返回 (no-op)
- [ ] 滑动窗口未填满 → warming up phase, 不调整
- [ ] Cooldown 中 → 不调整 (即使超阈值)
- [ ] Hysteresis 正确 (升降阈值不同)
- [ ] DRS 调整时 SetRenderScale 内部路径与手动调用路径一致
- [ ] Multi-instance 隔离: instance 0 启 DRS, instance 1 (DRS 关) renderScale 不变
- [ ] 手动 `SetRenderScale(0.6)` 后 DRS 接管, 状态机重新读 `g.renderScale` 作为起点
- [ ] DRS 仅在 `taauEnabled=true` 时实际触发 SetRenderScale (taauEnabled=false 时仅累积统计)

### 3.4 类型/边界

- [ ] SetDynamicEnabled(non-bool) → `luaL_checktype` raise
- [ ] SetDynamicTarget(non-number) → `luaL_checknumber` raise
- [ ] UpdateDRS(non-number) → `luaL_checknumber` raise
- [ ] SetDynamicConfig({windowSize=-1}) → 拒绝 + warning
- [ ] SetDynamicConfig({downThreshold=0.5}) → 必须 ≥ 1.0, 否则拒绝
- [ ] SetDynamicConfig({upThreshold=2.0}) → 必须 ≤ 1.0, 否则拒绝

### 3.5 性能要求

- [ ] UpdateDRS 调用开销 < 1 μs (滑动窗口循环 30 次)
- [ ] DRS 调整不增加额外 RT 重建 (复用 SetRenderScale 现有路径)
- [ ] 内存增量 < 200 字节 / instance (5 个 float + 4 int)

### 3.6 CI 与文档

- [ ] CI 6 平台全绿
- [ ] `scripts/smoke/taa.lua` 加 §15 (DRS, 12+ 子检查点)
- [ ] `samples/demo_taau/main.lua` HUD 显示 DRS 状态
- [ ] 文档 7 件套: ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO

---

## 4. 实施约束

### 4.1 技术约束

- **不改 backend interface**: SetRenderScale 已是公开 API, DRS 仅是其上层调用者
- **不改 shader**: 0 shader 改动, 与 F.1 共用同一份 FS_TAA / FS_TAA_SHARP
- **不破坏 ABI**: 仅在 `taa_renderer.cpp` 内部 `State` struct 加 5 字段; 外部头文件 `taa_renderer.h` 仅加新 API 声明
- **per-instance 隔离**: 复用 `g_states[MAX_INSTANCES=4]` 数组, 5 字段全部 instance-local

### 4.2 接口约束

- 沿用 `g_active` macro 模式: `#define g g_states[g_active]` 让新 API 自动操作 active instance
- DRS state 5 字段必须初始化 (CloneInstance / CreateInstance 时复位)
- `applyTAAUChange_()` 不变, 仅作为 SetRenderScale 副效应

### 4.3 集成约束

- DRS 仅在 `taauEnabled` 时影响 renderScale; `taauEnabled=false` 时 UpdateDRS 仍累积统计 (供 GetDynamicStats), 但不触发 SetRenderScale
- 与 F.1.1 autoMipBias 自动联动: SetRenderScale → applyTAAUChange_ → updateMipBias_ 链已经成立

---

## 5. 验收门控 (8 项, 全部 PASS 才算 F.1.4 完成)

1. ✅ Lua API 6 + 1 全部存在 (smoke type=function)
2. ✅ 默认值与零回归验证
3. ✅ 类型/边界错误处理
4. ✅ 滑动窗口 + Cooldown + Hysteresis 算法正确
5. ✅ Multi-instance 隔离
6. ✅ 与 manual SetRenderScale 融合无抖动
7. ✅ demo_taau 集成 + HUD
8. ✅ CI 6 平台全绿

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 共识锁定 — Q1~Q5 全 A 默认, 进入 Architect |
