-- Phase Q smoke: Light.Log (SDL_log.h)
--
-- ASCII-only per Windows CI convention.
-- Strategy: SDL_log writes to stderr (or platform-specific sink) but we
-- can fully verify the API surface by:
--   1. round-trip Get/SetPriority on a custom category that doesn't
--      affect existing engine logs
--   2. SetPriorities(CRITICAL) + Reset to confirm bulk + reset paths run
--   3. Boundary checks for invalid priority / negative category
--   4. Invoke every emit function at TRACE priority (filtered out by
--      default, so output stays clean) on a custom category set to
--      CRITICAL so messages are silent
--   5. SetPriorityPrefix round-trip with nil prefix

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Log")
if not ok then fail("require(Light.Log) failed: " .. tostring(mod)) end

-- 1) 14 fns
for _, k in ipairs({
    "GetPriority", "SetPriority", "SetPriorities", "ResetPriorities",
    "SetPriorityPrefix",
    "Log", "LogMessage",
    "LogTrace", "LogVerbose", "LogDebug",
    "LogInfo", "LogWarn", "LogError", "LogCritical",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Log." .. k .. " missing") end
end
pass("Light.Log module ok (14 functions)")

-- 2) constants
assert(mod.PRIORITY_INVALID  == 0, "PRIORITY_INVALID")
assert(mod.PRIORITY_TRACE    == 1, "PRIORITY_TRACE")
assert(mod.PRIORITY_VERBOSE  == 2, "PRIORITY_VERBOSE")
assert(mod.PRIORITY_DEBUG    == 3, "PRIORITY_DEBUG")
assert(mod.PRIORITY_INFO     == 4, "PRIORITY_INFO")
assert(mod.PRIORITY_WARN     == 5, "PRIORITY_WARN")
assert(mod.PRIORITY_ERROR    == 6, "PRIORITY_ERROR")
assert(mod.PRIORITY_CRITICAL == 7, "PRIORITY_CRITICAL")
pass("Light.Log priority constants ok (8)")

assert(mod.CATEGORY_APPLICATION == 0,  "CATEGORY_APPLICATION")
assert(mod.CATEGORY_ERROR       == 1,  "CATEGORY_ERROR")
assert(mod.CATEGORY_ASSERT      == 2,  "CATEGORY_ASSERT")
assert(mod.CATEGORY_SYSTEM      == 3,  "CATEGORY_SYSTEM")
assert(mod.CATEGORY_AUDIO       == 4,  "CATEGORY_AUDIO")
assert(mod.CATEGORY_VIDEO       == 5,  "CATEGORY_VIDEO")
assert(mod.CATEGORY_RENDER      == 6,  "CATEGORY_RENDER")
assert(mod.CATEGORY_INPUT       == 7,  "CATEGORY_INPUT")
assert(mod.CATEGORY_TEST        == 8,  "CATEGORY_TEST")
assert(mod.CATEGORY_GPU         == 9,  "CATEGORY_GPU")
assert(mod.CATEGORY_CUSTOM      == 19, "CATEGORY_CUSTOM")
pass("Light.Log category constants ok (11)")

-- 3) Per-category Get/Set round trip on a USER custom category
-- We use CUSTOM+5 to avoid interfering with engine internal subsystems
-- and to leave a wide gap from CATEGORY_CUSTOM itself.
local USER_CAT = mod.CATEGORY_CUSTOM + 5

-- Save current priority so we can restore at end
local saved = mod.GetPriority(USER_CAT)
assert(type(saved) == "number", "GetPriority must return number")

-- Set CRITICAL so emit path below is silent
mod.SetPriority(USER_CAT, mod.PRIORITY_CRITICAL)
assert(mod.GetPriority(USER_CAT) == mod.PRIORITY_CRITICAL,
       "SetPriority did not stick: got " .. tostring(mod.GetPriority(USER_CAT)))

mod.SetPriority(USER_CAT, mod.PRIORITY_ERROR)
assert(mod.GetPriority(USER_CAT) == mod.PRIORITY_ERROR,
       "SetPriority change did not stick")
pass("Get/SetPriority round-trip ok (custom user category)")

-- 4) SetPriorities + ResetPriorities (just exercise the call paths;
--    don't assert per-category state because engine code may have
--    locked specific categories during init).
mod.SetPriorities(mod.PRIORITY_CRITICAL)
assert(mod.GetPriority(USER_CAT) == mod.PRIORITY_CRITICAL,
       "SetPriorities did not propagate to user category")
mod.ResetPriorities()
pass("SetPriorities + ResetPriorities path ok")

-- After reset, set our custom category to CRITICAL so emit-path tests
-- below are quiet.
mod.SetPriority(USER_CAT, mod.PRIORITY_CRITICAL)

-- 5) SetPriorityPrefix - empty/nil prefix is valid (means "no prefix")
local pfx_ok, pfx_err = mod.SetPriorityPrefix(mod.PRIORITY_INFO, nil)
assert(pfx_ok, "SetPriorityPrefix(INFO, nil) failed: " .. tostring(pfx_err))
local pfx_ok2, pfx_err2 = mod.SetPriorityPrefix(mod.PRIORITY_INFO, "")
assert(pfx_ok2, "SetPriorityPrefix(INFO, '') failed: " .. tostring(pfx_err2))
pass("SetPriorityPrefix (nil + empty) ok")

-- 6) Emit each level on the (silenced) custom category so format-string
--    hardening is exercised. Includes literal '%' to verify it does NOT
--    reach printf (would crash if it did).
local naughty = "user-payload with 100% literal %s and %d"
mod.LogTrace(USER_CAT, naughty)
mod.LogVerbose(USER_CAT, naughty)
mod.LogDebug(USER_CAT, naughty)
mod.LogInfo(USER_CAT, naughty)
mod.LogWarn(USER_CAT, naughty)
mod.LogError(USER_CAT, naughty)
mod.LogCritical(USER_CAT, naughty)
mod.LogMessage(USER_CAT, mod.PRIORITY_CRITICAL, naughty)
mod.Log(naughty)  -- APPLICATION/INFO; visible if APPLICATION isn't silenced
pass("Emit levels (with embedded '%') exercised without crash")

-- 7) Boundary errors
local err_ok = pcall(mod.SetPriority, USER_CAT, 99)
if err_ok then fail("SetPriority(99) should raise") end
pass("SetPriority(99) boundary raises ok")

local err_ok2 = pcall(mod.SetPriority, -1, mod.PRIORITY_INFO)
if err_ok2 then fail("SetPriority(-1, ...) should raise") end
pass("SetPriority(-1) boundary raises ok")

local err_ok3 = pcall(mod.GetPriority, -7)
if err_ok3 then fail("GetPriority(-7) should raise") end
pass("GetPriority(-7) boundary raises ok")

-- nil msg should raise via luaL_checkstring
local err_ok4 = pcall(mod.Log, nil)
if err_ok4 then fail("Log(nil) should raise") end
pass("Log(nil) boundary raises ok")

-- restore (be polite to whatever else might inspect log state)
mod.SetPriority(USER_CAT, saved)

print("log smoke ok")
