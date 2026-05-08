-- Phase N smoke: Light.Hints (SDL3 runtime configuration hints)
--
-- ASCII-only per Windows CI convention.
--
-- Light.Hints wraps the SDL_hints string table; it has no SDL_Init
-- dependency and can be driven freely on headless CI.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

-- 1) module + fn registration
local ok, mod = pcall(require, "Light.Hints")
if not ok then fail("require(Light.Hints) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Hints not a table") end

for _, k in ipairs({
    "Get", "GetBoolean", "Set", "SetWithPriority", "Reset", "ResetAll",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Hints." .. k .. " missing") end
end
pass("Light.Hints module ok (6 functions)")

-- 2) priority constants
assert(mod.PRIORITY_DEFAULT  == 0, "PRIORITY_DEFAULT must be 0")
assert(mod.PRIORITY_NORMAL   == 1, "PRIORITY_NORMAL must be 1")
assert(mod.PRIORITY_OVERRIDE == 2, "PRIORITY_OVERRIDE must be 2")
pass("Light.Hints priority constants ok")

-- 3) set/get round-trip on a synthetic hint name (must not exist before)
local TEST = "LIGHT_TEST_HINT_SMOKE"
mod.Reset(TEST)  -- best-effort clean slate
local before = mod.Get(TEST)
-- SDL returns either nil or empty string when unset; both are acceptable
if before ~= nil and before ~= "" then
    pass("Light.Hints.Get(TEST) initial = '" .. tostring(before) .. "' (residual, will overwrite)")
else
    pass("Light.Hints.Get(TEST) initial = nil")
end

local set_ok, set_err = mod.Set(TEST, "hello")
if not set_ok then fail("Set failed: " .. tostring(set_err)) end
assert(mod.Get(TEST) == "hello", "Get after Set must return 'hello'")
pass("Light.Hints.Set + Get round-trip ok")

-- 4) SetWithPriority - numeric / string priority acceptance
-- Fresh hint for each test to keep priority semantics from interfering
-- with case-insensitivity checks.

mod.Reset(TEST)
assert(mod.SetWithPriority(TEST, "num2", 2), "numeric priority 2 must work")
assert(mod.Get(TEST) == "num2", "Get after numeric-priority set")

mod.Reset(TEST)
assert(mod.SetWithPriority(TEST, "str_override", "override"), "string 'override' must work")
assert(mod.Get(TEST) == "str_override", "Get after string 'override' set")

mod.Reset(TEST)
assert(mod.SetWithPriority(TEST, "str_normal", "NORMAL"), "case-insensitive 'NORMAL' must work")
assert(mod.Get(TEST) == "str_normal", "Get after case-insensitive NORMAL set")

mod.Reset(TEST)
assert(mod.SetWithPriority(TEST, "str_default", "DeFaUlT"), "case-insensitive 'DeFaUlT' must work")
assert(mod.Get(TEST) == "str_default", "Get after case-insensitive DEFAULT set")
pass("Light.Hints.SetWithPriority priority arg acceptance ok")

-- 5) SetWithPriority semantics - NORMAL must NOT override existing OVERRIDE.
-- SDL_SetHintWithPriority returns false when new priority is lower than
-- the current one and silently keeps the existing value.
mod.Reset(TEST)
assert(mod.SetWithPriority(TEST, "kept", 2), "OVERRIDE base set must work")
local blocked_ok, blocked_err = mod.SetWithPriority(TEST, "ignored", 1)
if blocked_ok then fail("NORMAL must NOT override existing OVERRIDE (got ok=true)") end
assert(mod.Get(TEST) == "kept", "OVERRIDE value must remain after blocked NORMAL set")
pass("Light.Hints.SetWithPriority priority blocking ok: " .. tostring(blocked_err))

-- Invalid priority
local bad_ok, bad_err = mod.SetWithPriority(TEST, "x", 99)
if bad_ok or not bad_err then fail("SetWithPriority(99) should fail") end
pass("Light.Hints.SetWithPriority(99) boundary ok: " .. tostring(bad_err))

local bad_ok2, bad_err2 = mod.SetWithPriority(TEST, "x", "invalid_name")
if bad_ok2 or not bad_err2 then fail("SetWithPriority('invalid_name') should fail") end
pass("Light.Hints.SetWithPriority('invalid_name') boundary ok: " .. tostring(bad_err2))

-- 5) Reset then Get
assert(mod.Reset(TEST), "Reset must succeed")
local after = mod.Get(TEST)
if after ~= nil and after ~= "" then
    fail("After Reset, Get must be nil or empty, got '" .. tostring(after) .. "'")
end
pass("Light.Hints.Reset ok (value cleared)")

-- 6) GetBoolean with default fallback
assert(mod.GetBoolean(TEST, true) == true, "default=true when unset must be true")
assert(mod.GetBoolean(TEST, false) == false, "default=false when unset must be false")
mod.Set(TEST, "1")
assert(mod.GetBoolean(TEST, false) == true, "'1' must read as true")
mod.Set(TEST, "0")
assert(mod.GetBoolean(TEST, true) == false, "'0' must read as false")
mod.Reset(TEST)
pass("Light.Hints.GetBoolean semantics ok")

-- 7) Boolean-to-string coercion on Set
mod.Set(TEST, true)
assert(mod.Get(TEST) == "1", "Set(_, true) must store '1'")
mod.Set(TEST, false)
assert(mod.Get(TEST) == "0", "Set(_, false) must store '0'")
mod.Reset(TEST)
pass("Light.Hints.Set boolean coercion ok")

-- 8) ResetAll (smoke only, can't assert global side effect)
mod.ResetAll()
pass("Light.Hints.ResetAll call ok (side effect not asserted)")

-- 9) boundary: nil name should error at luaL_checkstring
local ok1 = pcall(function() mod.Get(nil) end)
if ok1 then fail("Get(nil) must raise (luaL_checkstring)") end
pass("Light.Hints.Get(nil) boundary ok (raises)")

print("hints smoke ok")
