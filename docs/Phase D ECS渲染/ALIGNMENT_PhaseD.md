# ALIGNMENT — Phase D ECS 渲染系统

> **6A 工作流 · Stage 1 Align**
> 目标: 模糊需求 → 精确规范. 把 Phase C/C.x.1 完成的 ECS 与 Phase AS/AW 完成的渲染层桥接, 让用户能用 ECS 范式驱动 2D/3D 渲染.

---

## 1. 项目上下文分析

### 1.1 ECS 现状 (Phase C + C.x.1 完成)

| 能力 | API | 状态 |
|------|-----|------|
| 组件注册 | `Light.ECS.RegisterComponent(name, schema, opts)` (含 `opts.networked`) | ✓ |
| 世界 | `World:CreateEntity / DestroyEntity / RegisterSystem / Update(dt)` | ✓ |
| 实体 | `entity:Add(comp,data)`, `Set(comp,field,val)`, `Get`, `Remove`, `Has`, `Destroy` | ✓ |
| 系统调度 | `world:RegisterSystem(name, requiredComps, fn)` 按帧轮询命中实体 | ✓ |
| 网络化 | `world:NetworkSync(room)`, `MarkFullResync()`, `MirrorFromRoom(room)` | ✓ Phase C.x.1 |
| 增量同步 | `ecs_delta` event 携带 `{set, del}` patch | ✓ Phase C.x.1 |

**已嵌入实现**: `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` 内嵌 Lua 脚本.

---

### 1.2 渲染层现状 (Phase AS / AW / A7 完成)

| 模块 | 角色 | 关键 API |
|------|------|----------|
| `Light.Graphics` | 2D 绘图基元 + 变换矩阵 | `Draw, DrawQuad, Rectangle, Circle, Print, Push/Pop, Translate/Rotate/Scale, SetColor, SetCamera, SetPerspective, SetDepthTest, Set*Light` |
| `Light.Graphics.Image` | 2D 纹理 (Drawable) | `Image:New(path)`, `:GetWidth/Height/Dimensions/TextureId` |
| `Light.Graphics.Mesh` | 3D mesh userdata | `Mesh.New(verts,idx)`, `Mesh.LoadGLTF(path, primIdx, withMaterial)`, `:Draw(tex_or_material)` |
| `Light.Graphics.Material` | PBR 材质 | `mode, color, metallic, roughness, normalScale, alphaMode, texBaseColor, texMetallicRoughness, texNormal, texEmissive` |
| `Light.Graphics.Font` | TTF 字体 (stb_truetype) | `Font:New(path,size)`, `Graphics.Print(text, font, x, y)` |
| `Light.Graphics.Canvas` | 离屏 FBO | `Canvas:New(w,h)`, `Graphics.SetCanvas/PushCanvas/PopCanvas` |
| `BatchRenderer` (内部) | 2D 批渲染 | 自动合并 Quad/Triangle/Line draw call, 与 `g_render` 兼容 fallback |
| `RenderBackend` (`g_render`) | GL3.3 / LegacyGL 抽象 | mesh / shader / FBO / 光源 / depth / mat4 stack |
| `Light.UI.Window` | game loop 调度 | `Window:__call` 内置 `BeginFrame → Draw → Update(dt) → EndFrame → SwapBuffers` |

---

### 1.3 现有 ECS 用户代码模式 (Phase C.x.1 demo)

```lua
-- 注册 component (schema 仅用于网络化 patch 字段, 与渲染无关)
Light.ECS.RegisterComponent('Position', {'x', 'y', 'z'}, {networked = true})
Light.ECS.RegisterComponent('Velocity', {'dx', 'dy', 'dz'})

local world = Light.ECS.World.new()
local e = world:CreateEntity()
e:Add('Position', {x = 10, y = 20, z = 0})
e:Add('Velocity', {dx = 1, dy = 0, dz = 0})

-- 注册 system (帧更新逻辑)
world:RegisterSystem('Physics', {'Position', 'Velocity'}, function(ent, dt)
    local p = ent:Get('Position')
    local v = ent:Get('Velocity')
    ent:Set('Position', 'x', p.x + v.dx * dt)
end)

-- 主循环
window.Update = function(self, dt)
    world:Update(dt)
end
```

**关键观察**:
- `world:Update(dt)` 已驱动 system 自动遍历命中 entity 跑用户逻辑
- 但渲染 (`window.Draw`) 与 ECS 完全脱钩 — 用户得手动遍历 entity 调 `Light.Graphics.Draw`
- 缺**约定**告诉用户怎么写 "渲染 system" — 是不是该在 `Update` 里画? 还是 `Draw` 里画? 还是 ECS 自带 `Render(viewport)` 阶段?

---

### 1.4 业务域与目标

**用户最终想做的事**:
```lua
-- 期望最终形态
local hero = world:CreateEntity()
hero:Add('Transform2D', {x = 100, y = 200, rot = 0, sx = 1, sy = 1})
hero:Add('Sprite',      {image = heroImg})

-- 不写 system, 引擎自动绘制 → 每帧自动 Light.Graphics.Draw(heroImg, 100, 200, 0, 1, 1)
window:Open()  -- 内部自动调 world:Render() / Update()
```

**业务价值**:
1. 降低样板代码 — 用户不再每帧手动遍历 entity 调 Graphics API
2. 与网络层贯通 — server 改 `Transform2D.x`, client mirror 自动收到 + 自动重绘
3. 与物理层贯通 — Phase AU `Light.Physics3D` 的 body world matrix 可灌到 `Transform3D` 自动驱动 mesh 位置
4. 教学示例完整 — `demo_ecs_render` 演示 100 个 entity 在屏幕上跑动 + 网络同步

---

## 2. 任务边界

### 2.1 In Scope (本 Phase 必做)

- 提供**渲染相关内置 component** — 至少 `Transform2D`, `Transform3D`, `Sprite`, `MeshRenderer`, `Camera2D` (具体集合见 Q1 决策)
- 提供**内置渲染 system** — 引擎自动注册到 `world`, 每帧执行 `Light.Graphics` 调用
- 提供**桥接 API** — 让用户用一行代码把 world / window 接起来
- 写 1 个完整端到端 demo (`demo_ecs_render`) — 显示 N 个 sprite 跑动 + 用户输入控制
- 写 smoke 测试 — mock RenderBackend 验证 component 注册和 system 执行顺序
- 更新 docs/API_REFERENCE.md (新增模块章节)
- 6A 文档全套 (ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO)

### 2.2 Out of Scope (本 Phase 不做, 留 Phase D.x)

- ❌ Scene graph (parent/child 完整层级) — 复杂度爆炸, 先做扁平 transform, parent 留 D.x.1
- ❌ Culling / 视锥剔除 / 八叉树 — 性能优化, 当前 < 1000 entity 无需
- ❌ Lighting component (PointLight as ECS component) — 与 `Light.Graphics.AddPointLight` 直调重复, 留 D.x.2
- ❌ Material component (作为 ECS networked component) — Material handle 是 GPU 资源不可序列化, 留 D.x.3 设计资源 handle 同步
- ❌ Animation component (`Light.SkeletalAnim` ECS 化) — Phase AW 骨骼动画已经独立, 留 D.x.4 桥接
- ❌ Particle system, Tilemap, Post-process — 都是高级渲染, 留后续

### 2.3 兼容性约束

- 不破坏 Phase C v1 / C.x.1 现有 API. `RegisterComponent / CreateEntity / RegisterSystem / Update / NetworkSync` 全部签名不变
- 不破坏 `Light.Graphics.*` 任何现有 API
- 内置 system 必须可被用户跳过/替换 — 不要强制路径

---

## 3. 关键决策点 (待确认)

### Q1 — 渲染 component 集合: 引擎内置 vs 完全用户定义

**选项**:
- **A** (推荐): 引擎内置 `Transform2D`, `Transform3D`, `Sprite`, `MeshRenderer`, `Camera2D`, `Camera3D`. 用户 0 配置上手. 缺点: 命名/字段 schema 锁死, 改 schema 需 Phase D.x
- **B**: 完全用户定义, 引擎只提供"如何写渲染 system"文档. 灵活但需大量样板代码
- **C** (混合): 引擎提供**抽象接口** `Renderable` (含 `:Draw(graphics)` 方法), 用户用任意 component 名实现接口

**初步倾向**: A (引擎内置). 与"Phase D 是业务功能核心 = 降低样板代码"目标对齐, 用户不需要学 ECS 渲染设计.

---

### Q2 — 渲染 system 注册方式

**选项**:
- **A** (推荐): 引擎在 `world:NetworkSync(room)` 类似的位置, 提供 `world:AttachRenderer(window)` 自动注册 `Sprite/Render` system 到 world. 用户只调一行代码
- **B**: 用户自己用 `world:RegisterSystem('Sprite_Render', {'Transform2D','Sprite'}, fn)` 注册. 引擎仅提供文档说明
- **C**: 提供 `Light.ECS.RenderSystem` 工厂类, 用户实例化后挂到 world

**初步倾向**: A (一行接入). 与 Q1 配套.

---

### Q3 — Transform 父子层级

**选项**:
- **A** (推荐): 不支持, 每个 entity 独立 world transform. 简化实现, 满足 80% 场景
- **B**: 支持 `parent` component (单亲), 渲染 system 中递归计算世界矩阵. 增加复杂度但常用
- **C**: 完整 scene graph (含 dirty propagation). 出本 Phase 范围, 留 Phase D.x.1

**初步倾向**: A (扁平). 留 D.x.1 加 parent.

---

### Q4 — 2D vs 3D 同时支持

**选项**:
- **A**: 一次同时做 (Sprite + MeshRenderer + Camera2D + Camera3D). 工作量大但完整
- **B** (推荐): 先 2D (Transform2D + Sprite + Camera2D, ~4-5h), 3D 留 Phase D.x.1
- **C**: 先 3D, 2D 留 D.x.1 (与现有 demo 不一致, 不推荐)

**初步倾向**: B (先 2D). 用户最常见需求是 sprite 游戏, 验证 ECS 渲染设计后再补 3D 风险低.

---

### Q5 — 渲染 component 是否默认 networked

**选项**:
- **A**: `Transform2D`/`Sprite` 默认 `networked=true`. 多人游戏开箱即用
- **B** (推荐): 默认 `networked=false`. 单机不开销网络, 多人时用户显式 `RegisterComponent('Transform2D', ..., {networked=true})` 覆盖
- **C** (折中): 内置注册时不带 networked 标记, 用户在 `world:NetworkSync(room)` 后调 `world:MarkRenderNetworked()` 一次性把内置渲染 comp 都标 networked

**初步倾向**: B. 单机用户最多, 默认不收费.

**风险**: 如果用户在多人项目里忘记标 networked, 渲染数据不同步会沉默 bug. 需 demo + 文档强调.

---

### Q6 — 绘制顺序 / Z-sort

**选项**:
- **A**: 按 entity 创建顺序绘制 (最简)
- **B** (推荐): `Transform2D` 含 `z` 字段, 渲染 system sort by z 升序 (画家算法). 满足 sprite layer 需求
- **C**: 引入独立 `RenderLayer` component (一个枚举字段). 比 B 更显式但多一个 component

**初步倾向**: B. `Transform2D.z` 一物多用 (3D 时直接是世界 z), API 简洁.

---

### Q7 — dt 调度: ECS world:Update 接 Window:Update?

**选项**:
- **A**: 用户在 `window:Update(dt)` 内手动调 `world:Update(dt)`. 已是 Phase C demo 现状
- **B**: 引擎提供 `world:AttachToWindow(window)` 自动 hook `window.Update` + `window.Draw` 回调
- **C** (推荐, 配合 Q2): `world:AttachRenderer(window)` 同时绑定 update 和 render. 一行搞定

**初步倾向**: C. 与 Q2 同函数, 减少 API 表面.

---

### Q8 — 内置渲染 system 执行阶段: Update vs Draw

**问题**: `Light.UI.Window` 的 game loop 是 `BeginFrame → Draw → Update(dt) → EndFrame`. 渲染 system 应该在哪阶段触发?

**选项**:
- **A** (推荐): Draw 阶段. 与 `Light.Graphics.Draw` 调用语义一致, 用户写 `window.Draw = function() world:Render(camera) end`
- **B**: Update 阶段, 把 `Light.Graphics` 调用混进 Update. 违反 game loop 约定, 可能与 user Draw 冲突
- **C**: 引擎拆 ECS system 类型为 `update_systems` + `render_systems`, `world:Update` 跑 update, `world:Render` 跑 render. 显式分离

**初步倾向**: C (引入 `world:Render(camera)` 显式调用). user 控制力强, 也方便加 camera 参数.

---

### Q9 — Camera 抽象: ECS component vs 全局单例

**选项**:
- **A** (推荐): `Camera2D` 作为内置 component 挂在任意 entity 上, render system 找带 `Camera2D` 标签 + `Transform2D` 的 entity 作为当前相机. 用户切相机就改 `active=true` 字段
- **B**: 全局单例, `Light.ECS.SetActiveCamera(entityId)`. 简单但不 ECS 风格
- **C**: `world:Render(cameraParams)` 接受裸参数 (x,y,zoom), 完全不用 Camera component

**初步倾向**: A. 符合 ECS 哲学, 用户可同时有多个相机 entity (split-screen 场景).

---

### Q10 — Sprite component schema

**选项**:
- **A** (推荐 — 最小):
  ```
  Sprite { image, color={r,g,b,a}, visible=true }
  ```
  image 是 `Light.Graphics.Image` userdata, 网络化时只发 path? 还是不同步? (见 Q5)

- **B** (扩展):
  ```
  Sprite { image, color, visible, anchor={ax,ay}, flipX, flipY, quad={qx,qy,qw,qh} }
  ```
  支持 anchor 和 sprite sheet 裁切

- **C** (最大):
  ```
  Sprite { image, color, visible, anchor, flipX, flipY, quad, shader, customDraw }
  ```
  支持自定义 shader/绘制函数, 复杂但灵活

**初步倾向**: B. anchor 和 flipX 是 2D sprite 80% 用例, 加它们成本小.

---

## 4. 疑问澄清 (待用户决策)

按 6A 工作流 Stage 1, 决策点优先级排序:

| 优先级 | 决策 | 影响范围 |
|--------|------|----------|
| **P0** | Q4: 2D / 3D 范围 | 总工作量 4-5h vs 8-12h |
| **P0** | Q1: 引擎内置 vs 用户定义 component | 决定整个 API 走向 |
| **P1** | Q5: 渲染 component networked 默认值 | 影响 demo 写法和文档 |
| **P1** | Q8: Update / Draw / Render 阶段拆分 | 决定 API 表面 |
| **P2** | Q3 / Q6 / Q9 / Q10 | 二级实现细节, 可独立决策 |
| **P2** | Q2 / Q7 | 与 Q1/Q8 联动 |

---

## 5. 项目特性规范 (跨 Phase 共识)

- ✅ 中文注释 + 中文文档
- ✅ 6A 工作流 (本 Phase 完整 6 阶段)
- ✅ 与 Phase C v1 / C.x.1 API 兼容 (`RegisterComponent` / `World` / `entity:Add/Set/Get` 不变)
- ✅ 不引入新外部依赖
- ✅ 修改集中在 `ChocoLight/src/light_ecs.cpp` 内嵌 Lua + 1 个新 sample + 1 个新 smoke
- ✅ CI 6 平台全绿 (Windows / macOS / Linux / Android / iOS / Web)
- ✅ MSVC raw string 拼接遵循 Phase C.x.1 规避方案 (`)LUA" R"LUA(` 单行)

---

## 6. 文档版本

| 版本 | 日期 | 备注 |
|------|------|------|
| 0.1 | 2026-05-11 | 初版分析, 列出 10 个决策点供用户选择 |

---

## 7. 下一步

待用户确认 P0/P1 决策后, 进入 Stage 2 Architect, 生成 `CONSENSUS_PhaseD.md` 和 `DESIGN_PhaseD.md`.
