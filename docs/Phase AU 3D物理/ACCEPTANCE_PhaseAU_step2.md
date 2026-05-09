# Phase AU Step 2 — Light.Physics3D Lua 绑定 验收

## 阶段范围
完成 Bullet 3 的 Lua 绑定核心:World/RigidBody/Shape/Raycast/OnContact。

## 实现文件

| 文件 | 行数 | 说明 |
|------|------|------|
| `ChocoLight/src/light_physics3d.cpp` | 944 | Lua 绑定主体 (`#if CHOCO_HAS_BULLET` 分两路:实现 + stub) |
| `ChocoLight/CMakeLists.txt` | +3 | `LIGHT_SOURCES` 加 `light_physics3d.cpp` |
| `lumen-master/src/light/light.cpp` | +2 | `g_lightModules[]` 加 `Light.Physics3D` |
| `scripts/smoke/physics_3d.lua` | 217 | 9 大节烟雾测试 |
| `.github/workflows/build-templates.yml` | +6 | Windows runtime smoke 串接 `phaseATSmoke` + `phaseAUSmoke` |

## API 覆盖

### `Light.Physics3D` 顶层 (7 个工厂)
- `NewBox(hx, hy, hz)`
- `NewSphere(r)`
- `NewCylinder(r, halfHeight)` (Y 轴对齐)
- `NewCapsule(r, halfHeight)`
- `NewCone(r, h)`
- `NewStaticPlane(nx, ny, nz, d)`
- `NewWorld([gx=0, gy=-9.81, gz=0])`

### `world:` 方法 (11)
`SetGravity / GetGravity / Step / CreateBody / DestroyBody / GetBodyCount / RayCast / OnContact / Delete / __gc / __tostring`

### `body:` 方法 (35)
`Get/SetPosition`, `Get/SetRotation` (quaternion w,x,y,z), `GetTransform` (16 floats column-major),
`Get/SetLinearVelocity`, `Get/SetAngularVelocity`,
`ApplyForce / ApplyCentralForce / ApplyImpulse / ApplyCentralImpulse / ApplyTorque / ApplyTorqueImpulse`,
`Get/SetMass`, `Get/SetFriction`, `Get/SetRestitution`, `SetDamping / Get{Linear,Angular}Damping`,
`Set/GetGravity` (per-body override),
`SetCcdMotionThreshold / SetCcdSweptSphereRadius`,
`Activate / IsActive`, `SetKinematic / IsKinematic`,
`IsAlive / Delete / __gc / __tostring`.

### `shape:` (内部, 用户不直接调)
仅 `__gc`, `__tostring`。

## 关键设计决策

1. **CHOCO_HAS_BULLET 宏**:`#if CHOCO_HAS_BULLET` 分两路实现,关闭时所有工厂返回 `nil + err`,模块仍可 `require`。
2. **Body 持 Shape registry ref**:`shapeRef` 防 GC,Body Delete 时 unref。
3. **Body 持 selfRef**:`OnContact` 回调拿到同一个 Lua object,不创建临时 userdata。
4. **World GC 拆分**:`Delete` 仅释放 Bullet 资源(幂等),`__gc` 多调一次显式 `~World3D()` 释放 `std::vector` 等容器内存。
5. **owner 切断**:`World_ReleaseBullet` 时把每个 body 的 `owner` 设 nullptr,避免 GC 顺序导致 Body 访问悬空 World 指针。
6. **Bullet 内部 user pointer**:`btRigidBody::setUserPointer(Body3D*)` 用于 RayCast/OnContact 反查。

## 已知 bug 修复

| 类别 | 问题 | 修复 |
|------|------|------|
| 头文件 | `std::remove` 未引入 `<algorithm>` | 加 include |
| 头文件 | `placement new (b) Body3D()` 未引入 `<new>` | 加 include |
| 内存 | `Delete` 与 `__gc` 都注册到 `l_World_GC` 导致重复 `delete` Bullet 资源 / 重复 `~World3D()` | 拆为 `l_World_Delete` (幂等释放) + `l_World_GC` (释放 + 析构) |
| 悬空指针 | Body GC 晚于 World GC 时访问 `owner->bodies` | World 释放时把 `body->owner = nullptr` |

## 烟雾测试覆盖 (`scripts/smoke/physics_3d.lua`)

1. **模块加载** — 7 工厂函数存在
2. **Shape 工厂** — 6 种 shape 创建 + tostring
3. **World 生命周期** — 默认重力 (0,-9.81,0)、SetGravity、Delete 幂等
4. **RigidBody 创建** — static/dynamic/kinematic 三种 + 缺 shape 错误返回
5. **Body 方法** — 35 个方法逐个调,数值断言 friction/restitution/damping
6. **Step 重力积分** — body 60 步从 y=10 下落,断言 < 9.5 (重力生效)
7. **RayCast** — 命中路径(可能 nil 或 hit)+ 远端无命中 = nil
8. **OnContact** — 注册 fn → 模拟接触 → 取消(`nil`)
9. **销毁路径** — `DestroyBody` body count 递减、dead body 方法不崩、World Delete 幂等

## CI 验收
- Windows: 显式串接 `phaseAUSmoke` + 上一阶段补漏 `phaseATSmoke`
- Linux/macOS: glob 自动覆盖 `scripts/smoke/*.lua`
- Android/iOS/Web: 静态库链接 Bullet 通过 `target_link_libraries(Light PRIVATE bullet3)`

## 下一步
**Step 3 (可选)** — Joint(Hinge/Slider/Cone6Dof)、ConvexHull、Heightfield、TriangleMesh、CharacterController。

