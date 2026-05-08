-- Phase G smoke: Light.Gamepad
-- 仅做 API 注册和无副作用边界路径验证, 适配 CI headless (无手柄) 环境.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Gamepad")
if not ok then fail("require(Light.Gamepad) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Gamepad not a table") end

for _, k in ipairs({
    "GetGamepads", "Open", "Close",
    "GetID", "GetName", "GetType",
    "IsConnected", "GetConnectionState", "GetPowerInfo",
    "HasButton", "GetButton", "HasAxis", "GetAxis",
    "Rumble",
}) do
    if type(mod[k]) ~= "function" then
        fail("Light.Gamepad." .. k .. " missing or not a function")
    end
end
pass("Light.Gamepad module ok (14 functions)")

-- GetGamepads: CI 无手柄, 应返回空 table (非 nil)
local list, err = mod.GetGamepads()
if list == nil then
    -- 部分平台可能返回 nil + err
    pass("Light.Gamepad.GetGamepads returned nil (no joystick subsystem?): " .. tostring(err))
else
    if type(list) ~= "table" then fail("GetGamepads must return table") end
    pass("Light.Gamepad.GetGamepads count=" .. tostring(#list))
end

-- 边界: 非法 instance_id
local h, e = mod.Open(0)
if h ~= nil or e == nil then fail("Open(0) should return nil + err") end
pass("Light.Gamepad.Open(0) boundary ok: " .. tostring(e))

-- 边界: 非法 handle 调用各 API
local n, ne = mod.GetName(nil)
if n ~= nil or ne == nil then fail("GetName(nil) should return nil + err") end
pass("Light.Gamepad.GetName(nil) boundary ok: " .. tostring(ne))

local id, ie = mod.GetID(nil)
if id ~= nil or ie == nil then fail("GetID(nil) should return nil + err") end
pass("Light.Gamepad.GetID(nil) boundary ok: " .. tostring(ie))

-- IsConnected(nil) 应返回 false (无 err)
local connected = mod.IsConnected(nil)
if connected ~= false then fail("IsConnected(nil) should return false") end
pass("Light.Gamepad.IsConnected(nil) = false ok")

-- ConnectionState(nil) 应返回 "invalid"
local cs = mod.GetConnectionState(nil)
if cs ~= "invalid" then fail("GetConnectionState(nil) should be 'invalid', got " .. tostring(cs)) end
pass("Light.Gamepad.GetConnectionState(nil) = 'invalid' ok")

-- GetPowerInfo(nil) 应返回 ("error", nil)
local ps, pp = mod.GetPowerInfo(nil)
if ps ~= "error" or pp ~= nil then
    fail("GetPowerInfo(nil) bad: " .. tostring(ps) .. ", " .. tostring(pp))
end
pass("Light.Gamepad.GetPowerInfo(nil) = ('error', nil) ok")

-- HasButton/GetButton(nil, name) -> false
if mod.HasButton(nil, "south") ~= false then fail("HasButton(nil) should be false") end
if mod.GetButton(nil, "south") ~= false then fail("GetButton(nil) should be false") end
pass("Light.Gamepad.{Has,Get}Button(nil, name) = false ok")

-- HasAxis/GetAxis(nil, name) -> false / 0
if mod.HasAxis(nil, "leftx") ~= false then fail("HasAxis(nil) should be false") end
if mod.GetAxis(nil, "leftx") ~= 0 then fail("GetAxis(nil) should be 0") end
pass("Light.Gamepad.{Has,Get}Axis(nil, name) boundary ok")

-- Rumble 边界: 非法 handle
local r1, r1e = mod.Rumble(nil, 0, 0, 100)
if r1 ~= false or r1e == nil then fail("Rumble(nil) should be false + err") end
pass("Light.Gamepad.Rumble(nil) boundary ok: " .. tostring(r1e))

-- Rumble 边界: low 超范围
local r2, r2e = mod.Rumble(nil, 70000, 0, 100)
if r2 ~= false or r2e == nil then fail("Rumble(nil, 70000) should be false + err") end
pass("Light.Gamepad.Rumble bad low boundary ok: " .. tostring(r2e))

-- Close(nil) 边界
local cok, ce = mod.Close(nil)
if cok ~= false or ce == nil then fail("Close(nil) should be false + err") end
pass("Light.Gamepad.Close(nil) boundary ok: " .. tostring(ce))

print("gamepad smoke ok")
