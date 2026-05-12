-- Phase E.8 smoke: Light.Graphics.SSAO
--
-- API coverage (19):
--   Lifecycle 5:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
--   AutoEnable 2: SetAutoEnable / GetAutoEnable
--   Params 12 (6 pairs):
--     SetRadius / GetRadius                  (float [0.05, 5.0], default 0.5)
--     SetBias / GetBias                      (float [0, 0.2],    default 0.025)
--     SetIntensity / GetIntensity            (float [0, 4.0],    default 1.0)
--     SetKernelSize / GetKernelSize          (int {8, 16},       default 16)
--     SetPower / GetPower                    (float [0.5, 8.0],  default 2.0)
--     SetBlurEnabled / GetBlurEnabled        (bool,              default true)
--
-- Headless tolerant; ASCII-only.

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local S = Graphics.SSAO
if type(S) ~= "table" then fail("SSAO subtable missing (got " .. type(S) .. ")") end
pass("Light.Graphics.SSAO subtable present")

-- ============================================================
-- A) Surface (19 functions)
-- ============================================================

local fns = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetRadius", "GetRadius",
    "SetBias", "GetBias",
    "SetIntensity", "GetIntensity",
    "SetKernelSize", "GetKernelSize",
    "SetPower", "GetPower",
    "SetBlurEnabled", "GetBlurEnabled",
}
for _, k in ipairs(fns) do
    if type(S[k]) ~= "function" then
        fail("SSAO." .. k .. " missing (got " .. type(S[k]) .. ")")
    end
end
pass("SSAO module surface ok (" .. #fns .. " functions)")

-- ============================================================
-- B) IsSupported / IsEnabled initial state
-- ============================================================

local sup = S.IsSupported()
if type(sup) ~= "boolean" then fail("IsSupported not boolean") end
pass("S.IsSupported = " .. tostring(sup))

if S.IsEnabled() ~= false then fail("Initial IsEnabled must be false") end
pass("Initial IsEnabled() == false")

-- ============================================================
-- C) AutoEnable getter/setter (default false)
-- ============================================================

if S.GetAutoEnable() ~= false then fail("Default GetAutoEnable must be false") end
pass("Default GetAutoEnable() == false")

S.SetAutoEnable(true)
if S.GetAutoEnable() ~= true then fail("SetAutoEnable(true) round-trip failed") end
pass("SetAutoEnable(true) round-trip ok")

S.SetAutoEnable(false)
if S.GetAutoEnable() ~= false then fail("SetAutoEnable(false) round-trip failed") end
pass("SetAutoEnable(false) round-trip ok")

-- ============================================================
-- D) Default parameter values
-- ============================================================

local r = S.GetRadius()
if math.abs(r - 0.5) > 1e-4 then fail("Default Radius != 0.5 (got " .. tostring(r) .. ")") end
pass("Default Radius == 0.5")

local b = S.GetBias()
if math.abs(b - 0.025) > 1e-4 then fail("Default Bias != 0.025 (got " .. tostring(b) .. ")") end
pass("Default Bias == 0.025")

local i = S.GetIntensity()
if math.abs(i - 1.0) > 1e-4 then fail("Default Intensity != 1.0 (got " .. tostring(i) .. ")") end
pass("Default Intensity == 1.0")

local k = S.GetKernelSize()
if k ~= 16 then fail("Default KernelSize != 16 (got " .. tostring(k) .. ")") end
pass("Default KernelSize == 16")

local p = S.GetPower()
if math.abs(p - 2.0) > 1e-4 then fail("Default Power != 2.0 (got " .. tostring(p) .. ")") end
pass("Default Power == 2.0")

if S.GetBlurEnabled() ~= true then fail("Default BlurEnabled != true") end
pass("Default BlurEnabled == true")

-- ============================================================
-- E) Param Set/Get round-trip
-- ============================================================

S.SetRadius(1.5)
if math.abs(S.GetRadius() - 1.5) > 1e-4 then fail("SetRadius round-trip") end
pass("SetRadius(1.5) round-trip ok")

S.SetBias(0.05)
if math.abs(S.GetBias() - 0.05) > 1e-4 then fail("SetBias round-trip") end
pass("SetBias(0.05) round-trip ok")

S.SetIntensity(2.0)
if math.abs(S.GetIntensity() - 2.0) > 1e-4 then fail("SetIntensity round-trip") end
pass("SetIntensity(2.0) round-trip ok")

S.SetKernelSize(8)
if S.GetKernelSize() ~= 8 then fail("SetKernelSize(8) round-trip") end
pass("SetKernelSize(8) round-trip ok")

S.SetKernelSize(16)
if S.GetKernelSize() ~= 16 then fail("SetKernelSize(16) round-trip") end
pass("SetKernelSize(16) round-trip ok")

S.SetPower(4.0)
if math.abs(S.GetPower() - 4.0) > 1e-4 then fail("SetPower round-trip") end
pass("SetPower(4.0) round-trip ok")

S.SetBlurEnabled(false)
if S.GetBlurEnabled() ~= false then fail("SetBlurEnabled(false) round-trip") end
pass("SetBlurEnabled(false) round-trip ok")

S.SetBlurEnabled(true)
if S.GetBlurEnabled() ~= true then fail("SetBlurEnabled(true) round-trip") end
pass("SetBlurEnabled(true) round-trip ok")

-- ============================================================
-- F) Param clamping
-- ============================================================

-- 注: C++ float 转 Lua double 有精度损失 (例 0.05f -> 0.050000000745058), 用 epsilon 比较
local EPS = 1e-4
local function near(a, b) return math.abs(a - b) <= EPS end

S.SetRadius(-5.0)   -- below min
if not near(S.GetRadius(), 0.05) then fail("SetRadius(-5) clamp to 0.05, got " .. tostring(S.GetRadius())) end
pass("SetRadius(-5) -> clamp 0.05")

S.SetRadius(100.0)  -- above max
if not near(S.GetRadius(), 5.0) then fail("SetRadius(100) clamp to 5.0, got " .. tostring(S.GetRadius())) end
pass("SetRadius(100) -> clamp 5.0")

S.SetBias(-1.0)
if not near(S.GetBias(), 0.0) then fail("SetBias(-1) clamp to 0") end
pass("SetBias(-1) -> clamp 0.0")

S.SetBias(5.0)
if not near(S.GetBias(), 0.2) then fail("SetBias(5) clamp to 0.2") end
pass("SetBias(5) -> clamp 0.2")

S.SetIntensity(-1.0)
if not near(S.GetIntensity(), 0.0) then fail("SetIntensity(-1) clamp to 0") end
pass("SetIntensity(-1) -> clamp 0.0")

S.SetIntensity(100.0)
if not near(S.GetIntensity(), 4.0) then fail("SetIntensity(100) clamp to 4.0") end
pass("SetIntensity(100) -> clamp 4.0")

S.SetPower(0.01)
if not near(S.GetPower(), 0.5) then fail("SetPower(0.01) clamp to 0.5") end
pass("SetPower(0.01) -> clamp 0.5")

S.SetPower(100.0)
if not near(S.GetPower(), 8.0) then fail("SetPower(100) clamp to 8.0") end
pass("SetPower(100) -> clamp 8.0")

-- KernelSize special: only 8 or 16 (split at 12)
S.SetKernelSize(4)    -- < 12 -> snap to 8
if S.GetKernelSize() ~= 8 then fail("SetKernelSize(4) snap to 8") end
pass("SetKernelSize(4) -> snap 8")

S.SetKernelSize(12)   -- == 12 -> snap to 8 (per impl: n <= 12)
if S.GetKernelSize() ~= 8 then fail("SetKernelSize(12) snap to 8") end
pass("SetKernelSize(12) -> snap 8")

S.SetKernelSize(13)   -- > 12 -> snap to 16
if S.GetKernelSize() ~= 16 then fail("SetKernelSize(13) snap to 16") end
pass("SetKernelSize(13) -> snap 16")

S.SetKernelSize(64)   -- way over -> snap 16
if S.GetKernelSize() ~= 16 then fail("SetKernelSize(64) snap to 16") end
pass("SetKernelSize(64) -> snap 16")

-- ============================================================
-- G) Restore mid-range values
-- ============================================================

S.SetRadius(0.5)
S.SetBias(0.025)
S.SetIntensity(1.0)
S.SetKernelSize(16)
S.SetPower(2.0)
S.SetBlurEnabled(true)
pass("All params restored to defaults")

-- ============================================================
-- H) Enable / Resize / Disable lifecycle (headless tolerant)
-- ============================================================

local enabled = S.Enable(800, 600)
if type(enabled) ~= "boolean" then fail("Enable not boolean") end
pass("S.Enable(800, 600) returned " .. tostring(enabled))

if enabled then
    if S.IsEnabled() ~= true then fail("IsEnabled stays false after Enable") end
    pass("IsEnabled() == true after Enable")

    -- Resize same size (fast path)
    if S.Resize(800, 600) ~= true then fail("Resize same size") end
    pass("Resize(800,600) same size ok")

    -- Resize new size
    if S.Resize(1024, 768) ~= true then fail("Resize new size") end
    pass("Resize(1024,768) new size ok")

    -- Disable
    S.Disable()
    if S.IsEnabled() ~= false then fail("IsEnabled stays true after Disable") end
    pass("Enable/Resize/Disable lifecycle ok (live backend)")
else
    pass("S.Enable returned false (headless), IsEnabled stays false")
end
S.Disable()  -- idempotent
S.Disable()
pass("Double Disable safe")

-- ============================================================
-- I) Boundary cases (blur off, kernelSize=8)
-- ============================================================

S.SetBlurEnabled(false)
S.SetKernelSize(8)
if S.GetBlurEnabled() ~= false or S.GetKernelSize() ~= 8 then
    fail("Boundary blur=false / kernel=8 not preserved")
end
pass("Boundary blur=false + kernel=8 preserved (low-spec config)")

-- Restore
S.SetBlurEnabled(true)
S.SetKernelSize(16)

print("[OK] Phase E.8 smoke (Light.Graphics.SSAO): all checks passed")
