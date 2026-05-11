-- ChocoLight Sample: demo_ecs_skinned (Phase D.x.4)
--
-- 演示 ECS × 骨骼动画 集成: 用 ECS 范式声明 N 个角色 entity, 自动调度 animator + 渲染.
--
-- 主路径 (有 hero.glb + Light.UI.Window 时):
--   1. LoadSkinnedGLTF + NewAnimator + AddState (每个角色一份)
--   2. 创建 5 个角色 entity (Transform3D + SkinnedMeshRenderer + AnimationState)
--   3. world:Update 自动推进 animator + 桥接 state 切换
--   4. world:Render 自动调 Light.Animation.DrawSkinnedMesh
--
-- Fallback (无 hero.glb 或 headless 时):
--   - 仅打印 ECS API 验证 (类似 demo_animation 的"无资源仍可退出 ok"模式)
--   - CI runtime smoke 友好
--
-- 用法:
--   light.exe samples/demo_ecs_skinned/main.lua [duration=0]   # 主路径开窗
--   light.exe samples/demo_ecs_skinned/main.lua --headless     # 仅 API 验证

print("==== Light.ECS x Skeletal Animation Demo (Phase D.x.4) ====")

-- ==================== 1. 模块加载 ====================

local ECS = require 'Light.ECS'
local function tryRequire(name)
    local ok, m = pcall(require, name)
    if ok then return m end
    return nil
end
local UI    = tryRequire('Light.UI')
local Anim  = tryRequire('Light.Animation')
local Time  = tryRequire('Light.Time')

print(string.format("[1] modules: ECS=%s UI=%s Anim=%s Time=%s",
    type(ECS), type(UI), type(Anim), type(Time)))

-- ==================== 2. ECS world + 检测 ====================

local world = ECS.World.new()
print(string.format("[2] world ready, builtin Skinned* components: SMR=%s AS=%s",
    type(world._components.SkinnedMeshRenderer),
    type(world._components.AnimationState)))

-- 不支持 Light.Animation 时直接退出 (CI / 无 GPU 环境)
if not Anim or type(Anim.LoadSkinnedGLTF) ~= 'function' then
    print("[2] Light.Animation unavailable, API-only mode")
    print("demo_ecs_skinned ok (no animation module)")
    return
end

-- ==================== 3. 资源探测 ====================

local function file_exists(path)
    local f = io.open(path, "rb")
    if f then f:close(); return true end
    return false
end
local candidates = {
    'samples/demo_ecs_skinned/hero.glb',
    'samples/demo_animation/assets/character.glb',
    'samples/demo_animation/character.glb',
    'assets/character.glb',
}
local glb = nil
for _, p in ipairs(candidates) do if file_exists(p) then glb = p; break end end

if not glb then
    print("[3] no glTF asset found in any of:")
    for _, p in ipairs(candidates) do print("       - "..p) end
    print("[3] running API-only validation (ECS register + system flow)")

    -- 仅验证 ECS 集成不崩 (无真 animator, 用 mock)
    local mockAnim = {Update=function(self,dt) end,
                       Play=function(self,n) print("  mock anim Play: "..n) end}
    local e = world:CreateEntity()
    e:Add('Transform3D', {x=0,y=0,z=0})
    e:Add('SkinnedMeshRenderer', {mesh='__mock__', animator=mockAnim})
    e:Add('AnimationState',      {state='Idle'})
    world:Update(0.016)
    e:Set('AnimationState', {state='Run', crossfade=0.3})
    world:Update(0.016)
    print("[3] ECS animation system flow ok (mock animator)")
    print("demo_ecs_skinned ok (no asset)")
    return
end

print(string.format("[3] found asset: %s", glb))
local pack, err = Anim.LoadSkinnedGLTF(glb)
if not pack or not pack.skeleton then
    print(string.format("[3] LoadSkinnedGLTF failed: %s (skipping main path)", tostring(err)))
    print("demo_ecs_skinned ok (load failed)")
    return
end
print(string.format("[3] loaded: joints=%d clips=%d hasSkin=%s",
    pack.skeleton:GetJointCount(), #(pack.clipNames or {}),
    tostring(pack.hasSkin)))

-- 工厂: 每个 entity 独立 animator (避免共享 state)
local function makeAnimator()
    local an = Anim.NewAnimator(pack.skeleton)
    for _, name in ipairs(pack.clipNames or {}) do
        local clip = pack.clips and pack.clips[name]
        if clip then an:AddState(name, clip) end
    end
    return an
end

-- ==================== 4. 创建 5 个角色 entity ====================

local initialState = pack.clipNames and pack.clipNames[1] or 'Idle'
local heroes = {}
for i = 1, 5 do
    local e = world:CreateEntity()
    e:Add('Transform3D', {x=(i-3)*2.0, y=0, z=0, ry=(i-3)*20, sx=1, sy=1, sz=1})
    e:Add('SkinnedMeshRenderer', {mesh=pack.mesh, animator=makeAnimator()})
    e:Add('AnimationState',      {state=initialState, speed=1.0, looping=true})
    heroes[i] = e
end
print(string.format("[4] created %d hero entities with state='%s'", #heroes, initialState))

-- Camera3D (active=true)
local cam = world:CreateEntity()
cam:Add('Transform3D', {x=0, y=2, z=8})
cam:Add('Camera3D',    {active=true, fovY=60, aspect=4/3, nearZ=0.1, farZ=100,
                         targetX=0, targetY=1, targetZ=0,
                         upX=0, upY=1, upZ=0})

-- ==================== 5. UI 检查 + headless fallback ====================

local headless = false
for _, a in ipairs(arg or {}) do if a == '--headless' then headless = true end end

if headless or not UI or not UI.Window then
    print("[5] headless / no UI mode: running 30 update frames")
    for f = 1, 30 do world:Update(1/60) end
    -- 切换一次 state 验证 system 桥接
    for _, e in ipairs(heroes) do
        if #(pack.clipNames or {}) > 1 then
            e:Set('AnimationState', {state=pack.clipNames[2], crossfade=0.3})
        end
    end
    for f = 1, 10 do world:Update(1/60) end
    print(string.format("[5] state switched to '%s' for all heroes",
        pack.clipNames and pack.clipNames[2] or '(none)'))
    print("demo_ecs_skinned ok (headless)")
    return
end

-- ==================== 6. 主路径: 开窗 + game loop ====================

local duration = tonumber(arg and arg[1]) or 0
local Game = Light(Light.UI.Window):New()
local elapsed, frames = 0, 0

function Game:OnOpen()
    print("[6] window opened, rendering ECS skinned characters")
end

function Game:Update(dt)
    elapsed = elapsed + dt
    frames  = frames + 1
    world:Update(dt)
    if duration > 0 and elapsed >= duration then self:Close() end

    -- 每 2 秒在所有角色间切换 state (用 crossfade)
    if pack.clipNames and #pack.clipNames > 1 then
        local idx = math.floor(elapsed / 2) % #pack.clipNames + 1
        local target = pack.clipNames[idx]
        for _, e in ipairs(heroes) do
            local as = e._comps.AnimationState
            if as and as.state ~= target then
                e:Set('AnimationState', {state=target, crossfade=0.3, speed=1.0})
            end
        end
    end
end

function Game:Draw()
    Light.Graphics.SetColor(0.15, 0.15, 0.2, 1)
    world:Render()
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print(string.format("Phase D.x.4 ECS Skinned  heroes=%d  frame=%d  t=%.1fs",
        #heroes, frames, elapsed), nil, 10, 10, 0)
    Light.Graphics.SetColor(0.7, 0.7, 0.7, 1)
    Light.Graphics.Print("ESC=quit  state auto-cycles every 2s",
        nil, 10, 30, 0)
end

function Game:OnKey(key, sc, action) if action==1 and key==256 then self:Close() end end

Game:Open(800, 600, "ChocoLight Phase D.x.4 — ECS Skinned")
while Light.UI.Loop() do Light.UI.Resume() end
print(string.format("demo_ecs_skinned ok (ran %d frames in %.2fs)", frames, elapsed))
