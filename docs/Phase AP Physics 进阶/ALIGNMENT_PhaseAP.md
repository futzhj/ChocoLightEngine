# Phase AP — Physics 进阶 — Alignment

## 0. 目标

在 Phase AO 的基础上补 Box2D v2.4 **剩余实用 API**, 让引擎的 Physics 层从"能用"升级为"专业级游戏可用":

- **Joint Limits / Motor 扩展** — 让现有 Revolute/Prismatic 能做门、活塞、机械臂
- **5 种新 Joint 类型** — Gear/Wheel/Motor/Friction/Pulley
- **PreSolve/PostSolve 回调** — 破坏检测、无响应碰撞、音效触发
- **单体 RayCast + 邻居遍历** — Body.GetContactList/GetJointList + Fixture.RayCast
- **Body 高级属性** — GravityScale/FixedRotation/SleepingAllowed/ResetMassData
- **World 仿真控制** — SubStepping/WarmStarting/Dump

## 1. 现状 (Phase AO 基线)

| 类别 | 已有 |
|---|---|
| Joint 类型 | Distance/Revolute/Prismatic/Weld/Mouse |
| Joint 方法 | IsAlive/GetType/GetBodyA\|B/GetAnchorA\|B/GetReactionForce\|Torque/IsEnabled/Destroy + 类型特定 GetJointAngle/Speed/Translation/SetMotorSpeed/EnableMotor/SetTarget/SetLength/GetLength |
| World Query | RayCast/QueryAABB/DestroyJoint/GetJointCount |
| Body 坐标 | GetWorldPoint/LocalPoint/WorldVector/LocalVector/ApplyForceAtWorldPoint/ApplyLinearImpulseAtPoint/GetFixtures |
| Fixture | TestPoint/GetAABB/GetShapeType |
| Contact callback | BeginContact/EndContact (deferred, 安全) |
| 生命周期 | DestructionListener 自动级联 body→joint 失效 |

## 2. Phase AP 新增范围

### 2.1 P0 — Joint Limits / Motor 细化 (~12 fns)

**适用**: RevoluteJoint + PrismaticJoint

| 方法 | 签名 | 用途 |
|---|---|---|
| `Joint:EnableLimit(b)` | bool → — | 启用/禁用关节活动范围 |
| `Joint:IsLimitEnabled()` | — → bool | 查询 |
| `Joint:SetLimits(lo, hi)` | number, number → — | revolute 是弧度; prismatic 是像素 |
| `Joint:GetLowerLimit()` | — → number | 查询下限 |
| `Joint:GetUpperLimit()` | — → number | 查询上限 |
| `Joint:SetMaxMotorTorque(t)` | number → — | revolute 最大扭矩 |
| `Joint:GetMotorTorque(invDt)` | number → number | 读当前扭矩 (单位换算过) |
| `Joint:SetMaxMotorForce(f)` | number → — | prismatic 最大力 |
| `Joint:GetMotorForce(invDt)` | number → number | 读当前力 |
| `Joint:GetMotorSpeed()` | — → number | 读 revolute/prismatic 当前设定速度 |

**DistanceJoint 额外** (Phase AO 遗漏):
- `Joint:SetFrequency(hz)` / `GetFrequency()` / `SetDampingRatio(r)` / `GetDampingRatio()` — 4 fns, 用于弹簧效果

### 2.2 P0 — 5 种新 Joint 类型 (~25 fns)

| Joint | Create 签名 | 专属方法 |
|---|---|---|
| **WheelJoint** | `CreateWheelJoint(bA, bB, ax, ay, axisX, axisY)` | EnableMotor/SetMotorSpeed/SetMaxMotorTorque + EnableLimit/SetLimits(lo,hi) + GetJointTranslation/Speed + SetSpringFrequencyHz/GetSpringFrequencyHz + SetSpringDampingRatio/GetSpringDampingRatio |
| **MotorJoint** | `CreateMotorJoint(bA, bB)` | SetLinearOffset(x,y)/GetLinearOffset / SetAngularOffset(rad)/GetAngularOffset / SetMaxForce/GetMaxForce / SetMaxTorque/GetMaxTorque / SetCorrectionFactor/GetCorrectionFactor |
| **FrictionJoint** | `CreateFrictionJoint(bA, bB, ax, ay)` | SetMaxForce/GetMaxForce / SetMaxTorque/GetMaxTorque |
| **PulleyJoint** | `CreatePulleyJoint(bA, bB, groundAx, groundAy, groundBx, groundBy, ax, ay, bx, by, ratio)` | GetGroundAnchorA/B / GetLengthA/B / GetRatio / GetCurrentLengthA/B |
| **GearJoint** | `CreateGearJoint(joint1, joint2, ratio)` — 2 个 revolute/prismatic 组合 | SetRatio/GetRatio / GetJoint1/GetJoint2 |

> **GearJoint 难点**: Box2D 要求组合 joint 的 body 之间存在 **共同地面**, 且两个 joint 必须都是 revolute 或 prismatic。绑定层需要做类型检查。
>
> **PulleyJoint 难点**: 参数多达 11 个, 考虑参数化为 lua table 版本。第一版先走 flat args, smoke 再包一层 helper。

### 2.3 P1 — PreSolve / PostSolve Contact (~6 fns)

Box2D 的 b2ContactListener 还有两个虚方法:

```cpp
virtual void PreSolve(b2Contact* contact, const b2Manifold* oldManifold);
virtual void PostSolve(b2Contact* contact, const b2ContactImpulse* impulse);
```

**PreSolve** 在解算前同步调用, 可通过 `Contact:SetEnabled(false)` 单帧禁用该 contact (常用于单向平台、条件碰撞)。**必须同步**, 不能走 deferred queue。

**PostSolve** 在 solver 后同步调用, 报告 impulse.normalImpulses/tangentImpulses — 常用于音效(撞击音量 ∝ impulse)、破坏检测 (impulse 超阈值触发断裂)。

**架构决策** (重要, 可能需要用户确认):
- Phase AN Audio callback 和 Phase AO Contact BeginContact/EndContact 都是 deferred 队列到主线程,避免 Lua 重入 C++ 的风险。
- **PreSolve 不能 deferred** — 它是 Box2D 解算过程的一部分, 修改 contact.enabled 必须实时生效。
- **方案**: 在 `b2World::Step()` 内部允许 Lua reentrant, 用 pcall 保护。若用户 callback 抛错, 吞掉并 log,不影响 Step 继续。这是 Box2D 本身支持的模型 (C++ 回调本就在 Step 内)。
- **PostSolve 可 deferred** — 报告性, 不影响 Step 后续步骤。但为了 API 一致, 一起设计为同步 pcall。

**新增 API**:
- `World:PreSolve(fn)` — `fn(contact, oldManifold)` — oldManifold 只暴露 points 数量, 完整 manifold 太复杂, 本期不暴露结构
- `World:PostSolve(fn)` — `fn(contact, normalImpulses[], tangentImpulses[])` — impulses 为 array of number (最多 2 个点)
- `Contact:SetEnabled(b)` / `Contact:IsEnabled()` — 仅 PreSolve 内有效
- `Contact:SetFriction(f)` / `Contact:GetFriction()` — 仅 PreSolve 内有效
- `Contact:SetRestitution(r)` / `Contact:GetRestitution()` — 仅 PreSolve 内有效
- `Contact:ResetFriction()` / `Contact:ResetRestitution()`
- `Contact:GetManifoldPointCount()` — 简化版, 返回接触点数 (0/1/2)

### 2.4 P1 — 邻居遍历 (~3 fns)

- `Body:GetContactList()` → array of contact tables (**touching=true** 才 include, 避免冗余)
- `Body:GetJointList()` → array of joint tables
- `Fixture:RayCast(x1, y1, x2, y2, childIndex?)` → (hit_x, hit_y, normal_x, normal_y, fraction) 或 nil (未命中)

### 2.5 P2 — Body 高级属性 (~8 fns)

- `Body:GetGravityScale() / SetGravityScale(s)` — 单位无量纲, 常用于"太空感"
- `Body:IsFixedRotation() / SetFixedRotation(b)` — 禁止旋转 (角色刚体常用)
- `Body:IsSleepingAllowed() / SetSleepingAllowed(b)` — 是否允许自动休眠
- `Body:ResetMassData()` — 重新计算 mass, 改 fixture 后常用
- `Body:SetMassData(mass, cx, cy, I)` / `GetMassData()` — 手动设定质量属性

### 2.6 P2 — World 仿真控制 (~4 fns)

- `World:SetSubStepping(b) / GetSubStepping()` — continuous physics 子步开关
- `World:SetWarmStarting(b) / GetWarmStarting()` — solver warm-starting, 性能调优开关

**不做** (保守):
- `Dump()` — 调试用, C++ fprintf 实现, 绑定到 Lua 价值低
- `SetAutoClearForces` — 名字误导且很少用

## 3. 工作量估算

| 模块 | 新 fns | C++ 行 | smoke 行 |
|---|---|---|---|
| Joint Limits + Motor 扩展 | 12 | ~180 | 60 |
| WheelJoint | 9 | ~120 | 40 |
| MotorJoint | 9 | ~120 | 40 |
| FrictionJoint | 4 | ~60 | 20 |
| PulleyJoint | 6 | ~100 | 30 |
| GearJoint | 4 | ~80 | 25 |
| DistanceJoint 弹簧参数 | 4 | ~60 | 15 |
| PreSolve/PostSolve + Contact ops | 8 | ~180 | 60 |
| Body 邻居遍历 + Fixture RayCast | 3 | ~80 | 25 |
| Body 高级属性 | 8 | ~100 | 30 |
| World 仿真控制 | 4 | ~40 | 15 |
| **合计** | **~71 fns** | **~1120 行** | **~360 行** |

预计 **5-7 小时** (含本地编译 + 6 平台 CI 验证)。

## 4. 边界与约束

### 4.1 严格范围内

- 所有新 API 遵循现有单位约定: 像素/弧度/无量纲
- Joint 的 `Destroy/GC` 路径由 Phase AO 的 DestructionListener 统一处理, 新 Joint 类型无需额外 lifecycle 代码
- PreSolve/PostSolve 沿用 ContactListener, 不新增 listener 类
- 性能: 新 API 不触及 Step 热路径 (PreSolve/PostSolve 除外, 但它们本就在 Step 内)

### 4.2 刻意不做

- **Box2D v3 升级** — 涉及全量 API 切换, 另起 Phase
- **Liquid Particle** — Box2D 2.4 已移除
- **编辑器工具 / 约束可视化** — 工具层, 非引擎层
- **SetContactFilter (category/mask/group)** — Fixture 侧已可做, 独立 ContactFilter virtual 过度工程
- **TimeOfImpact / Sweep** — 高级特性, 应用场景少

### 4.3 API 命名决策

- **Joint 弹簧参数**: Box2D 用 `frequencyHz` / `dampingRatio`, 直译为 `SetSpringFrequencyHz` / `SetSpringDampingRatio`。DistanceJoint 对应 `SetFrequency/SetDampingRatio` (不加 Spring 前缀, 因为 DistanceJoint 本身就是弹簧)。
- **Limits**: revolute 用弧度, prismatic 用像素 (自动 ToMeters 换算)。
- **Motor Torque/Force**: Box2D 一些 getter 需要 invDt, 我们包装时统一接收 invDt 参数 (默认 60.0)。

## 5. 疑问与关键决策点

### 5.1 **Q1: PreSolve 重入安全性** — 需用户确认

Box2D 的 PreSolve 回调在 Step 的 solver iteration 中被调用。我们的 Lua callback 通过 `lua_pcall` 进入, 若用户在 callback 里调用 `World:Step()` 或 `World:CreateBody()` 会触发 **Box2D 的 assert**:

```
Assertion failed: !m_world->IsLocked() — can't modify world during Step
```

**决策候选**:
- **(A)** 不额外保护 — 用户需自行避免重入, 文档明确标注
- **(B)** 在 PreSolve/PostSolve 内记录 `world_locked` flag, Lua 侧 CreateBody 等拒绝并返回 nil
- **(C)** 彻底 deferred — PreSolve 变成"不可修改 contact"的纯观察接口, 失去 Box2D 的关键语义

**倾向**: **(A)** — Box2D 原生行为一致, 文档注明即可。Phase AN Audio 也是 deferred 选择 (A 的对立), 那是因为 audio thread 和 main thread 不同步; 这里 PreSolve 就在 Step 调用链内, 同线程, 语义明确。

### 5.2 **Q2: GearJoint 的 joint 生命周期耦合** — 基于 Box2D 文档决策

Box2D 要求: 组合用的 2 个 joint 必须在 GearJoint 销毁前保持存活。若用户先销毁组合 joint, **GearJoint 行为未定义**。

**决策**: GearJoint wrapper 持有 2 个 joint 的 selfRef, 但不强制, 文档标注"销毁顺序"即可。

### 5.3 **Q3: Contact 对象的生命周期** — 基于现有 ContactListener

现有 Contact table 由 BeginContact/EndContact 时创建, 但 PreSolve/PostSolve 要求 Contact 对象具备可调用方法 (SetEnabled 等)。

**决策**: 沿用现有 PushContactTable, 附加 `__contact` userdata (指向 b2Contact\*), 所有 Contact 方法从 userdata 取 b2Contact\*。**Contact 对象仅在 callback 内有效**, 用户保存到 Lua 变量再后续调用将访问野指针 — 文档必须警告。通过 `alive` flag 保护, callback 结束自动 invalidate。

## 6. 验收标准

### 6.1 功能

- 71 fns 全部注册到对应 table
- 新增 5 种 joint 能创建/销毁, Joint.GetType 返回正确字符串
- Joint Limits: 超出限制的 force 被正确夹断
- PreSolve: Contact:SetEnabled(false) 能单帧禁用碰撞 (球穿过平台)
- PostSolve: normalImpulses 值合理 (>0 on hit)
- GearJoint: 一个 wheel 旋转, 另一个 wheel 联动 (ratio 1:1 同步; 1:2 二倍速)
- Pulley: 一端下降, 另一端按 ratio 上升

### 6.2 回归

- Phase AO smoke 13 阶段仍全绿
- Phase AO API 行为不变
- 现有 Light.Physics smoke 不破坏
- CI 6 平台 all success

### 6.3 性能

- Step 热路径不新增分支开销 (PreSolve/PostSolve 在 callback 为 nil 时跳过)
- 无新的动态分配 (listener 沿用)

## 7. 中断询问

以下 **P0 中的关键决策** 需要用户确认:

**Q1**: PreSolve 重入安全策略 — 倾向 (A) 文档标注, 不做额外防护。你同意吗?

**Q2**: GearJoint 销毁顺序 — 倾向文档标注 (销毁组合 joint 前先销毁 GearJoint)。是否需要额外加强?(如 GearJoint wrapper 强持有组合 joint 的 ref, 防止误销毁)

**Q3**: PulleyJoint 创建签名 — 11 参数 flat args 还是 lua table?
  - flat: `CreatePulleyJoint(bA, bB, gAx, gAy, gBx, gBy, ax, ay, bx, by, ratio)`
  - table: `CreatePulleyJoint({bodyA=bA, bodyB=bB, groundAnchorA={x,y}, ...})`
  - **倾向**: flat, 和其他 CreateXxxJoint 一致

**Q4**: 范围合理性 — 71 fns 一次交付是否太大? 可以拆:
  - **AP.1**: Joint Limits + Motor + DistanceJoint 弹簧 (16 fns) — 最高频刚需
  - **AP.2**: 5 新 Joint 类型 (32 fns)
  - **AP.3**: PreSolve/PostSolve + Contact 扩展 (8 fns) — 高级, 可延后
  - **AP.4**: Body 高级 + World 控制 + 邻居 (15 fns) — 杂项补完
  - 或者**一次交付** (和 Phase AO 47 fns 类似量级, 复杂度可控)

## 8. 项目特性对齐

- 单位: 像素 / 弧度 / 秒 — 与 Phase AO / 现有绑定一致
- Handle 模式: wrapper struct + `__joint` field + selfRef — 与 Phase AO 一致
- GC: DestructionListener 统一处理 — 无需新机制
- CI: 6 平台 build-templates.yml — 同入口验证
- 文档: 每 fn 加 `/// @lua_api Light.Physics.Xxx` 注释, 与 Phase AO 风格一致
