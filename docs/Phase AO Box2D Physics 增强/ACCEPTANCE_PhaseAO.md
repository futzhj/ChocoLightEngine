# Phase AO — Box2D Physics 增强 — 验收

## 1. 实施概览

| 指标 | 目标 | 实际 |
|---|---|---|
| 新增 C++ 函数 | ~35 | **47** (Joint 18 + World 9 + Body 7 + Fixture 3 + helper 10) |
| 新增行数 | ~960 | ~830 (实际更紧凑, helper 复用多) |
| smoke 行数 | ~300 | ~290 |
| 本地 Release 编译 | pass | pass (MSVC 17.14) |
| smoke 全量 | pass | pass (13 阶段全绿) |

## 2. 交付的 API 清单

### 2.1 `Light.Physics.World` 新增 9 个

| fn | 签名摘要 |
|---|---|
| `CreateDistanceJoint` | (bodyA, bodyB, ax, ay, bx, by, collideConnected?) → Joint |
| `CreateRevoluteJoint` | (bodyA, bodyB, ax, ay, collideConnected?) → Joint |
| `CreatePrismaticJoint` | (bodyA, bodyB, ax, ay, axisX, axisY, collideConnected?) → Joint |
| `CreateWeldJoint` | (bodyA, bodyB, ax, ay, collideConnected?) → Joint |
| `CreateMouseJoint` | (bodyA, bodyB, tx, ty, maxForce?) → Joint |
| `DestroyJoint` | (joint) — 幂等 |
| `GetJointCount` | () → number |
| `RayCast` | (x1, y1, x2, y2, callback) — callback(fixture,hx,hy,nx,ny,frac) |
| `QueryAABB` | (x, y, w, h, callback) — callback(fixture) |

### 2.2 `Joint` 方法 (18 个)

通用 10 个: `IsAlive`, `GetType`, `GetBodyA`, `GetBodyB`, `GetAnchorA`, `GetAnchorB`, `GetReactionForce`, `GetReactionTorque`, `IsEnabled`, `Destroy`

类型专属 7 个 (类型不匹配安全 no-op): `GetJointAngle`, `GetJointSpeed`, `GetJointTranslation`, `SetMotorSpeed`, `EnableMotor`, `SetTarget`, `SetLength`, `GetLength`

元方法 1 个: `__tostring`

### 2.3 `Body` 新增 7 个

`GetWorldPoint`, `GetLocalPoint`, `GetWorldVector`, `GetLocalVector`, `ApplyForceAtWorldPoint`, `ApplyLinearImpulseAtPoint`, `GetFixtures`

### 2.4 `Fixture` 新增 3 个

`TestPoint`, `GetAABB`, `GetShapeType`

## 3. 核心设计决策

### 3.1 生命周期安全 — `PhysicsDestructionListener`

Box2D `DestroyBody` 会自动级联销毁 attached joint, 通过 `b2DestructionListener::SayGoodbye(b2Joint*)` 回调。绑定层实现:

```cpp
void PhysicsDestructionListener::SayGoodbye(b2Joint* joint) {
    PhysicsJoint* wrapper = JointFromB2(joint);
    if (!wrapper) return;
    InvalidateJoint(world_->L, wrapper, false);  // 不手动 DestroyJoint 避免 double-free
}
```

**效果**: `World:DestroyBody(ball)` 后, 原本挂在 ball 上的 joint 自动进入 `dead` 状态, `Joint:IsAlive()` 返回 false, 其他方法返回安全默认值, 无 use-after-free。smoke 第 8 阶段验证通过。

### 3.2 `Joint ↔ b2Joint` 反向链接

`PhysicsJoint` wrapper 通过 `b2Joint::GetUserData().pointer` 反向查找, 用于 `SayGoodbye` 定位 wrapper。同时给 `b2Body` 也补了 `GetUserData().pointer = wrapper`, 以支持 `Joint:GetBodyA()` 高效反解。

### 3.3 RayCast/QueryAABB 回调协议

RayCast Lua 回调返回值遵循 Box2D 官方语义:
- `nil` → 用当前 fraction (继续裁剪)
- `-1` → 忽略此 fixture
- `0` → 终止
- `1` → 继续但不裁剪
- 其他数字 → 作为新的 max fraction

QueryAABB: 返回 `true/nil` 继续, `false` 终止。smoke 第 11-12 阶段验证所有路径。

### 3.4 单位约定

与现有绑定一致, **所有对外接口均以像素为单位**, 内部通过 `PTM = 32.0f` 换算到米:
- 位置、距离 (`SetLength`) 使用 `ToMeters` / `PushPixels`
- 力 (`ApplyForceAtWorldPoint` 的 fx/fy) 不换算
- 角度、角速度使用弧度 (与 Box2D 一致)

## 4. Smoke 测试结果

`@e:\jinyiNew\Light\scripts\smoke\physics.lua` — 290 行, 13 阶段, 全部通过:

```
Light.Physics module ok (5 shape constructors)
Light.Physics.World legacy API ok
Light.Physics.World Phase AO API ok (9 fns)
World creation + gravity ok
Created 2 bodies + fixtures
Phase AO Fixture TestPoint/GetAABB/GetShapeType ok
Phase AO Body coordinate/force helpers ok
Phase AO 5 joint types created; common/type-specific ops ok
DestroyJoint + Joint:Destroy + double-destroy safe
DestructionListener: DestroyBody auto-invalidates joints ok
Dead joint methods are safe
Step x10 ok
RayCast ok (hit_count=1)
RayCast ignore path ok (hit_count=2)
Zero-length raycast handled
QueryAABB ok (found=1)
QueryAABB early-stop ok
Cross-world joint rejected
=== Phase AO physics smoke passed ===
```

**覆盖的边界条件**:
- double-destroy joint 安全 (`Joint:Destroy()` + `World:DestroyJoint()`)
- dead joint 方法全部安全 no-op
- DestroyBody 自动级联 joint 失效
- Zero-length raycast 安全
- cross-world joint 拒绝
- wrong-type getter (如 distance joint 调 `GetJointAngle`) 安全返回 0

## 5. 回归影响

### 5.1 修改点

`@e:\jinyiNew\Light\ChocoLight\src\light_physics.cpp`:
- `PhysicsWorld` 加 `joints` vector 和 `destructionListener` 字段
- `l_World_Call` 实例化 `PhysicsDestructionListener` + `SetDestructionListener`
- `l_World_GC` 先失效 joints 再失效 bodies, `delete destructionListener`
- `l_World_CreateBody` 设置 `body->GetUserData().pointer = wrapper`
- `InvalidateBody` 清除 `body->GetUserData().pointer`

### 5.2 向后兼容

- Light.Physics.World 所有原 12 fn 保持不变
- Body 所有原 33 fn 保持不变, 新增方法仅追加
- Fixture 所有原 13 fn 保持不变
- OnCollision/BeginContact/EndContact callback 行为不变
- Contact table 结构不变

### 5.3 ABI/单位

- `PTM = 32.0f` 未变
- 所有新方法的单位 (像素/弧度/无量纲) 与旧方法一致

## 6. 未实施 (按 ALIGNMENT 计划明确排除)

- P2: `Fixture.SetFilterData/GetFilterData` (实际已存在于旧绑定, ALIGNMENT 笔误)
- P2: GearJoint / MotorJoint / PulleyJoint / WheelJoint / FrictionJoint / RopeJoint
- P2: PreSolve / PostSolve callback
- P2: Joint limit set (EnableLimit/SetLimits) — 现有 EnableMotor/SetMotorSpeed 覆盖基本运动, limit 属进阶
- P2: Box2D v3 升级

如需进阶 joint 控制, 将在 Phase AP 追加。

## 7. 下一步

- [ ] `git commit` + `git push` 触发 GitHub Actions 多平台编译 (Windows/macOS/Linux)
- [ ] CI 6 平台全绿后关闭 Phase AO
- [ ] 更新 `@e:\jinyiNew\Light\docs\API_REFERENCE.md` 补 Joint + RayCast + QueryAABB 章节 (后续小 PR)
