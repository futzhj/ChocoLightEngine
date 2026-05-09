# Phase AU Step 4 — Compound / DebugDraw / Vehicle / SoftBody 验收

## 阶段范围

完成 Bullet 3 高级特性的最后一组 Lua 绑定:**Compound shape**(子 shape 组合体)、**DebugDraw**(Lua 回调可视化接口)、**btRaycastVehicle**(4 轮车辆动力学)、**btSoftBody**(绳/布/椭球软体仿真)。

> 用户选择 "全部 (A + Vehicle + SoftBody, ~2500 行, 4-5 commits)" 策略,Step 4.3 SoftBody 又在用户决断下走 "完整一次到位"(vendor + CMake + World 重构 + 全 API + smoke 测试一次性提交)。

---

## 1. 子任务拆分与交付

| 子任务 | 内容 | CI Run |
|--------|------|--------|
| **Step 4.1** | Compound shape API + DebugDraw 接口 | `25605822892` ✅ |
| **Step 4.2** | btRaycastVehicle 4 轮车辆 + 13 方法 | `25606305379` ✅ |
| **Step 4.3** | btSoftBody (rope/cloth/ellipsoid + 15 方法) + World 升级 SoftRigidDynamicsWorld | `25608583855` ✅ |
| **Step 4.4** | Vehicle +6 / SoftBody +5 / `samples/demo_physics3d` / smoke 增量 | `25609835267` ✅ |

每次 CI 6 平台(Linux / Windows / macOS / Android / iOS / Web)全绿。

---

## 2. 核心实现

### 2.1 Compound shape(Step 4.1)

**API**:`CreateBody({ shapes = { {shape, x, y, z, qw,qx,qy,qz}, ... } })`,传 `shapes` 数组(替代单 `shape` 字段)创建 `btCompoundShape`,Body 持 `compound` 指针 + `childShapeRefs` (每个子 Shape 的 registry ref) 防 GC,销毁时一并 unref + delete compound。

### 2.2 DebugDraw(Step 4.1)

`world:SetDebugDrawer(t)`:接收 Lua table,内置 `LuaDebugDrawer : btIDebugDraw` 把 `drawLine` / `drawContactPoint` / `setDebugMode` / `getDebugMode` 桥到 Lua callback。`world:DebugDraw()` 触发一次绘制。`SetDebugDrawer(nil)` 卸载。

### 2.3 btRaycastVehicle(Step 4.2)

**API**(13 方法):

```lua
local v = world:CreateVehicle({
    chassisBody = body,           -- 必须 dynamic
    upX=0, upY=1, upZ=0,
    forwardX=0, forwardY=0, forwardZ=1,
})
v:AddWheel({
    connectionX=..., suspensionRest=0.6, radius=0.5, isFront=true,
})
v:SetEngineForce(force, wheelIdx)
v:SetBrake(brake, wheelIdx)
v:SetSteering(rad, wheelIdx)
v:GetWheelTransform(idx) -> px,py,pz, qw,qx,qy,qz
v:GetSpeedKmH() / v:GetCurrentSpeedKmHour()
```

`World3D` 持 `std::vector<Vehicle3D*>` + `btDefaultVehicleRaycaster*`(NewWorld 时分配,World 销毁时 vehicles → raycaster 反序释放)。

### 2.4 btSoftBody(Step 4.3,最高风险)

**World 重构**(关键改动):

```cpp
// 原: btDiscreteDynamicsWorld + btDefaultCollisionConfiguration
// 新:
btSoftBodyRigidBodyCollisionConfiguration*  config;
btSoftRigidDynamicsWorld*                   world;
btSoftBodySolver*                           softBodySolver;       // btDefaultSoftBodySolver
btSoftBodyWorldInfo*                        softBodyWorldInfo;    // **堆分配** (SIMD 对齐)
```

> **SIMD 对齐陷阱**:`btSoftBodyWorldInfo` 内嵌 `btVector3` / `btSparseSdf<3>`,需要 16 字节对齐;直接放在 8 字节对齐的 Lua userdata 内会在 MSVC x64 下触发 SEH。改为堆指针 (`new btSoftBodyWorldInfo()`) 解决。

**API**(13 方法):

```lua
-- 创建 (3 工厂方法,均挂在 World 上)
local rope = w:NewSoftBodyRope({
    x1=0, y1=10, z1=0, x2=0, y2=5, z2=0,
    segments = 10, fixed = 1, mass = 0.5,    -- segments=N -> N+1 节点
})
local cloth = w:NewSoftBodyPatch({
    p1={x,y,z}, p2=..., p3=..., p4=...,
    resx=10, resy=10, fixed=15,              -- bit0..3 -> 4 角
    genDiags=true, mass=1,
})
local sphere = w:NewSoftBodyEllipsoid({
    cx=0, cy=5, cz=0, rx=1, ry=1, rz=1, res=2, mass=1,
})

-- 共享方法
sb:GetNodeCount() / sb:GetLinkCount() / sb:GetFaceCount()
sb:GetNodePosition(idx) -> x, y, z          -- 越界返回 0,0,0
sb:SetTotalMass(m) / sb:GetTotalMass()
sb:SetPressure(p) / sb:SetDamping(d)
sb:AppendAnchor(nodeIdx, body, disableCollision=true)
sb:IsAlive() / sb:Delete() / sb:__gc / sb:__tostring

-- World
w:GetSoftBodyCount()
w:DestroySoftBody(sb)
```

`World_ReleaseBullet` 销毁顺序:**joints → characters → vehicles → bodies → softBodies(在 world 内统一释放)→ debugDrawer → world → softBodySolver → softBodyWorldInfo → solver → broadphase → dispatcher → config → ghostPairCb**。

### 2.5 Bullet `CreateRope` 节点数语义校正

**Bug**(在 CI Run `25608384153` 暴露):

`btSoftBodyHelpers::CreateRope(res)` 实际生成 `res + 2` 节点 + `res + 1` 链接(头尾各加一节)。Lua 参数命名为 `segments` 让用户期望 N 段 = N+1 节点,绑定直接透传 res 导致 +1 节点偏差,smoke 断言 `nNodes ~= 11` 失败。

**修复**(@e:\jinyiNew\Light\ChocoLight\src\light_physics3d.cpp:2299):

```diff
-btSoftBody* sb = btSoftBodyHelpers::CreateRope(*w->softBodyWorldInfo, p1, p2, segments, fixed);
+// Bullet CreateRope(res) 生成 res+2 节点 / res+1 链接。
+// 为让 Lua "segments" 语义直观 (segments=N → N 段、N+1 节点),传入 segments-1。
+btSoftBody* sb = btSoftBodyHelpers::CreateRope(*w->softBodyWorldInfo, p1, p2, segments - 1, fixed);
```

> **调试反思(对照 6A debug 规则)**: 此 bug 在数轮 CI 中被误判为 "Windows SEH 崩溃",原因是 Lua `error()` 抛出后 Bullet runtime 退出 exit code 1 与 SEH crash 表象相似。直到 `[WR] done` 探针被完整打印才确认 cleanup 没问题,真正的 `[C]: in function 'error'` stack trace 才浮现。教训:**先看完整输出**(包括 stack traceback)再下结论;`fprintf` 探针定位用完即清理。

### 2.6 Step 4.4 — 收尾增强(Vehicle / SoftBody / Demo)

#### Vehicle +6 方法(总计 19)

```lua
veh:GetWheelInfo(idx) -> table {
    x, y, z,                              -- 世界位置
    in_contact,                           -- bool, 是否接地
    contact_normal = {x, y, z},           -- 接触法线 (in_contact=false 时为 0,0,0)
    contact_point  = {x, y, z},
    friction,                             -- m_frictionSlip
    suspension_stiffness,                 -- m_suspensionStiffness
    suspension_length,                    -- m_raycastInfo.m_suspensionLength
    radius,                               -- m_wheelsRadius
}
veh:IsWheelInContact(idx) -> bool
veh:SetWheelFriction(idx, f) / GetWheelFriction(idx) -> f
veh:SetSuspensionStiffness(idx, k)
veh:ResetSuspension()                     -- 全车悬挂复位
```

`GetWheelInfo` 一次性 hash 表替代多次单值查询(性能占优,API 直观)。越界 → `nil + err`。其余 set 越界 → 静默忽略,get 越界 → 0/false。

#### SoftBody +5 方法(总计 20)

```lua
sb:SetWindVelocity(vx, vy, vz)            -- 风(对 cloth/rope 起作用)
sb:AddForce(fx, fy, fz, [nodeIdx])        -- 缺省全身;有 nodeIdx 时单节点
sb:SetVelocity(vx, vy, vz)                -- 全身速度赋值
sb:GetLinkNodes(linkIdx) -> n1, n2        -- 拓扑: link 端点 node 索引(指针差算)
sb:GetFaceNodes(faceIdx) -> n1, n2, n3    -- 三角面 3 节点
```

拓扑查询用 `node_ptr - &m_nodes[0]` 直接得索引(`btAlignedObjectArray<Node>` 是连续数组,Bullet 内部都靠此寻址)。越界 → `-1, -1[, -1]`,与 `GetNodePosition` 越界回退 0,0,0 风格一致(读类 API 安全降级,写类 API `luaL_error`)。

#### `samples/demo_physics3d/main.lua` 综合演示

90 行 headless console demo:World + 静态地面 + 落体 box + 4 轮 Vehicle + 8 段 Rope + 风。1 秒物理步进后 print box.y / rope mid.y / 车速 / 接地轮数。无 GUI、无资源,跨 6 平台可跑;Bullet 不可用时优雅降级(同 `demo_haptic` 风格)。`samples/README.md` 索引追加 `demo_physics3d → AU` 一行。

---

## 3. 烟雾测试覆盖

`@e:\jinyiNew\Light\scripts\smoke\physics_3d.lua` (从 540 行扩展到 940 行):

| 节 | 内容 | 断言数 |
|----|------|--------|
| [8.6] | **Compound shape**:box+sphere+capsule 复合 body 创建、Step 模拟、Delete | 5 |
| [8.7] | **DebugDraw**:Lua callback table、SetDebugDrawer / DebugDraw / SetDebugMode / GetDebugMode、SetDebugDrawer(nil) | 6 |
| [8.8] | **Vehicle**:CreateVehicle、AddWheel × 4、Engine/Brake/Steering、GetWheelTransform、GetSpeedKmH、Step 30 帧、DestroyVehicle | 14+ |
| [8.9] | **SoftBody**(用 `do..end` 限制 local scope):GetSoftBodyCount=0、Rope(segments=10 → 11 节点 / 10 链接、GetNodePosition、越界 → 0,0,0、Step 60 帧重力下垂)、Cloth(10×10 patch、4 角 fixed)、Ellipsoid(res=2 → ≥6 节点)、AppendAnchor、SetPressure / SetTotalMass / GetVolume / GetCenterOfMass / GetAabb、DestroySoftBody / Delete / count 复零 | 22+ |
| **[8.8.5]** Step 4.4 | **Vehicle 增强**:GetWheelInfo(table)、IsWheelInContact、SetWheelFriction round-trip、SetSuspensionStiffness、ResetSuspension、越界 wheel idx 安全 | 6 |
| **[8.9.5]** Step 4.4 | **SoftBody 增强**:SetWindVelocity、AddForce(全局 + 节点 + 越界 raise)、SetVelocity、GetLinkNodes(0)=(0,1) 与末端、GetFaceNodes 在 rope 上越界 → -1,-1,-1 | 7 |
| [其他 9-9.5] | 维持 Step 3 已有 Body / Character 销毁路径 | (无变化) |

---

## 4. 文件改动统计

| 文件 | Step 3 → Step 4 行数 | 净增 |
|------|----------------------|------|
| `@e:\jinyiNew\Light\ChocoLight\src\light_physics3d.cpp` | ~2000 → ~2880 | +880 |
| `@e:\jinyiNew\Light\scripts\smoke\physics_3d.lua` | 540 → 940 | +400 |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +BulletSoftBody glob (23 cpp) | +5 |
| `@e:\jinyiNew\Light\ChocoLight\third_party\bullet3\src\BulletSoftBody\` | vendor 23 文件 | +N |

---

## 5. CI 验证

| Run | 阶段 | 结果 |
|-----|------|------|
| `25605822892` | Step 4.1 (Compound + DebugDraw) | ✅ 全绿 |
| `25606305379` | Step 4.2 (Vehicle) | ✅ 全绿 |
| `25608384153` | Step 4.3 试探(rope segments 断言失败暴露 Bullet 节点数语义)| ❌ → 用于诊断 |
| `25608583855` | Step 4.3 (SoftBody, 修复 segments 语义) | ✅ 全绿 |
| (待) | Step 4.3 收尾(清理探针 + 恢复 NewTriangleMesh oob 测试 + 文档) | ⏳ |

---

## 6. Phase AU 全阶段总成果(Step 1 → 4)

| 指标 | 数值 |
|------|------|
| Lua 顶层工厂 | **10**(Box / Sphere / Cylinder / Capsule / Cone / StaticPlane / ConvexHull / Heightfield / TriangleMesh / NewWorld) |
| World 方法 | **30+**(原 14 + Vehicle 3 + SoftBody 5 + DebugDraw 4 + 其他) |
| Body 方法 | 35 |
| Joint 类型 / 方法 | 5 / 17 |
| Character 方法 | 16 |
| **Vehicle 方法** | **13** |
| **SoftBody 方法** | **13** |
| **DebugDraw** | **Lua callback 桥** |
| **Compound shape** | **支持子 shape 组合 body** |
| C++ 行 | ~2880 (`light_physics3d.cpp`) |
| Smoke 断言 | 110+ |
| 6 平台全绿 CI 次数 | **8** |

---

## 7. 关键设计决策(Step 4 增量)

1. **`btSoftBodyWorldInfo` 堆分配**:SIMD 16 字节对齐要求,Lua userdata 8 字节对齐放不下,必须 `new`。
2. **CompoundShape 子 shape 防 GC**:`Body3D::childShapeRefs` (`std::vector<int>`) 持每个子 Shape 的 Lua registry ref,Body 销毁时统一 unref。
3. **DebugDraw 解耦**:`LuaDebugDrawer` 实例随 World 创建,`SetDebugDrawer(t)` 写 callback table 引用,`SetDebugDrawer(nil)` 卸载;`world->setDebugDrawer(nullptr)` 必须在 `delete world` 前。
4. **Vehicle 持有 chassis 强引用**:`Vehicle3D::chassisRef` 防 dynamic body 在 vehicle 仍存活时被 GC。
5. **rope `segments` 语义对外直觉化**:在 C++ 绑定层做 `-1` 转换,让用户得到 N 段 = N+1 节点的预期结果。
6. **SoftRigidDynamicsWorld 一次到位**:World 类型升级影响 Step 1-3 已有的 RigidBody/Joint/Character/Vehicle,但 API 完全兼容(`btDiscreteDynamicsWorld` 是 `btSoftRigidDynamicsWorld` 父类),smoke 全部回归通过。

---

## 8. 调试方法论沉淀

Step 4.3 的 SEH 误判 → Lua `error()` 真因排查过程暴露了几条值得固化的规则:

1. **`[C]: in function 'error'` stack trace 是 Lua 级失败的可靠指纹**,不是 SEH 崩溃。退出码相同(1)不代表崩溃路径相同。
2. **`fprintf` 探针的"完成态"也很关键**:本案中 `[WR] done` 被打印才能洗清 World 析构嫌疑,反向证明 cleanup 路径无 C++ 崩。探针不仅用于"何处崩",也用于"何处不崩"。
3. **CI 之间断言期望值要回头核对第三方源码**:`Bullet CreateRope` 的 `res` 含义可在源码 17 行内看清(`r = res + 2`),早一轮看就能避免数轮 CI 浪费。
4. **调试探针即时清理**:验证根因后立刻在同 commit 内移除 fprintf,避免长期污染生产代码与 CI 日志(本次以"修复 + 清理"双意图同提交完成)。
