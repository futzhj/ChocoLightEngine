# demo_animation — Phase AV 骨骼动画 + 状态机示例

演示 `Light.Animation` 模块的完整能力链：

- glTF 2.0 加载（`Anim.LoadSkinnedGLTF`）
- Skeleton / AnimationClip 数据结构
- Animator 状态机（AddState / Play / Update）
- Step 4 完整状态机：Transition / Crossfade / Event / Param
- SkinnedMesh + DrawSkinnedMesh（CPU 蒙皮）

## 资源说明

本 demo **不内置** glTF 资源（避免仓库膨胀）。若你想看到完整的状态机+蒙皮渲染演示，需自行准备一份角色 glTF（带 skeleton + 至少 2 个 animation clip）。

### 推荐资源

| 来源 | URL | 说明 |
|------|-----|------|
| Khronos glTF samples | https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedFigure | `RiggedFigure.glb` 含简单骨骼 |
| Khronos glTF samples | https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/CesiumMan | `CesiumMan.glb` 含动画 |
| Mixamo | https://www.mixamo.com/ | 含 idle/walk/run 等 clip 集合 |

把 glb 文件放到以下任一路径（demo 会按顺序探测）：

1. `samples/demo_animation/assets/character.glb`
2. `samples/demo_animation/character.glb`
3. `assets/character.glb`
4. `Light-0.2.3/assets/character.glb`

## 资源缺失时的行为

demo **优雅降级**：

- 不会崩溃
- 不会返回非 0 退出码
- 输出明确的"未找到 glTF 资源"提示并枚举尝试过的路径
- 仍会执行 `Light.Animation` API 表面验证 + 错误路径调用

这与所有 Phase A* sample 的策略一致（`demo_physics3d` / `demo_haptic` 等），保证 6 平台 CI smoke 始终绿。

## 运行

```
light.exe samples/demo_animation/main.lua
```

或在 Windows runtime smoke 中一并跑（已注册到 `.github/workflows/build-templates.yml`）。

## 演示输出（含 glTF 时）

```
==== Light.Animation demo ====
[1] Light.Animation API: ...
[2] 加载 glTF: samples/demo_animation/character.glb
[3] Skeleton: 23 关节
    Clips: 3 (Idle, Walk, Run)
    Play: Idle (duration=2.00s)
[4] Transition: Idle --(speed>0.5)--> Walk, fade=0.3s
    speed=0.0 → state=Idle isFading=false
    speed=1.0 → state=Idle isFading=true target=Walk progress=0.06
    fade 完成后 → state=Walk isFading=false
[5] Event 触发 1 次 (期望 1, trig=1.000s of 2.000s)
[6] SkinnedMesh: 3450 顶点 / 17280 索引
    DrawSkinnedMesh: pcall_ok=true msg=true
[7] cleanup 完成 (ClearTransitions / ClearEvents / Stop)

demo_animation ok
```

## 演示输出（无 glTF 时）

```
==== Light.Animation demo ====
[1] Light.Animation API: ...
[2] 未找到 glTF 资源 (尝试过):
       - samples/demo_animation/assets/character.glb
       - samples/demo_animation/character.glb
       - assets/character.glb
       - Light-0.2.3/assets/character.glb
    将仅演示 API 表面 / 错误路径 (不影响 demo 退出码)
[3-7] 跳过状态机演示 (无 Skeleton/Clip/Mesh 资源)
       Phase AV C++ 实现仍通过, smoke 由 scripts/smoke/animation.lua 覆盖
       Anim.LoadSkinnedGLTF (期望 nil): nil err=cgltf_parse_file failed (err 6)

demo_animation ok
```

## 状态机 Cookbook (Phase AV 常见模式)

下面收集业务中常见的 4 种状态机组织模式，每段代码可直接复制到你的项目。所有模式都用 Phase AV.x procedural API（`NewEmptySkeleton` / `NewEmptyClip`）演示，接入真实 glTF 时把前面的 Skeleton/Clip 构造替换为 `LoadSkinnedGLTF` 即可。

### 模式 1: `idle ↔ walk ↔ run` 三态带权移动

用 Param 驱动：`speed < 0.1` → idle；`speed < 0.5` → walk；`speed >= 0.5` → run。

```lua
local Anim = require 'Light.Animation'

local function setup_locomotion(sk, idle_clip, walk_clip, run_clip)
    local an = Anim.NewAnimator(sk)
    an:AddState('idle', idle_clip)
    an:AddState('walk', walk_clip)
    an:AddState('run',  run_clip)

    -- 从 idle 出发
    an:SetParam('speed', 0)
    an:Play('idle')

    -- 条件函数（闭包捕获 an; 读 param）
    local function to(target, lo, hi)
        return function()
            local s = an:GetParam('speed') or 0
            local cur = an:GetCurrentState()
            if cur == target then return false end       -- 已经在目标
            return s >= lo and s < hi
        end
    end

    local FADE = 0.15
    an:AddTransition('', 'idle', to('idle', -1, 0.1), FADE)      -- Any → idle
    an:AddTransition('', 'walk', to('walk', 0.1, 0.5), FADE)
    an:AddTransition('', 'run',  to('run',  0.5, 1e9), FADE)
    return an
end

-- runtime:
-- animator:SetParam('speed', input:GetAxis())
-- animator:Update(dt)
```

### 模式 2: `Any state → death` 任意状态转换

当 HP 归零，无论当前什么动作，立即切到 death。

```lua
an:AddState('death', death_clip)

-- fromState="" 表示 Any state；无 crossfade (duration=0) 立即切
an:AddTransition('', 'death',
    function() return (an:GetParam('hp') or 1) <= 0 end,
    0.1)
```

### 模式 3: Attack with Combo（事件驱动 + 计时器参数）

三连击 attack1 → attack2 → attack3，每次按下攻击键在合适窗口内触发下一段。

```lua
-- 在关键动作帧（挥刀最用力的时刻）插入 event，触发伤害检测
an:AddEvent('attack1', 0.3, function() spawn_hitbox(dmg=10) end)
an:AddEvent('attack2', 0.25, function() spawn_hitbox(dmg=15) end)
an:AddEvent('attack3', 0.4, function() spawn_hitbox(dmg=30) end)

-- combo window: 在 attack1 的 [0.4, 0.8] 期间按攻击键可接 attack2
an:AddTransition('attack1', 'attack2', function()
    local t = an:GetCurrentTime()
    return input_just_pressed('attack') and t >= 0.4 and t <= 0.8
end, 0.1)

an:AddTransition('attack2', 'attack3', function()
    local t = an:GetCurrentTime()
    return input_just_pressed('attack') and t >= 0.3 and t <= 0.7
end, 0.1)

-- 超时回 idle
an:AddTransition('attack3', 'idle', function()
    return an:GetCurrentTime() >= an:GetActiveClip():GetDuration() - 0.1
end, 0.15)
```

### 模式 4: Ragdoll-on-death（Animator 停止 + 物理接管）

死亡事件触发后，停止 animator 并把 Bullet rigidbody 的关节控制权交给物理系统。

```lua
local Physics3D = require 'Light.Physics3D'

an:AddEvent('death', 0.5, function()   -- 死亡动画 0.5s 处（倒地瞬间）
    an:Pause()                         -- 冻结动画
    -- 切换每个关节 rigid body 的 kinematic flag → 动态
    for i = 1, sk:GetJointCount() do
        local rb = ragdoll_bodies[i]
        if rb then
            rb:SetKinematic(false)
            rb:SetLinearVelocity(0, -2, 0)    -- 小初速度防静止
        end
    end
end)

-- render frame:
-- if not an:IsPaused() then
--     an:Update(dt)
-- else
--     -- Animator 停了, 从 ragdoll 读 RB 世界变换回写 Skeleton
--     for i = 1, sk:GetJointCount() do
--         local tx,ty,tz, qw,qx,qy,qz = ragdoll_bodies[i]:GetTransform()
--         sk:SetBindLocalTRS(i, tx,ty,tz, qw,qx,qy,qz, 1,1,1)
--     end
--     an:Update(0)   -- 触发一次矩阵重算
-- end
```

> **提示**：Phase AV.x 尚未实现"直接从外部写 joint matrices"的快速路径；模式 4 走 `SetBindLocalTRS` + `Update(0)` 是过渡方案。Phase AW 计划引入 `Animator:OverrideJointLocal(idx, T, R, S)` 精细控制。

---

## 相关文档

- `docs/api/Light_Animation.md` — API 完整参考
- `docs/Phase AV 骨骼动画/` — 6A 工作流文档
- `scripts/smoke/animation.lua` — 跨平台 smoke 测试
