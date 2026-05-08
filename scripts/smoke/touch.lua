-- Phase M smoke: Light.Touch
--
-- ASCII-only (UTF-8/Chinese broke light.exe on Windows CI).
--
-- IMPORTANT: this smoke does NOT invoke any function that would call
-- into SDL3. On the Windows CI host (no display, no SDL_INIT_VIDEO,
-- session-0 service), the very first SDL_InitSubSystem(EVENTS) +
-- SDL_GetTouchDevices() pair aborts the process before any Lua print
-- reaches stdout.
--
-- Real devices are still covered by the runtime when a window is
-- created (Light.UI.Window does SDL_INIT_VIDEO|EVENTS|GAMEPAD); see
-- samples for that flow.
--
-- This smoke covers what we actually care about in CI:
--   * module loads via require
--   * 4 fns + 2 constants are registered with correct types
--   * Lua-side type guard returns (nil, err) / "invalid" before
--     dispatching to SDL (so a bad call never reaches native code)

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

-- 1) module loads
local ok, mod = pcall(require, "Light.Touch")
if not ok then fail("require(Light.Touch) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Touch not a table") end

-- 2) 4 fn registrations
for _, k in ipairs({
    "GetDevices", "GetDeviceName", "GetDeviceType", "GetFingers",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Touch." .. k .. " missing") end
end
pass("Light.Touch module ok (4 functions)")

-- 3) constants
if type(mod.MOUSE_ID) ~= "number" then fail("Light.Touch.MOUSE_ID missing") end
if type(mod.TOUCH_ID) ~= "number" then fail("Light.Touch.TOUCH_ID missing") end
pass("Light.Touch constants ok (MOUSE_ID=" .. mod.MOUSE_ID
     .. ", TOUCH_ID=" .. mod.TOUCH_ID .. ")")

-- 4) Lua-side type guard (does NOT enter SDL):
--    GetDeviceName / GetFingers reject non-number id with (nil, err)
--    GetDeviceType returns the literal "invalid" for non-number id.

local n1, e1 = mod.GetDeviceName("not a number")
if n1 ~= nil or e1 == nil then fail("GetDeviceName(string) should be nil+err") end
pass("Light.Touch.GetDeviceName(string) boundary ok: " .. tostring(e1))

local f1, e2 = mod.GetFingers("not a number")
if f1 ~= nil or e2 == nil then fail("GetFingers(string) should be nil+err") end
pass("Light.Touch.GetFingers(string) boundary ok: " .. tostring(e2))

local t1 = mod.GetDeviceType("not a number")
if t1 ~= "invalid" then fail("GetDeviceType(string) should return 'invalid', got " .. tostring(t1)) end
pass("Light.Touch.GetDeviceType(non-number) = 'invalid' ok")

-- 5) GetDeviceName(nil) and GetFingers(nil) also rejected
local n2, e3 = mod.GetDeviceName(nil)
if n2 ~= nil or e3 == nil then fail("GetDeviceName(nil) should be nil+err") end
pass("Light.Touch.GetDeviceName(nil) boundary ok: " .. tostring(e3))

local f2, e4 = mod.GetFingers(nil)
if f2 ~= nil or e4 == nil then fail("GetFingers(nil) should be nil+err") end
pass("Light.Touch.GetFingers(nil) boundary ok: " .. tostring(e4))

print("touch smoke ok (Lua-layer only; SDL native paths exercised by samples)")
