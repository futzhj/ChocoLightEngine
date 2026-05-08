-- Phase AI smoke: Light.Joystick (SDL_joystick.h)
-- ASCII-only. CI runners typically have no joystick connected, so this
-- smoke only verifies module shape, constants, and that empty-list paths
-- behave gracefully.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Joystick")
if not ok then fail("require(Light.Joystick) failed: " .. tostring(mod)) end

-- 1) 42 fns
local fn_names = {
    -- Discovery (13)
    "HasJoystick","GetJoysticks",
    "GetJoystickNameForID","GetJoystickPathForID",
    "GetJoystickPlayerIndexForID","GetJoystickVendorForID",
    "GetJoystickProductForID","GetJoystickProductVersionForID",
    "GetJoystickTypeForID","GetJoystickGUIDForID",
    "SetJoystickEventsEnabled","JoystickEventsEnabled","UpdateJoysticks",
    -- Open / lookup / close (4)
    "OpenJoystick","GetJoystickFromID","GetJoystickFromPlayerIndex","CloseJoystick",
    -- Per-handle metadata (16)
    "GetJoystickName","GetJoystickPath","GetJoystickSerial",
    "GetJoystickPlayerIndex","SetJoystickPlayerIndex",
    "GetJoystickVendor","GetJoystickProduct","GetJoystickProductVersion",
    "GetJoystickFirmwareVersion","GetJoystickType","GetJoystickConnectionState",
    "GetJoystickGUID","GetJoystickID","GetJoystickProperties",
    "JoystickConnected","GetJoystickPowerInfo",
    -- Counts (3)
    "GetNumJoystickAxes","GetNumJoystickHats","GetNumJoystickButtons",
    -- Polling (3)
    "GetJoystickAxis","GetJoystickHat","GetJoystickButton",
    -- Effects (3)
    "RumbleJoystick","RumbleJoystickTriggers","SetJoystickLED",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Joystick." .. k .. " missing") end
end
pass("Light.Joystick module ok (" .. #fn_names .. " fns)")

-- 2) Constants
assert(mod.TYPE_UNKNOWN == 0)
assert(type(mod.TYPE_GAMEPAD) == "number")
assert(type(mod.TYPE_WHEEL) == "number")
assert(type(mod.TYPE_ARCADE_STICK) == "number")
assert(type(mod.TYPE_FLIGHT_STICK) == "number")
assert(type(mod.TYPE_DANCE_PAD) == "number")
assert(type(mod.TYPE_GUITAR) == "number")
assert(type(mod.TYPE_DRUM_KIT) == "number")
assert(type(mod.TYPE_ARCADE_PAD) == "number")
assert(type(mod.TYPE_THROTTLE) == "number")

assert(type(mod.CONNECTION_INVALID) == "number")
assert(type(mod.CONNECTION_UNKNOWN) == "number")
assert(type(mod.CONNECTION_WIRED) == "number")
assert(type(mod.CONNECTION_WIRELESS) == "number")

assert(mod.HAT_CENTERED == 0)
assert(mod.HAT_UP == 1)
assert(mod.HAT_RIGHT == 2)
assert(mod.HAT_DOWN == 4)
assert(mod.HAT_LEFT == 8)
assert(mod.HAT_RIGHTUP   == (mod.HAT_RIGHT  + mod.HAT_UP))
assert(mod.HAT_RIGHTDOWN == (mod.HAT_RIGHT  + mod.HAT_DOWN))
assert(mod.HAT_LEFTUP    == (mod.HAT_LEFT   + mod.HAT_UP))
assert(mod.HAT_LEFTDOWN  == (mod.HAT_LEFT   + mod.HAT_DOWN))

assert(mod.AXIS_MAX ==  32767)
assert(mod.AXIS_MIN == -32768)
pass("Constants ok (10 TYPE_*, 4 CONNECTION_*, 9 HAT_*, AXIS bounds)")

-- 3) Discovery on a system likely without a joystick must not crash.
local has = mod.HasJoystick()
assert(type(has) == "boolean")
local ids = mod.GetJoysticks()
assert(type(ids) == "table")
pass("HasJoystick=" .. tostring(has) .. ", GetJoysticks count=" .. #ids)

-- 4) Events on/off round-trip
mod.SetJoystickEventsEnabled(true)
assert(mod.JoystickEventsEnabled() == true)
mod.SetJoystickEventsEnabled(false)
assert(mod.JoystickEventsEnabled() == false)
mod.SetJoystickEventsEnabled(true)  -- restore default
pass("SetJoystickEventsEnabled round-trip ok")

-- 5) UpdateJoysticks must not crash even with no devices
mod.UpdateJoysticks()
pass("UpdateJoysticks ok (no-op on empty)")

-- 6) Bogus ID lookups must surface nil/0 gracefully
local bogus_id = 99999999
assert(mod.GetJoystickNameForID(bogus_id) == nil)
assert(mod.GetJoystickPathForID(bogus_id) == nil)
assert(type(mod.GetJoystickVendorForID(bogus_id)) == "number")
local guid = mod.GetJoystickGUIDForID(bogus_id)
assert(type(guid) == "string" and #guid == 32, "GUID length should be 32, got " .. tostring(guid))
pass("Bogus ID lookups graceful ok (GUID='" .. guid .. "')")

-- 7) OpenJoystick on a bogus id should fail cleanly with nil + err
local ok_open, err_open = mod.OpenJoystick(bogus_id)
assert(ok_open == nil and type(err_open) == "string",
       "OpenJoystick(bogus) should return nil + err, got " ..
       tostring(ok_open) .. ", " .. tostring(err_open))
pass("OpenJoystick(bogus) -> nil, '" .. err_open .. "' ok")

-- 8) GetJoystickFrom* on bogus -> nil (no error)
assert(mod.GetJoystickFromID(bogus_id) == nil)
assert(mod.GetJoystickFromPlayerIndex(99) == nil)
pass("GetJoystickFromID/PlayerIndex(bogus) -> nil ok")

-- 9) If a real joystick is connected (rare on CI), exercise its handle path
if #ids > 0 then
    local first_id = ids[1]
    local h, herr = mod.OpenJoystick(first_id)
    if h then
        pass("Real joystick detected: id=" .. tostring(first_id))
        assert(mod.JoystickConnected(h) == true)
        assert(type(mod.GetJoystickName(h) or "") == "string")
        assert(type(mod.GetNumJoystickAxes(h)) == "number")
        assert(type(mod.GetNumJoystickHats(h)) == "number")
        assert(type(mod.GetNumJoystickButtons(h)) == "number")
        local power, percent = mod.GetJoystickPowerInfo(h)
        assert(type(power) == "number" and type(percent) == "number")
        mod.CloseJoystick(h)
        pass("Real joystick handle round-trip ok")
    else
        pass("OpenJoystick on real id failed (treated as soft): " ..
             tostring(herr))
    end
else
    pass("No joystick on this system - handle path skipped (expected on CI)")
end

print("joystick smoke ok")
