-- Phase X smoke: Light.Version (SDL_version.h)
--
-- ASCII-only. Pure encoded version arithmetic; no SDL_Init required.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Version")
if not ok then fail("require(Light.Version) failed: " .. tostring(mod)) end

-- 1) 4 fns
for _, k in ipairs({"GetVersion", "GetRevision", "AsTuple", "AtLeast"}) do
    if type(mod[k]) ~= "function" then fail("Light.Version." .. k .. " missing") end
end
pass("Light.Version module ok (4 functions)")

-- 2) HEADER_* constants - SDL 3.x at minimum
assert(mod.HEADER_MAJOR == 3, "HEADER_MAJOR must be 3, got " .. tostring(mod.HEADER_MAJOR))
assert(mod.HEADER_MINOR >= 0, "HEADER_MINOR must be >= 0")
assert(mod.HEADER_MICRO >= 0, "HEADER_MICRO must be >= 0")
local expected = mod.HEADER_MAJOR * 1000000 + mod.HEADER_MINOR * 1000 + mod.HEADER_MICRO
assert(mod.HEADER_VERSION == expected,
       string.format("HEADER_VERSION %d != %d*1e6+%d*1e3+%d=%d",
                     mod.HEADER_VERSION, mod.HEADER_MAJOR, mod.HEADER_MINOR, mod.HEADER_MICRO, expected))
pass(string.format("HEADER constants ok: %d.%d.%d (encoded %d)",
                   mod.HEADER_MAJOR, mod.HEADER_MINOR, mod.HEADER_MICRO, mod.HEADER_VERSION))

-- 3) GetVersion - linked runtime version, must be SDL 3.x
local v = mod.GetVersion()
assert(type(v) == "number" and v >= 3000000 and v < 4000000,
       "GetVersion must be in SDL3 range [3000000..4000000), got " .. tostring(v))
pass(string.format("GetVersion ok: encoded=%d", v))

-- 4) GetRevision - opaque string, just must be a string (may be empty in
--    custom builds; SDL official builds embed git short hash).
local rev = mod.GetRevision()
assert(type(rev) == "string", "GetRevision must be string, got " .. type(rev))
pass(string.format("GetRevision ok: '%s' (%d chars)", rev, #rev))

-- 5) AsTuple round-trip on the runtime version
local M, m, p = mod.AsTuple()
assert(M == 3, "AsTuple major must be 3, got " .. tostring(M))
assert(M * 1000000 + m * 1000 + p == v,
       string.format("AsTuple decode mismatch: %d.%d.%d -> %d, want %d",
                     M, m, p, M * 1000000 + m * 1000 + p, v))
pass(string.format("AsTuple ok: %d.%d.%d", M, m, p))

-- 6) AsTuple on explicit encoded value
local aM, am, ap = mod.AsTuple(3 * 1000000 + 2 * 1000 + 7)  -- 3.2.7
assert(aM == 3 and am == 2 and ap == 7,
       string.format("AsTuple(3.2.7) wrong: %d.%d.%d", aM, am, ap))
pass("AsTuple(explicit) ok")

-- 7) AtLeast - logical correctness
assert(mod.AtLeast(3, 0, 0) == true, "AtLeast(3,0,0) must be true")
assert(mod.AtLeast(2, 0, 0) == true, "AtLeast(2,0,0) must be true (linked is SDL3)")
assert(mod.AtLeast(99, 0, 0) == false, "AtLeast(99,0,0) must be false")
assert(mod.AtLeast(M, m, p) == true,
       string.format("AtLeast(self %d.%d.%d) must be true", M, m, p))
-- Just one beyond should fail
assert(mod.AtLeast(M, m, p + 1) == false or mod.AtLeast(M, m + 1, 0) == false
       or mod.AtLeast(M + 1, 0, 0) == false,
       "AtLeast(self+1) must be false (any of micro/minor/major bump)")
pass("AtLeast logical ordering ok")

-- 8) AsTuple(0) edge - all zero
local zM, zm, zp = mod.AsTuple(0)
assert(zM == 0 and zm == 0 and zp == 0, "AsTuple(0) must be 0,0,0")
pass("AsTuple(0) edge ok")

print("version smoke ok")
