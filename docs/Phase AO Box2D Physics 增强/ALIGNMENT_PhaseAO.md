# Phase AO — Box2D Physics 增强 — Alignment

## 0. 目标

将 `Light.Physics` / `Light.Physics.World` 从"基础 Body+Fixture"层升级为"完整物理游戏可写"层,补全 Joint、Raycast、AABB Query、Sensor、TestPoint。

## 1. 现状

**Box2D 版本**: v2.x C++ API (`b2World`, `b2Body`, `b2Fixture`, `b2Joint`, `b2ContactListener`),不是 Box2D v3 C API。

**已绑** (`@e:\jinyiNew\Light\ChocoLight\src\light_physics.cpp`,~50 fns):

| 模块 | 已绑 fns |
|---|---|
| `Light.Physics` | 5 个 NewXxxShape (Circle/Rectangle/Polygon/Edge/Chain) |
| `Light.Physics.World` | 12 fns (SetGravity/GetGravity/Step/CreateBody/DestroyBody/GetBodyCount/SetAllowSleeping/SetContinuousPhysics/ClearForces + OnCollision/BeginContact/EndContact deferred callback) |
| `Body` | 33 fns (Position/Angle/Velocity/ApplyForce/Impulse/Torque/Mass/Inertia/Type/Damping/Bullet/Awake/Active/CreateFixture/DestroyFixture/AddBox/AddCircle/SetRestitution/SetFriction/...) |
| `Fixture` | ~5 fns (GetBody/SetDensity/...) |
| `Contact` | 4 fns (GetBodyA/B, GetFixtureA/B) |

**已实现的 callback 桥**: `PhysicsContactListener` → `world->contactEvents` queue → `Step()` 后 `DispatchContactEvents` 在主线程批量回调 Lua,与 Phase AN Audio callback 同思路(deferred,主线程同步)。

## 2. 缺口分析(Phase AO 范围)

| 类别 | 缺失 fns | 价值 |
|---|---|---|
| **Joints** | 完全缺失,需新增 12+ fns | P0 必须 |
| **Raycast** | `World.RayCast` 缺失 | P0 必须 |
| **Query AABB** | `World.QueryAABB` 缺失 | P0 必须 |
| **Sensor** | `Fixture.SetSensor/IsSensor` 缺失 | P1 |
| **TestPoint** | `Fixture.TestPoint` 缺失 | P1 |
| **Body 坐标转换** | `GetWorldPoint/LocalPoint/Vector` 缺失 | P1 |
| **Filter data** | `SetFilterData/GetFilterData` 缺失 | P2(本期不做) |
| **PreSolve/PostSolve** | 缺失 | P2(本期不做) |
| **GearJoint/MotorJoint** | 缺失 | P2(本期不做) |

## 3. 绑定决策

### 3.1 P0 — Joints(5 种基础 + 通用 ops)

绑定 5 种最常用 joint:

| Joint 类型 | 用途 | 关键参数 |
|---|---|---|
| `DistanceJoint` | 弹簧/绳索固定距离 | bodyA/B, anchorA/B, length, frequency, damping |
| `RevoluteJoint` | 关节(齿轮、铰链) | bodyA/B, anchor, motor speed/torque, limits |
| `PrismaticJoint` | 滑轨/活塞 | bodyA/B, anchor, axis, motor force/speed, limits |
| `WeldJoint` | 刚性焊接 | bodyA/B, anchor |
| `MouseJoint` | 鼠标拖拽 | targetBody, target xy, maxForce |

每种 joint 的 ops:
- `World.CreateXxxJoint(defs_table)` → joint userdata
- `World.DestroyJoint(joint)`
- `Joint.GetBodyA() / GetBodyB() / GetType() / IsActive() / GetAnchorA() / GetAnchorB() / GetReactionForce(inv_dt) / GetReactionTorque(inv_dt)`
- 对 RevoluteJoint/PrismaticJoint 额外: `EnableMotor / SetMotorSpeed / SetMaxMotorTorque / GetMotorSpeed / EnableLimit / SetLimits / GetJointAngle / GetJointSpeed`
- 对 MouseJoint 额外: `SetTarget / GetTarget / SetMaxForce`

**Joint handle 模式** (与 Body/Fixture 一致):
```cpp
struct PhysicsJoint {
    b2Joint* joint;
    PhysicsWorld* owner;
    int selfRef;
    int userRef;
    bool alive;
};
```
通过 `__joint` field 存 userdata,Lua table 暴露 method。

### 3.2 P0 — Raycast

`World.RayCast(x1, y1, x2, y2, callback)`:
- C++ 端临时 `class LuaRayCastCallback : public b2RayCastCallback` 包装 Lua function
- callback 签名: `function(fixture, x, y, normal_x, normal_y, fraction) -> control_value`
  - 返回 `-1` = 跳过此 fixture
  - 返回 `0` = 终止 raycast
  - 返回 `fraction` = 截断到此 fraction
  - 返回 `1` = 继续 raycast,记录所有命中

### 3.3 P0 — Query AABB

`World.QueryAABB(x1, y1, x2, y2, callback)`:
- 临时 `class LuaQueryCallback : public b2QueryCallback`
- callback 签名: `function(fixture) -> bool` (返回 false 终止)

### 3.4 P1 — Sensor + TestPoint

- `Fixture.SetSensor(bool)`, `Fixture.IsSensor()`
- `Fixture.TestPoint(x, y) -> bool`(像素坐标)

### 3.5 P1 — Body 坐标助手

- `Body.GetWorldPoint(local_x, local_y) -> world_x, world_y`
- `Body.GetLocalPoint(world_x, world_y) -> local_x, local_y`
- `Body.GetWorldVector(local_x, local_y) -> world_x, world_y`
- `Body.GetLocalVector(world_x, world_y) -> local_x, local_y`

## 4. 工作量估算

| 模块 | fns 数 | 行数 |
|---|---|---|
| Joint 基础设施 (struct + helpers + GC) | — | ~150 |
| 5 种 Joint Create* | 5 | ~250 |
| 通用 Joint ops | 8 | ~120 |
| Revolute/Prismatic 专属 ops | 10 | ~150 |
| MouseJoint 专属 ops | 3 | ~50 |
| Raycast + LuaRayCastCallback | 1 | ~80 |
| QueryAABB + LuaQueryCallback | 1 | ~50 |
| Fixture Sensor + TestPoint | 3 | ~50 |
| Body 坐标助手 | 4 | ~60 |
| **C++ 总计** | **~35 fns** | **~960 行** |
| smoke 物理场景测试 | — | ~300 行 |

合计 ~1260 行,5-7 小时(因 Box2D v2 C++ API 较成熟,实施风险低,无 SDL 上游 bug 风险)。

## 5. 验收

### 5.1 功能验收

- 35 fns 全部注册到对应 table
- Joint 创建后能通过 `World.DestroyJoint` 安全销毁,GC 不重复 free
- Raycast: 一条线段穿越多个 fixture,callback 按 fraction 升序回调
- QueryAABB: 矩形覆盖多个 fixture,callback 全部触达
- Sensor fixture 不参与碰撞响应,但仍触发 BeginContact/EndContact
- TestPoint: 在 circle 圆心命中 true,远点 false

### 5.2 端到端 smoke

构造 1 个完整物理场景:
1. 静态地面 + 边墙(EdgeShape)
2. 动态盒子(从空中落下)
3. RevoluteJoint 把两个盒子接成"哑铃"
4. DistanceJoint 把"哑铃"系到天花板(spring rope 效果)
5. World.Step 多步,验证位置/速度演化
6. Raycast 从顶部往下射,命中"哑铃" body
7. QueryAABB 框选场景中所有 body
8. TestPoint 在哑铃上验证命中
9. DestroyBody 级联销毁所有 joint(Box2D 自动)

### 5.3 回归

- 所有现有 Light.Physics smoke 不破坏
- 16 个 sibling smoke 全绿
- CI 6 平台 success

## 6. 非目标

- Box2D v3 升级(版本切换,本期不动)
- GearJoint / MotorJoint / PulleyJoint(P2,延后)
- PreSolve / PostSolve callback(P2,延后)
- ContactFilter(category/mask/group bits — P2)
- Particle system(Box2D 2.4 已移除)

## 7. 关键决策

无需中断询问。设计与现有 Body/Fixture handle 模式一致,扩展即可。直接进 Architect → Atomize → Automate。
