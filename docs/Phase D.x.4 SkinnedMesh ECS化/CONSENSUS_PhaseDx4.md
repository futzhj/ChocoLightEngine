# CONSENSUS — Phase D.x.4 SkinnedMesh ECS 化

> **6A 工作流 · Stage 1 Align 收尾**
> 用户决策: 进阶版 (一次做完 8-10h), 含 params/event/morph 完整集成.

## 1. 决策记录

| 编号 | 决策 | 选定 |
|------|------|------|
| Q1 | component 拆分 | **独立 `SkinnedMeshRenderer` + 独立 `AnimationState`** |
| Q2 | AnimationState 字段范围 | **进阶 (state, speed, paused, time, crossfade, params, morphWeights, onAnimEvent)** |
| Q3 | state 切换桥接 | **`crossfade` 字段控制**: `0` → Play, `>0` → Crossfade(dur) |
| Q4 | 网络化范围 | **同步 state/speed/paused/params** (mesh/animator userdata 本地; morphWeights 不同步以省带宽) |
| Q5 | 工作量 | **进阶版 8-10h** (含 params 透传 + event bus + morph 控制) |
| Q6 | demo 资源 | **用户自备 glTF** (无资源时 fallback 验证 API + 提示) |

## 2. 最终需求边界

### 2.1 必做 (本 Phase 验收范围)

1. **2 个内置 component**:
   - `SkinnedMeshRenderer { mesh, animator, material, visible, skinningMode }`
   - `AnimationState { state, speed, paused, time, crossfade, params, morphWeights, onAnimEvent }`
2. **`world:Update(dt)` 内置 _AnimationSystem**:
   - 自动调 `animator:Update(dt)` 推进时间
   - 检测 `AnimationState.state` 变化 → 调 `animator:Play(state)` 或 `animator:Crossfade(state, crossfade)`
   - 检测 `params` 字段变化 → 调 `animator:SetParam(k, v)`
   - 检测 `morphWeights` 字段变化 → 调 `animator:SetMorphWeight(i, v)`
   - 检测 `speed/paused` 字段变化 → 调对应 setter
   - 首次见 entity 时, 若有 `onAnimEvent` 回调, 注册到 animator (转发为 `onAnimEvent(entity, eventName)`)
3. **`world:Render()` 3D 阶段加 `_DrawSkinnedMesh` 调度**:
   - 在 `_DrawMesh` 后遍历 entity, 命中 `Transform3D + SkinnedMeshRenderer + AnimationState`
   - 调 `Light.Animation.DrawSkinnedMesh(smr.mesh, smr.animator, modelMat, smr.material)`
4. **`MarkRenderNetworked` 扩展**:
   - SkinnedMeshRenderer **不标 networked** (mesh/animator userdata 不可序列化)
   - AnimationState **标 networked** (state/speed/paused/params 同步)
   - client 端 `MirrorFromRoom` 接收 AnimationState 变化 → 触发本地 animator 切换 (要求 client 预先 AddState)
5. **smoke `scripts/smoke/ecs_skinned.lua`**:
   - mock Light.Animation 各方法
   - 验证 component 注册 / Update 推进 / 字段桥接 / Render 调度 / 网络化标记
6. **demo `samples/demo_ecs_skinned/main.lua`**:
   - 主路径: `LoadSkinnedGLTF + NewAnimator + AddState` 创建 5 个角色 entity
   - 用户自备 `hero.glb` 时显示蒙皮角色; 无资源时降级到 API 验证 (类 demo_animation)

### 2.2 不做 (留后续 Phase)

- parent 层级 (Phase D.x.1)
- 引擎级资源加载抽象 / asset manager
- 多 Animator 共享 Skeleton (Phase D.x.4.2)
- ECS 序列化 (Phase E)

## 3. 关键技术约束

1. **嵌入 Lua 字节数**: 当前 g_ecsScript 21.7KB 已拆为 3 段 (6.6+8.5+6.4KB). Phase D.x.4 估增 ~5KB → 第 3 段会逼近 12KB 仍安全, 但需监控. **超 13KB 即立即增加新拼接点**.
2. **Lua 5.1 兼容**: 用 `(table.unpack or unpack)` 双兼容, 已有先例
3. **userdata 不可序列化**: `_BuildEntityState` 浅拷贝时, `Sprite.image / SkinnedMeshRenderer.mesh / animator` 等 userdata 字段在 cjson encode 时变 `nil`. 验收要求 mirror 端不依赖这些字段, 仅依赖网络化数据 (AnimationState)
4. **不破坏 Phase C/C.x.1/D 任何 API**: SkinnedMeshRenderer/AnimationState 是 **追加** 不是替换, 现有 demo 不受影响
5. **不强制资源**: Light.Animation 不可用时, demo 降级仍正常退出 Exit=0

## 4. 接口契约

### 4.1 用户侧 (final UX)

```lua
-- 加载资源 (用户自己, 一次)
local pack = Light.Animation.LoadSkinnedGLTF('samples/demo_ecs_skinned/hero.glb')
local function makeAnimator()
    local an = Light.Animation.NewAnimator(pack.skeleton)
    for name, clip in pairs(pack.clips) do an:AddState(name, clip) end
    return an
end

-- ECS 实体创建
local e = world:CreateEntity()
e:Add('Transform3D',           {x=0, y=0, z=0, ry=180})
e:Add('SkinnedMeshRenderer',   {mesh=pack.mesh, animator=makeAnimator(), material=mat})
e:Add('AnimationState',        {state='Idle', speed=1.0, looping=true,
                                 onAnimEvent=function(entity, evName)
                                     print('anim event:', evName)
                                 end})

-- 每帧 (零样板)
function Game:Update(dt) world:Update(dt) end  -- 自动: animator:Update + state 桥接
function Game:Draw()     world:Render() end    -- 自动: DrawSkinnedMesh

-- 切换状态 (data-driven)
e:Set('AnimationState', {state='Run', crossfade=0.3})

-- 参数透传 (data-driven)
e:Set('AnimationState', {params={isMoving=1.0, walkSpeed=2.5}})

-- 手动 morph weight
e:Set('AnimationState', {morphWeights={[1]=0.8, [2]=0.2}})
```

### 4.2 网络化 (server/client)

```lua
-- Server
world:MarkRenderNetworked()        -- AnimationState 自动 networked
world:NetworkSync(room)

-- Client (mirror)
local mirror = ECS.MirrorFromRoom(room)
-- 客户端需自己创建 animator + AddState 同样的 clip 名
-- mirror 收到 AnimationState{state='Run', crossfade=0.3} 后, 系统调本地 animator:Crossfade('Run', 0.3)
```

## 5. 验收标准 (定稿)

| 编号 | 描述 | 必通过 |
|------|------|--------|
| Dx4-AC1 | SkinnedMeshRenderer + AnimationState 注册到 `_components` + `_builtin_render_comps` | ✅ |
| Dx4-AC2 | world:Update 自动调 `animator:Update(dt)` 推进时间 | ✅ |
| Dx4-AC3 | AnimationState.state 改变 → `animator:Play(s)` 或 `animator:Crossfade(s, dur)` | ✅ |
| Dx4-AC4 | AnimationState.params 改变 → `animator:SetParam(k, v)` | ✅ |
| Dx4-AC5 | AnimationState.morphWeights 改变 → `animator:SetMorphWeight(i, v)` | ✅ |
| Dx4-AC6 | AnimationState.speed/paused 改变 → `animator:SetSpeed`/`Pause`/`Resume` | ✅ |
| Dx4-AC7 | onAnimEvent 回调注册一次, animator 触发 event 时调 `onAnimEvent(entity, name)` | ✅ |
| Dx4-AC8 | world:Render 命中 entity 调 `Anim.DrawSkinnedMesh` 一次 (visible=false 不调) | ✅ |
| Dx4-AC9 | MarkRenderNetworked 后 AnimationState 标 networked, SkinnedMeshRenderer **不**标 | ✅ |
| Dx4-AC10 | server 改 state, mirror 用本地 animator 切换 | ✅ |
| Dx4-AC11 | demo_ecs_skinned 跑通 (有资源时显示角色; 无资源 fallback 退出 Exit=0) | ✅ |
| Dx4-AC12 | Phase D / Phase AW 现有 smoke 全过 (`ecs_render.lua` `ecs_network.lua` `image_from_bytes.lua` `physics_3d.lua`) | ✅ |
| Dx4-AC13 | CI 6/6 平台 success | ✅ |

## 6. 文档版本

| 版本 | 日期 | 备注 |
|------|------|------|
| 1.0 | 2026-05-11 | 用户选进阶版, 决策点全锁定 |
