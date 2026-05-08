-- Phase U smoke: Light.CPUInfo (SDL_cpuinfo.h)
--
-- ASCII-only per Windows CI convention.
-- All queries are CPU-feature probes; no SDL_Init required.
-- Hardware varies across runners so we only assert TYPES + SANITY ranges,
-- not specific feature presence.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.CPUInfo")
if not ok then fail("require(Light.CPUInfo) failed: " .. tostring(mod)) end

-- 1) 18 fns
for _, k in ipairs({
    "GetNumLogicalCPUCores", "GetCPUCacheLineSize", "GetSystemRAM", "GetSIMDAlignment",
    "HasAltiVec", "HasMMX",
    "HasSSE", "HasSSE2", "HasSSE3", "HasSSE41", "HasSSE42",
    "HasAVX", "HasAVX2", "HasAVX512F",
    "HasARMSIMD", "HasNEON",
    "HasLSX", "HasLASX",
}) do
    if type(mod[k]) ~= "function" then fail("Light.CPUInfo." .. k .. " missing") end
end
pass("Light.CPUInfo module ok (18 functions)")

-- 2) Constant
assert(mod.CACHELINE_SIZE == 128, "CACHELINE_SIZE must be 128, got " .. tostring(mod.CACHELINE_SIZE))
pass("CACHELINE_SIZE constant ok")

-- 3) Hardware integer queries - sanity ranges
local cores = mod.GetNumLogicalCPUCores()
assert(type(cores) == "number" and cores >= 1 and cores <= 4096,
       "GetNumLogicalCPUCores out of sane range [1..4096]: " .. tostring(cores))
pass(string.format("GetNumLogicalCPUCores ok: %d", cores))

local cls = mod.GetCPUCacheLineSize()
-- Real cache lines are 32 / 64 / 128 / 256 bytes typically.
assert(type(cls) == "number" and cls > 0 and cls <= 1024,
       "GetCPUCacheLineSize out of range: " .. tostring(cls))
pass(string.format("GetCPUCacheLineSize ok: %d bytes", cls))

local ram = mod.GetSystemRAM()
-- CI runners typically have 4-16 GB.
assert(type(ram) == "number" and ram > 0 and ram < (1024 * 1024),
       "GetSystemRAM out of range [1..1TB]: " .. tostring(ram))
pass(string.format("GetSystemRAM ok: %d MB", ram))

local simd = mod.GetSIMDAlignment()
-- SIMD alignment must be a power of 2 in the range [4..256].
assert(type(simd) == "number" and simd >= 4 and simd <= 256
       and (simd == 4 or simd == 8 or simd == 16 or simd == 32
            or simd == 64 or simd == 128 or simd == 256),
       "GetSIMDAlignment must be power-of-2 in [4..256]: " .. tostring(simd))
pass(string.format("GetSIMDAlignment ok: %d bytes", simd))

-- 4) All Has* functions return boolean
local feature_names = {
    "HasAltiVec", "HasMMX",
    "HasSSE", "HasSSE2", "HasSSE3", "HasSSE41", "HasSSE42",
    "HasAVX", "HasAVX2", "HasAVX512F",
    "HasARMSIMD", "HasNEON",
    "HasLSX", "HasLASX",
}
local features = {}
for _, n in ipairs(feature_names) do
    local v = mod[n]()
    assert(type(v) == "boolean", n .. " must return boolean, got " .. type(v))
    features[#features + 1] = string.format("%s=%s", n:sub(4), tostring(v))
end
pass("CPU features: " .. table.concat(features, ", "))

-- 5) SSE feature implication: if SSE2 then SSE; if SSE3 then SSE2; etc.
-- This catches a corrupt SDL build that misreports overlapping features.
if mod.HasSSE2() then assert(mod.HasSSE(),  "SSE2 implies SSE") end
if mod.HasSSE3() then assert(mod.HasSSE2(), "SSE3 implies SSE2") end
if mod.HasSSE41() then assert(mod.HasSSE3(), "SSE4.1 implies SSE3") end
if mod.HasSSE42() then assert(mod.HasSSE41(), "SSE4.2 implies SSE4.1") end
if mod.HasAVX2() then assert(mod.HasAVX(),  "AVX2 implies AVX") end
if mod.HasAVX512F() then assert(mod.HasAVX2(), "AVX512F implies AVX2") end
pass("SSE/AVX feature ordering implications hold")

-- 6) Determinism: two consecutive probes must agree
assert(mod.HasSSE2() == mod.HasSSE2(), "HasSSE2 not deterministic")
assert(mod.GetNumLogicalCPUCores() == mod.GetNumLogicalCPUCores(), "core count flapping")
pass("CPU probes deterministic")

print("cpuinfo smoke ok")
