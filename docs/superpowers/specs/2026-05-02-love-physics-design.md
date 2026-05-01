# LÖVE 风格 Physics 全套设计

## 1. 背景

当前 `Light.Physics` 使用 Box2D v3 C API，并且 `CMakeLists.txt` 在 Android 上跳过 Box2D：

- `light_physics.cpp` 依赖 `b2WorldId` / `b2BodyId` 等 v3 API。
- Android 构建中 `light_physics.cpp` 未加入编译，导致移动端没有 Physics 能力。
- 用户目标是补齐类似 LÖVE2D 的内置 Box2D 物理能力，并优先解决 Android 适配。

本设计只定义方案，不实施代码。

## 2. 目标

1. 使用 vendored Box2D v2.4.1 替换当前 Box2D v3 FetchContent。
2. Windows / Linux / macOS / Android / iOS 默认启用 `Light.Physics`。
3. 保持现有 `Light.Physics.World` / `Body` API 兼容。
4. 扩展为接近 LÖVE2D `love.physics` 的对象模型：`World`、`Body`、`Shape`、`Fixture`、`Joint`、`Contact`。
5. 保持 ChocoLight 当前 Lua OOP 风格，不直接照搬 LÖVE 命名，但能力覆盖尽量对齐。
6. 所有新增 API 后续需要补 `@lua_api` 注解并重新生成 `docs/api`。

## 3. 非目标

1. 不追求与 LÖVE2D 100% 参数级兼容。
2. 不在第一轮实现可视化物理编辑器。
3. 不实现 3D 物理。
4. 不把 Box2D 作为动态库暴露给用户；Box2D 只作为 Light 内部静态依赖。
5. 不允许 Lua 层直接持有裸 Box2D 指针。

## 4. 依赖与构建设计

### 4.1 Box2D 版本

使用 Box2D `v2.4.1` 源码，放入：

```text
ChocoLight/third_party/box2d/
```

### 4.2 CMake 调整

移除当前 v3 FetchContent：

```cmake
FetchContent_Declare(box2d ... v3.0.0)
FetchContent_MakeAvailable(box2d)
```

改为 vendored 源码：

```cmake
set(BOX2D_BUILD_TESTBED OFF CACHE BOOL "" FORCE)
set(BOX2D_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/box2d EXCLUDE_FROM_ALL)
```

`light_physics.cpp` 改为默认全平台加入 `LIGHT_SOURCES`。如果某个平台临时无法编译，使用 `CHOCO_HAS_BOX2D=0` 编译空壳模块，而不是直接缺失导出符号。

### 4.3 链接策略

`Light` 链接 Box2D 静态库：

```cmake
target_link_libraries(Light PRIVATE box2d)
target_compile_definitions(Light PRIVATE CHOCO_HAS_BOX2D=1)
```

Android / iOS 使用 `Light` 静态库最终链接进应用，不暴露 Box2D 动态库。

## 5. 模块架构

### 5.1 C++ 内部对象

```text
PhysicsWorld
  - b2World* world
  - PhysicsContactListener* listener
  - vector<PhysicsBody*> bodies
  - vector<PhysicsJoint*> joints
  - Lua callback refs

PhysicsBody
  - b2Body* body
  - PhysicsWorld* owner
  - vector<PhysicsFixture*> fixtures
  - Lua self ref
  - alive flag

PhysicsShape
  - enum shape type
  - unique ownership of b2Shape clone data

PhysicsFixture
  - b2Fixture* fixture
  - PhysicsBody* owner
  - Lua self ref
  - alive flag

PhysicsJoint
  - b2Joint* joint
  - PhysicsWorld* owner
  - Lua self ref
  - alive flag

PhysicsContactEvent
  - event type
  - body/fixture refs
  - contact flags
```

### 5.2 所有权规则

- `PhysicsWorld` 拥有 `b2World`。
- `b2World` 拥有所有 `b2Body` 和 `b2Joint`。
- `b2Body` 拥有所有 `b2Fixture`。
- Lua userdata 只保存包装对象和 `alive` 状态，不直接拥有 Box2D 实体。
- `World.__gc` 负责释放整个物理世界，并将所有 Body / Fixture / Joint 标记为失效。
- `DestroyBody` / `DestroyFixture` / `DestroyJoint` 必须先解除 Lua registry 引用，再调用 Box2D 销毁。

## 6. Lua API 设计

### 6.1 兼容旧 API

保留现有调用：

```lua
local world = Light(Light.Physics.World):New()
world:SetGravity(0, 980)
world:Step(dt)

local body = world:CreateBody("dynamic", 100, 100)
body:AddBox(32, 32)
body:AddCircle(16)
body:SetLinearVelocity(100, 0)
```

兼容函数可作为新对象模型的快捷封装：

- `body:AddBox(w, h)` 内部创建 RectangleShape + Fixture。
- `body:AddCircle(r)` 内部创建 CircleShape + Fixture。
- 旧代码忽略返回值时行为不变；新代码可接收返回的 `Fixture`。

### 6.2 World API

建议新增：

```lua
world:GetGravity() -> gx, gy
world:ClearForces()
world:SetAllowSleeping(enabled)
world:SetContinuousPhysics(enabled)
world:CreateBody(type, x, y) -> Body
world:DestroyBody(body)
world:GetBodyCount() -> number
world:OnCollision(callback)
world:BeginContact(callback)
world:EndContact(callback)
world:RayCast(x1, y1, x2, y2, callback)
world:QueryAABB(x1, y1, x2, y2, callback)
world:CreateDistanceJoint(bodyA, bodyB, options) -> Joint
world:CreateRevoluteJoint(bodyA, bodyB, x, y, options) -> Joint
world:CreateWeldJoint(bodyA, bodyB, x, y, options) -> Joint
world:CreatePrismaticJoint(bodyA, bodyB, x, y, ax, ay, options) -> Joint
world:DestroyJoint(joint)
```

`PreSolve(callback)` 与 `PostSolve(callback)` 归入 P3 高级 Contact 阶段，不进入 P0/P1 的首轮实现范围。

### 6.3 Body API

建议新增：

```lua
body:GetType() -> string
body:SetType(type)
body:GetPosition() -> x, y
body:SetPosition(x, y)
body:GetAngle() -> radians
body:SetAngle(angle)
body:GetLinearVelocity() -> vx, vy
body:SetLinearVelocity(vx, vy)
body:GetAngularVelocity() -> number
body:SetAngularVelocity(v)
body:ApplyForce(fx, fy)
body:ApplyImpulse(ix, iy)
body:ApplyTorque(torque)
body:SetAwake(enabled)
body:IsAwake() -> boolean
body:SetActive(enabled)
body:IsActive() -> boolean
body:SetBullet(enabled)
body:IsBullet() -> boolean
body:SetLinearDamping(value)
body:GetLinearDamping() -> number
body:SetAngularDamping(value)
body:GetAngularDamping() -> number
body:CreateFixture(shape, density) -> Fixture
body:DestroyFixture(fixture)
body:GetMass() -> number
body:GetInertia() -> number
body:GetWorldCenter() -> x, y
body:GetLocalCenter() -> x, y
```

### 6.4 Shape API

新增独立 Shape 类型：

```lua
local circle = Light.Physics.NewCircleShape(radius)
local rect = Light.Physics.NewRectangleShape(width, height)
local poly = Light.Physics.NewPolygonShape({x1,y1, x2,y2, x3,y3})
local edge = Light.Physics.NewEdgeShape(x1, y1, x2, y2)
local chain = Light.Physics.NewChainShape({x1,y1, x2,y2, x3,y3}, loop)
```

Shape 是可复用定义对象。`CreateFixture(shape)` 时复制到底层 Box2D，避免一个 Lua Shape 被多个 Fixture 共享后产生生命周期问题。

### 6.5 Fixture API

```lua
fixture:GetBody() -> Body
fixture:GetShape() -> Shape
fixture:SetDensity(value)
fixture:GetDensity() -> number
fixture:SetFriction(value)
fixture:GetFriction() -> number
fixture:SetRestitution(value)
fixture:GetRestitution() -> number
fixture:SetSensor(enabled)
fixture:IsSensor() -> boolean
fixture:SetFilterData(categoryBits, maskBits, groupIndex)
fixture:GetFilterData() -> categoryBits, maskBits, groupIndex
fixture:SetUserData(value)
fixture:GetUserData() -> value
```

`SetUserData` 使用 Lua registry 引用，不把 Lua 指针塞进 Box2D 原始 userData 中。

### 6.6 Contact API

碰撞回调参数建议从旧版 `(bodyA, bodyB)` 扩展为 Contact 对象，同时保留旧版 `OnCollision`：

```lua
world:OnCollision(function(bodyA, bodyB) end)
world:BeginContact(function(contact) end)
world:EndContact(function(contact) end)
```

`Contact` 支持：

```lua
contact:GetBodyA() -> Body
contact:GetBodyB() -> Body
contact:GetFixtureA() -> Fixture
contact:GetFixtureB() -> Fixture
contact:IsTouching() -> boolean
contact:SetEnabled(enabled)
contact:SetFriction(value)
contact:SetRestitution(value)
```

`PreSolve` / `PostSolve` 后续可提供更完整接触信息，但第一版应先保证 Begin/EndContact 稳定。

### 6.7 Joint API

第一轮完整设计包含以下 Joint：

- `DistanceJoint`
- `RevoluteJoint`
- `WeldJoint`
- `PrismaticJoint`
- `RopeJoint`
- `MouseJoint`

通用方法：

```lua
joint:GetType() -> string
joint:GetBodyA() -> Body
joint:GetBodyB() -> Body
joint:GetAnchorA() -> x, y
joint:GetAnchorB() -> x, y
joint:GetReactionForce(invDt) -> fx, fy
joint:GetReactionTorque(invDt) -> number
joint:SetCollideConnected(enabled)
joint:GetCollideConnected() -> boolean
```

每类 Joint 再补对应参数，例如 DistanceJoint 的长度、频率、阻尼比，RevoluteJoint 的 motor/limit。

## 7. 坐标与单位

沿用当前约定：

```text
Lua 像素坐标 / PTM = Box2D 米坐标
PTM = 32.0
```

- Lua 输入输出默认使用像素。
- 角度使用弧度。
- 速度使用像素/秒。
- 重力使用像素/秒²。
- Box2D 内部统一除以 `PTM`。

## 8. 碰撞事件数据流

Box2D v2 的 `b2ContactListener` 在 `world->Step()` 过程中触发。为避免 Lua 回调中修改世界导致 Box2D 状态不稳定，采用事件队列：

1. `BeginContact` / `EndContact` 在 listener 中只记录 `PhysicsContactEvent`。
2. `world->Step()` 返回后统一分发 Lua 回调。
3. `OnCollision` 使用 BeginContact 事件兼容旧接口。
4. `PreSolve` / `PostSolve` 因为需要影响 contact 状态，默认延后实现或以受限回调提供。

## 9. 错误处理

- 传入已销毁对象时不崩溃，返回默认值并记录 `CC::LOG_WARN`。
- 创建非法多边形时返回 `nil` 或抛 Lua 参数错误，优先使用 `luaL_error` 暴露调用错误。
- Box2D 创建失败时返回 `nil`。
- Android 上如果 Box2D 未启用，`Light.Physics` 仍注册模块，但构造 `World` 时明确报错。

## 10. 测试设计

### 10.1 编译验证

- Windows: `cmake -B build -A x64 && cmake --build build --config Release`
- Linux: GitHub Actions Linux job 必须不再 `|| true` 掩盖 ChocoLight 失败。
- macOS: 同 Linux，移除 ChocoLight 构建失败忽略。
- Android: `templates/android-sdl3` Gradle APK 编译通过。
- iOS: CMake 配置通过，必要时补 Xcode 构建验证。

### 10.2 Lua Smoke Tests

新增或整理测试脚本：

1. World 创建、设置重力、Step 后 dynamic body 下落。
2. Rectangle / Circle fixture 创建。
3. Fixture density/friction/restitution/sensor 设置读取。
4. BeginContact / EndContact 回调触发。
5. QueryAABB 返回目标 fixture。
6. RayCast 命中目标 fixture。
7. DistanceJoint 或 RevoluteJoint 创建并约束 body。
8. DestroyBody / DestroyFixture / DestroyJoint 后访问不崩溃。

## 11. 实施分期

完整目标拆为五期，避免一次性大改失控。

### P0: Box2D v2 vendor + 构建恢复

- 引入 `third_party/box2d`。
- CMake 切换到 vendored Box2D。
- `light_physics.cpp` 迁移到 Box2D v2 基础 API。
- Android 编译包含 Physics。

### P1: World / Body / Shape / Fixture 核心对象

- 完成兼容旧 API。
- 新增 Shape 和 Fixture 基础能力。
- 完成 BeginContact / EndContact 和旧 `OnCollision`。

### P2: Joint 系列

- 新增 Distance / Revolute / Weld / Prismatic / Rope / Mouse Joint。
- 完成 Joint 生命周期和 Lua 包装。

### P3: Query / RayCast / 高级 Contact

- 新增 AABB Query、RayCast。
- 增强 Contact 对象。
- 评估 `PreSolve` / `PostSolve` 可变 contact 支持。

### P4: 文档、示例、CI 加固

- 补齐 `@lua_api` 标注。
- 生成 `docs/api/Light_Physics*.md`。
- 更新 `ENGINE_EVALUATION.md` 和 Phase3 状态。
- 移除 CI 中 ChocoLight 构建的 `|| true`。
- 添加 Android 物理示例。

## 12. 风险与对策

| 风险 | 影响 | 对策 |
|------|------|------|
| Box2D v2 C++ API 与 v3 差异大 | 需要重写 `light_physics.cpp` | 保持 Lua API，内部集中迁移 |
| Lua GC 与 Box2D 生命周期冲突 | 悬空指针/崩溃 | 所有包装对象加 `alive`，World 统一拥有实体 |
| 碰撞回调中修改世界 | Box2D Step 状态不稳定 | Begin/End 队列化，Step 后分发 |
| Android NDK STL/异常设置差异 | 编译失败 | 静态链接 Box2D，避免异常依赖，CI 验证 |
| Full LÖVE 风格范围过大 | 实施周期变长 | P0-P4 分期交付 |

## 13. 验收标准

最终完成时满足：

1. `ENGINE_EVALUATION.md` 中 `Box2D Android 适配` 可标记完成。
2. Android APK 编译通过，且 `Light.Physics` 可创建世界和刚体。
3. 桌面平台旧 Physics Lua 脚本无需修改仍可运行。
4. 新 Shape / Fixture / Contact / Joint API 有文档和示例。
5. CI 不再忽略 ChocoLight 编译失败。
