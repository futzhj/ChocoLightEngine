# Phase AU Step 3 — 高级 3D 物理特性 验收

## 阶段范围

完成 Bullet 3 高级特性的 Lua 绑定:数据驱动 mesh shape、5 种 Joint constraint、CharacterController(FPS 角色控制器)。

> 在 Step 3 实施过程中发现并修复 **`btScalar` 根因 bug**(引擎级影响,非局部),整个 Phase AU 各 step 因此受益。

---

## 1. 子任务拆分与交付

| 子任务 | 内容 | 提交 | CI Run |
|--------|------|------|--------|
| **Step 3.1** | 数据驱动 shape:`NewConvexHull` / `NewHeightfield` / `NewTriangleMesh` + **`btScalar` 根因修复** | `687941c` + `a34d344` | `25604017110` ✅ |
| **Step 3.2** | 5 种 Joint constraint:P2P / Hinge / Slider / ConeTwist / 6DOF + 17 方法 | `d12f4ff` | `25604255845` ✅ |
| **Step 3.3** | `CharacterController`:`btKinematicCharacterController` + Ghost object + 16 方法 | `d10c58d` | `25605414493` ✅ |

每次 CI 6 平台(Linux / Windows / macOS / Android / iOS / Web)全绿。Windows runtime smoke 实际跑了所有 49+ 项新断言。

---

## 2. 核心实现

### 2.1 数据驱动 shape(Step 3.1)

**新增工厂(`Light.Physics3D` 顶层)**:

```lua
-- 凸包: 顶点云 (>=4 点, 推荐 < 100)
local hull = Light.Physics3D.NewConvexHull({ x1,y1,z1,  x2,y2,z2,  ... })

-- 高度场: width × height grid, heights 是 width*height 个 float
local hf = Light.Physics3D.NewHeightfield(width, height, heights, minHeight, maxHeight, scaleX, scaleY, scaleZ)

-- 三角网格: vertices (3*N floats) + indices (3*M ints, 顺时针)
local mesh = Light.Physics3D.NewTriangleMesh({ x1,y1,z1, ... }, { i1,j1,k1,  i2,j2,k2, ... })
```

**实现要点**:

- `Shape3D` 持 `heightData` / `vertexData` / `indexData` / `indexArray`(`btTriangleIndexVertexArray*`),Bullet 内部仅存指针不拷贝,Shape GC 时统一释放
- 顶点 / 索引数据从 Lua table 拷出后由 C++ 持有,生命周期与 shape 绑定
- 错误参数(顶点 < 4、index 越界、heights 数量错)→ `nil + err message`

### 2.2 关键根因修复:`btScalar`

**问题**:在 Step 3.1 实施时,5/6 平台编译失败(`btTriangleIndexVertexArray` 模板推导错)。

**根因**(`bullet3/src/LinearMath/btScalar.h:309`):

```cpp
#if defined(BT_USE_DOUBLE_PRECISION)
    typedef double btScalar;
#else
    typedef float btScalar;
#endif
```

`#if defined(...)` 检查的是**宏是否被定义**,不是宏的值。原 `CMakeLists.txt` 写了 `BT_USE_DOUBLE_PRECISION=0`,导致 `defined` 为真 → `btScalar = double` → 我们传 `float*` 给 `btTriangleIndexVertexArray(int, int*, int, int, btScalar*, int)` 时不匹配。

**修复**(`@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt`):

```diff
-    target_compile_definitions(bullet3 PUBLIC BT_USE_DOUBLE_PRECISION=0)
+    # do NOT define BT_USE_DOUBLE_PRECISION (defined() returns true even for =0)
+    # leaving undefined makes btScalar = float (matches our float* vertex buffers)
```

同时清理 Emscripten 分支的相同 bug,只保留 `BT_NO_SIMD_OPERATOR_OVERLOADS`。

**影响**:此修复是 Phase AU 引擎级共性问题,Step 1/2/3 全部受益。

### 2.3 5 种 Joint constraint(Step 3.2)

**统一 API**(单 `Light.Physics3D.Joint` 元表,按 `type` 字段分发):

```lua
local joint = world:CreateJoint({
    type = "p2p" | "hinge" | "slider" | "conetwist" | "6dof",
    bodyA = bodyA, bodyB = bodyB,
    -- p2p / hinge: pivot + axis
    pivotA = {x,y,z}, pivotB = {x,y,z},
    axisA  = {x,y,z}, axisB  = {x,y,z},
    -- slider / conetwist / 6dof: frame {x,y,z, qw,qx,qy,qz}
    frameA = { 0,0,0, 1,0,0,0 }, frameB = { ... },
    disableCollisions = false,
})
```

**Joint 方法分组**(共 17 个):

| 通用 | Hinge | Slider | ConeTwist | 6DOF |
|------|-------|--------|-----------|------|
| `GetType` | `SetLimit(low,high,...)` | `SetLowerLinLimit/SetUpperLinLimit` | `SetConeTwistLimit(s1,s2,t)` | `SetLinearLowerLimit(x,y,z)` |
| `IsAlive` | `GetHingeAngle` | `SetLowerAngLimit/SetUpperAngLimit` | | `SetLinearUpperLimit` |
| `SetEnabled/IsEnabled` | `EnableMotor(b,vel,imp)` | `GetLinearPos` | | `SetAngularLowerLimit` |
| `Delete / __gc / __tostring` | | | | `SetAngularUpperLimit` |

**关键设计**:

- `World3D::joints` (`std::vector<Joint3D*>`) 持有 joint(防 GC)
- `Joint3D::bodyARef/bodyBRef/selfRef` 三 registry ref 防止 body / 自身 GC
- `World_ReleaseBullet` 顺序:**joints → bodies**(constraint 引用 body,反序会 use-after-free)
- `SetLimit` 只用于 hinge;ConeTwist 改名 `SetConeTwistLimit` 避免参数歧义

### 2.4 CharacterController(Step 3.3)

**API**:

```lua
local char = world:CreateCharacter({
    shape = caps,                       -- 必须 isConvex()
    x = 0, y = 2, z = 0,
    stepHeight = 0.35,                  -- 自动越过的台阶高度
    upX = 0, upY = 1, upZ = 0,          -- 重力反方向
})

char:SetWalkDirection(vx, vy, vz)
char:Jump([vx, vy, vz])                 -- 无参用默认 jumpSpeed; 带参覆盖
char:OnGround() / char:CanJump()
char:SetJumpSpeed(s) / char:GetJumpSpeed()
char:SetGravity(x,y,z) / char:GetGravity() -> x,y,z
char:SetMaxSlope(rad) / char:GetMaxSlope()
char:SetFallSpeed(s)
char:GetPosition() -> x,y,z
char:SetPosition(x,y,z)                 -- warp (内部同时同步 ghost 和 controller)
```

**关键架构**:

- 引入 `<BulletCollision/CollisionDispatch/btGhostObject.h>` + `<BulletDynamics/Character/btKinematicCharacterController.h>`
- `World3D` 增加 `btGhostPairCallback* ghostPairCb`,**`NewWorld` 时自动安装**到 broadphase pair cache(CharacterController 必需基础设施)
- `Character3D` 持 `btPairCachingGhostObject*` + `btKinematicCharacterController*` + `shapeRef` + `selfRef`
- 入 world:`addCollisionObject(ghost, CharacterFilter, StaticFilter | DefaultFilter)` + `addAction(ctrl)`
- shape 校验:`isConvex()` 拒绝 plane / heightfield / triangleMesh(返回 `nil + err`)
- `World_ReleaseBullet` 顺序:**joints → characters → bodies → ghostPairCb**

---

## 3. 烟雾测试覆盖

`@e:\jinyiNew\Light\scripts\smoke\physics_3d.lua`(从 250 行扩展到 540 行):

| 节 | 内容 | 断言数 |
|----|------|--------|
| [1]-[8] | Step 1+2 已有(模块/Shape/World/Body/Step/RayCast/OnContact) | 35+ |
| **[8.5]** | **Joint** 5 种 constraint:创建 + GetType + IsAlive,Hinge SetLimit/GetAngle/EnableMotor,Slider 4 limits + GetLinearPos,ConeTwist SetLimit,6DOF 4 limits;错误参数(bogus type / 缺 bodyA);Set/IsEnabled;DestroyJoint;Delete;Step 10 帧带 5 joints | 13+ |
| [9] | Body / World 销毁(用 `count_before/after` 替代硬编码) | 5 |
| **[9.5]** | **Character** 创建(capsule);Set/Get JumpSpeed/MaxSlope/Gravity;SetFallSpeed;SetWalkDirection + Step 30 帧;OnGround/CanJump;Jump 无参/带方向;SetPosition warp;错误参数(plane / heightfield 非 convex);DestroyCharacter;Delete | 19+ |

---

## 4. 文件改动统计

| 文件 | Step 2 → Step 3 行数 | 净增 |
|------|----------------------|------|
| `@e:\jinyiNew\Light\ChocoLight\src\light_physics3d.cpp` | 944 → ~2000 | +1056 |
| `@e:\jinyiNew\Light\scripts\smoke\physics_3d.lua` | 217 → 540 | +323 |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | (root cause fix) | +5 / -4 |

---

## 5. CI 验证

| Run | 阶段 | 6 平台 |
|-----|------|--------|
| `25603875738` | step 3.1 试探(失败,5/6 出错揭示 btScalar bug) | ❌ → 用于诊断 |
| `25604017110` | step 3.1 + btScalar 根因修复 | ✅ 全绿 |
| `25604255845` | step 3.2 (5 种 Joint) | ✅ 全绿 |
| `25605414493` | step 3.3 (CharacterController) | ✅ 全绿 |

---

## 6. Phase AU 总成果(Step 1 → 3)

| 指标 | 数值 |
|------|------|
| Lua 顶层工厂 | **10** (`NewBox` / `NewSphere` / `NewCylinder` / `NewCapsule` / `NewCone` / `NewStaticPlane` / `NewConvexHull` / `NewHeightfield` / `NewTriangleMesh` / `NewWorld`) |
| World 方法 | 14(原 11 + Joint 3 + Character 3,合计 17,扣除统计前已有的) |
| Body 方法 | 35(Step 2 完成后稳定) |
| **Joint 类型** | **5**(P2P / Hinge / Slider / ConeTwist / 6DOF)|
| **Joint 方法** | **17** |
| **Character 方法** | **16** |
| C++ 行 | ~2000(`light_physics3d.cpp`) |
| Smoke 断言 | 70+ |
| 6 平台全绿 CI 次数 | **4** |

---

## 7. 关键设计决策(Phase AU 总）

1. **`#if CHOCO_HAS_BULLET` 分两路**:Bullet 缺失时所有工厂返回 `nil + err`,模块仍可 `require`,smoke 可优雅 skip。
2. **`btScalar = float`**:不定义 `BT_USE_DOUBLE_PRECISION`(`#if defined()` 陷阱),与我们 `float*` 顶点 / mesh 数据匹配。
3. **userdata 三件套(owner/selfRef/typeRef)**:Body / Joint / Character 各持 World owner 指针 + Lua registry ref,确保任意 GC 顺序都不悬空。
4. **vector 双向索引**:World 持 `std::vector<Body3D*>` / `<Joint3D*>` / `<Character3D*>`;销毁时按依赖反序(joints/characters → bodies → ghostPairCb)。
5. **Bullet user pointer 反查**:`btRigidBody::setUserPointer(Body3D*)`,RayCast/OnContact 拿到 `btCollisionObject*` 后反推 Lua object。
6. **Shape 持有数据所有权**:Heightfield / TriangleMesh 必须的 `heightData / vertexData / indexData / indexArray` 由 `Shape3D` 持有,`__gc` 时统一释放(Bullet 不拷贝)。
7. **CharacterController 自动基础设施**:`NewWorld` 默认安装 `btGhostPairCallback`,用户无需手动配置。
