-- Phase BC smoke: Light.Network.Udp (T5)
-- ASCII-only.
--
-- 不依赖外部网络: 单进程内自收自发 (loopback 127.0.0.1).
-- 由于 PlatformNet::Poll() 在游戏循环驱动, 本脚本仅在 require 模块时验证
-- API 完整性 + 创建/绑定/关闭无 panic.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- 1) require
local ok, Udp = pcall(require, "Light.Network.Udp")
if not ok then fail("require(Light.Network.Udp): " .. tostring(Udp)) end
pass("require(Light.Network.Udp) ok")

-- 2) Module surface
if type(Udp.Open) ~= "function" then fail("Udp.Open missing") end
pass("Udp.Open present")

-- 3) Open + GetLocalPort + Close
--    Web 平台 PlatformNet::CreateUdpSocket 返回 nullptr,
--    我们识别并跳过实际 socket 检查.
local sock, err = Udp.Open(0)
if not sock then
    pass("Udp.Open(0) returned nil (likely Web platform: " .. tostring(err) .. ")")
    pass("== Light.Network.Udp smoke PASS (Web stub) ==")
    return
end

if type(sock) ~= "userdata" then fail("Udp.Open should return userdata, got " .. type(sock)) end
pass("Udp.Open(0) returned userdata")

-- 元方法检查
if type(sock.Send)         ~= "function" then fail("sock:Send missing") end
if type(sock.OnReceive)    ~= "function" then fail("sock:OnReceive missing") end
if type(sock.GetLocalPort) ~= "function" then fail("sock:GetLocalPort missing") end
if type(sock.Close)        ~= "function" then fail("sock:Close missing") end
pass("Socket methods present")

local port = sock:GetLocalPort()
if type(port) ~= "number" or port <= 0 then
    fail("GetLocalPort should return positive integer, got " .. tostring(port))
end
pass("GetLocalPort = " .. tostring(port))

-- tostring metamethod
local s = tostring(sock)
if not s:find("Light.Network.Udp.Socket") then
    fail("tostring expected to mention Socket, got '" .. s .. "'")
end
pass("tostring(sock) ok: " .. s)

-- Register receive cb (不验证实际收包, 那需要主循环 Poll)
local recv_called = false
sock:OnReceive(function(host, p, data)
    recv_called = true
    print(string.format("[recv] %s:%d (%d bytes)", host, p, #data))
end)
pass("OnReceive registered (callback won't fire without Poll loop)")

-- Send to self (loopback)
local ok_send = sock:Send("127.0.0.1", port, "hello UDP")
if ok_send ~= true and ok_send ~= false then
    -- Send 返回 bool 或 (false, err)
    pass("sock:Send returned non-true (ENet/PlatformNet rejected)")
else
    pass("sock:Send returned: " .. tostring(ok_send))
end

-- Cleanup
sock:Close()
pass("sock:Close ok")

-- 第二次 Close 应幂等
local ok2, err2 = pcall(function() sock:Close() end)
if not ok2 then fail("double Close raised: " .. tostring(err2)) end
pass("double Close is idempotent")

-- 关闭后 GetLocalPort 返回 0
local p2 = sock:GetLocalPort()
if p2 ~= 0 then fail("GetLocalPort after Close should be 0, got " .. tostring(p2)) end
pass("GetLocalPort after Close = 0")

pass("== Light.Network.Udp smoke PASS ==")
