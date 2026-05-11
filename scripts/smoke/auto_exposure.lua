-- Phase E.5 smoke: Light.Graphics.AutoExposure (Eye Adaptation surface)
--
-- API coverage (18 functions):
--   Enable / Disable / IsEnabled / IsSupported / Resize
--   SetAutoEnable / GetAutoEnable           (default false, NOT true!)
--   SetTargetEV / GetTargetEV
--   SetSpeedUp / GetSpeedUp
--   SetSpeedDown / GetSpeedDown
--   SetMinEV / GetMinEV
--   SetMaxEV / GetMaxEV
--   GetCurrentEV / GetCurrentExposure / GetMeasuredLuminance
--
-- Headless guard: no GL context at smoke stage, Enable() typically
-- returns false (IsSupported=false). All getters must still return
-- sensible defaults; subsequent calls must be safe (no-op).
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local AE = Graphics.AutoExposure
if type(AE) ~= "table" then
    fail("Light.Graphics.AutoExposure missing or not a table (got " .. type(AE) .. ")")
end
pass("Light.Graphics.AutoExposure subtable present")

-- ============================================================
-- 1) Module surface: 18 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetTargetEV", "GetTargetEV",
    "SetSpeedUp", "GetSpeedUp",
    "SetSpeedDown", "GetSpeedDown",
    "SetMinEV", "GetMinEV",
    "SetMaxEV", "GetMaxEV",
    "GetCurrentEV", "GetCurrentExposure", "GetMeasuredLuminance",
}
for _, k in ipairs(fn_names) do
    if type(AE[k]) ~= "function" then
        fail("Light.Graphics.AutoExposure." .. k .. " missing or not a function (got " .. type(AE[k]) .. ")")
    end
end
pass("Light.Graphics.AutoExposure module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) IsSupported / IsEnabled — initial state probes
-- ============================================================

local is_sup = AE.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = AE.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("Initial IsEnabled() == false")

-- ============================================================
-- 3) AutoEnable flag — default false (DIFFERENT from Bloom default true!)
-- ============================================================

local ae_default = AE.GetAutoEnable()
if type(ae_default) ~= "boolean" then
    fail("GetAutoEnable should return boolean, got " .. type(ae_default))
end
if ae_default ~= false then
    fail("Default GetAutoEnable() must be FALSE (AE has different default than Bloom), got " .. tostring(ae_default))
end
pass("Default GetAutoEnable() == false (AE manual-by-default)")

AE.SetAutoEnable(true)
if AE.GetAutoEnable() ~= true then fail("AutoEnable round-trip true failed") end
AE.SetAutoEnable(false)
if AE.GetAutoEnable() ~= false then fail("AutoEnable round-trip false failed") end
pass("AutoEnable round-trip true/false ok")

-- ============================================================
-- 4) TargetEV — round-trip (no clamp; user offset can be any value)
-- ============================================================

local tev_def = AE.GetTargetEV()
if type(tev_def) ~= "number" then fail("GetTargetEV should return number") end
if tev_def ~= 0.0 then
    fail("Default TargetEV must be 0.0, got " .. tostring(tev_def))
end
pass("Default TargetEV == 0.0")

AE.SetTargetEV(2.5)
if math.abs(AE.GetTargetEV() - 2.5) > 1e-5 then fail("TargetEV round-trip 2.5 failed") end
AE.SetTargetEV(-3.0)
if math.abs(AE.GetTargetEV() - (-3.0)) > 1e-5 then fail("TargetEV round-trip -3.0 failed") end
AE.SetTargetEV(0.0)   -- restore
pass("TargetEV round-trip 2.5/-3.0/0.0 ok")

-- ============================================================
-- 5) SpeedUp — round-trip + clamp [0.1, 20]
-- ============================================================

local su_def = AE.GetSpeedUp()
if type(su_def) ~= "number" then fail("GetSpeedUp should return number") end
if math.abs(su_def - 3.0) > 1e-5 then
    fail("Default SpeedUp must be 3.0, got " .. tostring(su_def))
end
pass("Default SpeedUp == 3.0")

AE.SetSpeedUp(5.0)
if math.abs(AE.GetSpeedUp() - 5.0) > 1e-5 then fail("SpeedUp round-trip 5.0 failed") end

-- clamp lower bound
AE.SetSpeedUp(0.0)
local clamped_lo = AE.GetSpeedUp()
if clamped_lo < 0.1 - 1e-5 then
    fail("SpeedUp(0.0) should clamp >= 0.1, got " .. tostring(clamped_lo))
end
pass("SpeedUp lower clamp ok (" .. tostring(clamped_lo) .. " >= 0.1)")

-- clamp upper bound
AE.SetSpeedUp(100.0)
local clamped_hi = AE.GetSpeedUp()
if clamped_hi > 20.0 + 1e-5 then
    fail("SpeedUp(100.0) should clamp <= 20.0, got " .. tostring(clamped_hi))
end
pass("SpeedUp upper clamp ok (" .. tostring(clamped_hi) .. " <= 20.0)")

AE.SetSpeedUp(3.0)   -- restore default

-- ============================================================
-- 6) SpeedDown — round-trip + clamp [0.1, 20]
-- ============================================================

local sd_def = AE.GetSpeedDown()
if math.abs(sd_def - 1.0) > 1e-5 then
    fail("Default SpeedDown must be 1.0, got " .. tostring(sd_def))
end
pass("Default SpeedDown == 1.0")

AE.SetSpeedDown(2.0)
if math.abs(AE.GetSpeedDown() - 2.0) > 1e-5 then fail("SpeedDown round-trip 2.0 failed") end

AE.SetSpeedDown(-1.0)
if AE.GetSpeedDown() < 0.1 - 1e-5 then fail("SpeedDown lower clamp failed") end
AE.SetSpeedDown(50.0)
if AE.GetSpeedDown() > 20.0 + 1e-5 then fail("SpeedDown upper clamp failed") end

AE.SetSpeedDown(1.0)   -- restore default
pass("SpeedDown round-trip + clamp ok")

-- ============================================================
-- 7) MinEV / MaxEV — round-trip + invariant min <= max
-- ============================================================

local min_def = AE.GetMinEV()
local max_def = AE.GetMaxEV()
if math.abs(min_def - (-8.0)) > 1e-5 then fail("Default MinEV must be -8.0") end
if math.abs(max_def - 8.0) > 1e-5 then fail("Default MaxEV must be 8.0") end
pass("Default MinEV/MaxEV == -8/+8")

AE.SetMinEV(-4.0)
if math.abs(AE.GetMinEV() - (-4.0)) > 1e-5 then fail("MinEV round-trip failed") end
AE.SetMaxEV(6.0)
if math.abs(AE.GetMaxEV() - 6.0) > 1e-5 then fail("MaxEV round-trip failed") end

-- Invariant: SetMinEV beyond MaxEV pushes MaxEV up
AE.SetMinEV(10.0)   -- min > current max=6, max should rise
local min_after = AE.GetMinEV()
local max_after = AE.GetMaxEV()
if min_after > max_after + 1e-5 then
    fail("Invariant min<=max violated: min=" .. tostring(min_after) .. ", max=" .. tostring(max_after))
end
pass("MinEV/MaxEV invariant min<=max ok")

-- restore defaults
AE.SetMinEV(-8.0)
AE.SetMaxEV(8.0)

-- ============================================================
-- 8) Current EV / Exposure / Luminance — debug getters
-- ============================================================

local cur_ev = AE.GetCurrentEV()
local cur_exp = AE.GetCurrentExposure()
local cur_lum = AE.GetMeasuredLuminance()
if type(cur_ev) ~= "number" then fail("GetCurrentEV should return number") end
if type(cur_exp) ~= "number" then fail("GetCurrentExposure should return number") end
if type(cur_lum) ~= "number" then fail("GetMeasuredLuminance should return number") end
-- Disabled state: getters return defaults (0 / 1.0 / 0)
if cur_exp <= 0.0 then
    fail("GetCurrentExposure should be > 0 (default 1.0 when disabled), got " .. tostring(cur_exp))
end
pass("Debug getters return numbers (EV=" .. tostring(cur_ev)
       .. ", exposure=" .. tostring(cur_exp)
       .. ", measuredLuma=" .. tostring(cur_lum) .. ")")

-- ============================================================
-- 9) Enable/Disable lifecycle (headless tolerant)
-- ============================================================

local enable_ret = AE.Enable(640, 360)
if type(enable_ret) ~= "boolean" then
    fail("Enable should return boolean, got " .. type(enable_ret))
end
-- Headless: enable_ret commonly false. If true, IsEnabled must follow.
if enable_ret then
    if AE.IsEnabled() ~= true then
        fail("Enable returned true but IsEnabled() is false")
    end
    pass("Enable(640,360) returned true; IsEnabled true")

    -- Resize same size: should be no-op true
    if AE.Resize(640, 360) ~= true then fail("Resize same size should return true") end

    AE.Disable()
    if AE.IsEnabled() ~= false then fail("After Disable, IsEnabled should be false") end
    pass("Enable/Resize/Disable lifecycle ok (live backend)")
else
    pass("Enable(640,360) returned false (headless / no AE backend); IsEnabled stays false")
end

-- Double Disable should be safe (no-op)
AE.Disable()
AE.Disable()
pass("Double Disable safe (no-op)")

-- ============================================================
-- 10) AutoEnable=false guard: HDR.Enable does NOT trigger AE
-- ============================================================
-- Only meaningful if HDR subtable is present.

local HDR = Graphics.HDR
if type(HDR) == "table" and type(HDR.Enable) == "function" then
    AE.SetAutoEnable(false)
    -- Try to enable HDR; if it fails (headless) just skip this section.
    local hdr_ok = HDR.Enable(640, 360)
    if hdr_ok then
        if AE.IsEnabled() then
            fail("AutoEnable=false but HDR.Enable triggered AE auto-enable")
        end
        pass("AutoEnable=false guard ok (HDR.Enable did NOT trigger AE)")
        HDR.Disable()
    else
        pass("HDR.Enable returned false (headless), AutoEnable guard test skipped")
    end
end

print("[OK] Phase E.5 smoke (Light.Graphics.AutoExposure): all checks passed")
