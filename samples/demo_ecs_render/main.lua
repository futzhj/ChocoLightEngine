-- ChocoLight Sample: demo_ecs_render (Phase D)
--
-- 演示 ECS × 渲染 + 网络化 集成. 一份 main.lua 三种模式:
--   solo   (默认): 单进程, N 个 entity 自动跑动, 开窗渲染 (沙盒/学习)
--   server       : ECS world 作 Room.Host, 跑模拟 + NetworkSync(room) 广播增量
--   client       : Join 远端 Room, MirrorFromRoom + 开窗渲染 mirror world
--
-- 用法:
--   light.exe samples/demo_ecs_render/main.lua                                # solo
--   light.exe samples/demo_ecs_render/main.lua server [port=9111]             # 终端 A
--   light.exe samples/demo_ecs_render/main.lua client [host=127.0.0.1] [port=9111]  # 终端 B
--
-- 主要演示项:
--   D-T1  内置 Transform2D / Sprite / Camera2D 自动注册
--   D-T2  world:Render 顶层调度 (2D camera -> sprite z-sort)
--   D-T3  Sprite anchor / color / 画家算法
--   D-T7  client 端 MirrorFromRoom 收 ecs_delta 后用 mirror:Render() 自动渲染

print("==== Light.ECS Render Demo (Phase D) ====")

-- ==================== 1. 模块加载 ====================

local ECS = require 'Light.ECS'
local function tryRequire(name)
    local ok, m = pcall(require, name)
    if ok then return m end
    return nil
end
local UI   = tryRequire('Light.UI')
local Time = tryRequire('Light.Time')
local Net  = tryRequire('Light.Network')
local Room = tryRequire('Light.Network.Room')

local mode     = arg and arg[1] or 'solo'
local arg2     = arg and arg[2]
local arg3     = arg and arg[3]
local duration = 0  -- solo: ESC 退出; server/client: 600 帧 ≈ 10s

print(string.format("[1] modules: ECS=%s UI=%s Time=%s Net=%s Room=%s  mode=%s",
    type(ECS), type(UI), type(Time), type(Net), type(Room), mode))

-- ==================== 2. 通用辅助 ====================

-- 给 world 注册用户 Velocity component + Move system (含边界反弹)
local function setupVelocityAndMove(world, screenW, screenH)
    world:RegisterComponent('Velocity', {vx=0, vy=0})
    world:AddSystem('Move', {'Transform2D', 'Velocity'}, function(ents, dt)
        for _, e in ipairs(ents) do
            local tf = e._comps.Transform2D
            local v  = e._comps.Velocity
            local newX = tf.x + v.vx * dt
            local newY = tf.y + v.vy * dt
            if newX < 0 or newX > screenW then v.vx = -v.vx; newX = math.max(0, math.min(screenW, newX)) end
            if newY < 0 or newY > screenH then v.vy = -v.vy; newY = math.max(0, math.min(screenH, newY)) end
            e:Set('Transform2D', {x=newX, y=newY})
            e:Set('Velocity',    {vx=v.vx, vy=v.vy})
        end
    end)
end

-- 创建 N 个随机 entity (Transform2D + Velocity + Sprite color)
local function spawnEntities(world, n, screenW, screenH)
    math.randomseed(42)
    for i = 1, n do
        local e = world:CreateEntity()
        e:Add('Transform2D', {
            x = math.random(50, screenW - 50),
            y = math.random(50, screenH - 50),
            z = i,
        })
        e:Add('Velocity', {
            vx = math.random(-150, 150),
            vy = math.random(-150, 150),
        })
        e:Add('Sprite', {
            color = {
                r = math.random() * 0.8 + 0.2,
                g = math.random() * 0.8 + 0.2,
                b = math.random() * 0.8 + 0.2,
                a = 1.0,
            },
            anchor = {ax=0.5, ay=0.5},
        })
    end
end

-- 给 world 加 Camera2D (active=true)
local function spawnCamera(world)
    local cam = world:CreateEntity()
    cam:Add('Transform2D', {x=0, y=0})
    cam:Add('Camera2D',    {active=true, zoom=1.0})
    return cam
end

-- 通用 Draw: 调 world:Render() 后用 Light.Graphics.Rectangle 画 fallback (没 image 时)
local function drawWorld(world)
    Light.Graphics.SetColor(0.1, 0.1, 0.15, 1)
    world:Render()
    for _, e in ipairs(world._entities) do
        local tf = e._comps.Transform2D
        local sp = e._comps.Sprite
        if tf and sp and not sp.image then
            local c = sp.color or {}
            Light.Graphics.SetColor(c.r or 1, c.g or 1, c.b or 1, c.a or 1)
            local size = 16
            local ax = (sp.anchor and sp.anchor.ax) or 0
            local ay = (sp.anchor and sp.anchor.ay) or 0
            Light.Graphics.Rectangle(2, tf.x - size*ax, tf.y - size*ay, 0, size, size, 0)
        end
    end
end

-- ==================== 3. 模式分支 ====================

if mode == 'server' then
    -- ============ SERVER 模式 ============
    if not Room then error('server mode needs Light.Network.Room') end
    local port = tonumber(arg2) or 9111
    local world = ECS.World.new()
    setupVelocityAndMove(world, 800, 600)
    spawnEntities(world, 20, 800, 600)
    print(string.format("[server] spawned 20 entities, port=%d", port))

    -- 关键: 把内置渲染 component 标 networked, NetworkSync 才会同步它们
    world:MarkRenderNetworked()
    -- user Velocity 也标 networked (让 client 看到完整运动状态)
    world._networked_comps.Velocity = true

    local room = Room.Host('0.0.0.0', port, 8)
    if not room then error('Room.Host failed on port '..port) end
    room:OnJoin(function(pid, hello)
        print(string.format('[server] join pid=%d name=%s', pid, (hello and hello.name) or '?'))
        world:MarkFullResync()
        return true
    end)
    room:OnLeave(function(pid) print('[server] leave pid='..pid) end)

    world:NetworkSync(room)
    print('[server] running ~10s broadcasting ecs_delta...')

    local frames = 0
    while frames < 600 do
        Net.Resume()
        world:Update(0.016)
        Time.Delay(16)
        frames = frames + 1
        if frames % 60 == 0 then
            print(string.format('[server t=%ds] entities=%d', frames/60, #world._entities))
        end
    end
    world:NetworkSync(nil)
    room:Close()
    print('demo_ecs_render server ok')
    return
end

if mode == 'client' then
    -- ============ CLIENT 模式 ============
    if not Room then error('client mode needs Light.Network.Room') end
    if not UI or not UI.Window then error('client mode needs Light.UI.Window') end

    local host = arg2 or '127.0.0.1'
    local port = tonumber(arg3) or 9111
    print(string.format("[client] connecting %s:%d", host, port))

    local room = Room.Join(host, port, {name='ecs_render_observer'})
    if not room then error('Room.Join failed') end
    room:OnReady(function() print('[client] room ready') end)
    room:OnKick(function(reason) print('[client] kicked: '..(reason or '?')) end)

    local mirror = ECS.MirrorFromRoom(room)
    -- mirror 不需要主动 Mark; ecs_delta 收到时 _ApplyDelta 会自动建 entity 和 component
    spawnCamera(mirror)  -- mirror 自己加个相机以应用视图变换

    -- 等握手
    for i = 1, 20 do Net.Resume(); Time.Delay(10) end

    local Game = Light(Light.UI.Window):New()
    function Game:OnOpen()
        print('[client] window opened, rendering mirror world')
    end
    function Game:Update(dt)
        Net.Resume()
        -- 注意: mirror world 不调 Update (没有 Move system, 全靠 server 推 ecs_delta)
    end
    function Game:Draw()
        drawWorld(mirror)
        Light.Graphics.SetColor(1,1,0,1)
        Light.Graphics.Print(string.format("Client mirror: %d entities  (server-authoritative)",
            #mirror._entities), nil, 10, 10, 0)
    end
    function Game:OnKey(key, sc, action) if action==1 and key==256 then self:Close() end end

    Game:Open(800, 600, 'ChocoLight Phase D — ECS Render Client')
    while Light.UI.Loop() do Light.UI.Resume() end
    room:Close()
    print('demo_ecs_render client ok')
    return
end

-- ============ SOLO 模式 (默认) ============
local n = tonumber(arg2) or 50
duration = tonumber(arg3) or 0

local world = ECS.World.new()
print(string.format("[2] world created, builtin components: %s",
    table.concat({"Transform2D","Sprite","Camera2D",
                  "Transform3D","MeshRenderer","Camera3D"}, ", ")))

setupVelocityAndMove(world, 800, 600)
spawnEntities(world, n, 800, 600)
print(string.format("[3] created %d entities with Transform2D+Velocity+Sprite", n))
spawnCamera(world)
print("[3] added Camera2D entity at (0,0) zoom=1")

if not UI or not UI.Window then
    -- Headless 验证模式
    print("[4] UI.Window unavailable, headless mode")
    for frame = 1, 5 do world:Update(1/60) end
    local sample = world._entities[1]
    if sample then
        print(string.format("[4] after 5 frames entity #1: x=%.1f y=%.1f",
            sample._comps.Transform2D.x, sample._comps.Transform2D.y))
    end
    print("demo_ecs_render solo ok (no window)")
    return
end

-- 开窗
local Game = Light(Light.UI.Window):New()
function Game:OnOpen()
    print("[5] window opened, running ECS render demo (solo)")
    if Light.Graphics and Light.Graphics.GetBackendName then
        print("[5] backend: " .. Light.Graphics.GetBackendName())
    end
end

local elapsed = 0
local frames  = 0
function Game:Update(dt)
    elapsed = elapsed + dt
    frames  = frames + 1
    world:Update(dt)
    if duration > 0 and elapsed >= duration then self:Close() end
end

function Game:Draw()
    drawWorld(world)
    Light.Graphics.SetColor(1, 1, 0, 1)
    Light.Graphics.Print(string.format("Phase D Solo  entities=%d  frames=%d  elapsed=%.1fs",
        n, frames, elapsed), nil, 10, 10, 0)
    Light.Graphics.SetColor(0.7, 0.7, 0.7, 1)
    Light.Graphics.Print("ESC=quit  args: [n_entities=50] [duration=0]", nil, 10, 30, 0)
end

function Game:OnKey(key, sc, action) if action==1 and key==256 then self:Close() end end

Game:Open(800, 600, "ChocoLight Phase D — ECS Render Demo (Solo)")
while Light.UI.Loop() do Light.UI.Resume() end
print(string.format("demo_ecs_render solo ok (ran %d frames in %.2fs)", frames, elapsed))
