-- Phase E.7 smoke: Light.Graphics.LensFlare
--
-- API coverage (23):
--   Lifecycle 5:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
--   AutoEnable 2: SetAutoEnable / GetAutoEnable
--   Params 14 (7 pairs):
--     SetThreshold / GetThreshold
--     SetIntensity / GetIntensity
--     SetGhostCount / GetGhostCount
--     SetGhostDispersal / GetGhostDispersal
--     SetHaloWidth / GetHaloWidth
--     SetChromaticAberration / GetChromaticAberration
--     SetDistortionEnabled / GetDistortionEnabled
--   Phase E.7.4 Texture 2: SetFlareTexture / GetFlareTextureId
--
-- Headless tolerant; ASCII-only.

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local LF = Graphics.LensFlare
if type(LF) ~= "table" then fail("LensFlare subtable missing (got " .. type(LF) .. ")") end
pass("Light.Graphics.LensFlare subtable present")

-- ============================================================
-- A) Surface (21 functions)
-- ============================================================

local fns = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetThreshold", "GetThreshold",
    "SetIntensity", "GetIntensity",
    "SetGhostCount", "GetGhostCount",
    "SetGhostDispersal", "GetGhostDispersal",
    "SetHaloWidth", "GetHaloWidth",
    "SetChromaticAberration", "GetChromaticAberration",
    "SetDistortionEnabled", "GetDistortionEnabled",
    "SetFlareTexture", "GetFlareTextureId",   -- Phase E.7.4
}
for _, k in ipairs(fns) do
    if type(LF[k]) ~= "function" then
        fail("LensFlare." .. k .. " missing (got " .. type(LF[k]) .. ")")
    end
end
pass("LensFlare module surface ok (" .. #fns .. " functions)")

-- ============================================================
-- B) IsSupported / IsEnabled initial state
-- ============================================================

local sup = LF.IsSupported()
if type(sup) ~= "boolean" then fail("IsSupported not boolean") end
pass("LF.IsSupported = " .. tostring(sup))

if LF.IsEnabled() ~= false then fail("Initial IsEnabled must be false") end
pass("Initial IsEnabled() == false")

-- ============================================================
-- C) AutoEnable default FALSE + round-trip
-- ============================================================

if LF.GetAutoEnable() ~= false then
    fail("Default AutoEnable must be false, got " .. tostring(LF.GetAutoEnable()))
end
LF.SetAutoEnable(true)
if LF.GetAutoEnable() ~= true then fail("AutoEnable round-trip true failed") end
LF.SetAutoEnable(false)
if LF.GetAutoEnable() ~= false then fail("AutoEnable round-trip false failed") end
pass("AutoEnable default false + round-trip ok")

-- ============================================================
-- D) Threshold default 1.0 + round-trip + clamp >= 0
-- ============================================================

if math.abs(LF.GetThreshold() - 1.0) > 1e-5 then
    fail("Default Threshold must be 1.0, got " .. tostring(LF.GetThreshold()))
end
LF.SetThreshold(2.5)
if math.abs(LF.GetThreshold() - 2.5) > 1e-5 then fail("Threshold round-trip 2.5 failed") end
LF.SetThreshold(-1.0)
if LF.GetThreshold() < 0.0 - 1e-5 then fail("Threshold negative clamp failed") end
LF.SetThreshold(1.0)
pass("Threshold default 1.0 + round-trip + clamp ok")

-- ============================================================
-- E) Intensity default 0.4 + round-trip + clamp >= 0
-- ============================================================

if math.abs(LF.GetIntensity() - 0.4) > 1e-5 then
    fail("Default Intensity must be 0.4, got " .. tostring(LF.GetIntensity()))
end
LF.SetIntensity(1.5)
if math.abs(LF.GetIntensity() - 1.5) > 1e-5 then fail("Intensity round-trip 1.5 failed") end
LF.SetIntensity(-2.0)
if LF.GetIntensity() < 0.0 - 1e-5 then fail("Intensity negative clamp failed") end
LF.SetIntensity(0.4)
pass("Intensity default 0.4 + round-trip + clamp ok")

-- ============================================================
-- F) GhostCount default 4 + round-trip + clamp [0, 8]
-- ============================================================

if LF.GetGhostCount() ~= 4 then
    fail("Default GhostCount must be 4, got " .. tostring(LF.GetGhostCount()))
end
LF.SetGhostCount(6)
if LF.GetGhostCount() ~= 6 then fail("GhostCount round-trip 6 failed") end
LF.SetGhostCount(-3)
if LF.GetGhostCount() ~= 0 then fail("GhostCount lower clamp failed, got " .. tostring(LF.GetGhostCount())) end
LF.SetGhostCount(99)
if LF.GetGhostCount() ~= 8 then fail("GhostCount upper clamp failed, got " .. tostring(LF.GetGhostCount())) end
LF.SetGhostCount(0)
if LF.GetGhostCount() ~= 0 then fail("GhostCount = 0 should be valid (no ghost)") end
LF.SetGhostCount(4)
pass("GhostCount default 4 + round-trip + clamp [0,8] + zero-valid ok")

-- ============================================================
-- G) GhostDispersal default 0.4 + round-trip + clamp [0, 2.0]
-- ============================================================

if math.abs(LF.GetGhostDispersal() - 0.4) > 1e-5 then
    fail("Default GhostDispersal must be 0.4")
end
LF.SetGhostDispersal(1.2)
if math.abs(LF.GetGhostDispersal() - 1.2) > 1e-5 then fail("GhostDispersal round-trip failed") end
LF.SetGhostDispersal(-0.5)
if LF.GetGhostDispersal() < 0.0 - 1e-5 then fail("GhostDispersal lower clamp failed") end
LF.SetGhostDispersal(10.0)
if LF.GetGhostDispersal() > 2.0 + 1e-5 then
    fail("GhostDispersal upper clamp <= 2.0 failed, got " .. tostring(LF.GetGhostDispersal()))
end
LF.SetGhostDispersal(0.4)
pass("GhostDispersal default 0.4 + round-trip + clamp [0,2] ok")

-- ============================================================
-- H) HaloWidth default 0.5 + round-trip + clamp [0, 1.0]
-- ============================================================

if math.abs(LF.GetHaloWidth() - 0.5) > 1e-5 then
    fail("Default HaloWidth must be 0.5")
end
LF.SetHaloWidth(0.8)
if math.abs(LF.GetHaloWidth() - 0.8) > 1e-5 then fail("HaloWidth round-trip failed") end
LF.SetHaloWidth(-1.0)
if LF.GetHaloWidth() < 0.0 - 1e-5 then fail("HaloWidth lower clamp failed") end
LF.SetHaloWidth(5.0)
if LF.GetHaloWidth() > 1.0 + 1e-5 then
    fail("HaloWidth upper clamp <= 1.0 failed, got " .. tostring(LF.GetHaloWidth()))
end
LF.SetHaloWidth(0.0)   -- valid: no halo
if LF.GetHaloWidth() ~= 0.0 then fail("HaloWidth = 0 should be valid (no halo)") end
LF.SetHaloWidth(0.5)
pass("HaloWidth default 0.5 + round-trip + clamp [0,1] + zero-valid ok")

-- ============================================================
-- I) ChromaticAberration default 0.005 + round-trip + clamp [0, 0.02]
-- ============================================================

if math.abs(LF.GetChromaticAberration() - 0.005) > 1e-5 then
    fail("Default ChromaticAberration must be 0.005")
end
LF.SetChromaticAberration(0.015)
if math.abs(LF.GetChromaticAberration() - 0.015) > 1e-5 then fail("ChromaticAberration round-trip failed") end
LF.SetChromaticAberration(-0.01)
if LF.GetChromaticAberration() < 0.0 - 1e-5 then fail("ChromaticAberration lower clamp failed") end
LF.SetChromaticAberration(1.0)
if LF.GetChromaticAberration() > 0.02 + 1e-5 then
    fail("ChromaticAberration upper clamp <= 0.02 failed, got " .. tostring(LF.GetChromaticAberration()))
end
LF.SetChromaticAberration(0.005)
pass("ChromaticAberration default 0.005 + round-trip + clamp [0,0.02] ok")

-- ============================================================
-- J) DistortionEnabled default true + round-trip
-- ============================================================

if LF.GetDistortionEnabled() ~= true then
    fail("Default DistortionEnabled must be true, got " .. tostring(LF.GetDistortionEnabled()))
end
LF.SetDistortionEnabled(false)
if LF.GetDistortionEnabled() ~= false then fail("DistortionEnabled round-trip false failed") end
LF.SetDistortionEnabled(true)
if LF.GetDistortionEnabled() ~= true then fail("DistortionEnabled round-trip true failed") end
pass("DistortionEnabled default true + round-trip ok")

-- ============================================================
-- K) Enable / Resize / Disable lifecycle (headless tolerant)
-- ============================================================

local lf_en = LF.Enable(640, 360)
if type(lf_en) ~= "boolean" then fail("LF.Enable should return boolean") end
if lf_en then
    if LF.IsEnabled() ~= true then fail("Enable=true but IsEnabled=false") end
    if LF.Resize(640, 360) ~= true then fail("Same-size Resize should return true") end
    if LF.Resize(800, 600) ~= true then fail("Different-size Resize should succeed") end
    LF.Disable()
    if LF.IsEnabled() ~= false then fail("After Disable still enabled") end
    pass("Enable/Resize/Disable lifecycle ok (live backend)")
else
    pass("LF.Enable returned false (headless), IsEnabled stays false")
end
LF.Disable()  -- idempotent
LF.Disable()
pass("Double Disable safe")

-- ============================================================
-- L) Boundary cases (GhostCount=0, HaloWidth=0, CA=0)
-- ============================================================

LF.SetGhostCount(0)
LF.SetHaloWidth(0.0)
LF.SetChromaticAberration(0.0)
-- module remains usable; Process becomes essentially a clear pass (no crash)
if LF.GetGhostCount() ~= 0 or LF.GetHaloWidth() ~= 0.0 or LF.GetChromaticAberration() ~= 0.0 then
    fail("Boundary zero values not preserved")
end
pass("Boundary GhostCount=0, HaloWidth=0, CA=0 preserved (no crash potential)")

-- Restore defaults
LF.SetGhostCount(4)
LF.SetHaloWidth(0.5)
LF.SetChromaticAberration(0.005)

-- ============================================================
-- M) Phase E.7.4 - SetFlareTexture / GetFlareTextureId
-- ============================================================

-- M.1 default state: id == 0 (use 1x1 white fallback)
local initId = LF.GetFlareTextureId()
if type(initId) ~= "number" then fail("GetFlareTextureId not number") end
if initId ~= 0 then fail("Initial FlareTextureId must be 0 (got " .. tostring(initId) .. ")") end
pass("Initial GetFlareTextureId() == 0 (white fallback)")

-- M.2 set to a number id, round-trip
LF.SetFlareTexture(42)
if LF.GetFlareTextureId() ~= 42 then fail("SetFlareTexture(number) round-trip failed") end
pass("SetFlareTexture(42) -> GetFlareTextureId() == 42")

-- M.3 reset via nil
LF.SetFlareTexture(nil)
if LF.GetFlareTextureId() ~= 0 then fail("SetFlareTexture(nil) did not reset to 0") end
pass("SetFlareTexture(nil) reset to 0")

-- M.4 set via 0 (explicit fallback) - same as nil
LF.SetFlareTexture(99)
LF.SetFlareTexture(0)
if LF.GetFlareTextureId() ~= 0 then fail("SetFlareTexture(0) did not reset to 0") end
pass("SetFlareTexture(0) explicit fallback ok")

-- M.5 zero-arg fallback (same as nil)
LF.SetFlareTexture(77)
LF.SetFlareTexture()
if LF.GetFlareTextureId() ~= 0 then fail("SetFlareTexture() zero-arg did not reset to 0") end
pass("SetFlareTexture() zero-arg fallback ok")

-- M.6 Image-table-like (mock with GetTextureId() method)
local fakeImage = setmetatable({}, {})
function fakeImage:GetTextureId() return 1234 end
LF.SetFlareTexture(fakeImage)
if LF.GetFlareTextureId() ~= 1234 then
    fail("SetFlareTexture(Image-like) did not call :GetTextureId()")
end
pass("SetFlareTexture(Image-like) -> 1234 via :GetTextureId()")

-- M.7 invalid type rejected
local okErr, errMsg = pcall(LF.SetFlareTexture, true)
if okErr then
    fail("SetFlareTexture(boolean) should error")
end
pass("SetFlareTexture(boolean) rejected: " .. tostring(errMsg):sub(1, 80))

-- M.8 table without GetTextureId rejected
local okErr2, errMsg2 = pcall(LF.SetFlareTexture, {})
if okErr2 then
    fail("SetFlareTexture(empty table) should error")
end
pass("SetFlareTexture(empty table) rejected: " .. tostring(errMsg2):sub(1, 80))

-- restore final state
LF.SetFlareTexture(nil)

print("[OK] Phase E.7 smoke (Light.Graphics.LensFlare): all checks passed")
