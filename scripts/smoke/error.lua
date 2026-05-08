-- Phase V smoke: Light.Error (SDL_error.h)
--
-- ASCII-only. SDL maintains per-thread error string; engine is single-threaded.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Error")
if not ok then fail("require(Light.Error) failed: " .. tostring(mod)) end

-- 1) 6 fns
for _, k in ipairs({"Get", "Set", "Clear", "OutOfMemory", "Unsupported", "InvalidParam"}) do
    if type(mod[k]) ~= "function" then fail("Light.Error." .. k .. " missing") end
end
pass("Light.Error module ok (6 functions)")

-- 2) Set + Get round-trip with plain ASCII
mod.Clear()
local ret = mod.Set("phase v test message")
assert(ret == false, "Set must return false (SDL convention), got " .. tostring(ret))
assert(mod.Get() == "phase v test message",
       "Get round-trip mismatch: '" .. mod.Get() .. "'")
pass("Set + Get round-trip ok")

-- 3) Set must NOT interpret % as printf format
mod.Clear()
mod.Set("100% complete: %d items %s done")
assert(mod.Get() == "100% complete: %d items %s done",
       "% chars must pass through verbatim, got: '" .. mod.Get() .. "'")
pass("Set ignores % printf escapes (safe pass-through)")

-- 4) Clear
mod.Set("dummy")
assert(mod.Get() ~= "", "precondition: error must be non-empty")
local cleared = mod.Clear()
assert(cleared == true, "Clear must return true, got " .. tostring(cleared))
assert(mod.Get() == "", "Clear must zero the error, got: '" .. mod.Get() .. "'")
pass("Clear ok")

-- 5) OutOfMemory standard message
mod.Clear()
local oom_ret = mod.OutOfMemory()
assert(oom_ret == false, "OutOfMemory must return false")
local oom_msg = mod.Get()
assert(#oom_msg > 0 and oom_msg:lower():find("memor"),
       "OutOfMemory message must mention 'memor', got: '" .. oom_msg .. "'")
pass("OutOfMemory ok: " .. oom_msg)

-- 6) Unsupported standard message
mod.Clear()
local us_ret = mod.Unsupported()
assert(us_ret == false, "Unsupported must return false")
local us_msg = mod.Get()
-- SDL formats as "That operation is not supported"
assert(us_msg:find("not supported"),
       "Unsupported message must contain 'not supported', got: '" .. us_msg .. "'")
pass("Unsupported ok: " .. us_msg)

-- 7) InvalidParam includes the named parameter
mod.Clear()
local ip_ret = mod.InvalidParam("widget_id")
assert(ip_ret == false, "InvalidParam must return false")
local ip_msg = mod.Get()
assert(ip_msg:find("widget_id"),
       "InvalidParam message must include parameter name 'widget_id', got: '" .. ip_msg .. "'")
assert(ip_msg:lower():find("invalid"),
       "InvalidParam message must mention 'invalid', got: '" .. ip_msg .. "'")
pass("InvalidParam ok: " .. ip_msg)

-- 8) Empty Set
mod.Clear()
mod.Set("")
-- SDL stores empty as no error - Get returns "" either way
assert(mod.Get() == "", "empty Set keeps Get empty, got '" .. mod.Get() .. "'")
pass("Set('') boundary ok")

-- 9) Long Set (1024 chars) - must not truncate to nothing
mod.Clear()
local long_msg = string.rep("X", 1024)
mod.Set(long_msg)
local got = mod.Get()
-- SDL may internally cap message length but should preserve at least the
-- first chunk; we only assert it's non-empty.
assert(#got > 0, "long Set must keep something readable, got empty")
pass(string.format("long Set (1024 X's) keeps %d chars", #got))

mod.Clear()
print("error smoke ok")
