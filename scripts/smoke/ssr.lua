-- Phase E.9+E.10+E.11+E.12 smoke: Light.Graphics.SSR
--
-- API coverage (34):
--   Lifecycle 5:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
--   AutoEnable 2: SetAutoEnable / GetAutoEnable
--   Params 26 (13 pairs):
--     SetMaxSteps / GetMaxSteps              (int   [8, 128],     default 64)
--     SetStepSize / GetStepSize              (float [0.01, 1.0],  default 0.1)
--     SetThickness / GetThickness            (float [0.01, 5.0],  default 0.5)
--     SetMaxDistance / GetMaxDistance        (float [1.0, 1000.0],default 50.0)
--     SetIntensity / GetIntensity            (float [0.0, 2.0],   default 0.7)
--     SetEdgeFade / GetEdgeFade              (float [0.0, 0.5],   default 0.1)
--     SetBlurEnabled / GetBlurEnabled        (bool,               default false; Phase E.10 active)
--     SetBlurRadius / GetBlurRadius          (float [0.5, 4.0],   default 1.5; Phase E.10)
--     SetBilateralEnabled / GetBilateralEnabled (bool,            default true; Phase E.11)
--     SetBlurDepthSigma / GetBlurDepthSigma  (float [50, 500],    default 200; Phase E.11)
--     SetTemporalEnabled / GetTemporalEnabled (bool,              default true; Phase E.12)
--     SetTemporalAlpha / GetTemporalAlpha    (float [0.5, 0.99],  default 0.9; Phase E.12)
--     SetRejectionMode / GetRejectionMode    (int   {0, 1},       default 1; Phase E.12)
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
-- A) Surface (34 functions, Phase E.12 adds 3 pairs: Temporal, Alpha, RejectionMode)
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
    "SetBlurRadius", "GetBlurRadius",   -- Phase E.10
    "SetBilateralEnabled", "GetBilateralEnabled",   -- Phase E.11
    "SetBlurDepthSigma", "GetBlurDepthSigma",       -- Phase E.11
    "SetTemporalEnabled", "GetTemporalEnabled",     -- Phase E.12
    "SetTemporalAlpha", "GetTemporalAlpha",         -- Phase E.12
    "SetRejectionMode", "GetRejectionMode",         -- Phase E.12
    "GetReflectionTexId",
    "Process",                                       -- Phase F.0.10.3 (region overload)
    -- Phase F.0.10.9.x.2 — Multi-Instance SSR (5 fn, 与 HDR/TAA/Bloom 同模板)
    "CreateInstance", "DestroyInstance", "SetActiveInstance",
    "GetActiveInstance", "GetInstanceCount",
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

-- Phase E.10 — BlurRadius default
local br = S.GetBlurRadius()
if math.abs(br - 1.5) > 1e-4 then fail("Default BlurRadius != 1.5 (got " .. tostring(br) .. ")") end
pass("Default BlurRadius == 1.5 (Phase E.10)")

-- Phase E.11 — BilateralEnabled default
if S.GetBilateralEnabled() ~= true then fail("Default BilateralEnabled != true (Phase E.11)") end
pass("Default BilateralEnabled == true (Phase E.11)")

-- Phase E.11 — BlurDepthSigma default
local bs = S.GetBlurDepthSigma()
if math.abs(bs - 200.0) > 1e-3 then fail("Default BlurDepthSigma != 200 (got " .. tostring(bs) .. ")") end
pass("Default BlurDepthSigma == 200 (Phase E.11)")

-- Phase E.12 — TemporalEnabled default (= true, TAA-style 业界标准)
if S.GetTemporalEnabled() ~= true then fail("Default TemporalEnabled != true (Phase E.12)") end
pass("Default TemporalEnabled == true (Phase E.12)")

-- Phase E.12 — TemporalAlpha default
local ta = S.GetTemporalAlpha()
if math.abs(ta - 0.9) > 1e-4 then fail("Default TemporalAlpha != 0.9 (got " .. tostring(ta) .. ")") end
pass("Default TemporalAlpha == 0.9 (Phase E.12)")

-- Phase E.12 — RejectionMode default (= 1, neighborhood clip)
if S.GetRejectionMode() ~= 1 then fail("Default RejectionMode != 1 (got " .. tostring(S.GetRejectionMode()) .. ")") end
pass("Default RejectionMode == 1 (Phase E.12)")

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

-- Phase E.10 — BlurRadius round-trip
S.SetBlurRadius(2.5)
if math.abs(S.GetBlurRadius() - 2.5) > 1e-4 then fail("SetBlurRadius round-trip") end
pass("SetBlurRadius(2.5) round-trip ok")

-- Phase E.11 — BilateralEnabled round-trip
S.SetBilateralEnabled(false)
if S.GetBilateralEnabled() ~= false then fail("SetBilateralEnabled(false) round-trip") end
pass("SetBilateralEnabled(false) round-trip ok")
S.SetBilateralEnabled(true)
if S.GetBilateralEnabled() ~= true then fail("SetBilateralEnabled(true) round-trip") end
pass("SetBilateralEnabled(true) round-trip ok")

-- Phase E.11 — BlurDepthSigma round-trip
S.SetBlurDepthSigma(150.0)
if math.abs(S.GetBlurDepthSigma() - 150.0) > 1e-3 then fail("SetBlurDepthSigma round-trip") end
pass("SetBlurDepthSigma(150) round-trip ok")

-- Phase E.12 — TemporalEnabled round-trip
S.SetTemporalEnabled(false)
if S.GetTemporalEnabled() ~= false then fail("SetTemporalEnabled(false) round-trip") end
pass("SetTemporalEnabled(false) round-trip ok")
S.SetTemporalEnabled(true)
if S.GetTemporalEnabled() ~= true then fail("SetTemporalEnabled(true) round-trip") end
pass("SetTemporalEnabled(true) round-trip ok")

-- Phase E.12 — TemporalAlpha round-trip
S.SetTemporalAlpha(0.75)
if math.abs(S.GetTemporalAlpha() - 0.75) > 1e-4 then fail("SetTemporalAlpha round-trip") end
pass("SetTemporalAlpha(0.75) round-trip ok")

-- Phase E.12 — RejectionMode round-trip
S.SetRejectionMode(0)
if S.GetRejectionMode() ~= 0 then fail("SetRejectionMode(0) round-trip") end
pass("SetRejectionMode(0) round-trip ok")
S.SetRejectionMode(1)
if S.GetRejectionMode() ~= 1 then fail("SetRejectionMode(1) round-trip") end
pass("SetRejectionMode(1) round-trip ok")

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

-- Phase E.10 — BlurRadius clamp [0.5, 4.0]
S.SetBlurRadius(-10.0)
if not near(S.GetBlurRadius(), 0.5) then fail("SetBlurRadius(-10) clamp to 0.5, got " .. tostring(S.GetBlurRadius())) end
pass("SetBlurRadius(-10) -> clamp 0.5")

S.SetBlurRadius(99.0)
if not near(S.GetBlurRadius(), 4.0) then fail("SetBlurRadius(99) clamp to 4.0, got " .. tostring(S.GetBlurRadius())) end
pass("SetBlurRadius(99) -> clamp 4.0")

-- Phase E.11 — BlurDepthSigma clamp [50, 500]
S.SetBlurDepthSigma(-100.0)
if not near(S.GetBlurDepthSigma(), 50.0) then fail("SetBlurDepthSigma(-100) clamp to 50, got " .. tostring(S.GetBlurDepthSigma())) end
pass("SetBlurDepthSigma(-100) -> clamp 50")

-- Phase E.12 — TemporalAlpha clamp [0.5, 0.99]
S.SetTemporalAlpha(-0.5)
if not near(S.GetTemporalAlpha(), 0.5) then fail("SetTemporalAlpha(-0.5) clamp to 0.5, got " .. tostring(S.GetTemporalAlpha())) end
pass("SetTemporalAlpha(-0.5) -> clamp 0.5")

S.SetTemporalAlpha(2.0)
if not near(S.GetTemporalAlpha(), 0.99) then fail("SetTemporalAlpha(2.0) clamp to 0.99, got " .. tostring(S.GetTemporalAlpha())) end
pass("SetTemporalAlpha(2.0) -> clamp 0.99")

-- Phase E.12 — RejectionMode clamp {0, 1}
S.SetRejectionMode(-5)
if S.GetRejectionMode() ~= 0 then fail("SetRejectionMode(-5) clamp to 0, got " .. tostring(S.GetRejectionMode())) end
pass("SetRejectionMode(-5) -> clamp 0")

S.SetRejectionMode(99)
if S.GetRejectionMode() ~= 1 then fail("SetRejectionMode(99) clamp to 1, got " .. tostring(S.GetRejectionMode())) end
pass("SetRejectionMode(99) -> clamp 1")

S.SetBlurDepthSigma(9999.0)
if not near(S.GetBlurDepthSigma(), 500.0) then fail("SetBlurDepthSigma(9999) clamp to 500, got " .. tostring(S.GetBlurDepthSigma())) end
pass("SetBlurDepthSigma(9999) -> clamp 500")

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
S.SetBlurRadius(1.5)   -- Phase E.10
S.SetBilateralEnabled(true)   -- Phase E.11
S.SetBlurDepthSigma(200.0)    -- Phase E.11
S.SetTemporalEnabled(true)    -- Phase E.12
S.SetTemporalAlpha(0.9)       -- Phase E.12
S.SetRejectionMode(1)         -- Phase E.12
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

-- ============================================================
-- K) Phase E.10 — BlurEnabled × BlurRadius 联动行为
--    验证: 独立状态位 (BlurEnabled 与 BlurRadius 互不干扰)
-- ============================================================

-- 1. BlurEnabled 状态 × BlurRadius 调整: 互不影响
S.SetBlurEnabled(true)
S.SetBlurRadius(3.0)
if S.GetBlurEnabled() ~= true or math.abs(S.GetBlurRadius() - 3.0) > 1e-4 then
    fail("BlurEnabled × BlurRadius independence broken")
end
pass("BlurEnabled=true + BlurRadius=3.0 独立状态位保持")

-- 2. BlurEnabled=false 后 BlurRadius 仍可设置且生效 (不被 clear)
S.SetBlurEnabled(false)
if math.abs(S.GetBlurRadius() - 3.0) > 1e-4 then
    fail("BlurRadius 被 Disable BlurEnabled 意外重置")
end
pass("BlurEnabled=false 后 BlurRadius 保持 3.0 (独立)")

-- 3. Low-end 预设: blur 关 + intensity 低 (移动端省性能)
S.SetBlurEnabled(false)
S.SetMaxSteps(16)
S.SetIntensity(0.3)
if S.GetBlurEnabled() ~= false or S.GetMaxSteps() ~= 16 or math.abs(S.GetIntensity() - 0.3) > 1e-4 then
    fail("Low-end preset 不一致")
end
pass("Low-end 预设 (blur=off, steps=16, intensity=0.3) 保持")

-- 4. High-end 预设: blur 开 + radius=2.0 + steps=128 (云渲染 / 高端桌面)
S.SetBlurEnabled(true)
S.SetBlurRadius(2.0)
S.SetMaxSteps(128)
if S.GetBlurEnabled() ~= true or math.abs(S.GetBlurRadius() - 2.0) > 1e-4 or S.GetMaxSteps() ~= 128 then
    fail("High-end preset 不一致")
end
pass("High-end 预设 (blur=on, radius=2.0, steps=128) 保持")

-- restore
S.SetBlurEnabled(false)
S.SetBlurRadius(1.5)
S.SetMaxSteps(64)
S.SetIntensity(0.7)

-- ============================================================
-- L) Phase E.11 — Bilateral × BlurDepthSigma 联动行为
-- ============================================================

-- 1. Bilateral on + sigma 1· round-trip combo
S.SetBilateralEnabled(true)
S.SetBlurDepthSigma(300.0)
if S.GetBilateralEnabled() ~= true or math.abs(S.GetBlurDepthSigma() - 300.0) > 1e-3 then
    fail("Bilateral=true + sigma=300 combo broken")
end
pass("Bilateral=true + sigma=300 保持 (高严格场景)")

-- 2. Bilateral off + sigma value not affected
S.SetBilateralEnabled(false)
if math.abs(S.GetBlurDepthSigma() - 300.0) > 1e-3 then
    fail("BlurDepthSigma 被 Bilateral=false 意外重置")
end
pass("Bilateral=false 后 BlurDepthSigma 保持 300 (独立)")

-- 3. Phase E.11 默认高质量预设 (BlurEnabled=on, Bilateral=on, sigma=200)
S.SetBlurEnabled(true)
S.SetBilateralEnabled(true)
S.SetBlurDepthSigma(200.0)
if not (S.GetBlurEnabled() == true and S.GetBilateralEnabled() == true
        and math.abs(S.GetBlurDepthSigma() - 200.0) < 1e-3) then
    fail("Phase E.11 默认高质量预设不一致")
end
pass("Phase E.11 默认预设 (blur+bilateral+sigma=200) 保持")

-- 4. Phase E.10 向后兼容预设 (BlurEnabled=on, Bilateral=off)
S.SetBilateralEnabled(false)
if S.GetBilateralEnabled() ~= false or S.GetBlurEnabled() ~= true then
    fail("Phase E.10 向后兼容预设 (blur=on, bilateral=off) 不一致")
end
pass("Phase E.10 向后兼容预设 (blur=on, bilateral=off) 保持")

-- restore
S.SetBlurEnabled(false)
S.SetBlurRadius(1.5)
S.SetMaxSteps(64)
S.SetIntensity(0.7)
S.SetBilateralEnabled(true)
S.SetBlurDepthSigma(200.0)
S.SetTemporalEnabled(true)
S.SetTemporalAlpha(0.9)
S.SetRejectionMode(1)

-- ============================================================
-- M) Phase E.12 — Temporal × Bilateral × Blur 联动行为
-- ============================================================

-- 1. Temporal=on + Blur=off + Bilateral=on （默认高质量预设）
S.SetTemporalEnabled(true)
S.SetBlurEnabled(false)
S.SetBilateralEnabled(true)
if not (S.GetTemporalEnabled() == true and S.GetBlurEnabled() == false
        and S.GetBilateralEnabled() == true) then
    fail("Phase E.12 默认组合 (temporal=on, blur=off, bilateral=on) 不一致")
end
pass("Phase E.12 默认组合 (temporal=on, blur=off, bilateral=on) 保持")

-- 2. Temporal=on + Blur=on + Bilateral=on （最高质量预设）
S.SetBlurEnabled(true)
if not (S.GetTemporalEnabled() == true and S.GetBlurEnabled() == true
        and S.GetBilateralEnabled() == true) then
    fail("Phase E.12 最高质量预设 (temporal+blur+bilateral) 不一致")
end
pass("Phase E.12 最高质量预设 (temporal+blur+bilateral) 保持")

-- 3. Temporal=off 向后兼容 (Phase E.11 行为, temporal 不变变其他参数)
S.SetTemporalEnabled(false)
if S.GetTemporalEnabled() ~= false or S.GetBlurEnabled() ~= true
        or S.GetBilateralEnabled() ~= true then
    fail("Temporal=off 后其他参数变动（不独立）")
end
pass("Temporal=off 不影响 Blur/Bilateral (独立状态位)")

-- 4. Alpha 调节独立于 RejectionMode
S.SetTemporalAlpha(0.6)
S.SetRejectionMode(0)
if math.abs(S.GetTemporalAlpha() - 0.6) > 1e-4 or S.GetRejectionMode() ~= 0 then
    fail("TemporalAlpha / RejectionMode 独立设置失败")
end
S.SetTemporalAlpha(0.95)
if math.abs(S.GetTemporalAlpha() - 0.95) > 1e-4 or S.GetRejectionMode() ~= 0 then
    fail("调整 alpha 后 RejectionMode 被覆盖")
end
pass("TemporalAlpha / RejectionMode 独立 (互不干扰)")

-- 5. 默认预设 round-trip完整恢复
S.SetTemporalEnabled(true)
S.SetTemporalAlpha(0.9)
S.SetRejectionMode(1)
S.SetBlurEnabled(false)
if not (S.GetTemporalEnabled() == true
        and math.abs(S.GetTemporalAlpha() - 0.9) < 1e-4
        and S.GetRejectionMode() == 1
        and S.GetBlurEnabled() == false) then
    fail("默认预设 round-trip 完整恢复失败")
end
pass("Phase E.12 默认预设 round-trip 完整恢复")

-- ============================================================
-- Phase F.0.10.3 — Process(region) overload defense (6 PASS)
-- ============================================================
-- HDR 未启 + SSR 未启 时, Process 应返 nil + err string (silent skip, 不崩)
-- 与 MotionBlur.Process / Bloom.Process 同模式

-- 测试 1: 无参 Process (full-screen) - HDR 未启 → nil + err
local r1, e1 = S.Process()
if r1 ~= nil then
    fail("SSR.Process() with HDR off should return nil; got " .. tostring(r1))
end
if type(e1) ~= "string" then
    fail("SSR.Process() with HDR off should return err string; got " .. type(e1))
end
pass("SSR.Process() with HDR off returns nil + err string")

-- 测试 2: 4 args region Process - HDR 未启 → nil + err
local r2, e2 = S.Process(0, 0, 100, 100)
if r2 ~= nil or type(e2) ~= "string" then
    fail("SSR.Process(0,0,100,100) with HDR off should return nil + err; got " ..
         tostring(r2) .. ", " .. type(e2))
end
pass("SSR.Process(x,y,w,h) with HDR off returns nil + err string")

-- 测试 3: 部分 region 参数 (3 个) → 拒绝
local r3, e3 = S.Process(0, 0, 100)
if r3 ~= nil or type(e3) ~= "string" or not string.find(e3, "expected 0 or 4 args") then
    fail("SSR.Process(0,0,100) should reject with 'expected 0 or 4 args' err; got " ..
         tostring(r3) .. ", " .. tostring(e3))
end
pass("SSR.Process partial args rejected (3 args)")

-- 测试 4: w<0 拒绝
local r4, e4 = S.Process(0, 0, -1, 100)
if r4 ~= nil or type(e4) ~= "string" or not string.find(e4, "w/h must be >= 0") then
    fail("SSR.Process(0,0,-1,100) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r4) .. ", " .. tostring(e4))
end
pass("SSR.Process w<0 rejected")

-- 测试 5: h<0 拒绝
local r5, e5 = S.Process(0, 0, 100, -1)
if r5 ~= nil or type(e5) ~= "string" or not string.find(e5, "w/h must be >= 0") then
    fail("SSR.Process(0,0,100,-1) should reject with 'w/h must be >= 0' err; got " ..
         tostring(r5) .. ", " .. tostring(e5))
end
pass("SSR.Process h<0 rejected")

-- 测试 6: 类型错 (传 string 而非 integer) → luaL_error 抛
local ok6 = pcall(function() S.Process("a", "b", "c", "d") end)
if ok6 then
    fail("SSR.Process('a','b','c','d') should throw luaL_error; succeeded")
end
pass("SSR.Process type error throws luaL_error")

-- ============================================================
-- Phase F.0.10.9.x.2 — Multi-Instance SSR (5 fn round-trip)
-- 与 HDR/TAA/Bloom multi-instance 同模板: default + 3 user, MAX=4
-- ============================================================

S.SetActiveInstance(0)   -- 防御性复位

-- MI.1 初始状态
if S.GetInstanceCount() ~= 1 then
    fail("MI.1 初始 count expect 1, got " .. tostring(S.GetInstanceCount()))
end
if S.GetActiveInstance() ~= 0 then
    fail("MI.1 初始 active expect 0, got " .. tostring(S.GetActiveInstance()))
end
pass("MI.1 初始 instance count=1, active=0")

-- MI.2 Create x3 + 槽满
local sid1 = S.CreateInstance()
local sid2 = S.CreateInstance()
local sid3 = S.CreateInstance()
if sid1 ~= 1 or sid2 ~= 2 or sid3 ~= 3 then
    fail("MI.2 Create x3 expect 1/2/3, got " .. tostring(sid1) .. "/" .. tostring(sid2) .. "/" .. tostring(sid3))
end
if S.GetInstanceCount() ~= 4 then fail("MI.2 count expect 4") end
if S.CreateInstance() ~= 0 then fail("MI.2 第 4 次 Create expect 0") end
pass("MI.2 Create x3 + 第 4 次 returns 0 (槽满)")

-- MI.3 SetActiveInstance round-trip
if not S.SetActiveInstance(sid2) then fail("MI.3 SetActive(2) failed") end
if S.GetActiveInstance() ~= 2 then fail("MI.3 GetActive after set expect 2") end
S.SetActiveInstance(0)
pass("MI.3 SetActiveInstance round-trip (0 <-> 2)")

-- MI.4 Per-instance 参数隔离 (intensity)
S.SetActiveInstance(0); S.SetIntensity(0.2)
S.SetActiveInstance(sid1); S.SetIntensity(1.8)
S.SetActiveInstance(0)
if math.abs(S.GetIntensity() - 0.2) > 1e-4 then
    fail("MI.4 instance 0 intensity 被污染, expect 0.2, got " .. S.GetIntensity())
end
S.SetActiveInstance(sid1)
if math.abs(S.GetIntensity() - 1.8) > 1e-4 then
    fail("MI.4 instance 1 intensity expect 1.8, got " .. S.GetIntensity())
end
S.SetActiveInstance(0)
pass("MI.4 Per-instance 参数隔离 (intensity 0=0.2, 1=1.8)")

-- MI.5 Per-instance temporal state 隔离 (rejectionMode)
S.SetActiveInstance(0); S.SetRejectionMode(0)
S.SetActiveInstance(sid1); S.SetRejectionMode(1)
S.SetActiveInstance(0)
if S.GetRejectionMode() ~= 0 then
    fail("MI.5 instance 0 rejectionMode expect 0, got " .. S.GetRejectionMode())
end
pass("MI.5 Per-instance temporal state 隔离 (rejectionMode 0=0, 1=1)")

-- MI.6 DestroyInstance(0) 拒绝
if S.DestroyInstance(0) ~= false then fail("MI.6 Destroy(0) should reject") end
pass("MI.6 DestroyInstance(0) 拒绝")

-- MI.7 SetActiveInstance(无效) 拒绝
if S.SetActiveInstance(99) ~= false then fail("MI.7 SetActive(99) should reject") end
pass("MI.7 SetActiveInstance(无效 id) 拒绝")

-- MI.8 清理
S.DestroyInstance(sid1)
S.DestroyInstance(sid2)
S.DestroyInstance(sid3)
if S.GetInstanceCount() ~= 1 then fail("MI.8 cleanup count expect 1") end
pass("MI.8 Destroy 全部 user instance, count=1")

print("[OK] Phase E.9+E.10+E.11+E.12+F.0.10.3+F.0.10.9.x.2 smoke (Light.Graphics.SSR): all checks passed")
