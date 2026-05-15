-- Phase F.0 smoke: Light.Graphics.TAA (Temporal Anti-Aliasing master pipeline surface)
--
-- API coverage (13 functions):
--   Lifecycle 5: Enable / Disable / IsEnabled / IsSupported / Resize
--   Params     6: SetBlendAlpha / GetBlendAlpha (clamp [0, 1], default 0.92)
--                 SetNeighborhoodClip / GetNeighborhoodClip (default true)
--                 SetJitterEnabled / GetJitterEnabled (default true)
--   Status     2: GetFrameCounter (Halton index 0..7, debug HUD)
--                 GetCurrentJitter (returns 2 numbers, sub-pixel offset)
--
-- Backend dual projection (Phase F.0 architecture):
--   - GetProjection() always returns unjittered (SSR/SSAO/velocity unaffected)
--   - LoadJitteredProjection() injected by ApplyJitter() each frame BeginScene
--   - vCurClip in vertex shader uses unjittered uCurViewProj (velocity untouched by jitter)
--
-- Headless guard: same as motion_blur.lua. Enable() MUST either
--   (a) return false cleanly when no GL ctx (typical) OR
--   (b) return true if host already has GL ctx.
-- All Set/Get round-trip + type-error must work regardless of Enable state.
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local TAA = Graphics.TAA
if type(TAA) ~= "table" then
    fail("Light.Graphics.TAA missing or not a table (got " .. type(TAA) .. ")")
end
pass("Light.Graphics.TAA subtable present")

-- ============================================================
-- 1) Module surface: 13 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetBlendAlpha", "GetBlendAlpha",
    "SetNeighborhoodClip", "GetNeighborhoodClip",
    "SetJitterEnabled", "GetJitterEnabled",
    "GetFrameCounter", "GetCurrentJitter",
}
for _, k in ipairs(fn_names) do
    if type(TAA[k]) ~= "function" then
        fail("Light.Graphics.TAA." .. k .. " missing or not a function (got " .. type(TAA[k]) .. ")")
    end
end
pass("Light.Graphics.TAA module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) Initial state probes (IsSupported / IsEnabled)
-- ============================================================

local is_sup = TAA.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = TAA.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("IsEnabled() = false initially (default OFF, matches Phase E modules)")

-- ============================================================
-- 3) Default values round-trip
-- ============================================================

-- BlendAlpha default 0.92 (history weight, slightly higher than SSR Temporal's 0.9)
local def_alpha = TAA.GetBlendAlpha()
if type(def_alpha) ~= "number" then
    fail("GetBlendAlpha should return number, got " .. type(def_alpha))
end
if math.abs(def_alpha - 0.92) > 1e-4 then
    fail("Default GetBlendAlpha must be 0.92, got " .. tostring(def_alpha))
end
pass("Default BlendAlpha = 0.92")

-- NeighborhoodClip default true (9-tap AABB clip enabled)
local def_clip = TAA.GetNeighborhoodClip()
if type(def_clip) ~= "boolean" then
    fail("GetNeighborhoodClip should return boolean, got " .. type(def_clip))
end
if def_clip ~= true then
    fail("Default GetNeighborhoodClip must be true, got " .. tostring(def_clip))
end
pass("Default NeighborhoodClip = true")

-- JitterEnabled default true (super-sampling effect enabled)
local def_jit = TAA.GetJitterEnabled()
if type(def_jit) ~= "boolean" then
    fail("GetJitterEnabled should return boolean, got " .. type(def_jit))
end
if def_jit ~= true then
    fail("Default GetJitterEnabled must be true, got " .. tostring(def_jit))
end
pass("Default JitterEnabled = true")

-- ============================================================
-- 4) Set/Get round-trip — BlendAlpha
-- ============================================================

local function approx_eq(a, b, eps)
    eps = eps or 1e-4
    return math.abs(a - b) <= eps
end

-- Round-trip 0.5..0.99 range
for _, v in ipairs({0.5, 0.7, 0.85, 0.92, 0.95, 0.99}) do
    TAA.SetBlendAlpha(v)
    local got = TAA.GetBlendAlpha()
    if not approx_eq(got, v) then
        fail("BlendAlpha round-trip failed: set=" .. v .. " got=" .. tostring(got))
    end
end
pass("BlendAlpha round-trip ok (0.5..0.99)")

-- clamp test: > 1 should clamp to 1.0; < 0 should clamp to 0.0
TAA.SetBlendAlpha(2.0)
if not approx_eq(TAA.GetBlendAlpha(), 1.0) then
    fail("BlendAlpha clamp upper bound failed: expected 1.0, got " .. tostring(TAA.GetBlendAlpha()))
end
TAA.SetBlendAlpha(-0.5)
if not approx_eq(TAA.GetBlendAlpha(), 0.0) then
    fail("BlendAlpha clamp lower bound failed: expected 0.0, got " .. tostring(TAA.GetBlendAlpha()))
end
pass("BlendAlpha clamp [0, 1] ok")

-- restore default
TAA.SetBlendAlpha(0.92)

-- ============================================================
-- 5) Set/Get round-trip — NeighborhoodClip
-- ============================================================

TAA.SetNeighborhoodClip(false)
if TAA.GetNeighborhoodClip() ~= false then
    fail("NeighborhoodClip round-trip false failed")
end
TAA.SetNeighborhoodClip(true)
if TAA.GetNeighborhoodClip() ~= true then
    fail("NeighborhoodClip round-trip true failed")
end
pass("NeighborhoodClip round-trip ok")

-- type-error: string should fail (return nil + err)
local r1, r2 = TAA.SetNeighborhoodClip("yes")
if r1 ~= nil or type(r2) ~= "string" then
    fail("SetNeighborhoodClip with string should return nil + err string")
end
pass("SetNeighborhoodClip type-error rejected (string)")
-- ensure state not changed by failed call
if TAA.GetNeighborhoodClip() ~= true then
    fail("Failed SetNeighborhoodClip should not change state")
end

-- ============================================================
-- 6) Set/Get round-trip — JitterEnabled
-- ============================================================

TAA.SetJitterEnabled(false)
if TAA.GetJitterEnabled() ~= false then
    fail("JitterEnabled round-trip false failed")
end
TAA.SetJitterEnabled(true)
if TAA.GetJitterEnabled() ~= true then
    fail("JitterEnabled round-trip true failed")
end
pass("JitterEnabled round-trip ok")

-- type-error: number should fail
local r3, r4 = TAA.SetJitterEnabled(1)
if r3 ~= nil or type(r4) ~= "string" then
    fail("SetJitterEnabled with number should return nil + err string")
end
pass("SetJitterEnabled type-error rejected (number)")

-- ============================================================
-- 7) Status query — GetFrameCounter / GetCurrentJitter
-- ============================================================

local fc = TAA.GetFrameCounter()
if type(fc) ~= "number" or fc < 0 or fc > 7 then
    fail("GetFrameCounter must return integer in [0, 7], got " .. tostring(fc))
end
pass("GetFrameCounter() in [0, 7] (value=" .. tostring(fc) .. ")")

local jx, jy = TAA.GetCurrentJitter()
if type(jx) ~= "number" or type(jy) ~= "number" then
    fail("GetCurrentJitter must return 2 numbers, got " .. type(jx) .. ", " .. type(jy))
end
-- Halton-2,3 8-sample range: ±0.5 pixel (matches kHaltonJitter table)
if math.abs(jx) > 0.5 + 1e-4 or math.abs(jy) > 0.5 + 1e-4 then
    fail("GetCurrentJitter out of ±0.5 pixel range: x=" .. tostring(jx) .. " y=" .. tostring(jy))
end
pass("GetCurrentJitter() returns 2 numbers in ±0.5 px (x=" .. string.format("%.4f", jx)
     .. " y=" .. string.format("%.4f", jy) .. ")")

-- ============================================================
-- 8) Lifecycle — Enable / Disable / Resize (headless safe)
-- ============================================================

-- Note: Enable typically returns false in headless smoke (no GL context)
--       Disable/Resize must be no-throw regardless
local enable_ret = TAA.Enable(640, 360)
if type(enable_ret) ~= "boolean" then
    fail("Enable should return boolean, got " .. type(enable_ret))
end
pass("Enable(640, 360) returns boolean (value=" .. tostring(enable_ret) .. ")")

if enable_ret then
    -- GL context present: test full lifecycle
    if TAA.IsEnabled() ~= true then fail("After Enable, IsEnabled must be true") end
    if not TAA.Resize(800, 600) then fail("Resize should succeed when enabled") end
    pass("Enable + Resize ok (GL context present)")
    TAA.Disable()
    if TAA.IsEnabled() ~= false then fail("After Disable, IsEnabled must be false") end
    pass("Disable transitions to false")
else
    -- Headless: Enable failed, but Disable should still be no-throw
    TAA.Disable()
    pass("Headless: Enable returned false; Disable no-throw")
end

-- ============================================================
-- 9) Idempotency — Disable when not enabled is no-op
-- ============================================================

TAA.Disable()
TAA.Disable()
TAA.Disable()
if TAA.IsEnabled() ~= false then
    fail("Triple Disable should leave IsEnabled = false")
end
pass("Triple Disable is idempotent (no-op when not enabled)")

-- ============================================================
-- 10) Coexistence with SSR Temporal (Phase F.0 design: full coexistence)
-- ============================================================

-- TAA Enable should NOT auto-disable SSR Temporal (per CONSENSUS decision #2)
-- Even when GL absent, calling SSR.SetTemporalEnabled is safe (state-only mutation)
local SSR = Graphics.SSR
if type(SSR) == "table" and type(SSR.SetTemporalEnabled) == "function" then
    -- Cache + mutate + restore
    local cached = SSR.GetTemporalEnabled and SSR.GetTemporalEnabled() or true
    SSR.SetTemporalEnabled(true)
    -- TAA toggle should not touch SSR Temporal state
    TAA.SetJitterEnabled(false)
    TAA.SetJitterEnabled(true)
    if SSR.GetTemporalEnabled and SSR.GetTemporalEnabled() ~= true then
        fail("TAA jitter toggle must not affect SSR.GetTemporalEnabled (coexistence)")
    end
    -- restore
    SSR.SetTemporalEnabled(cached)
    pass("TAA + SSR Temporal coexistence verified (no forced mutex)")
else
    pass("SSR subtable absent; coexistence test skipped")
end

print("")
print("=== Phase F.0 TAA smoke: ALL TESTS PASSED ===")
print("Functions covered: " .. #fn_names .. " / 13")
print("Highlights:")
print("  - default OFF, alpha=0.92, neighborhoodClip=true, jitterEnabled=true")
print("  - clamp: BlendAlpha [0, 1]")
print("  - type-error: SetNeighborhoodClip / SetJitterEnabled reject non-boolean")
print("  - status: GetFrameCounter [0, 7], GetCurrentJitter in ±0.5 px range")
print("  - coexistence: TAA toggle does not affect SSR Temporal state")
