-- Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 smoke: Light.Graphics.TAA (Temporal Anti-Aliasing master pipeline surface)
--
-- API coverage (25 functions = F.0 13 + F.0.1 2 + F.0.2 2 + F.0.3 2 + F.0.4 2 + F.0.5 2 + F.0.6 2):
--   Lifecycle 5: Enable / Disable / IsEnabled / IsSupported / Resize
--   Params     6: SetBlendAlpha / GetBlendAlpha (clamp [0, 1], default 0.92)
--                 SetNeighborhoodClip / GetNeighborhoodClip (default true)
--                 SetJitterEnabled / GetJitterEnabled (default true)
--   Phase F.0.1 — Sharpening 2: SetSharpness / GetSharpness (clamp [0, 2], default 0.5)
--                                4-tap unsharp mask, 0=blit fallback, > 0 启用 sharpen pass
--   Phase F.0.4 — Anti-flicker 2: SetAntiFlicker / GetAntiFlicker (boolean, default true)
--                                  Karis luma-weighted blend, false = 纯 alpha blend (F.0 原始)
--   Phase F.0.2/F.0.3 — Clip mode  2: SetClipMode / GetClipMode ("rgb"/"ycocg"/"variance", default "ycocg")
--                                       “ycocg” = AABB clip in YCoCg space (色彩边缘鲁棒)
--                                       “variance” = Salvi 2016 / UE5 default: clip = [mean - γσ, mean + γσ]
--   Phase F.0.3 — Variance gamma 2: SetVarianceGamma / GetVarianceGamma (clamp [0, 4], default 1.0)
--                                       仅 ClipMode=="variance" 生效; γ 越小 clip 越严
--   Phase F.0.5 — Half-res 2: SetHalfResHistory / GetHalfResHistory (boolean, default false)
--                              true: history RT (w/2, h/2), VRAM -75%; 零回归 (默认 OFF)
--   Phase F.0.6 — Sharpen mode 2: SetSharpenMode / GetSharpenMode ("unsharp"/"cas", default "unsharp")
--                                    "unsharp" = F.0.1 4-tap unsharp mask (sharpness [0,2])
--                                    "cas"     = AMD FidelityFX FSR1 5-tap CAS (sharpness [0,1] internal clamp)
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
    "SetSharpness", "GetSharpness",                -- Phase F.0.1
    "SetAntiFlicker", "GetAntiFlicker",            -- Phase F.0.4
    "SetClipMode", "GetClipMode",                  -- Phase F.0.2/F.0.3
    "SetVarianceGamma", "GetVarianceGamma",        -- Phase F.0.3
    "SetHalfResHistory", "GetHalfResHistory",      -- Phase F.0.5
    "SetSharpenMode", "GetSharpenMode",            -- Phase F.0.6
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

-- Phase F.0.1: Sharpness default 0.5 (4-tap unsharp mask)
local def_sharp = TAA.GetSharpness()
if type(def_sharp) ~= "number" then
    fail("GetSharpness should return number, got " .. type(def_sharp))
end
if math.abs(def_sharp - 0.5) > 1e-4 then
    fail("Default GetSharpness must be 0.5, got " .. tostring(def_sharp))
end
pass("Default Sharpness = 0.5 (Phase F.0.1)")

-- Phase F.0.4: AntiFlicker default true (Karis luma weighting blend)
local def_af = TAA.GetAntiFlicker()
if type(def_af) ~= "boolean" then
    fail("GetAntiFlicker should return boolean, got " .. type(def_af))
end
if def_af ~= true then
    fail("Default GetAntiFlicker must be true, got " .. tostring(def_af))
end
pass("Default AntiFlicker = true (Phase F.0.4)")

-- Phase F.0.2: ClipMode default "ycocg" (YCoCg AABB clip)
local def_cm = TAA.GetClipMode()
if type(def_cm) ~= "string" then
    fail("GetClipMode should return string, got " .. type(def_cm))
end
if def_cm ~= "ycocg" then
    fail("Default GetClipMode must be 'ycocg', got '" .. tostring(def_cm) .. "'")
end
pass("Default ClipMode = 'ycocg' (Phase F.0.2)")

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
-- 6.5) Phase F.0.1 — Sharpness round-trip + clamp
-- ============================================================

-- Round-trip 0..2 range
for _, v in ipairs({0.0, 0.3, 0.5, 0.8, 1.0, 1.5, 2.0}) do
    TAA.SetSharpness(v)
    local got = TAA.GetSharpness()
    if not approx_eq(got, v) then
        fail("Sharpness round-trip failed: set=" .. v .. " got=" .. tostring(got))
    end
end
pass("Sharpness round-trip ok (0..2)")

-- clamp test: > 2 -> 2; < 0 -> 0
TAA.SetSharpness(3.0)
if not approx_eq(TAA.GetSharpness(), 2.0) then
    fail("Sharpness clamp upper bound failed: expected 2.0, got " .. tostring(TAA.GetSharpness()))
end
TAA.SetSharpness(-0.5)
if not approx_eq(TAA.GetSharpness(), 0.0) then
    fail("Sharpness clamp lower bound failed: expected 0.0, got " .. tostring(TAA.GetSharpness()))
end
pass("Sharpness clamp [0, 2] ok")

-- Sharpness=0 路径验证 (TAARenderer::Process 会走纯 blit 路径而非 sharpen pass)
TAA.SetSharpness(0.0)
if TAA.GetSharpness() ~= 0.0 then
    fail("SetSharpness(0) failed")
end
pass("Sharpness = 0 路径 ok (Process 内部应走 BlitTAAToHDR 原路径)")

-- 复位 default
TAA.SetSharpness(0.5)

-- ============================================================
-- 6.6) Phase F.0.4 — AntiFlicker round-trip + type-error
-- ============================================================

TAA.SetAntiFlicker(false)
if TAA.GetAntiFlicker() ~= false then
    fail("AntiFlicker round-trip false failed")
end
TAA.SetAntiFlicker(true)
if TAA.GetAntiFlicker() ~= true then
    fail("AntiFlicker round-trip true failed")
end
pass("AntiFlicker round-trip ok")

-- type-error: number / string 都应返回 nil + err (与 SetNeighborhoodClip / SetJitterEnabled 同模式)
local af_r1, af_r2 = TAA.SetAntiFlicker(1)
if af_r1 ~= nil or type(af_r2) ~= "string" then
    fail("SetAntiFlicker with number should return nil + err string")
end
pass("SetAntiFlicker type-error rejected (number)")

local af_r3, af_r4 = TAA.SetAntiFlicker("yes")
if af_r3 ~= nil or type(af_r4) ~= "string" then
    fail("SetAntiFlicker with string should return nil + err string")
end
pass("SetAntiFlicker type-error rejected (string)")

-- 确保失败调用未改变状态
if TAA.GetAntiFlicker() ~= true then
    fail("Failed SetAntiFlicker should not change state")
end
pass("AntiFlicker state preserved on failed call")

-- 与 sharpening 共存验证 (两者可任意组合)
TAA.SetAntiFlicker(true)
TAA.SetSharpness(0.8)
if TAA.GetAntiFlicker() ~= true or math.abs(TAA.GetSharpness() - 0.8) > 1e-4 then
    fail("AntiFlicker + Sharpness coexist failed")
end
pass("AntiFlicker(true) + Sharpness(0.8) coexist ok (Karis blend + 4-tap sharpen 双启)")

-- 复位 default
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)

-- ============================================================
-- 6.7) Phase F.0.2 — ClipMode round-trip + case-insensitive + invalid value
-- ============================================================

-- Round-trip “rgb” / “ycocg” (小写名主路径)
TAA.SetClipMode("rgb")
if TAA.GetClipMode() ~= "rgb" then
    fail("ClipMode round-trip 'rgb' failed, got '" .. tostring(TAA.GetClipMode()) .. "'")
end
TAA.SetClipMode("ycocg")
if TAA.GetClipMode() ~= "ycocg" then
    fail("ClipMode round-trip 'ycocg' failed, got '" .. tostring(TAA.GetClipMode()) .. "'")
end
pass("ClipMode round-trip ok ('rgb' / 'ycocg')")

-- 大小写不敏感：“RGB” / “YCoCg” / “YCOCG” / “Rgb” 都应被接受
for _, v in ipairs({"RGB", "YCoCg", "YCOCG", "Rgb"}) do
    local ok = TAA.SetClipMode(v)
    if ok ~= true then
        fail("ClipMode case-insensitive failed for input '" .. v .. "': SetClipMode returned " .. tostring(ok))
    end
end
-- 最后一个是 "Rgb" → 应该返 "rgb" (规范化存储)
if TAA.GetClipMode() ~= "rgb" then
    fail("ClipMode after 'Rgb' should normalize to 'rgb', got '" .. tostring(TAA.GetClipMode()) .. "'")
end
pass("ClipMode case-insensitive ok ('RGB'/'YCoCg'/'YCOCG'/'Rgb' → normalized)")

-- 非法字符串 返 nil + err
local cm_r1, cm_r2 = TAA.SetClipMode("abc")
if cm_r1 ~= nil or type(cm_r2) ~= "string" then
    fail("SetClipMode with invalid string 'abc' should return nil + err")
end
pass("SetClipMode invalid value 'abc' rejected (nil+err)")

local cm_r3, cm_r4 = TAA.SetClipMode("")
if cm_r3 ~= nil or type(cm_r4) ~= "string" then
    fail("SetClipMode with empty string should return nil + err")
end
pass("SetClipMode empty string rejected (nil+err)")

-- 非 string 返 nil + err (与 SetVelocityFormat 同模式)
local cm_r5, cm_r6 = TAA.SetClipMode(123)
if cm_r5 ~= nil or type(cm_r6) ~= "string" then
    fail("SetClipMode with number should return nil + err")
end
pass("SetClipMode type-error rejected (number)")

local cm_r7, cm_r8 = TAA.SetClipMode(true)
if cm_r7 ~= nil or type(cm_r8) ~= "string" then
    fail("SetClipMode with boolean should return nil + err")
end
pass("SetClipMode type-error rejected (boolean)")

-- 确保失败调用未改变状态
TAA.SetClipMode("ycocg")   -- 先重置到默认
TAA.SetClipMode("abc")     -- 该失败
if TAA.GetClipMode() ~= "ycocg" then
    fail("Failed SetClipMode should not change state")
end
pass("ClipMode state preserved on failed call")

-- F.0.1 + F.0.2 + F.0.4 三启共存验证 (sharpening + clip mode + anti-flicker 同时生效)
TAA.SetSharpness(0.8)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("ycocg")
if math.abs(TAA.GetSharpness() - 0.8) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "ycocg" then
    fail("F.0.1 + F.0.2 + F.0.4 三启共存 failed")
end
pass("Sharpness=0.8 + AntiFlicker=true + ClipMode='ycocg' 三启共存 ok")

-- 复位 default
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("ycocg")

-- ============================================================
-- 6.8) Phase F.0.3 — Variance clipping: “variance” ClipMode + VarianceGamma round-trip + clamp
-- ============================================================

-- Variance clipMode round-trip
local vc_ok = TAA.SetClipMode("variance")
if vc_ok ~= true then
    fail("SetClipMode('variance') should return true, got " .. tostring(vc_ok))
end
if TAA.GetClipMode() ~= "variance" then
    fail("ClipMode round-trip 'variance' failed, got '" .. tostring(TAA.GetClipMode()) .. "'")
end
pass("ClipMode round-trip ok ('variance', Phase F.0.3)")

-- 大小写不敏感验证 "VARIANCE" / "Variance" 都应被接受
for _, v in ipairs({"VARIANCE", "Variance", "vArIaNcE"}) do
    local ok = TAA.SetClipMode(v)
    if ok ~= true then
        fail("ClipMode case-insensitive failed for input '" .. v .. "': " .. tostring(ok))
    end
end
if TAA.GetClipMode() ~= "variance" then
    fail("ClipMode after mixed-case 'variance' should normalize to 'variance', got '" .. tostring(TAA.GetClipMode()) .. "'")
end
pass("ClipMode case-insensitive ok ('VARIANCE'/'Variance'/'vArIaNcE' → normalized 'variance')")

-- VarianceGamma 默认值 1.0 (Salvi 2016 / UE5 推荐)
local def_vg = TAA.GetVarianceGamma()
if type(def_vg) ~= "number" then
    fail("GetVarianceGamma should return number, got " .. type(def_vg))
end
if math.abs(def_vg - 1.0) > 1e-4 then
    fail("Default GetVarianceGamma must be 1.0, got " .. tostring(def_vg))
end
pass("Default VarianceGamma = 1.0 (Salvi 2016 / UE5, Phase F.0.3)")

-- Round-trip 范围 [0, 4]
for _, v in ipairs({0.0, 0.25, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0}) do
    TAA.SetVarianceGamma(v)
    local got = TAA.GetVarianceGamma()
    if not approx_eq(got, v) then
        fail("VarianceGamma round-trip failed: set=" .. v .. " got=" .. tostring(got))
    end
end
pass("VarianceGamma round-trip ok ([0, 4])")

-- clamp test: > 4 → 4; < 0 → 0
TAA.SetVarianceGamma(10.0)
if not approx_eq(TAA.GetVarianceGamma(), 4.0) then
    fail("VarianceGamma clamp upper bound failed: expected 4.0, got " .. tostring(TAA.GetVarianceGamma()))
end
TAA.SetVarianceGamma(-2.0)
if not approx_eq(TAA.GetVarianceGamma(), 0.0) then
    fail("VarianceGamma clamp lower bound failed: expected 0.0, got " .. tostring(TAA.GetVarianceGamma()))
end
pass("VarianceGamma clamp [0, 4] ok")

-- type-error: string / nil / boolean 都应报错 (luaL_checknumber 会 raise error)
local vg_ok1, vg_err1 = pcall(TAA.SetVarianceGamma, "foo")
if vg_ok1 then
    fail("SetVarianceGamma with string should raise error")
end
pass("SetVarianceGamma type-error rejected (string) [" .. tostring(vg_err1):sub(1, 60) .. "...]")

local vg_ok2, vg_err2 = pcall(TAA.SetVarianceGamma, true)
if vg_ok2 then
    fail("SetVarianceGamma with boolean should raise error")
end
pass("SetVarianceGamma type-error rejected (boolean)")

-- F.0.1 + F.0.2 + F.0.3 + F.0.4 四启共存验证
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.5)
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.5) > 1e-4 then
    fail("F.0.1 + F.0.2 + F.0.3 + F.0.4 四启共存 failed")
end
pass("Sharpness=0.5 + AntiFlicker=true + ClipMode='variance' + VarianceGamma=1.5 四启共存 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)

-- ============================================================
-- 6.9) Phase F.0.5 — Half-res history RT
-- ============================================================

-- 默认值 = false (零回归, 与 F.0/0.1/0.2/0.3/0.4 行为一致)
local def_hr = TAA.GetHalfResHistory()
if type(def_hr) ~= "boolean" then
    fail("GetHalfResHistory should return boolean, got " .. type(def_hr))
end
if def_hr ~= false then
    fail("Default GetHalfResHistory must be false (零回归), got " .. tostring(def_hr))
end
pass("Default HalfResHistory = false (Phase F.0.5 零回归)")

-- round-trip: false → true → false
local hr_set1 = TAA.SetHalfResHistory(true)
if hr_set1 ~= true then
    fail("SetHalfResHistory(true) should return true, got " .. tostring(hr_set1))
end
if TAA.GetHalfResHistory() ~= true then
    fail("Round-trip true failed, got " .. tostring(TAA.GetHalfResHistory()))
end
pass("HalfResHistory round-trip true ok")

TAA.SetHalfResHistory(false)
if TAA.GetHalfResHistory() ~= false then
    fail("Round-trip false failed, got " .. tostring(TAA.GetHalfResHistory()))
end
pass("HalfResHistory round-trip false ok")

-- type-error: 非 boolean 报错 (与 SetNeighborhoodClip / SetJitterEnabled / SetAntiFlicker 同模式)
local hr_ok1, hr_err1 = pcall(TAA.SetHalfResHistory, "true")
if hr_ok1 then
    fail("SetHalfResHistory with string should raise error")
end
pass("SetHalfResHistory type-error rejected (string) [" .. tostring(hr_err1):sub(1, 60) .. "...]")

local hr_ok2, hr_err2 = pcall(TAA.SetHalfResHistory, 1)
if hr_ok2 then
    fail("SetHalfResHistory with number should raise error")
end
pass("SetHalfResHistory type-error rejected (number)")

local hr_ok3, hr_err3 = pcall(TAA.SetHalfResHistory, nil)
if hr_ok3 then
    fail("SetHalfResHistory with nil should raise error")
end
pass("SetHalfResHistory type-error rejected (nil)")

-- 状态独立验证: 切换 halfRes 不影响 alpha / clipMode / sharpness / antiFlicker
local a_before  = TAA.GetBlendAlpha()
local cm_before = TAA.GetClipMode()
local sh_before = TAA.GetSharpness()
local af_before = TAA.GetAntiFlicker()
local vg_before = TAA.GetVarianceGamma()

TAA.SetHalfResHistory(true)
TAA.SetHalfResHistory(false)
TAA.SetHalfResHistory(true)

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or TAA.GetClipMode()           ~= cm_before
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or TAA.GetAntiFlicker()        ~= af_before
   or math.abs(TAA.GetVarianceGamma() - vg_before) > 1e-4 then
    fail("HalfResHistory toggle should not affect alpha/clipMode/sharpness/antiFlicker/varianceGamma")
end
pass("HalfResHistory 切换不影响其他参数 (状态独立)")

-- F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 五启共存验证
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.5)
TAA.SetHalfResHistory(true)
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.5) > 1e-4
   or TAA.GetHalfResHistory() ~= true then
    fail("F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 五启共存 failed")
end
pass("Sharpness=0.5 + AntiFlicker=true + ClipMode='variance' + VarianceGamma=1.5 + HalfResHistory=true 五启共存 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)

-- ============================================================
-- 6.10) Phase F.0.6 — Sharpen mode ("unsharp" / "cas")
-- ============================================================

-- 默认值 = "unsharp" (零回归, F.0.1 4-tap unsharp mask)
local def_sm = TAA.GetSharpenMode()
if type(def_sm) ~= "string" then
    fail("GetSharpenMode should return string, got " .. type(def_sm))
end
if def_sm ~= "unsharp" then
    fail("Default GetSharpenMode must be 'unsharp', got '" .. tostring(def_sm) .. "'")
end
pass("Default SharpenMode = 'unsharp' (Phase F.0.6 零回归)")

-- round-trip: unsharp → cas → unsharp
local sm_set1 = TAA.SetSharpenMode("cas")
if sm_set1 ~= true then fail("SetSharpenMode('cas') should return true, got " .. tostring(sm_set1)) end
if TAA.GetSharpenMode() ~= "cas" then
    fail("Round-trip 'cas' failed, got '" .. TAA.GetSharpenMode() .. "'")
end
pass("SharpenMode round-trip 'cas' ok")

TAA.SetSharpenMode("unsharp")
if TAA.GetSharpenMode() ~= "unsharp" then
    fail("Round-trip 'unsharp' failed, got '" .. TAA.GetSharpenMode() .. "'")
end
pass("SharpenMode round-trip 'unsharp' ok")

-- 大小写不敏感 ("CAS" / "Cas" / "UNSHARP" / "Unsharp" 均应接受)
TAA.SetSharpenMode("CAS")
if TAA.GetSharpenMode() ~= "cas" then
    fail("Case-insensitive 'CAS' failed, got '" .. TAA.GetSharpenMode() .. "'")
end
pass("SharpenMode case-insensitive 'CAS' → 'cas' ok")

TAA.SetSharpenMode("Cas")
if TAA.GetSharpenMode() ~= "cas" then fail("Case-insensitive 'Cas' failed") end
pass("SharpenMode case-insensitive 'Cas' → 'cas' ok")

TAA.SetSharpenMode("UNSHARP")
if TAA.GetSharpenMode() ~= "unsharp" then fail("Case-insensitive 'UNSHARP' failed") end
pass("SharpenMode case-insensitive 'UNSHARP' → 'unsharp' ok")

-- invalid 字符串 → 返 nil + err (与 SetClipMode 同模式), state 不变
TAA.SetSharpenMode("unsharp")          -- 重置
local sm_invalid_ok, sm_invalid_err = TAA.SetSharpenMode("foo")
if sm_invalid_ok ~= nil then
    fail("SetSharpenMode('foo') should return nil, got " .. tostring(sm_invalid_ok))
end
if type(sm_invalid_err) ~= "string" or not sm_invalid_err:find("foo") then
    fail("SetSharpenMode('foo') should err with 'foo' mentioned, got '" .. tostring(sm_invalid_err) .. "'")
end
if TAA.GetSharpenMode() ~= "unsharp" then
    fail("After invalid mode, state must remain 'unsharp', got '" .. TAA.GetSharpenMode() .. "'")
end
pass("SharpenMode invalid 'foo' → nil+err, state 不变 ok")

-- type-error: 非 string → 返 nil + err (与 SetClipMode 同模式, 不使用 luaL_check)
local sm_ok1, sm_err1 = TAA.SetSharpenMode(123)
if sm_ok1 ~= nil then fail("SetSharpenMode(123) should return nil, got " .. tostring(sm_ok1)) end
pass("SharpenMode type-error rejected (number) [" .. tostring(sm_err1):sub(1, 60) .. "...]")

local sm_ok2, sm_err2 = TAA.SetSharpenMode(true)
if sm_ok2 ~= nil then fail("SetSharpenMode(true) should return nil") end
pass("SharpenMode type-error rejected (boolean)")

-- 状态独立验证: 切换 sharpenMode 不影响 alpha / clipMode / sharpness / antiFlicker / halfRes
local a_before  = TAA.GetBlendAlpha()
local cm_before = TAA.GetClipMode()
local sh_before = TAA.GetSharpness()
local af_before = TAA.GetAntiFlicker()
local hr_before = TAA.GetHalfResHistory()

TAA.SetSharpenMode("cas")
TAA.SetSharpenMode("unsharp")
TAA.SetSharpenMode("cas")

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or TAA.GetClipMode()           ~= cm_before
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or TAA.GetAntiFlicker()        ~= af_before
   or TAA.GetHalfResHistory()     ~= hr_before then
    fail("SharpenMode toggle should not affect alpha/clipMode/sharpness/antiFlicker/halfRes")
end
pass("SharpenMode 切换不影响其他参数 (状态独立)")

-- F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 六启共存验证
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.5)
TAA.SetHalfResHistory(true)
TAA.SetSharpenMode("cas")
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.5) > 1e-4
   or TAA.GetHalfResHistory() ~= true
   or TAA.GetSharpenMode() ~= "cas" then
    fail("F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 六启共存 failed")
end
pass("Sharpness=0.5 + AntiFlicker=true + ClipMode='variance' + VarianceGamma=1.5 + HalfResHistory=true + SharpenMode='cas' 六启共存 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)
TAA.SetSharpenMode("unsharp")

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
print("=== Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 TAA smoke: ALL TESTS PASSED ===")
print("Functions covered: " .. #fn_names .. " / 25")
print("Highlights:")
print("  - default OFF, alpha=0.92, neighborhoodClip=true, jitterEnabled=true, sharpness=0.5, antiFlicker=true, clipMode='ycocg', varianceGamma=1.0, halfResHistory=false, sharpenMode='unsharp'")
print("  - clamp: BlendAlpha [0, 1], Sharpness [0, 2], VarianceGamma [0, 4]")
print("  - type-error: SetNeighborhoodClip / SetJitterEnabled / SetAntiFlicker / SetHalfResHistory reject non-boolean; SetClipMode / SetSharpenMode reject non-string / invalid value; SetVarianceGamma reject non-number")
print("  - Phase F.0.6: 5-tap CAS (AMD FSR1) vs 4-tap unsharp; CAS contrast-adaptive + HDR safe; sharpenMode='cas' -> peak ∈ [-1/8, -1/5] @ sharpness ∈ [0, 1]")
print("  - status: GetFrameCounter [0, 7], GetCurrentJitter in ±0.5 px range")
print("  - coexistence: TAA toggle does not affect SSR Temporal state")
print("  - Phase F.0.1: 4-tap unsharp mask, sharpness=0 走 blit fallback (零 ALU)")
print("  - Phase F.0.4: Karis luma-weighted blend, antiFlicker=false 走 F.0 纯 alpha blend")
print("  - Phase F.0.2: YCoCg AABB clip, clipMode='rgb' 走 F.0 三通道 RGB clip (零 ALU 增量)")
print("  - Phase F.0.3: variance clip = mean ± γσ (Salvi 2016 / UE5), 优于 AABB 的 single-outlier 鲁棒性")
print("  - Phase F.0.5: half-res history RT (w/2,h/2), VRAM -75% (1080p 33.2MB→8.3MB; 4K 132.7MB→33.2MB), 默认 OFF")
