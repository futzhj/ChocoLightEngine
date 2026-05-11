# CONSENSUS — Phase D ECS 渲染系统

> **6A 工作流 · Stage 1 Align (收尾) → Stage 2 Architect (入口)**
> 固化用户所有决策, 锁定需求边界 + 技术方案. 后续 DESIGN/TASK/IMPL 不再改动这些前提.

---

## 1. 决策固化表

| 决策点 | 选项 | 状态 |
|-------|------|------|
| **Q1 component 来源** | 引擎内置 6 个核心 component (`Transform2D`/`Transform3D`/`Sprite`/`MeshRenderer`/`Camera2D`/`Camera3D`) | ✓ 固化 |
| **Q2 接入方式** | 用户在 `window.Draw` / `window.Update` 中显式调用 `world:Render(cam)` 与 `world:Update(dt)` | ✓ 固化 |
| **Q3 parent 层级** | 不支持 (扁平 transform), parent 留 Phase D.x.1 | ✓ 固化 |
| **Q4 维度** | 2D + 3D 全套, 一次交付 | ✓ 固化 |
| **Q5 networked 默认** | 渲染 component 默认 `networked = false`, 多人用户显式覆盖 | ✓ 固化 |
| **Q6 绘制顺序** | `Transform2D.z`/`Transform3D.z` 升序排序 (画家算法) | ✓ 固化 |
| **Q7 dt 调度** | 用户手动 `world:Update(dt)`, 与 Q2 一致 | ✓ 固化 |
| **Q8 阶段拆分** | 显式 `world:Render(camera2d, camera3d)` 一行调用 | ✓ 固化 |
| **Q9 Camera 模型** | Camera 作为 ECS component, `active=true` 标签生效 | ✓ 固化 |
| **Q10 Sprite schema** | `{image, color={r,g,b,a}, visible, anchor={ax,ay}, flipX, flipY, quad={qx,qy,qw,qh}}` | ✓ 固化 |

---

## 2. 最终需求描述

### 2.1 用户故事 (User Story)

**作为** 一个游戏开发者
**我希望** 用 ECS 范式声明 entity + component, 不写手动遍历代码就能渲染
**以便于** 把渲染逻辑与游戏逻辑解耦, 享受 ECS 数据驱动的好处, 同时与 Phase C 网络层无缝贯通

### 2.2 期望用户代码 (验收用例)

```lua
-- 0. 初始化
local Light = require('Light.UI'); require('Light.ECS')
local window = Light(Light.UI.Window):New({title='ECS Render', width=800, height=600})
local world  = Light.ECS.World.new()

-- 1. 加载资源
local heroImg = Light(Light.Graphics.Image):New('hero.png')

-- 2. 创建相机 entity (active=true 标志当前激活相机)
local cam = world:CreateEntity()
cam:Add('Transform2D', {x=0, y=0})
cam:Add('Camera2D', {active=true, zoom=1.0, viewportW=800, viewportH=600})

-- 3. 创建 sprite entity
local hero = world:CreateEntity()
hero:Add('Transform2D', {x=100, y=200, rot=0, sx=1, sy=1, z=10})
hero:Add('Sprite', {image=heroImg, color={r=1,g=1,b=1,a=1}, visible=true,
                    anchor={ax=0.5, ay=0.5}, flipX=false, flipY=false})

-- 4. 注册用户逻辑 system (与现有 ECS API 兼容, 无破坏)
world:RegisterSystem('Movement', {'Transform2D'}, function(ent, dt)
    local t = ent:Get('Transform2D')
    ent:Set('Transform2D', 'x', t.x + 50 * dt)
end)

-- 5. 主循环 (用户显式两行调用)
window.Update = function(_, dt) world:Update(dt)  end  -- 跑用户 system
window.Draw   = function(_)     world:Render()    end  -- 跑内置渲染 system
window()                                                -- 启动 game loop
```

### 2.3 验收标准

| 编号 | 描述 | 验证方式 |
|------|------|----------|
| D-AC1 | 6 个内置 component 已注册, 可 `Add` / `Set` / `Get` | smoke 断言 |
| D-AC2 | 不调 `world:Render` 时, 不调任何 `Light.Graphics.Draw` (验证 mock graphics call 次数为 0) | smoke 断言 |
| D-AC3 | 调 `world:Render` 时, 对每个 (Transform2D + Sprite) 命中的 entity 调一次 `Light.Graphics.Draw` | mock 断言 |
| D-AC4 | 当 Sprite.visible=false 时, 不调 `Light.Graphics.Draw` | smoke 断言 |
| D-AC5 | 多 sprite 按 `Transform2D.z` 升序绘制 (画家算法) | mock call 顺序断言 |
| D-AC6 | 3D entity (Transform3D + MeshRenderer + Camera3D.active=true) 时, 调 `mesh:Draw(material)` 且 `SetCamera` 正确 | smoke + 真实窗口手动验证 |
| D-AC7 | Sprite.anchor 影响绘制位置 (anchor={0.5,0.5} 时 entity 中心对齐 Transform 坐标) | 视觉对比 demo |
| D-AC8 | 渲染 component 默认 networked=false; 多人模式时用户调 `world:MarkRenderNetworked()` 后可同步 | smoke 验证 dirty 跟踪不触发 |
| D-AC9 | demo `demo_ecs_render` 一个 server + 一个 client 时, server 移动的 hero 在 client 上跟随移动 | 端到端手动验证 |
| D-AC10 | Phase C/C.x.1 现有 API (`RegisterComponent` / `World.new` / `entity:Add/Set/Get` / `NetworkSync` / `MirrorFromRoom`) 行为不变 | 现有 smoke 全过 |
| D-AC11 | CI 6 平台全绿 | GitHub Actions |

---

## 3. 技术实现方案 (Architect 入口)

### 3.1 内置 component schema

```lua
-- 2D
Transform2D : { x, y, z, rot, sx, sy, ox, oy }
              -- z 用于画家算法 sort; ox/oy 为变换原点 (绕谁旋转)
Sprite       : { image, color={r,g,b,a}, visible, anchor={ax,ay}, flipX, flipY, quad={qx,qy,qw,qh} }
              -- image 是 Light.Graphics.Image userdata; quad={0,0,0,0} 表示画整张图
Camera2D     : { active, zoom, viewportW, viewportH }
              -- 配合 Transform2D 决定相机位置/旋转

-- 3D
Transform3D    : { x, y, z, rx, ry, rz, sx, sy, sz }
                 -- 简化版欧拉角 (Phase D.x 可换四元数)
MeshRenderer   : { mesh, material, visible }
                 -- mesh 是 Light.Graphics.Mesh userdata, material 是 Material userdata
Camera3D       : { active, fovY, aspect, nearZ, farZ, targetX, targetY, targetZ, upX, upY, upZ }
                 -- 配合 Transform3D 决定相机位置, target/up 决定看向
```

**Networked 选择**: 全部默认 `networked = false`. 用户可:
1. 在 `world:NetworkSync(room)` 之前调 `world:MarkRenderNetworked()` 一次性把所有内置渲染 component 重新注册为 networked
2. 或自己手动 `Light.ECS.RegisterComponent('Transform2D', {...}, {networked=true})` 提前覆盖

### 3.2 内置渲染 system

引擎在 `World.new()` 内部自动注册 4 个 render system (不进入 `_user_systems` 列表, 不在 `world:Update` 中跑):

| System 名 | required comps | 执行内容 |
|-----------|----------------|----------|
| `_Render_Camera2D_Setup` | `Camera2D + Transform2D` | 找到 `active=true` 的 entity, 设当前 2D 相机视图 (调 `Light.Graphics.PushMatrix + Translate(-camX, -camY) + Scale(zoom)`) |
| `_Render_Sprite` | `Transform2D + Sprite` (`visible=true`) | 按 `Transform2D.z` 排序后, 对每个 entity 调 `Light.Graphics.Push/Translate/Rotate/Scale/SetColor/DrawSprite` |
| `_Render_Camera3D_Setup` | `Camera3D + Transform3D` | 找 active 相机, 调 `Light.Graphics.SetPerspective + SetCamera` 配置 3D 视图 |
| `_Render_Mesh` | `Transform3D + MeshRenderer` (`visible=true`) | 对每个 entity 调 `Light.Graphics.PushMatrix + Translate/Rotate/Scale + mesh:Draw(material) + PopMatrix` |

### 3.3 world:Render API

```lua
function ECSWorld:Render()
    -- 1. 跑 _Render_Camera2D_Setup (若有 Camera2D entity)
    -- 2. 跑 _Render_Sprite (z-sort + draw)
    -- 3. Pop 2D camera transform
    -- 4. 跑 _Render_Camera3D_Setup (若有 Camera3D entity)
    -- 5. 跑 _Render_Mesh
    -- 6. Pop 3D depth / camera state
end
```

### 3.4 world:MarkRenderNetworked API

```lua
function ECSWorld:MarkRenderNetworked()
    -- 把 Transform2D/Transform3D/Sprite/MeshRenderer/Camera2D/Camera3D 6 个 component
    -- 在 _registered_components 中重新标记为 networked = true
    -- 注意: 必须在 NetworkSync(room) 之前调用, 否则 dirty 跟踪不会启动
end
```

### 3.5 与现有 Phase C/C.x.1 API 兼容性

- `RegisterComponent / World.new / entity:Add/Set/Get/Remove / world:RegisterSystem / Update` 全部签名不变
- `NetworkSync(room) / MarkFullResync / MirrorFromRoom` 不变
- 新增 (非破坏): `world:Render([cam2dEntityId, cam3dEntityId])`, `world:MarkRenderNetworked()`
- 内置 component schema 与已有用户 component 命名空间不冲突 (用户若已有 `Transform2D` 同名 component, 引擎检测到 `_registered_components.Transform2D` 已存在则跳过内置注册并发警告)

---

## 4. 不变性 (Invariants)

- 不引入新外部依赖
- 不破坏 Phase C v1 / C.x.1 任何 API
- 不破坏 `Light.Graphics.*` 任何 API
- MSVC raw string 拼接遵循 Phase C.x.1 规避方案 (`)LUA" R"LUA(` 单行)
- 修改集中在: `ChocoLight/src/light_ecs.cpp` 内嵌 Lua + `samples/demo_ecs_render/` 新增 + `scripts/smoke/ecs_render.lua` 新增

---

## 5. 范围 (Scope)

### 5.1 In Scope

- 内置 6 个 component
- 内置 4 个 render system
- `world:Render`, `world:MarkRenderNetworked` 2 个新 API
- `samples/demo_ecs_render` 端到端示例 + README
- `scripts/smoke/ecs_render.lua` mock graphics smoke 测试
- 6A 文档全套

### 5.2 Out of Scope (留 Phase D.x.N)

- ❌ parent/scene graph (D.x.1)
- ❌ ECS PointLight component (D.x.2)
- ❌ Material 作为 networked component + GPU 资源 handle 同步 (D.x.3)
- ❌ SkeletalAnim 接入 ECS (D.x.4)
- ❌ 视锥剔除 / 八叉树 / GPU instancing (D.x.5)
- ❌ ECS Particle / Tilemap / Post-process

---

## 6. 估时

| 任务簇 | 估时 | 备注 |
|--------|------|------|
| component 注册 + schema 校验 | 1h | 6 个 component |
| `_Render_Sprite` system + z-sort | 1.5h | 含 anchor/flip/quad |
| `_Render_Camera2D_Setup` | 0.5h | 简单视图变换 |
| `_Render_Mesh` system | 1h | mesh:Draw 调用包装 |
| `_Render_Camera3D_Setup` | 0.5h | SetPerspective + SetCamera |
| `world:Render` + `MarkRenderNetworked` | 0.5h | 顶层调度 |
| smoke 测试 (mock Light.Graphics) | 1.5h | 35+ 断言 |
| `demo_ecs_render` + README | 1.5h | 2D 多 sprite + 用户输入 + 单机 |
| `demo_ecs_render_network` server/client | 1.5h | 联合 Phase C.x.1 demo 改造 |
| 6A 文档全套 (Stage 6 Assess) | 1h | ACCEPTANCE/FINAL/TODO |
| **总计** | **~10h** | 落在 8-12h 估算区间 |

---

## 7. 文档版本

| 版本 | 日期 | 备注 |
|------|------|------|
| 1.0 | 2026-05-11 | Stage 1 Align 决策固化版, 共 10 个决策点全部锁定 |
