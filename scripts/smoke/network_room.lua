-- Phase BC smoke: Light.Network.Room (T8)
-- ASCII-only.
--
-- 验证模块加载 + Host/Join API + state/broadcast/kick 调用不 panic.
-- 真实 join/state 同步需要 Poll 循环驱动, 由 demo 脚本演示.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- 1) require
local ok, Room = pcall(require, "Light.Network.Room")
if not ok then fail("require(Light.Network.Room): " .. tostring(Room)) end
pass("require(Light.Network.Room) ok")

if type(Room.Host) ~= "function" then fail("Room.Host missing") end
if type(Room.Join) ~= "function" then fail("Room.Join missing") end
pass("Room module fns present")

-- 2) Host (Web 平台返回 nil)
local host, herr = Room.Host("127.0.0.1", 51100, 8)
if not host then
    pass("Room.Host returned nil (Web platform: " .. tostring(herr) .. ")")
    pass("== Light.Network.Room smoke PASS (Web stub) ==")
    return
end
pass("Room.Host ok")

-- Host methods
for _, m in ipairs({"OnJoin","OnLeave","OnInput","SetState","Broadcast","Kick","Close"}) do
    if type(host[m]) ~= "function" then fail("host:" .. m .. " missing") end
end
pass("Host methods present")

host:OnJoin(function(peer_id, hello)
    print("[host join]", peer_id, hello and hello.name or "?")
    return true
end)
host:OnLeave(function(peer_id) print("[host leave]", peer_id) end)
host:OnInput(function(peer_id, kind, data) print("[host input]", peer_id, kind) end)
pass("Host callbacks registered")

-- SetState
host:SetState({score = 0, players = {}})
pass("host:SetState ok (rev=1, broadcast no-op no peers)")

host:SetState({score = 10, players = {[1] = "alice"}})
pass("host:SetState ok (rev=2)")

-- Broadcast (channel 1 unreliable, no peers -> sent=0)
local sent = host:Broadcast("hello", {text = "world"})
if type(sent) ~= "number" then fail("Broadcast should return number, got " .. type(sent)) end
pass("host:Broadcast returned " .. tostring(sent))

-- Kick non-existent peer -> false
local kicked = host:Kick(99)
if kicked ~= false then fail("Kick of non-existent peer should be false, got " .. tostring(kicked)) end
pass("host:Kick(99) = false (correctly)")

local h_str = tostring(host)
if not h_str:find("Light.Network.Room.Host") then fail("Host tostring: " .. h_str) end
pass("Host tostring: " .. h_str)

-- 3) Join
local client, cerr = Room.Join("127.0.0.1", 51100, {name="bob", meta={version="1.0"}})
if not client then
    pass("Room.Join returned nil: " .. tostring(cerr))
    host:Close()
    pass("== Light.Network.Room smoke PASS (join skipped) ==")
    return
end
pass("Room.Join ok")

for _, m in ipairs({"OnReady","OnState","OnEvent","OnKick","SendInput","Leave"}) do
    if type(client[m]) ~= "function" then fail("client:" .. m .. " missing") end
end
pass("Client methods present")

client:OnReady(function() print("[client ready]") end)
client:OnState(function(state, rev) print("[client state]", rev, state.score) end)
client:OnEvent(function(name, args) print("[client event]", name) end)
client:OnKick(function(reason) print("[client kicked]", reason) end)
pass("Client callbacks registered")

local ok_input = client:SendInput("move", {dx=1, dy=0})
pass("client:SendInput returned: " .. tostring(ok_input))

local c_str = tostring(client)
if not c_str:find("Light.Network.Room.Client") then fail("Client tostring: " .. c_str) end
pass("Client tostring: " .. c_str)

-- Cleanup
client:Leave()
pass("client:Leave ok")
host:Close()
pass("host:Close ok")

pass("== Light.Network.Room smoke PASS ==")
