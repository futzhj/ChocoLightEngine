# Phase AU — 3D 物理增强 — 对齐文档 (含成本警告)

> **6A Stage 1**: Align (Phase AU 3D 物理)
>
> ⚠️ **CI 风险预警**: 引入 Bullet 这种重量级第三方库 (~50k LoC, 数百源文件) 需要 6 平台 (Windows/Linux/macOS/Android/iOS/WASM) 都能编译。比之前的 cgltf/miniaudio 单文件方案风险大数倍。

---

## 1. 现状回顾 (2D 物理已完成)

`light_physics.cpp` (3500 行) 已实现完整 Box2D 2D 物理 (Phase AO + AP):

| 模块 | 数量 |
|---|---|
| `Light.Physics` 形状构造器 | 5 (Circle/Rect/Polygon/Edge/Chain) |
| `Light.Physics.World` 方法 | 60+ |
| Joint 类型 | 11 (Distance/Revolute/Prismatic/Weld/Mouse/Wheel/Motor/Friction/Pulley/Gear) |
| Body / Fixture / Contact / RayCast / QueryAABB | 全部齐备 |
| 仿真控制 (SubStepping/WarmStarting/PreSolve/PostSolve) | ✅ |

**Box2D 是 2D 专用,无 3D 替代。引入 3D 物理需要新库。**

---

## 2. Phase AU 范围方案对比

| 方案 | 库 | 集成行数 | 第三方 LoC | CI 风险 | 完整性 |
|---|---|---|---|---|---|
| **A. 自实现最简** | (无) | ~600 | 0 | 🟢 极低 | 仅 Box/Sphere + Raycast |
| **B. ReactPhysics3D** | RP3D | ~1500 | ~20k | 🟡 中 | 完整 RigidBody + Joint + ConvexHull |
| **C. Bullet 全套** | Bullet 3 | ~3000 | ~50k | 🟠 高 | 完整 (Bullet 是工业标准) |
| **D. Jolt Physics** | Jolt | ~2500 | ~30k | 🟠 高 | 现代高性能 |

---

## 3. 各方案详细描述

### 3.1 方案 A — 自实现最简 (推荐 CI 安全优先)

```lua
-- 不依赖任何第三方库, 自己实现核心
local World = Light.Physics3D.World.New({gravity_y=-9.81})
World:SetGravity(0, -9.81, 0)
World:Step(dt)

-- 仅 2 种 shape
local box  = Light.Physics3D.Shape.NewBox(hx, hy, hz)
local sph  = Light.Physics3D.Shape.NewSphere(r)

-- RigidBody (Euler 积分, 无角速度旋转积分)
local body = World:CreateBody({type="dynamic"|"static"|"kinematic", mass=1, x=0, y=10, z=0, shape=box})
body:SetVelocity(vx, vy, vz)
body:GetPosition() -> x, y, z
body:GetVelocity()
body:ApplyForce(fx, fy, fz)
body:ApplyImpulse(ix, iy, iz)
body:SetMass(m) / GetMass()
body:SetFriction(f)
body:SetRestitution(r)

-- Box-Box / Box-Sphere / Sphere-Sphere SAT 碰撞检测
World:OnCollision(function(bA, bB) ... end)

-- 简单 raycast (Box AABB + Sphere)
World:RayCast(x1, y1, z1, x2, y2, z2) -> hit_body, x, y, z, nx, ny, nz

-- 触发器
body:SetTrigger(true)
World:OnTrigger(function(bA, bB, isEntered) ... end)
```

**特点**:
- 无第三方库, 0 编译风险
- 工作量 ~600 行 C++ (Vec3/Mat3/Quat 数学 + 简单碰撞 + Euler 积分)
- 缺点: 无 ConvexHull, 无 TriangleMesh, 无 Joint, 无 CCD (穿透问题)
- 用例: 简单 3D 游戏 (类 Minecraft / 简单 platformer / 物理玩具)

### 3.2 方案 B — ReactPhysics3D (轻量第三方)

ReactPhysics3D 是单库 ~20k LoC, ZLIB 许可, 现代 C++14, 库小 (~500KB)。

```lua
-- 完整 RigidBody + Joint + ConvexHull + TriangleMesh
local body = World:CreateRigidBody({mass=1, ...})
body:AddCollider(box_shape, transform)  -- 多 collider per body

local joint = World:CreateBallJoint(bodyA, bodyB, anchorA, anchorB)
World:CreateHingeJoint(...)
World:CreateSliderJoint(...)
World:CreateFixedJoint(...)
```

**特点**:
- 完整 3D 物理特性 (Joint, ConvexHull, TriangleMesh, CCD)
- 第三方源码 ~20k LoC (~150 文件), 需要全部加入 CMakeLists
- CI 风险: 中等 (需测 iOS/Android/WASM 编译)
- 工作量: ~1500 行 Lua bindings + 库集成

### 3.3 方案 C — Bullet 3D 全套 (用户最初选择)

Bullet Physics 是工业标准, 但库巨大:

| 子库 | LoC |
|---|---|
| LinearMath | ~10k |
| BulletCollision | ~25k |
| BulletDynamics | ~10k |
| BulletSoftBody | ~5k (可选, 软体仿真) |
| **总** | **~50k** |

**特点**:
- 文档 / 教程最丰富, 大量游戏使用 (GTA / Borderlands / Tomb Raider)
- 工作量: ~3000 行 Lua bindings + 巨型库集成 (~50k LoC, ~200 文件)
- CI 风险: 高 (Bullet 在 iOS / WASM 偶尔需手动 patch)
- DLL 大小膨胀 +5-10 MB

### 3.4 方案 D — Jolt Physics (现代选择)

Jolt 是最新的现代 3D 物理库 (Horizon Zero Dawn 用), MIT 许可。

**特点**:
- 比 Bullet 更快, 更现代 C++17
- 库大小 ~30k LoC
- API 比 Bullet 复杂, 文档少
- CI 风险: 高 (需现代 C++17, 老 Android NDK 可能有问题)

---

## 4. 决策矩阵

| 优先级 | 推荐方案 |
|---|---|
| **CI 100% 安全** | A (自实现) |
| **完整性 + 平衡** | B (ReactPhysics3D) |
| **功能最强** | C (Bullet) |
| **现代化** | D (Jolt) |

---

## 5. 工作量与风险

| 方案 | C++ 行 | 第三方 LoC | 集成时间 | CI 一次成功率估计 |
|---|---|---|---|---|
| A 自实现 | 600 | 0 | 3h | 95% |
| B RP3D | 1500 | 20k | 6h | 70% |
| C Bullet | 3000 | 50k | 10h+ | 50% |
| D Jolt | 2500 | 30k | 8h | 60% |

参考: 之前 7 个 Phase 的 CI 一次绿率是 **75%** (6/8); 引入大型第三方库通常会拉低这个数字。

---

## 6. 待用户确认决策

**问题**: 选哪个方案?

考虑:
- 当前 ChocoLight 引擎以**简单 2D 游戏 / 工具**为主, 之前的 Phase AS 3D 渲染只是基础。
- 实际游戏需要 3D 物理时, 通常**Box + Sphere + Raycast 就足够 90% 场景** (FPS 简单射击 / 第三人称动作 / 平台跳跃)。
- Joint / ConvexHull / TriangleMesh 主要给 **车辆模拟 / 布娃娃** 等高级场景, 但当前引擎没用户在做这些。

**建议**: 优先方案 A (自实现最简), 工作量小、CI 安全、覆盖 90% 实际需求。如果将来真正需要更复杂物理, 再做 Phase AU.x 引入 ReactPhysics3D 或 Bullet。
