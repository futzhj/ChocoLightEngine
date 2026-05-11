# ALIGNMENT — Phase D.x.4 SkinnedMesh ECS 化

> **6A 工作流 · Stage 1 Align**
> 把 Phase AW (`Light.Animation` 骨骼动画) 接入 Phase D (`Light.ECS` 渲染) 集成层.

## 1. 项目上下文

### 1.1 Phase AW 现状 (`@e:\jinyiNew\Light\ChocoLight\src\light_animation.cpp`)

**4 个 userdata 类型** 已实现 + 全局函数 4 个 + 子模块 4 个:

| API | 类型 | 关键方法 |
|-----|------|----------|
| `Skeleton` | userdata | `GetJointCount/GetJointName/FindJoint/GetJointParent/GetInverseBindMatrix` + setter |
| `Clip` | userdata | `GetName/GetDuration/GetSamplerCount/Sample` + setter |
| `Animator` | userdata | `Update(dt) / Play(name) / Crossfade(name, dur) / AddState / AddTransition / AddEvent / SetParam / SetMorphWeight` + 35 方法 |
| `SkinnedMesh` | userdata | `GetVertexCount/GetSkeleton/HasMorphTargets/GetMorphTargetCount` |
| `Light.Animation.LoadSkinnedGLTF(path)` | 全局 | 返回 `{skeleton, clips, clipNames, mesh, meshes, hasSkin}` |
| `Light.Animation.NewAnimator(skel)` | 全局 | 创建 Animator |
| `Light.Animation.DrawSkinnedMesh(mesh, animator, modelMat, material)` | 全局 | **关键渲染入口**, 自动分流 CPU/GPU 蒙皮 |
| `Light.Animation.Set/GetSkinningMode(...)` | 全局 | "auto" / "cpu" / "gpu" |

**特性**:
- Phase AV Step 4: 状态机 Transition / Crossfade / Event / Param 完整
- Phase AW: GPU 骨骼蒙皮 (单次顶点上传 + 每帧 jointMatrices UBO)
- Phase AX: Morph Target 支持 (含 GPU SkinMorph shader)
- Phase AY: morph-only fallback + 智能 LoadSkinnedGLTF

### 1.2 Phase D 现状 (`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:519-700`)

**6 个内置渲染 component** + 1 个 `world:Render()` 调度入口:

| Component | 字段 | 用途 |
|-----------|------|------|
| Transform2D | `x, y, z, rot, sx, sy, ox, oy` | 2D 实体位姿 |
| Sprite | `image, color, anchor, flipX/Y, quad, visible` | 2D 渲染 |
| Camera2D | `active, zoom, viewportW/H` | 2D 视图 |
| Transform3D | `x, y, z, rx/ry/rz, sx/sy/sz` | 3D 实体位姿 |
| **MeshRenderer** | `visible, mesh, material` (用户 Add 时传) | **3D 渲染** → 调 `mesh:Draw(material)` |
| Camera3D | `active, fovY, aspect, nearZ, farZ, target/up` | 3D 视图 |

**Render 流程** (`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:643`):

```
world:Render()
  ├ 2D camera setup (Push/Scale/Translate(-cam))
  ├ _CollectSprites + z-sort + _DrawSprite (循环)
  ├ Pop camera
  ├ 3D camera setup (SetPerspective + SetCamera + SetDepthTest)
  ├ 遍历 _entities + _DrawMesh (Transform3D + MeshRenderer.mesh)
  └ SetDepthTest(false)
```

### 1.3 现有 Demo

- `@e:\jinyiNew\Light\samples\demo_animation\main.lua`: 全 API 探查 (无 ECS)
- `@e:\jinyiNew\Light\samples\demo_morph_target\main.lua`: morph weight UI 控制
- `@e:\jinyiNew\Light\samples\demo_skinning_perf\main.lua`: GPU/CPU 蒙皮性能基准
- **缺 ECS × 骨骼动画端到端 demo**

---

## 2. 原始需求

把 ECS 渲染框架 (Phase D) 与骨骼动画 (Phase AW) 桥接, 让用户用 ECS 范式声明角色实体, 不写手动遍历就能播放/切换动画 + 蒙皮渲染. 是 3D 角色游戏的核心刚需.

## 3. 边界确认

### 3.1 In Scope (本 Phase 必做)

- [x] 新增 1 个内置 component: **`SkinnedMeshRenderer`** (mesh + animator + material + visible + skinningMode 字段)
- [x] 新增 1 个内置 component: **`AnimationState`** (state / speed / paused / params / morphOverride 字段)
- [x] 在 `world:Render` 3D 阶段加 `_DrawSkinnedMesh` 调度 (在 mesh 后, 调 `Light.Animation.DrawSkinnedMesh`)
- [x] 在 `world:Update` 中加 anim system 自动推进 `animator:Update(dt)`
- [x] AnimationState 字段改变 (例: state 字符串) 自动桥接到 `animator:Play / animator:Crossfade`
- [x] `MarkRenderNetworked` 扩展: 把 AnimationState 也标 networked (mesh/animator userdata 不同步, 仅状态数据)
- [x] smoke test 覆盖核心调用 + mock `Light.Animation.DrawSkinnedMesh`
- [x] demo `demo_ecs_skinned`: 5 个角色 entity 各自播不同 clip

### 3.2 Out of Scope (本 Phase 不做)

- ❌ 资源加载抽象 (用户自己 `LoadSkinnedGLTF`, 把 pack 传给 component)
- ❌ parent 层级 (留 Phase D.x.1)
- ❌ morph weight 网络化同步 (mesh handle 不可序列化, 数据量大)
- ❌ event callback ECS 转发 (动画 event 仍用 `animator:AddEvent`, 不接入 ECS event bus)
- ❌ 多 Animator 共享 Skeleton (1:1 关系简化设计)

---

## 4. 需求理解 (对现有项目的理解)

### 4.1 集成切入点

```
ECSWorld:_RegisterBuiltinRenderComponents()
    └ 增加 SkinnedMeshRenderer + AnimationState 注册

ECSWorld:Update(dt)
    └ 跑用户 system (现有) 后, 加 _AnimationSystem 自动调 animator:Update(dt)
    └ 检测 AnimationState 字段变化 → 桥接 Play/Crossfade

ECSWorld:Render() / _DrawSkinnedMesh(tf, smr, gfx)
    └ 3D 阶段在 _DrawMesh 后加 SkinnedMesh 调度
    └ 调 Light.Animation.DrawSkinnedMesh(smr.mesh, smr.animator, modelMat, smr.material)
```

### 4.2 数据流

```
用户脚本:
    local pack = Light.Animation.LoadSkinnedGLTF('hero.glb')
    local animator = Light.Animation.NewAnimator(pack.skeleton)
    animator:AddState('Idle', pack.clips.Idle)
    animator:AddState('Run',  pack.clips.Run)

    local e = world:CreateEntity()
    e:Add('Transform3D', {x=0, y=0, z=0})
    e:Add('SkinnedMeshRenderer', {mesh=pack.mesh, animator=animator, material=mat})
    e:Add('AnimationState',      {state='Idle', looping=true})

    -- 切换 (data-driven, 自动 Crossfade)
    e:Set('AnimationState', {state='Run', crossfade=0.3})

    -- 每帧
    function Game:Update(dt) world:Update(dt) end  -- 内置 _AnimationSystem 推进 animator
    function Game:Draw()     world:Render() end    -- 内置 _DrawSkinnedMesh 渲染
```

### 4.3 网络化语义

- **mesh / animator userdata 不同步** (本地资源)
- **AnimationState 数据同步** (state/speed/paused/params) — server 改 state, client mirror 看到后**用本地 animator 跑相同 state**
- **要求 client 端预先创建 Animator + AddState** (在 join 前) — 让 mirror 收到 state="Run" 时能 `animator:Crossfade("Run", 0.3)`

---

## 5. 疑问澄清 (待用户决策)

### Q1 - SkinnedMeshRenderer 独立 vs 扩展 MeshRenderer?

- **A**: 独立 `SkinnedMeshRenderer` component (清晰职责, 与 MeshRenderer 平级)
- **B**: 扩展 MeshRenderer 加 animator 字段 (代码复用, 但分发逻辑复杂: 普通 mesh `:Draw()` vs SkinnedMesh `Anim.DrawSkinnedMesh(mesh, animator, ...)` 不同入口)

**推荐 A**.

### Q2 - AnimationState 字段范围?

- **基础版** (本 Phase): `state, looping, speed, paused, time, crossfade`
- **进阶版** (留 Phase D.x.4.1): 含 `params={}, morphOverride={}`

**推荐基础版**.

### Q3 - state 字段桥接到 Play vs Crossfade?

- **A**: 用户在 Set 时传 `crossfade` 字段, 默认 `0`(立即 Play), `>0` 走 Crossfade
- **B**: 总是 Crossfade(默认 0.3s)

**推荐 A** (用户可控).

### Q4 - 网络化范围?

- **A**: 仅同步 AnimationState 数据 (state/speed/paused) — client 端用本地 animator 跑
- **B**: 不同步, 留给用户自己写
- **C**: 同步 + params 透传 (state machine 完整状态)

**推荐 A** (核心刚需, 复杂度可控).

### Q5 - 工作量 ?

- **基础版 (4-6h)**: 2 component + auto system + render 接入 + smoke + 1 demo
- **进阶版 (8-10h)**: 基础 + params 透传 + event ECS 桥 + morph weight ECS 控制

**推荐基础版**, 进阶留 Phase D.x.4.1.

### Q6 - demo 是否需要 glTF 资源?

- **A**: 用户自备 hero.glb (放 `samples/demo_ecs_skinned/`); demo fallback 时仅打印 API 验证 (类 demo_animation)
- **B**: 引擎程序化生成 trivial skeleton + Skin (用 `NewEmptySkeleton + Skeleton:SetJointParent / Clip:AddSampler`) — 复杂

**推荐 A** (与 demo_animation 同模式).

---

## 6. 任务边界限制 (强约束)

1. **不改 `Light.Animation` API** — 仅 ECS 层加 wrapper
2. **不破坏 Phase D 现有 6 个内置 component** — 仅追加 2 个新 component
3. **不破坏 Phase C/C.x.1 网络同步** — `MarkRenderNetworked` 兼容旧行为, 仅追加新内置
4. **MSVC raw string 16KB 边界** — 嵌入 Lua 总长每段 < 14KB (安全阈值), 必要时新增 raw string 拼接点
5. **Lua 5.1 严格语法** — `table.unpack` 一律走 `(table.unpack or unpack)`

---

## 7. 验收标准 (草稿, CONSENSUS 阶段定稿)

| 编号 | 描述 | 验证手段 |
|------|------|----------|
| Dx4-AC1 | 2 个内置 component 自动注册 (SkinnedMeshRenderer + AnimationState) | smoke 检查 `w._components`/`w._builtin_render_comps` |
| Dx4-AC2 | world:Update 自动跑 `animator:Update(dt)` 推进时间 | smoke mock animator, 调用前后比较 `currentTime` |
| Dx4-AC3 | AnimationState.state 改变自动触发 Play/Crossfade | smoke mock animator, 验证 `Play/Crossfade` 被调用 |
| Dx4-AC4 | world:Render 调 `Light.Animation.DrawSkinnedMesh` 每个命中 entity 一次 | smoke mock 全局函数, 验证调用次数和参数 |
| Dx4-AC5 | visible=false 时不调 DrawSkinnedMesh | smoke |
| Dx4-AC6 | MarkRenderNetworked 后 AnimationState 标 networked, SkinnedMeshRenderer **不**标 (userdata 不同步) | smoke |
| Dx4-AC7 | demo `demo_ecs_skinned` 跑通 (有 hero.glb 时显示 5 个角色; 无资源时 fallback 验证 API) | 用户本地手工跑 |
| Dx4-AC8 | Phase D/Phase AW 现有 smoke 全过 (`ecs_render.lua`, `image_from_bytes.lua`) | smoke 回归 |
| Dx4-AC9 | CI 6/6 平台 success | gh run view |

---

## 8. 后续 Phase 关联

- **Phase D.x.4.1** (进阶, ~4h): params 网络化 + event ECS bus + morph weight ECS 控制
- **Phase D.x.4.2** (~3h): 多 Animator 共享 Skeleton (角色 LOD / 实例化)
- **Phase E** (~6h): ECS 序列化 + replay (含 AnimationState 时间倒流)

---

## 9. 文档版本

| 版本 | 日期 | 备注 |
|------|------|------|
| 1.0 | 2026-05-11 | Phase D.x.4 Stage 1 Align 初稿, 待用户决策 6 个 Q |
