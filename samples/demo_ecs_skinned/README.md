# demo_ecs_skinned

Phase D.x.4 示例: 演示 **ECS × 骨骼动画** 集成 (Phase D × Phase AW 桥接).

## 功能

- 加载 glTF (`pack.skeleton + pack.clips + pack.mesh`)
- 创建 5 个角色 entity, 每个挂 `Transform3D + SkinnedMeshRenderer + AnimationState`
- `world:Update(dt)` 内置 `_AnimationSystem` 自动:
  - 推进所有 `animator:Update(dt)`
  - 检测 `AnimationState.state` 变化 → `animator:Play / Crossfade`
  - 检测 `params / morphWeights / speed / paused` 变化 → 对应桥接
- `world:Render()` 内置 `_DrawSkinnedMesh` 调度自动调 `Light.Animation.DrawSkinnedMesh`
- 每 2 秒在所有角色间循环切换 clip (用 0.3s crossfade)
- 无资源时自动降级到 API-only 验证模式 (CI runtime 友好)

## 用法

```bash
# 主路径 (需 hero.glb / character.glb)
light.exe samples/demo_ecs_skinned/main.lua           # 无限运行, ESC 退出
light.exe samples/demo_ecs_skinned/main.lua 5         # 5 秒自动退出

# Headless 验证 (CI / 无 GPU)
light.exe samples/demo_ecs_skinned/main.lua --headless
```

## 资源放置

glTF 文件按优先级搜索:
1. `samples/demo_ecs_skinned/hero.glb` (推荐)
2. `samples/demo_animation/assets/character.glb`
3. `samples/demo_animation/character.glb`
4. `assets/character.glb`

任意 glTF 2.0 含 skin 的角色模型均可 (如 RiggedFigure / CesiumMan / Fox).

## 关键代码

```lua
-- 加载资源 (一次)
local pack = Anim.LoadSkinnedGLTF('hero.glb')

-- 工厂: 每个 entity 独立 animator (clip 状态独立)
local function makeAnimator()
    local an = Anim.NewAnimator(pack.skeleton)
    for _, name in ipairs(pack.clipNames) do
        an:AddState(name, pack.clips[name])
    end
    return an
end

-- ECS 实体创建
local e = world:CreateEntity()
e:Add('Transform3D', {x=0, y=0, z=0, ry=90})
e:Add('SkinnedMeshRenderer', {mesh=pack.mesh, animator=makeAnimator()})
e:Add('AnimationState',      {state='Idle', speed=1.0})

-- 每帧 (零样板)
function Game:Update(dt) world:Update(dt) end  -- 自动: animator:Update + state 桥接
function Game:Draw()     world:Render() end    -- 自动: DrawSkinnedMesh

-- 切换 (data-driven, 自动 Crossfade)
e:Set('AnimationState', {state='Run', crossfade=0.3})

-- 参数透传 (例: blend tree 控制)
e:Set('AnimationState', {params={isMoving=1.0, walkSpeed=2.5}})

-- 手动 morph (面部表情)
e:Set('AnimationState', {morphWeights={[1]=0.8, [2]=0.2}})
```

## 内置 AnimationState 字段

| 字段 | 类型 | 默认 | 说明 |
|------|------|------|------|
| `state` | string | `''` | 状态名 (要求 animator 已 AddState) |
| `crossfade` | number | `0` | `>0` → Crossfade(dur); `0` → Play |
| `speed` | number | `1.0` | 播放速率 |
| `paused` | bool | `false` | 暂停 |
| `looping` | bool | `true` | (字段保留, 桥接到 animator:SetLooping 待 Phase D.x.4.1) |
| `params` | table | `{}` | `{[name]=number}` → animator:SetParam |
| `morphWeights` | table | `{}` | `{[idx]=weight}` 1-based → animator:SetMorphWeight |
| `time` | number | `0` | (字段保留, server-authoritative 同步用) |

## 网络化

- `SkinnedMeshRenderer.mesh / animator` 是 **userdata**, 不可序列化, **不同步**
- `AnimationState` 全字段 **同步** (server `Set` → client mirror 自动应用)
- 客户端需**预先创建 animator + AddState 相同 clip 名**, 才能在收到 state 变化时本地播放对应动画

```lua
-- Server
world:MarkRenderNetworked()    -- AnimationState 自动 networked, SMR 排除
world:NetworkSync(room)
-- 改 state 自动广播到 client mirror
e:Set('AnimationState', {state='Run', crossfade=0.3})

-- Client
local mirror = ECS.MirrorFromRoom(room)
-- 客户端先自己 LoadSkinnedGLTF + 创建 animator + AddState
-- mirror 收到 AnimationState{state='Run', crossfade=0.3} 会自动调本地 animator:Crossfade
```

## 已知限制

- Transform3D 旋转: 当前 `_BuildModelMatrix3D` 仅支持 **Y 轴旋转** (`ry`), 角色游戏 80% 用例覆盖; X/Z 轴留 Phase D.x.4.x
- onAnimEvent: 字段保留, 但引擎不自动注册转发. 用户需自己 `animator:AddEvent(state, time, function(an) ... end)` 注册 (Phase D.x.4.1 加 ECS 自动桥接)
- LookAt 计算: 不内置, 用户用 `Transform3D.ry` 自行控制朝向
