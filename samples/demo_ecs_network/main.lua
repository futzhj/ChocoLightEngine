-- ChocoLight Sample: demo_ecs_network (Phase C C-T6)
--
-- 端到端验证 Phase C ECS 网络化 — 双进程演示 server-authoritative ECS + client mirror.
-- 同一 main.lua 通过 arg[1] 切换 server/client 模式.
--
-- 用法:
--   终端 A: light.exe samples/demo_ecs_network/main.lua server [port=9101]
--   终端 B: light.exe samples/demo_ecs_network/main.lua client [host=127.0.0.1] [port=9101]
--
-- 验证项:
--   C-T1  RegisterComponent opts.networked       -- 显式标记 Position/Velocity
--   C-T2  entity:Set 触发 dirty                  -- Move system 改 Position
--   C-T3  world:NetworkSync(room) + _SyncToRoom  -- 每帧自动 PatchState
--   C-T4  ECS.MirrorFromRoom(room) + _ApplyState -- client 自动镜像 server 状态
--
-- 预期:
--   server 跑 ~10s, 创建 3 个移动实体, 每帧改 Position
--   client 每秒打印 mirror 中 entity 数量 + 当前坐标 (来自 server 推送)

local Net  = require 'Light.Network'
local Room = require 'Light.Network.Room'
local ECS  = require 'Light.ECS'
local Time = require 'Light.Time'

local mode = arg and arg[1] or 'server'

local function tick() return Time.GetTicks() end
local function sleep(ms) Time.Delay(ms) end

-- ============================================================
--                          SERVER
-- ============================================================
if mode == 'server' then
    local port = tonumber(arg[2]) or 9101
    print(string.format('[server] starting on port=%d', port))

    -- 1) 创建 ECS world + 注册 networked component schema
    local world = ECS.World.new()
    world:RegisterComponent('Position', {x=0, y=0}, {networked=true})
    world:RegisterComponent('Velocity', {vx=0, vy=0}, {networked=true})
    world:RegisterComponent('Tag',      {name=''})    -- 非 networked, 演示过滤

    -- 2) Move 系统: 每帧把 Velocity 累加到 Position
    --    用 entity:Set 而不是直接改字段 → 触发 dirty 跟踪
    world:AddSystem('Move', {'Position', 'Velocity'}, function(ents, dt)
        for _, e in ipairs(ents) do
            local p, v = e.Position, e.Velocity
            e:Set('Position', {x = p.x + v.vx * dt, y = p.y + v.vy * dt})
        end
    end)

    -- 3) 创建 3 个不同运动模式的 entity
    local e1 = world:CreateEntity()
    e1:Add('Position', {x=0,   y=0})
    e1:Add('Velocity', {vx=10, vy=0})
    e1:Add('Tag', {name='right_mover'})

    local e2 = world:CreateEntity()
    e2:Add('Position', {x=100, y=100})
    e2:Add('Velocity', {vx=-5, vy=5})
    e2:Add('Tag', {name='diag_mover'})

    local e3 = world:CreateEntity()
    e3:Add('Position', {x=50,  y=50})
    e3:Add('Velocity', {vx=0,  vy=-3})
    e3:Add('Tag', {name='up_mover'})

    -- 4) Room host
    local room = Room.Host('0.0.0.0', port, 8)
    if not room then error('Room.Host failed') end

    room:OnJoin(function(pid, hello)
        print(string.format('[server room] join pid=%d name=%s',
                            pid, (hello and hello.name) or '?'))
        -- Phase C.x.1: 新 peer 进来, 标记下一帧全量重发, 让新人拿完整快照
        world:MarkFullResync()
        return true
    end)
    room:OnLeave(function(pid)
        print('[server room] leave pid='..pid)
    end)

    -- 5) 绑定 ECS → Room. 之后 world:Update 会自动 Broadcast('ecs_delta', ...)
    world:NetworkSync(room)

    print('[server] ready, running ~10s with 3 networked entities...')

    -- 6) 主循环: 600 帧 @ 60fps ≈ 10s
    local frames = 0
    local last_log = 0
    while frames < 600 do
        Net.Resume()
        world:Update(0.016)
        sleep(16)
        frames = frames + 1

        -- 每秒服务端打印一次 entity 状态便于交叉对照
        if frames - last_log >= 60 then
            last_log = frames
            print(string.format('[server t=%ds]', frames / 60))
            for _, e in ipairs(world:Query('Position')) do
                local p = e.Position
                print(string.format('  e%d: pos=(%.2f, %.2f)', e._id, p.x, p.y))
            end
        end

        -- 5s 时新增一个 entity, 验证 client mirror 增量同步
        if frames == 300 then
            local e4 = world:CreateEntity()
            e4:Add('Position', {x=200, y=0})
            e4:Add('Velocity', {vx=0, vy=20})
            e4:Add('Tag', {name='late_joiner'})
            print('[server] spawned late_joiner e'..e4._id)
        end

        -- 8s 时销毁 e2, 验证 client mirror 清除
        if frames == 480 then
            world:DestroyEntity(e2)
            print('[server] destroyed e'..e2._id..' (diag_mover)')
        end
    end

    world:NetworkSync(nil)
    room:Close()
    print('[server] done')

-- ============================================================
--                          CLIENT
-- ============================================================
elseif mode == 'client' then
    local host_addr = arg[2] or '127.0.0.1'
    local port      = tonumber(arg[3]) or 9101
    print(string.format('[client] connecting to %s:%d', host_addr, port))

    -- 1) Room client
    local room = Room.Join(host_addr, port, {name='ecs_observer', meta={ver=1}})
    if not room then error('Room.Join failed') end

    room:OnReady(function() print('[client room] ready') end)
    room:OnKick (function(reason) print('[client room] kicked: '..(reason or '?')) end)

    -- 2) Mirror world: 内部已 hook room:OnState, 自动 ApplyState
    --    client 注册同样的 component schema (networked 标志可有可无, 镜像端不发包)
    local mirror = ECS.MirrorFromRoom(room)
    mirror:RegisterComponent('Position', {x=0, y=0})
    mirror:RegisterComponent('Velocity', {vx=0, vy=0})

    -- 3) 等 connect 建立 (200ms ENet 握手)
    for i = 1, 20 do Net.Resume(); sleep(10) end

    -- 4) 主循环: 每秒打印 mirror 当前状态
    local frames = 0
    local last_log = 0
    while frames < 600 do
        Net.Resume()
        sleep(16)
        frames = frames + 1

        if frames - last_log >= 60 then
            last_log = frames
            local ents = mirror:Query('Position')
            print(string.format('[client t=%ds] mirror has %d entit%s',
                                frames / 60, #ents, #ents == 1 and 'y' or 'ies'))
            for _, e in ipairs(ents) do
                local p = e.Position
                local v = e.Velocity
                print(string.format('  e%d: pos=(%.2f, %.2f) vel=(%.1f, %.1f)',
                                    e._id,
                                    p and p.x or 0, p and p.y or 0,
                                    v and v.vx or 0, v and v.vy or 0))
            end
        end
    end

    room:Close()
    print('[client] done')

else
    error('unknown mode: '..tostring(mode)..' (expected "server" or "client")')
end
