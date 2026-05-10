-- ChocoLight Sample: demo_udp_echo (Phase BC + v2)
--
-- 端到端验证 Phase BC 网络层 — RPC + Room 的双进程 demo.
-- 同一 main.lua 通过 arg[1] 切换 server/client 模式.
--
-- 用法:
--   终端 A: light.exe samples/demo_udp_echo/main.lua server [port=9001]
--   终端 B: light.exe samples/demo_udp_echo/main.lua client [host=127.0.0.1] [port=9001]
--
--   (端口 P 用于 Room, P+1 用于 RPC. 默认 9001 / 9002.)
--
-- 验证项:
--   T5  PlatformNet ENet              -- 双进程 UDP 通信
--   T6  RPC client:Call / server      -- Echo 方法 (request → response)
--   T8  Room.Host / Room.Join         -- Hello → State 同步
--   v2  client:Call timeout_ms        -- Sleep 方法故意慢响应 + 100ms 超时
--   v2  room:PatchState 增量          -- server 中途 patch state, 验证 client OnState 触发
--   v2  room:Kick(reason)             -- server 5s 后 kick, client OnKick 收到 reason

local Net  = require 'Light.Network'
local Room = require 'Light.Network.Room'
local Rpc  = require 'Light.Network.Rpc'
local Time = require 'Light.Time'

local mode = arg and arg[1] or 'server'

-- 共用辅助
local function tick() return Time.GetTicks() end
local function sleep(ms) Time.Delay(ms) end

-- ============================================================
--                          SERVER
-- ============================================================
if mode == 'server' then
    local port = tonumber(arg[2]) or 9001
    print(string.format('[server] starting on port=%d (RPC %d)', port, port + 1))

    -- RPC server: 注册 Echo + Sleep
    local rpc = Rpc.Listen('0.0.0.0', port + 1, 8)
    if not rpc then error('Rpc.Listen failed') end

    rpc:RegisterMethod('Echo', function(params)
        return {
            echoed      = params and params.msg or '',
            server_time = tick(),
        }
    end)

    rpc:RegisterMethod('Sleep', function(params)
        -- 故意慢响应, 触发 client timeout
        local ms = (params and params.ms) or 0
        local t0 = tick()
        while tick() - t0 < ms do end
        return {slept = ms}
    end)

    rpc:OnEvent(function(evt)
        print(string.format('[server rpc] %s peer=%s',
                            evt.type, tostring(evt.peer_id)))
    end)

    -- Room host
    local room = Room.Host('0.0.0.0', port, 8)
    if not room then error('Room.Host failed') end

    local first_peer = nil
    room:OnJoin(function(pid, hello)
        print(string.format('[server room] join pid=%d name=%s',
                            pid, (hello and hello.name) or '?'))
        first_peer = first_peer or pid
        return true
    end)
    room:OnLeave(function(pid)
        print('[server room] leave pid='..pid)
    end)
    room:OnInput(function(pid, kind, data)
        print(string.format('[server room] input pid=%d kind=%s', pid, kind))
    end)

    -- 初始 state
    room:SetState({
        score   = 0,
        round   = 1,
        players = {},
    })

    print('[server] ready, running for ~10s...')

    -- 主循环: 600 帧 @ 60fps ≈ 10s
    local frames = 0
    while frames < 600 do
        Net.Resume()
        sleep(16)
        frames = frames + 1

        -- 60 帧 (1s) 后做 PatchState (顶层 set)
        if frames == 60 then
            room:PatchState({score = 10, lastEvent = 'first_patch'})
            print('[server] PatchState set: score=10, lastEvent=first_patch')
        end

        -- 120 帧 (2s) 后做 PatchState (set + delete)
        if frames == 120 then
            room:PatchState({score = 20}, {'lastEvent'})
            print('[server] PatchState set+del: score=20, -lastEvent')
        end

        -- 180 帧 (3s) 后广播事件
        if frames == 180 then
            local n = room:Broadcast('round_start', {round = 2})
            print('[server] Broadcast round_start to '..n..' peer(s)')
        end

        -- 300 帧 (5s) 后 Kick (验证 reason 送达)
        if frames == 300 and first_peer then
            local ok = room:Kick(first_peer, 'demo finished, bye')
            print('[server] Kick pid='..first_peer..' ok='..tostring(ok))
        end
    end

    rpc:Close()
    room:Close()
    print('[server] done')

-- ============================================================
--                          CLIENT
-- ============================================================
elseif mode == 'client' then
    local host_addr = arg[2] or '127.0.0.1'
    local port      = tonumber(arg[3]) or 9001
    print(string.format('[client] connecting to %s:%d (RPC %d)',
                        host_addr, port, port + 1))

    -- RPC client
    local rpc = Rpc.Connect(host_addr, port + 1)
    if not rpc then error('Rpc.Connect failed') end
    rpc:OnEvent(function(evt)
        print(string.format('[client rpc] %s%s',
                            evt.type,
                            evt.message and (' ('..evt.message..')') or ''))
    end)

    -- Room client
    local room = Room.Join(host_addr, port, {name = 'demo_user', meta = {ver = 1}})
    if not room then error('Room.Join failed') end

    room:OnReady(function()
        print('[client room] ready')
    end)
    room:OnState(function(state, rev)
        print(string.format('[client room] state rev=%d score=%s lastEvent=%s round=%s',
                            rev,
                            tostring(state.score),
                            tostring(state.lastEvent),
                            tostring(state.round)))
    end)
    room:OnEvent(function(name, args)
        print(string.format('[client room] event %s args=%s',
                            name, args and tostring(args.round) or '?'))
    end)
    room:OnKick(function(reason)
        print('[client room] KICKED with reason: '..(reason or '?'))
    end)

    -- 等待 connect 建立 (200ms ENet 握手)
    for i = 1, 20 do Net.Resume(); sleep(10) end

    -- Test 1: Echo
    rpc:Call('Echo', {msg = 'hello phase BC'}, function(err, result)
        if err then
            print(string.format('[client] Echo error: code=%d msg=%s',
                                err.code, err.message))
        else
            print(string.format('[client] Echo result: echoed=%s server_time=%s',
                                tostring(result.echoed),
                                tostring(result.server_time)))
        end
    end)

    -- Test 2: Sleep with 100ms timeout (server sleeps 1000ms → 必定 timeout)
    rpc:Call('Sleep', {ms = 1000}, function(err, result)
        if err and err.code == -32001 then
            print('[client] Sleep TIMEOUT fired correctly (-32001)')
        elseif err then
            print(string.format('[client] Sleep error: code=%d msg=%s',
                                err.code, err.message))
        else
            print('[client] Sleep result: slept='..tostring(result.slept))
        end
    end, 100)  -- ← Phase BC v2 timeout_ms

    -- 主循环 ~10s 等服务端打 patch / kick
    local frames = 0
    while frames < 600 do
        Net.Resume()
        sleep(16)
        frames = frames + 1
    end

    rpc:Close()
    room:Close()
    print('[client] done')

else
    error('unknown mode: '..tostring(mode)..' (expected "server" or "client")')
end
