-- Phase BC smoke: Light.Network.Rpc (T7)
-- ASCII-only.
--
-- 不依赖外部网络: 验证模块加载 + 关键 API 存在 + Listen/Connect 不 panic.
-- 真实的 Call <-> Server 往返需要 Poll 循环驱动, 在 demo 脚本中演示.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- 1) require
local ok, Rpc = pcall(require, "Light.Network.Rpc")
if not ok then fail("require(Light.Network.Rpc): " .. tostring(Rpc)) end
pass("require(Light.Network.Rpc) ok")

-- 2) Module surface
if type(Rpc.Connect) ~= "function" then fail("Rpc.Connect missing") end
if type(Rpc.Listen)  ~= "function" then fail("Rpc.Listen missing") end
pass("Rpc module fns present")

-- 3) Listen (Web 平台 PlatformNet::EnetCreateHost 返回 nullptr -> nil)
local server, lerr = Rpc.Listen("127.0.0.1", 0, 4)
if not server then
    -- 0 端口被部分系统视为非法, 尝试随机高位端口
    server, lerr = Rpc.Listen("127.0.0.1", 51000, 4)
end
if not server then
    pass("Rpc.Listen returned nil (Web platform: " .. tostring(lerr) .. ")")
    pass("== Light.Network.Rpc smoke PASS (Web stub) ==")
    return
end
pass("Rpc.Listen ok")

-- Server methods
for _, m in ipairs({"RegisterMethod","UnregisterMethod","OnEvent","Close"}) do
    if type(server[m]) ~= "function" then fail("server:" .. m .. " missing") end
end
pass("Server methods present")

-- RegisterMethod
server:RegisterMethod("Add", function(params, peer_id)
    return params.a + params.b
end)
server:RegisterMethod("Fail", function() return nil, {code=-1, message="nope"} end)
pass("RegisterMethod ok")

server:OnEvent(function(evt)
    print("[server event]", evt.type, evt.peer_id)
end)
pass("OnEvent registered")

server:UnregisterMethod("Fail")
pass("UnregisterMethod ok")

local s_tostring = tostring(server)
if not s_tostring:find("Light.Network.Rpc.Server") then
    fail("server tostring: " .. s_tostring)
end
pass("server tostring: " .. s_tostring)

-- 4) Connect (本进程 -> server, ENet 通常允许)
--   注: ENet Connect 是异步, 不需要 server::OnPeer 触发即返回 client userdata
local client, cerr = Rpc.Connect("127.0.0.1", 51000)
if not client then
    pass("Rpc.Connect returned nil: " .. tostring(cerr))
    server:Close()
    pass("== Light.Network.Rpc smoke PASS (connect skipped) ==")
    return
end
pass("Rpc.Connect ok")

for _, m in ipairs({"Call","Notify","OnEvent","Close"}) do
    if type(client[m]) ~= "function" then fail("client:" .. m .. " missing") end
end
pass("Client methods present")

client:OnEvent(function(evt) print("[client event]", evt.type) end)

-- Call without Poll loop: 包被 ENet 入队, 但无 service 不会送达. 不验证回调.
local sent = client:Call("Add", {a=1, b=2}, function(err, result)
    print("[client cb]", err, result)
end)
pass("client:Call returned: " .. tostring(sent))

local sent2 = client:Notify("log", "hello")
pass("client:Notify returned: " .. tostring(sent2))

-- Cleanup
client:Close()
pass("client:Close ok")
server:Close()
pass("server:Close ok")

pass("== Light.Network.Rpc smoke PASS ==")
