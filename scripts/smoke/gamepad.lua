-- Phase G/AJ smoke: Light.Gamepad
-- Phase G: 14 fns 基础 API
-- Phase AJ: ~50 fns 扩展 (Mapping/Discovery/Lookup/Per-handle/Events/Strings/Touchpad/Sensor/Effects)
-- 仅做 API 注册和无副作用边界路径验证, 适配 CI headless (无手柄) 环境.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Gamepad")
if not ok then fail("require(Light.Gamepad) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Gamepad not a table") end

-- ------------------------------------------------------------------
-- Phase G 基础 14 fns (向后兼容必须)
-- ------------------------------------------------------------------
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
pass("Light.Gamepad module ok (Phase G 14 functions)")

-- ------------------------------------------------------------------
-- Phase AJ 扩展 API surface 验证
-- ------------------------------------------------------------------
local AJ_FNS = {
    -- Discovery (no handle)
    "HasGamepad", "IsGamepad",
    "GetNameForID", "GetPathForID", "GetPlayerIndexForID", "GetGUIDForID",
    "GetVendorForID", "GetProductForID", "GetProductVersionForID",
    "GetTypeForID", "GetRealTypeForID",
    -- Mapping
    "AddMapping", "AddMappingsFromFile", "ReloadMappings",
    "GetMappings", "GetMappingForGUID", "GetMapping",
    "SetMapping", "GetMappingForID",
    -- Lookup
    "GetGamepadFromID", "GetGamepadFromPlayerIndex",
    -- Per-handle (new)
    "GetPath", "GetRealType",
    "GetVendor", "GetProduct", "GetProductVersion", "GetFirmwareVersion",
    "GetSerial", "GetSteamHandle",
    "GetPlayerIndex", "SetPlayerIndex",
    "GetProperties", "GetJoystick",
    -- Events
    "SetEventsEnabled", "EventsEnabled", "Update",
    -- Strings
    "GetTypeFromString", "GetStringForType",
    "GetButtonFromString", "GetStringForButton",
    "GetAxisFromString",  "GetStringForAxis",
    "GetButtonLabel", "GetButtonLabelForType",
    -- Touchpad
    "GetNumTouchpads", "GetNumTouchpadFingers", "GetTouchpadFinger",
    -- Sensor
    "HasSensor", "SetSensorEnabled", "SensorEnabled",
    "GetSensorDataRate", "GetSensorData",
    -- Effects
    "RumbleTriggers", "SetLED", "SendEffect",
}
local aj_count = 0
for _, k in ipairs(AJ_FNS) do
    if type(mod[k]) ~= "function" then
        fail("Light.Gamepad." .. k .. " missing or not a function (Phase AJ)")
    end
    aj_count = aj_count + 1
end
pass("Light.Gamepad Phase AJ fns ok (" .. aj_count .. " functions)")

-- ------------------------------------------------------------------
-- 常量校验
-- ------------------------------------------------------------------
local CONSTS = {
    "TYPE_UNKNOWN", "TYPE_STANDARD", "TYPE_XBOX360", "TYPE_XBOXONE",
    "TYPE_PS3", "TYPE_PS4", "TYPE_PS5",
    "TYPE_NINTENDO_SWITCH_PRO",
    "TYPE_NINTENDO_SWITCH_JOYCON_LEFT", "TYPE_NINTENDO_SWITCH_JOYCON_RIGHT",
    "TYPE_NINTENDO_SWITCH_JOYCON_PAIR",
    -- TYPE_GAMECUBE: SDL3 v3.2.30 未包含, 已省略
    "BUTTON_SOUTH", "BUTTON_EAST", "BUTTON_WEST", "BUTTON_NORTH",
    "BUTTON_BACK", "BUTTON_GUIDE", "BUTTON_START",
    "BUTTON_LEFT_STICK", "BUTTON_RIGHT_STICK",
    "BUTTON_LEFT_SHOULDER", "BUTTON_RIGHT_SHOULDER",
    "BUTTON_DPAD_UP", "BUTTON_DPAD_DOWN", "BUTTON_DPAD_LEFT", "BUTTON_DPAD_RIGHT",
    "BUTTON_MISC1", "BUTTON_MISC2", "BUTTON_MISC3",
    "BUTTON_MISC4", "BUTTON_MISC5", "BUTTON_MISC6",
    "BUTTON_RIGHT_PADDLE1", "BUTTON_LEFT_PADDLE1",
    "BUTTON_RIGHT_PADDLE2", "BUTTON_LEFT_PADDLE2", "BUTTON_TOUCHPAD",
    "AXIS_LEFTX", "AXIS_LEFTY", "AXIS_RIGHTX", "AXIS_RIGHTY",
    "AXIS_LEFT_TRIGGER", "AXIS_RIGHT_TRIGGER",
    "BUTTON_LABEL_UNKNOWN", "BUTTON_LABEL_A", "BUTTON_LABEL_B",
    "BUTTON_LABEL_X", "BUTTON_LABEL_Y",
    "BUTTON_LABEL_CROSS", "BUTTON_LABEL_CIRCLE",
    "BUTTON_LABEL_SQUARE", "BUTTON_LABEL_TRIANGLE",
    "CONNECTION_INVALID", "CONNECTION_UNKNOWN", "CONNECTION_WIRED", "CONNECTION_WIRELESS",
    "AXIS_MAX", "AXIS_MIN",
}
for _, k in ipairs(CONSTS) do
    if type(mod[k]) ~= "number" then
        fail("Light.Gamepad." .. k .. " missing or not a number constant")
    end
end
if mod.AXIS_MAX ~= 32767 then fail("AXIS_MAX must be 32767") end
if mod.AXIS_MIN ~= -32768 then fail("AXIS_MIN must be -32768") end
pass("Light.Gamepad constants ok (" .. #CONSTS .. " consts)")

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

-- ------------------------------------------------------------------
-- Phase AJ: Discovery (no handle) 边界
-- ------------------------------------------------------------------
if type(mod.HasGamepad()) ~= "boolean" then fail("HasGamepad must return bool") end
pass("Light.Gamepad.HasGamepad ok: " .. tostring(mod.HasGamepad()))

if mod.IsGamepad(0) ~= false then fail("IsGamepad(0) should be false") end
pass("Light.Gamepad.IsGamepad(0) = false ok")

-- ID-based queries on invalid id 0: should return nil/0/-1/empty-string-guid (无 handle err)
if mod.GetNameForID(0)           ~= nil then fail("GetNameForID(0) should be nil") end
if mod.GetPathForID(0)           ~= nil then fail("GetPathForID(0) should be nil") end
if mod.GetPlayerIndexForID(0)    ~= -1  then fail("GetPlayerIndexForID(0) should be -1") end
if mod.GetVendorForID(0)         ~= 0   then fail("GetVendorForID(0) should be 0") end
if mod.GetProductForID(0)        ~= 0   then fail("GetProductForID(0) should be 0") end
if mod.GetProductVersionForID(0) ~= 0   then fail("GetProductVersionForID(0) should be 0") end
local guid_zero = mod.GetGUIDForID(0)
if type(guid_zero) ~= "string" or #guid_zero ~= 32 then
    fail("GetGUIDForID(0) should be 32-char zero-guid string, got " .. tostring(guid_zero))
end
pass("Light.Gamepad ForID(0) boundary ok (guid='" .. guid_zero .. "')")

-- ------------------------------------------------------------------
-- Phase AJ: Mapping
-- ------------------------------------------------------------------
local mappings, merr = mod.GetMappings()
if mappings == nil then
    pass("GetMappings returned nil: " .. tostring(merr))
else
    if type(mappings) ~= "table" then fail("GetMappings must return table") end
    pass("Light.Gamepad.GetMappings count=" .. tostring(#mappings))
end

-- AddMapping 接受 invalid 字符串应返回 -1 + err
local ar, aerr = mod.AddMapping("THIS_IS_NOT_A_VALID_MAPPING")
if ar ~= -1 or aerr == nil then fail("AddMapping(garbage) should return -1 + err") end
pass("Light.Gamepad.AddMapping(garbage) = -1 ok: " .. tostring(aerr))

-- AddMappingsFromFile 不存在路径应返回 -1 + err
local fr, ferr = mod.AddMappingsFromFile("/nonexistent_path_xyz.txt")
if fr ~= -1 or ferr == nil then fail("AddMappingsFromFile(nonexistent) should return -1 + err") end
pass("Light.Gamepad.AddMappingsFromFile(missing) = -1 ok")

-- GetMapping(nil) 边界
local mh, mhe = mod.GetMapping(nil)
if mh ~= nil or mhe == nil then fail("GetMapping(nil) should be nil + err") end
pass("Light.Gamepad.GetMapping(nil) boundary ok")

-- GetMappingForGUID(zero-guid): SDL3 可能返回 nil + err
local g_map, g_err = mod.GetMappingForGUID(guid_zero)
if g_map ~= nil and type(g_map) ~= "string" then
    fail("GetMappingForGUID return type wrong")
end
pass("Light.Gamepad.GetMappingForGUID(zero) ok: " .. tostring(g_map or g_err))

-- ------------------------------------------------------------------
-- Phase AJ: Lookup
-- ------------------------------------------------------------------
if mod.GetGamepadFromID(0) ~= nil then fail("GetGamepadFromID(0) should be nil") end
if mod.GetGamepadFromPlayerIndex(99) ~= nil then fail("GetGamepadFromPlayerIndex(99) should be nil") end
pass("Light.Gamepad.Get*From* invalid lookup ok")

-- ------------------------------------------------------------------
-- Phase AJ: Per-handle (nil) 边界
-- ------------------------------------------------------------------
local p, pe = mod.GetPath(nil)
if p ~= nil or pe == nil then fail("GetPath(nil) should be nil + err") end
if mod.GetRealType(nil)        ~= 0  then fail("GetRealType(nil) should be 0 (TYPE_UNKNOWN)") end
if mod.GetVendor(nil)          ~= 0  then fail("GetVendor(nil) should be 0") end
if mod.GetProduct(nil)         ~= 0  then fail("GetProduct(nil) should be 0") end
if mod.GetProductVersion(nil)  ~= 0  then fail("GetProductVersion(nil) should be 0") end
if mod.GetFirmwareVersion(nil) ~= 0  then fail("GetFirmwareVersion(nil) should be 0") end
if mod.GetSerial(nil)          ~= nil then fail("GetSerial(nil) should be nil") end
if mod.GetSteamHandle(nil)     ~= 0  then fail("GetSteamHandle(nil) should be 0") end
if mod.GetPlayerIndex(nil)     ~= -1 then fail("GetPlayerIndex(nil) should be -1") end
local sp, spe = mod.SetPlayerIndex(nil, 0)
if sp ~= false or spe == nil then fail("SetPlayerIndex(nil) should be false + err") end
if mod.GetProperties(nil)      ~= 0  then fail("GetProperties(nil) should be 0") end
if mod.GetJoystick(nil)        ~= nil then fail("GetJoystick(nil) should be nil") end
pass("Light.Gamepad per-handle (nil) boundary ok")

-- ------------------------------------------------------------------
-- Phase AJ: Events (no-op safe)
-- ------------------------------------------------------------------
if type(mod.EventsEnabled()) ~= "boolean" then fail("EventsEnabled must return bool") end
mod.SetEventsEnabled(true)
mod.SetEventsEnabled(false)
mod.Update()
pass("Light.Gamepad events api callable ok")

-- ------------------------------------------------------------------
-- Phase AJ: Type/Button/Axis string conversion
-- ------------------------------------------------------------------
-- Type 双向转换
if mod.GetTypeFromString("xbox360") ~= mod.TYPE_XBOX360 then
    fail("GetTypeFromString('xbox360') should == TYPE_XBOX360")
end
if mod.GetStringForType(mod.TYPE_XBOX360) ~= "xbox360" then
    fail("GetStringForType(TYPE_XBOX360) should == 'xbox360'")
end
pass("Light.Gamepad TYPE string roundtrip ok")

-- Button 双向转换 — SDL3 v3.2.30 内部 mapping 字符串用 legacy ABXY 名 ("a"/"b"/"x"/"y")
-- 而 BUTTON_SOUTH/EAST/WEST/NORTH 是面向应用的 layout-neutral 命名
-- 因此 "a" -> BUTTON_SOUTH (位置等价); "south" 不被 SDL3 识别 (返回 -1)
if mod.GetButtonFromString("a") ~= mod.BUTTON_SOUTH then
    fail("GetButtonFromString('a') should == BUTTON_SOUTH (SDL3 mapping uses legacy ABXY)")
end
if mod.GetButtonFromString("b") ~= mod.BUTTON_EAST then
    fail("GetButtonFromString('b') should == BUTTON_EAST")
end
local btn_str = mod.GetStringForButton(mod.BUTTON_SOUTH)
if btn_str == nil then fail("GetStringForButton(BUTTON_SOUTH) should not be nil") end
-- SDL3 v3.2.30 GetStringForButton 返回 "a" (legacy mapping name)
pass("Light.Gamepad BUTTON string roundtrip ok ('a' <-> SOUTH; '" .. btn_str .. "')")

-- Axis 双向转换
if mod.GetAxisFromString("leftx") ~= mod.AXIS_LEFTX then
    fail("GetAxisFromString('leftx') should == AXIS_LEFTX")
end
if mod.GetStringForAxis(mod.AXIS_LEFTX) == nil then
    fail("GetStringForAxis(AXIS_LEFTX) should not be nil")
end
pass("Light.Gamepad AXIS string roundtrip ok")

-- 无效字符串应返回 invalid (-1 / "invalid"-equivalent)
if mod.GetTypeFromString("__no_such_type__") == mod.TYPE_XBOX360 then
    fail("GetTypeFromString('__nope__') should not be TYPE_XBOX360")
end
if mod.GetButtonFromString("__no_such_button__") ~= -1 then
    fail("GetButtonFromString('__nope__') should be -1 (BUTTON_INVALID)")
end
if mod.GetAxisFromString("__no_such_axis__") ~= -1 then
    fail("GetAxisFromString('__nope__') should be -1 (AXIS_INVALID)")
end
pass("Light.Gamepad invalid string -> invalid enum ok")

-- GetButtonLabel(nil, name) -> BUTTON_LABEL_UNKNOWN
-- Note: SDL3 mapping 字符串用 legacy ABXY ('a'/'b'/'x'/'y'), 因此用 'a' 而非 'south'
if mod.GetButtonLabel(nil, "a") ~= mod.BUTTON_LABEL_UNKNOWN then
    fail("GetButtonLabel(nil) should be BUTTON_LABEL_UNKNOWN")
end
if mod.GetButtonLabelForType(mod.TYPE_PS5, "a") == mod.BUTTON_LABEL_UNKNOWN then
    -- PS5 的 SOUTH (= legacy 'a' position) 应被映射为 CROSS, 不应是 UNKNOWN
    fail("GetButtonLabelForType(PS5, 'a') should not be UNKNOWN")
end
pass("Light.Gamepad BUTTON_LABEL ok")

-- ------------------------------------------------------------------
-- Phase AJ: Touchpad (nil) 边界
-- ------------------------------------------------------------------
if mod.GetNumTouchpads(nil)         ~= 0 then fail("GetNumTouchpads(nil) should be 0") end
if mod.GetNumTouchpadFingers(nil, 0) ~= 0 then fail("GetNumTouchpadFingers(nil) should be 0") end
local fres, ferr2 = mod.GetTouchpadFinger(nil, 0, 0)
if fres ~= nil or ferr2 == nil then fail("GetTouchpadFinger(nil) should be nil + err") end
pass("Light.Gamepad touchpad (nil) boundary ok")

-- ------------------------------------------------------------------
-- Phase AJ: Sensor (nil) 边界
-- ------------------------------------------------------------------
if mod.HasSensor(nil, "accel") ~= false then fail("HasSensor(nil) should be false") end
if mod.SensorEnabled(nil, "accel") ~= false then fail("SensorEnabled(nil) should be false") end
if mod.GetSensorDataRate(nil, "accel") ~= 0 then fail("GetSensorDataRate(nil) should be 0") end
local seok, seerr = mod.SetSensorEnabled(nil, "accel", true)
if seok ~= false or seerr == nil then fail("SetSensorEnabled(nil) should be false + err") end
local sd, sderr = mod.GetSensorData(nil, "accel", 3)
if sd ~= nil or sderr == nil then fail("GetSensorData(nil) should be nil + err") end
-- 无效 sensor_type 路径
if mod.HasSensor(nil, "__bad__") ~= false then fail("HasSensor(nil, bad_type) should be false") end
pass("Light.Gamepad sensor (nil) boundary ok")

-- ------------------------------------------------------------------
-- Phase AJ: Effects (nil) 边界
-- ------------------------------------------------------------------
local rt, rte = mod.RumbleTriggers(nil, 0, 0, 100)
if rt ~= false or rte == nil then fail("RumbleTriggers(nil) should be false + err") end

local lr, lre = mod.SetLED(nil, 0, 0, 0)
if lr ~= false or lre == nil then fail("SetLED(nil) should be false + err") end

local sr, sre = mod.SendEffect(nil, "x")
if sr ~= false or sre == nil then fail("SendEffect(nil) should be false + err") end
pass("Light.Gamepad effects (nil) boundary ok")

print("gamepad smoke ok (Phase G + Phase AJ)")
