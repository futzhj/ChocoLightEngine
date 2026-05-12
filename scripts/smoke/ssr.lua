-- Phase E.9 smoke: Light.Graphics.SSR
--
-- API coverage (22):
--   Lifecycle 5:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
--   AutoEnable 2: SetAutoEnable / GetAutoEnable
--   Params 14 (7 pairs):
--     SetMaxSteps / GetMaxSteps              (int   [8, 128],     default 64)
--     SetStepSize / GetStepSize              (float [0.01, 1.0],  default 0.1)
--     SetThickness / GetThickness            (float [0.01, 5.0],  default 0.5)
--     SetMaxDistance / GetMaxDistance        (float [1.0, 1000.0],default 50.0)
--     SetIntensity / GetIntensity            (float [0.0, 2.0],   default 0.7)
--     SetEdgeFade / GetEdgeFade              (float [0.0, 0.5],   default 0.1)
--     SetBlurEnabled / GetBlurEnabled        (bool,               default false)
--   Debug 1: GetReflectionTexId
--
-- Headless tolerant; ASCII-only.

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local S = Graphics.SSR
if type(S) ~= "table" then fail("SSR subtable missing (got " .. type(S) .. ")") end
pass("Light.Graphics.SSR subtable present")

-- ============================================================
-- A) Surface (22 functions)
-- ============================================================

local fns = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetMaxSteps", "GetMaxSteps",
    "SetStepSize", "GetStepSize",
    "SetThickness", "GetThickness",
    "SetMaxDistance", "GetMaxDistance",
    "SetIntensity", "GetIntensity",
    "SetEdgeFade", "GetEdgeFade",
    "SetBlurEnabled", "GetBlurEnabled",
    "GetReflectionTexId",
}
for _, k in ipairs(fns) do
    if type(S[k]) ~= "function" then
        fail("SSR." .. k .. " missing (got " .. type(S[k]) .. ")")
    end
end
pass("SSR module surface ok (" .. #fns .. " functions)")

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

local ms = S.GetMaxSteps()
if ms ~= 64 then fail("Default MaxSteps != 64 (got " .. tostring(ms) .. ")") end
pass("Default MaxSteps == 64")

local ss = S.GetStepSize()
if math.abs(ss - 0.1) > 1e-4 then fail("Default StepSize != 0.1 (got " .. tostring(ss) .. ")") end
pass("Default StepSize == 0.1")

local th = S.GetThickness()
if math.abs(th - 0.5) > 1e-4 then fail("Default Thickness != 0.5 (got " .. tostring(th) .. ")") end
pass("Default Thickness == 0.5")

local md = S.GetMaxDistance()
if math.abs(md - 50.0) > 1e-3 then fail("Default MaxDistance != 50 (got " .. tostring(md) .. ")") end
pass("Default MaxDistance == 50.0")

local it = S.GetIntensity()
if math.abs(it - 0.7) > 1e-4 then fail("Default Intensity != 0.7 (got " .. tostring(it) .. ")") end
pass("Default Intensity == 0.7")

local ef = S.GetEdgeFade()
if math.abs(ef - 0.1) > 1e-4 then fail("Default EdgeFade != 0.1 (got " .. tostring(ef) .. ")") end
pass("Default EdgeFade == 0.1")

if S.GetBlurEnabled() ~= false then fail("Default BlurEnabled != false") end
pass("Default BlurEnabled == false")

-- ============================================================
-- E) Param Set/Get round-trip
-- ============================================================

S.SetMaxSteps(32)
if S.GetMaxSteps() ~= 32 then fail("SetMaxSteps round-trip") end
pass("SetMaxSteps(32) round-trip ok")

S.SetStepSize(0.5)
if math.abs(S.GetStepSize() - 0.5) > 1e-4 then fail("SetStepSize round-trip") end
pass("SetStepSize(0.5) round-trip ok")

S.SetThickness(1.0)
if math.abs(S.GetThickness() - 1.0) > 1e-4 then fail("SetThickness round-trip") end
pass("SetThickness(1.0) round-trip ok")

S.SetMaxDistance(100.0)
if math.abs(S.GetMaxDistance() - 100.0) > 1e-3 then fail("SetMaxDistance round-trip") end
pass("SetMaxDistance(100) round-trip ok")

S.SetIntensity(1.5)
if math.abs(S.GetIntensity() - 1.5) > 1e-4 then fail("SetIntensity round-trip") end
pass("SetIntensity(1.5) round-trip ok")

S.SetEdgeFade(0.3)
if math.abs(S.GetEdgeFade() - 0.3) > 1e-4 then fail("SetEdgeFade round-trip") end
pass("SetEdgeFade(0.3) round-trip ok")

S.SetBlurEnabled(true)
if S.GetBlurEnabled() ~= true then fail("SetBlurEnabled(true) round-trip") end
pass("SetBlurEnabled(true) round-trip ok")

S.SetBlurEnabled(false)
if S.GetBlurEnabled() ~= false then fail("SetBlurEnabled(false) round-trip") end
pass("SetBlurEnabled(false) round-trip ok")

-- ============================================================
-- F) Param clamping
-- ============================================================

local EPS = 1e-4
local function near(a, b) return math.abs(a - b) <= EPS end

-- MaxSteps clamp [8, 128]
S.SetMaxSteps(-10)
if S.GetMaxSteps() ~= 8 then fail("SetMaxSteps(-10) clamp to 8, got " .. tostring(S.GetMaxSteps())) end
pass("SetMaxSteps(-10) -> clamp 8")

S.SetMaxSteps(500)
if S.GetMaxSteps() ~= 128 then fail("SetMaxSteps(500) clamp to 128, got " .. tostring(S.GetMaxSteps())) end
pass("SetMaxSteps(500) -> clamp 128")

-- StepSize clamp [0.01, 1.0]
S.SetStepSize(-1.0)
if not near(S.GetStepSize(), 0.01) then fail("SetStepSize(-1) clamp to 0.01") end
pass("SetStepSize(-1) -> clamp 0.01")

S.SetStepSize(5.0)
if not near(S.GetStepSize(), 1.0) then fail("SetStepSize(5) clamp to 1.0") end
pass("SetStepSize(5) -> clamp 1.0")

-- Thickness clamp [0.01, 5.0]
S.SetThickness(-1.0)
if not near(S.GetThickness(), 0.01) then fail("SetThickness(-1) clamp to 0.01") end
pass("SetThickness(-1) -> clamp 0.01")

S.SetThickness(100.0)
if not near(S.GetThickness(), 5.0) then fail("SetThickness(100) clamp to 5.0") end
pass("SetThickness(100) -> clamp 5.0")

-- MaxDistance clamp [1.0, 1000.0]
S.SetMaxDistance(0.0)
if not near(S.GetMaxDistance(), 1.0) then fail("SetMaxDistance(0) clamp to 1.0") end
pass("SetMaxDistance(0) -> clamp 1.0")

S.SetMaxDistance(99999.0)
if not near(S.GetMaxDistance(), 1000.0) then fail("SetMaxDistance(99999) clamp to 1000") end
pass("SetMaxDistance(99999) -> clamp 1000.0")

-- Intensity clamp [0.0, 2.0]
S.SetIntensity(-5.0)
if not near(S.GetIntensity(), 0.0) then fail("SetIntensity(-5) clamp to 0") end
pass("SetIntensity(-5) -> clamp 0.0")

S.SetIntensity(99.0)
if not near(S.GetIntensity(), 2.0) then fail("SetIntensity(99) clamp to 2.0") end
pass("SetIntensity(99) -> clamp 2.0")

-- EdgeFade clamp [0.0, 0.5]
S.SetEdgeFade(-1.0)
if not near(S.GetEdgeFade(), 0.0) then fail("SetEdgeFade(-1) clamp to 0") end
pass("SetEdgeFade(-1) -> clamp 0.0")

S.SetEdgeFade(5.0)
if not near(S.GetEdgeFade(), 0.5) then fail("SetEdgeFade(5) clamp to 0.5") end
pass("SetEdgeFade(5) -> clamp 0.5")

-- ============================================================
-- G) Restore defaults
-- ============================================================

S.SetMaxSteps(64)
S.SetStepSize(0.1)
S.SetThickness(0.5)
S.SetMaxDistance(50.0)
S.SetIntensity(0.7)
S.SetEdgeFade(0.1)
S.SetBlurEnabled(false)
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

    -- Reflection tex id must be non-zero when enabled
    local rid = S.GetReflectionTexId()
    if type(rid) ~= "number" then fail("GetReflectionTexId not number") end
    if rid == 0 then fail("GetReflectionTexId should be non-zero after Enable") end
    pass("GetReflectionTexId() = " .. tostring(rid) .. " (non-zero ok)")

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

    -- After Disable, reflection tex id must return 0
    local rid_off = S.GetReflectionTexId()
    if rid_off ~= 0 then fail("GetReflectionTexId should be 0 after Disable, got " .. tostring(rid_off)) end
    pass("GetReflectionTexId() == 0 after Disable")
else
    -- Headless: GetReflectionTexId must be 0 (Enable failed -> not enabled)
    if S.GetReflectionTexId() ~= 0 then fail("Headless GetReflectionTexId should be 0") end
    pass("S.Enable returned false (headless), GetReflectionTexId() == 0")
end
S.Disable()  -- idempotent
S.Disable()
pass("Double Disable safe")

-- ============================================================
-- I) Low-spec config (32 steps + intensity 0.3) — performance fallback
-- ============================================================

S.SetMaxSteps(32)
S.SetIntensity(0.3)
if S.GetMaxSteps() ~= 32 or math.abs(S.GetIntensity() - 0.3) > 1e-4 then
    fail("Low-spec config not preserved")
end
pass("Low-spec config (steps=32, intensity=0.3) preserved")

-- Restore
S.SetMaxSteps(64)
S.SetIntensity(0.7)

-- ============================================================
-- J) HDR-linked autoEnable round-trip
-- ============================================================

local HDR = Graphics.HDR
if type(HDR) == "table" and type(HDR.Enable) == "function" then
    S.SetAutoEnable(true)
    local hdr_ok = HDR.Enable(640, 480)
    if hdr_ok then
        -- autoEnable=true 时, HDR.Enable 应自动拉起 SSR (若 supported)
        if S.IsSupported() then
            if S.IsEnabled() ~= true then
                fail("autoEnable=true 时 HDR.Enable 后 SSR 未自动启用")
            end
            pass("autoEnable=true + HDR.Enable 自动拉起 SSR")
            local rid = S.GetReflectionTexId()
            if rid == 0 then fail("autoEnable 后 GetReflectionTexId == 0") end
            pass("autoEnable 拉起后 GetReflectionTexId = " .. tostring(rid))
        else
            pass("SSR not supported, autoEnable no-op (acceptable)")
        end
        HDR.Disable()
        -- HDR.Disable 应连带 SSR Disable
        if S.IsEnabled() ~= false then
            fail("HDR.Disable 后 SSR 仍 enabled (OnHDRDisabled 联动 fail)")
        end
        pass("HDR.Disable 联动 SSR Disable 正常")
    else
        pass("HDR.Enable headless 返回 false, 跳过 autoEnable 联动验证")
    end
    S.SetAutoEnable(false)
else
    pass("Light.Graphics.HDR 不可用, 跳过 autoEnable 联动验证")
end

print("[OK] Phase E.9 smoke (Light.Graphics.SSR): all checks passed")
