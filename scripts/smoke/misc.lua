-- Phase AA smoke: Light.Misc (SDL_misc.h)
--
-- ASCII-only.
--
-- We deliberately do NOT call OpenURL with a real URL, because SDL on
-- Windows hands the string to ShellExecute which may actually spawn a
-- browser/handler process on the CI runner. The smoke verifies:
--   1) function presence
--   2) Lua-side type guards (raises on non-string / no-arg)
--
-- A guarded call with an empty string is performed in a pcall just to
-- prove the binding exists end-to-end without forcing a successful
-- shell invocation - SDL is free to return either success or failure
-- on an empty URL.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Misc")
if not ok then fail("require(Light.Misc) failed: " .. tostring(mod)) end

-- 1) Function presence
if type(mod.OpenURL) ~= "function" then fail("Light.Misc.OpenURL missing") end
pass("Light.Misc module ok (1 function)")

-- 2) Empty URL - just confirms the C call returns a boolean, no shell side
-- effect on most platforms. The smoke does not require a particular outcome.
local r1, e1 = mod.OpenURL("")
assert(type(r1) == "boolean", "OpenURL must return boolean, got " .. type(r1))
if r1 then
    pass("OpenURL(empty) returned true (binding ok)")
else
    assert(type(e1) == "string", "OpenURL false 2nd return must be string err")
    pass("OpenURL(empty) returned false ok: " .. tostring(e1))
end

-- 3) Boundary: non-coercible argument raises (table)
local rok = pcall(mod.OpenURL, {})
if rok then fail("OpenURL(table) should raise") end
pass("OpenURL(table) raises ok")

-- 4) Boundary: nil argument raises
local rok2 = pcall(mod.OpenURL)
if rok2 then fail("OpenURL() with no arg should raise") end
pass("OpenURL(no arg) raises ok")

print("misc smoke ok")
