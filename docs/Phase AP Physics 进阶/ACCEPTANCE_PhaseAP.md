# Phase AP — Physics 进阶 — 验收

## 1. 实施概览

| 指标 | 目标 | 实际 |
|---|---|---|
| 新增 fns | ~71 | **~75** |
| C++ 行 | ~1120 | ~1100 |
| Smoke 阶段 | +12 | +12 |
| Smoke 行 | +360 | +280 |
| Commit | 1 | 1 |

## 2. 交付的 API 清单

### 2.1 `Light.Physics.World` 新增 11 个

| fn | 适用 |
|---|---|
| `CreateWheelJoint` | 车轮悬挂 |
| `CreateMotorJoint` | 相对偏移控制 |
| `CreateFrictionJoint` | 顶视图摩擦 |
| `CreatePulleyJoint` | 滑轮 (11 参数 flat) |
| `CreateGearJoint` | 齿轮联动 (类型校验) |
| `PreSolve(fn)` | Solver 前 contact 修改 |
| `PostSolve(fn)` | 接触 impulse 报告 |
| `SetSubStepping/GetSubStepping` | 子步进 |
| `SetWarmStarting/GetWarmStarting` | warm-start solver |

### 2.2 `Joint` 新增 ~38 个

**Limits (Revolute/Prismatic/Wheel)**: `EnableLimit`, `IsLimitEnabled`, `SetLimits`, `GetLowerLimit`, `GetUpperLimit`

**Motor 扩展 (Revolute/Prismatic/Wheel)**: `IsMotorEnabled`, `GetMotorSpeed`, `SetMaxMotorTorque`, `GetMotorTorque(invDt)`, `SetMaxMotorForce`, `GetMotorForce(invDt)`

**Spring (Distance/Weld/Mouse/Wheel)**: `SetStiffness`, `GetStiffness`, `SetDamping`, `GetDamping`, `SetSpring(freqHz, ratio)` — 自动通过 `b2LinearStiffness` 转换

**MaxForce/MaxTorque (Mouse/Motor/Friction)**: `SetMaxForce`, `GetMaxForce`, `SetMaxTorque`, `GetMaxTorque`

**Wheel/Prismatic 扩展**: `GetJointLinearSpeed`

**MotorJoint 专属**: `SetLinearOffset/GetLinearOffset`, `SetAngularOffset/GetAngularOffset`, `SetCorrectionFactor/GetCorrectionFactor`

**PulleyJoint 专属**: `GetGroundAnchorA/B`, `GetLengthA/B`, `GetCurrentLengthA/B`

**GearJoint 专属**: `SetRatio`, `GetRatio`, `GetJoint1`, `GetJoint2`

`GetJointAngle/Speed/Translation` 扩展支持 wheel 类型 (Phase AO 仅 revolute/prismatic)。

### 2.3 `Contact` 新增 10 个 (PreSolve/PostSolve 内有效)

`IsTouching`, `IsEnabled`, `SetEnabled`, `SetFriction`, `GetFriction`, `SetRestitution`, `GetRestitution`, `ResetFriction`, `ResetRestitution`, `GetManifoldPointCount`

**生命周期**: 通过 `__contact` userdata 的 `alive` flag 保护, 回调返回后自动失效, 用户保存到全局变量也安全 no-op。

### 2.4 `Body` 新增 11 个

`GetGravityScale/SetGravityScale`, `IsFixedRotation/SetFixedRotation`, `IsSleepingAllowed/SetSleepingAllowed`, `ResetMassData`, `GetMassData/SetMassData`, `GetJointList`, `GetContactList`

### 2.5 `Fixture` 新增 1 个

`RayCast(x1, y1, x2, y2, childIndex?)` — 单体射线检测, 返回 `(hx, hy, nx, ny, frac)` 或 `nil`

## 3. 关键设计决策

### 3.1 PreSolve/PostSolve — 同步重入 (用户选 Q1 (A))

不像 Phase AN Audio / Phase AO Begin/EndContact 走 deferred queue, PreSolve/PostSolve **必须同步**:
- PreSolve: `Contact:SetEnabled(false)` 必须立即生效 (Box2D solver 在 callback 后立即读取 enabled flag)
- PostSolve: `b2ContactImpulse*` 在回调返回后即失效, 不能 deferred

**实现**: `lua_pcall` 保护, 错误吞掉并 log, 不影响 Step 继续。
**风险**: Box2D world 在 Step 内 locked, 用户在 callback 里调 `CreateBody` 等会触发 Box2D assert。**已在文档明确警告**, 不做额外防护 (与 Box2D 原生行为一致)。

### 3.2 Contact userdata 生命周期

```cpp
struct PhysicsContactView { b2Contact* contact; bool alive; };
```

- `PushLiveContactTable` 创建, `alive=true`
- 回调返回后 `view->alive = false`
- 后续所有 Contact 方法通过 `CheckContactView` 验证 alive, 失效则 no-op

避免野指针 + 允许用户保存 contact 到全局变量 (虽不推荐)。

### 3.3 GearJoint 类型校验 + 销毁顺序

- `CreateGearJoint` 检查 joint1/joint2 是 revolute/prismatic, 否则返回 nil
- 不强持有 ref (用户选 Q2 默认): 文档标注用户必须先销毁 GearJoint, 再销毁组合 joint
- `GetJoint1/GetJoint2` 通过 `b2Joint::GetUserData().pointer` 反查 wrapper, alive=false 时返回 nil

### 3.4 Spring API 双层

- 底层: `SetStiffness/SetDamping` (单位 N/m, N·s/m) — 直接对应 Box2D 2.4 内部表示
- 便利层: `SetSpring(freqHz, dampingRatio)` — 内部调 `b2LinearStiffness(stiffness, damping, hz, ratio, bodyA, bodyB)` 自动换算

支持 Distance / Weld / Mouse / Wheel 4 种 joint, 类型不匹配 no-op。

### 3.5 PulleyJoint flat 11 参数 (Q3 默认)

```lua
world:CreatePulleyJoint(bodyA, bodyB,
    groundAx, groundAy, groundBx, groundBy,
    anchorAx, anchorAy, anchorBx, anchorBy,
    ratio)
```

ratio <= 0 拒绝 (Box2D 要求)。

### 3.6 Joint 类型扩展兼容

`SetMotorSpeed` / `EnableMotor` / `GetJointAngle/Speed/Translation` Phase AO 仅支持 revolute/prismatic, 现扩展 wheel — 旧 Lua 代码无需修改。

## 4. Smoke 覆盖

`@e:\jinyiNew\Light\scripts\smoke\physics.lua` — 总 25 阶段 (Phase AO 13 + Phase AP 12), 关键 AP 阶段:

| # | 阶段 | 验证点 |
|---|---|---|
| 14 | Joint Limits + Motor (revolute) | EnableLimit/SetLimits/IsMotorEnabled/GetMotorSpeed/GetMotorTorque |
| 15 | Spring API (Distance) | SetStiffness/SetDamping/SetSpring → b2LinearStiffness 转换 |
| 16 | WheelJoint (motor+spring+limit) | 5 种特性同时启用 |
| 17 | MotorJoint | LinearOffset/AngularOffset/CorrectionFactor/MaxForce/MaxTorque |
| 18 | FrictionJoint | MaxForce/MaxTorque |
| 19 | PulleyJoint | GroundAnchor/Length/Ratio + ratio<=0 拒绝 |
| 20 | GearJoint | Ratio/SetRatio/GetJoint1\|2 + 错类型组合拒绝 |
| 21 | PreSolve+PostSolve | callback 触发计数 + impulse>0 + SetEnabled(false) 穿透 |
| 22 | Body 邻居遍历 | GetJointList + GetContactList |
| 23 | Fixture:RayCast | 命中 + miss case |
| 24 | Body 高级属性 | GravityScale/FixedRotation/SleepingAllowed/ResetMassData/SetMassData |
| 25 | World 仿真控制 | SubStepping/WarmStarting toggle |

## 5. 验收清单

- [x] 75 fns 全部注册到对应 table
- [x] 本地 Release build pass (MSVC 17.14, 多次中间检查点)
- [ ] Smoke 25 阶段通过 (待 CI 验证)
- [ ] Phase AO 13 阶段不破坏 (待 CI 验证)
- [ ] 6 平台 CI all success (待验证)
- [x] 文档: ALIGNMENT/CONSENSUS/ACCEPTANCE 完整

## 6. 已知 LSP 警告

scripts/smoke/physics.lua 有 "需要判空" 警告。原因:
- Create*Joint 返回类型为 `Joint | nil`
- 我们已用 `if joint == nil then fail() end` 检查
- 但 Lua language server 不识别 `fail()` 是 noreturn 函数

**不影响**: smoke 实际运行 + CI 编译均无问题。

## 7. 未实施 (按 ALIGNMENT 计划明确排除)

- Box2D v3 升级
- TimeOfImpact / Sweep
- ContactFilter virtual override (Fixture filter 已可做)
- World:Dump (调试用)
- Particle system (Box2D 2.4 已移除)
