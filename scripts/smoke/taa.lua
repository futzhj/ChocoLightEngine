-- Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.8 + F.0.9 + F.0.12 + F.0.13 + F.0.14 smoke: Light.Graphics.TAA (Temporal Anti-Aliasing master pipeline surface)
--
-- API coverage (40 functions = F.0 13 + F.0.1 2 + F.0.2 2 + F.0.3 2 + F.0.4 2 + F.0.5 2 + F.0.6/F.0.12 2 + F.0.8 4 + F.0.9/F.0.14 2 + F.0.13 4 + F.0.10 5):
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
--   Phase F.0.6/F.0.12 — Sharpen mode 2: SetSharpenMode / GetSharpenMode ("unsharp"/"cas"/"rcas", default "unsharp")
--                                    "unsharp" = F.0.1 4-tap unsharp mask (sharpness [0,2])
--                                    "cas"     = AMD FidelityFX FSR1 5-tap CAS (sharpness [0,1] internal clamp)
--                                    "rcas"    = AMD FidelityFX FSR2 5-tap RCAS (Robust CAS, sharpness [0,2])
--   Phase F.0.8 — Motion-adaptive γ 4: SetMotionGamma / GetMotionGamma (clamp [0, 4], default 1.5)
--                                       SetMotionAdaptive / GetMotionAdaptive (boolean, default false)
--                                       UE5 高级形式: 静止区域 γ=varianceGamma, 高速区域 lerp 到 motionGamma
--                                       仅 ClipMode=="variance" 生效; 默认关零回归
--   Phase F.0.13 — Motion-adaptive sharpness 4: SetMotionAdaptiveSharpness / Get (boolean, default false)
--                                                  SetMotionSharpness / Get (clamp [0, 2], default 0.1)
--                                                  高速运动时 effSharpness lerp 到 motionSharpness 减 trail
--                                                  backend Frobenius distance 自动估计 motion factor; 默认关零回归
--   Phase F.0.9/F.0.14 — Custom upsampler 2: SetUpscaleMode / GetUpscaleMode ("bilinear"/"bicubic"/"lanczos", default "bilinear")
--                                       "bilinear" = F.0.5 GL_LINEAR stretch (零回归)
--                                       "bicubic"  = Catmull-Rom 9-tap (Sigggraph 2018 Filmic SMAA)
--                                       "lanczos"  = Lanczos-2 25-tap 5x5 (Phase F.0.14, 超高画质)
--                                       仅 sharpness=0 && halfResHistory=true 时实际生效
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
    "SetMotionGamma", "GetMotionGamma",            -- Phase F.0.8
    "SetMotionAdaptive", "GetMotionAdaptive",      -- Phase F.0.8
    "SetMotionAdaptiveSharpness", "GetMotionAdaptiveSharpness",  -- Phase F.0.13
    "SetMotionSharpness", "GetMotionSharpness",    -- Phase F.0.13
    "SetUpscaleMode", "GetUpscaleMode",            -- Phase F.0.9
    "CreateInstance", "DestroyInstance",            -- Phase F.0.10
    "SetActiveInstance", "GetActiveInstance",       -- Phase F.0.10
    "GetInstanceCount",                              -- Phase F.0.10
    "Process",                                       -- Phase F.0.10.2 (manual TAA region process)
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

-- Phase F.0.12: round-trip rcas
local sm_set2 = TAA.SetSharpenMode("rcas")
if sm_set2 ~= true then fail("SetSharpenMode('rcas') should return true, got " .. tostring(sm_set2)) end
if TAA.GetSharpenMode() ~= "rcas" then
    fail("Round-trip 'rcas' failed, got '" .. TAA.GetSharpenMode() .. "'")
end
pass("SharpenMode round-trip 'rcas' ok (Phase F.0.12 FSR2 Robust CAS)")
TAA.SetSharpenMode("unsharp")  -- 复位

-- 大小写不敏感 ("CAS" / "Cas" / "UNSHARP" / "Unsharp" / "RCAS" / "Rcas" 均应接受)
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

-- Phase F.0.12: rcas 大小写不敏感
TAA.SetSharpenMode("RCAS")
if TAA.GetSharpenMode() ~= "rcas" then fail("Case-insensitive 'RCAS' failed") end
pass("SharpenMode case-insensitive 'RCAS' → 'rcas' ok")

TAA.SetSharpenMode("Rcas")
if TAA.GetSharpenMode() ~= "rcas" then fail("Case-insensitive 'Rcas' failed") end
pass("SharpenMode case-insensitive 'Rcas' → 'rcas' ok")

TAA.SetSharpenMode("RCAs")
if TAA.GetSharpenMode() ~= "rcas" then fail("Case-insensitive 'RCAs' failed") end
pass("SharpenMode case-insensitive 'RCAs' → 'rcas' ok")

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
TAA.SetSharpenMode("rcas")   -- Phase F.0.12 加入三轮循环
TAA.SetSharpenMode("unsharp")

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or TAA.GetClipMode()           ~= cm_before
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or TAA.GetAntiFlicker()        ~= af_before
   or TAA.GetHalfResHistory()     ~= hr_before then
    fail("SharpenMode toggle should not affect alpha/clipMode/sharpness/antiFlicker/halfRes")
end
pass("SharpenMode 切换 (unsharp↔cas↔rcas) 不影响其他参数 (状态独立)")

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
-- 6.11) Phase F.0.8 — Motion-Adaptive Variance γ (UE5 高级形式)
-- ============================================================

-- 6.11.1) MotionGamma 默认 = 1.5 (UE5 推荐)
local def_mg = TAA.GetMotionGamma()
if type(def_mg) ~= "number" then
    fail("GetMotionGamma should return number, got " .. type(def_mg))
end
if math.abs(def_mg - 1.5) > 1e-4 then
    fail("Default GetMotionGamma must be 1.5 (UE5 推荐), got " .. tostring(def_mg))
end
pass("Default MotionGamma = 1.5 (Phase F.0.8 UE5 推荐)")

-- 6.11.2) MotionAdaptive 默认 = false (零回归)
local def_ma = TAA.GetMotionAdaptive()
if type(def_ma) ~= "boolean" then
    fail("GetMotionAdaptive should return boolean, got " .. type(def_ma))
end
if def_ma ~= false then
    fail("Default GetMotionAdaptive must be false (零回归), got " .. tostring(def_ma))
end
pass("Default MotionAdaptive = false (Phase F.0.8 零回归)")

-- 6.11.3) MotionGamma round-trip
TAA.SetMotionGamma(2.5)
if math.abs(TAA.GetMotionGamma() - 2.5) > 1e-4 then
    fail("MotionGamma round-trip 2.5 failed, got " .. tostring(TAA.GetMotionGamma()))
end
pass("MotionGamma round-trip 2.5 ok")

TAA.SetMotionGamma(0.0)
if math.abs(TAA.GetMotionGamma() - 0.0) > 1e-4 then fail("MotionGamma round-trip 0.0 failed") end
pass("MotionGamma round-trip 0.0 ok")

-- 6.11.4) MotionGamma clamp [0, 4]
TAA.SetMotionGamma(-1.0)
if math.abs(TAA.GetMotionGamma() - 0.0) > 1e-4 then
    fail("MotionGamma clamp [-1.0 → 0.0] failed, got " .. tostring(TAA.GetMotionGamma()))
end
pass("MotionGamma clamp lower [-1 → 0] ok")

TAA.SetMotionGamma(10.0)
if math.abs(TAA.GetMotionGamma() - 4.0) > 1e-4 then
    fail("MotionGamma clamp [10 → 4] failed, got " .. tostring(TAA.GetMotionGamma()))
end
pass("MotionGamma clamp upper [10 → 4] ok")

-- 6.11.5) MotionGamma type-error
local mg_ok, mg_err = pcall(TAA.SetMotionGamma, "not-a-number")
if mg_ok then fail("SetMotionGamma with string should raise error") end
pass("MotionGamma type-error rejected (string) [" .. tostring(mg_err):sub(1, 60) .. "...]")

-- 6.11.6) MotionAdaptive round-trip
TAA.SetMotionAdaptive(true)
if TAA.GetMotionAdaptive() ~= true then fail("MotionAdaptive round-trip true failed") end
pass("MotionAdaptive round-trip true ok")

TAA.SetMotionAdaptive(false)
if TAA.GetMotionAdaptive() ~= false then fail("MotionAdaptive round-trip false failed") end
pass("MotionAdaptive round-trip false ok")

-- 6.11.7) MotionAdaptive type-error (luaL_checktype TBOOLEAN)
local ma_ok1, ma_err1 = pcall(TAA.SetMotionAdaptive, "true")
if ma_ok1 then fail("SetMotionAdaptive(string) should raise error") end
pass("MotionAdaptive type-error rejected (string)")

local ma_ok2, ma_err2 = pcall(TAA.SetMotionAdaptive, 1)
if ma_ok2 then fail("SetMotionAdaptive(number) should raise error") end
pass("MotionAdaptive type-error rejected (number)")

-- 6.11.8) 状态独立验证: 切换 motion-adaptive 不影响其他参数
local a_before  = TAA.GetBlendAlpha()
local cm_before = TAA.GetClipMode()
local sh_before = TAA.GetSharpness()
local vg_before = TAA.GetVarianceGamma()
local hr_before = TAA.GetHalfResHistory()
local sm_before = TAA.GetSharpenMode()

TAA.SetMotionAdaptive(true)
TAA.SetMotionGamma(2.0)
TAA.SetMotionAdaptive(false)
TAA.SetMotionGamma(1.5)         -- 复位

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or TAA.GetClipMode()           ~= cm_before
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or math.abs(TAA.GetVarianceGamma() - vg_before) > 1e-4
   or TAA.GetHalfResHistory()     ~= hr_before
   or TAA.GetSharpenMode()        ~= sm_before then
    fail("MotionAdaptive/MotionGamma toggle should not affect alpha/clipMode/sharpness/varianceGamma/halfRes/sharpenMode")
end
pass("MotionAdaptive/MotionGamma 切换不影响其他参数 (状态独立)")

-- 6.11.9) F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.8 七启共存验证
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(true)
TAA.SetSharpenMode("cas")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(true)
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.0) > 1e-4
   or TAA.GetHalfResHistory() ~= true
   or TAA.GetSharpenMode() ~= "cas"
   or math.abs(TAA.GetMotionGamma() - 1.5) > 1e-4
   or TAA.GetMotionAdaptive() ~= true then
    fail("F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.8 七启共存 failed")
end
pass("Sharpness=0.5 + AntiFlicker=true + ClipMode='variance' + VarianceGamma=1.0 + HalfResHistory=true + SharpenMode='cas' + MotionGamma=1.5 + MotionAdaptive=true 七启共存 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)
TAA.SetSharpenMode("unsharp")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(false)

-- ============================================================
-- 6.12) Phase F.0.9 — Custom Upsampler ("bilinear" / "bicubic" Catmull-Rom)
-- ============================================================

-- 6.12.1) UpscaleMode 默认 = "bilinear" (零回归)
local def_um = TAA.GetUpscaleMode()
if type(def_um) ~= "string" then
    fail("GetUpscaleMode should return string, got " .. type(def_um))
end
if def_um ~= "bilinear" then
    fail("Default GetUpscaleMode must be 'bilinear' (零回归), got '" .. tostring(def_um) .. "'")
end
pass("Default UpscaleMode = 'bilinear' (Phase F.0.9 零回归)")

-- 6.12.2) round-trip
local um_set1 = TAA.SetUpscaleMode("bicubic")
if um_set1 ~= true then fail("SetUpscaleMode('bicubic') should return true") end
if TAA.GetUpscaleMode() ~= "bicubic" then fail("Round-trip 'bicubic' failed") end
pass("UpscaleMode round-trip 'bicubic' ok")

TAA.SetUpscaleMode("bilinear")
if TAA.GetUpscaleMode() ~= "bilinear" then fail("Round-trip 'bilinear' failed") end
pass("UpscaleMode round-trip 'bilinear' ok")

-- 6.12.3) 大小写不敏感
TAA.SetUpscaleMode("BICUBIC")
if TAA.GetUpscaleMode() ~= "bicubic" then fail("Case-insensitive 'BICUBIC' failed") end
pass("UpscaleMode case-insensitive 'BICUBIC' → 'bicubic' ok")

TAA.SetUpscaleMode("BiCuBiC")
if TAA.GetUpscaleMode() ~= "bicubic" then fail("Case-insensitive 'BiCuBiC' failed") end
pass("UpscaleMode case-insensitive 'BiCuBiC' → 'bicubic' ok")

TAA.SetUpscaleMode("BILINEAR")
if TAA.GetUpscaleMode() ~= "bilinear" then fail("Case-insensitive 'BILINEAR' failed") end
pass("UpscaleMode case-insensitive 'BILINEAR' → 'bilinear' ok")

-- 6.12.4) invalid 返 nil + err, state 不变
TAA.SetUpscaleMode("bicubic")        -- 先设为 bicubic
local um_inv_ok, um_inv_err = TAA.SetUpscaleMode("foo")
if um_inv_ok ~= nil then
    fail("SetUpscaleMode('foo') should return nil, got " .. tostring(um_inv_ok))
end
if type(um_inv_err) ~= "string" or not um_inv_err:find("foo") then
    fail("SetUpscaleMode('foo') should err with 'foo' mentioned, got '" .. tostring(um_inv_err) .. "'")
end
if TAA.GetUpscaleMode() ~= "bicubic" then
    fail("After invalid mode, state must remain 'bicubic', got '" .. TAA.GetUpscaleMode() .. "'")
end
pass("UpscaleMode invalid 'foo' → nil+err, state 不变 ok")

-- 6.12.5) type-error
local um_ok1, um_err1 = TAA.SetUpscaleMode(123)
if um_ok1 ~= nil then fail("SetUpscaleMode(123) should return nil") end
pass("UpscaleMode type-error rejected (number)")

local um_ok2 = TAA.SetUpscaleMode(true)
if um_ok2 ~= nil then fail("SetUpscaleMode(true) should return nil") end
pass("UpscaleMode type-error rejected (boolean)")

-- 6.12.6) 状态独立验证: 切 upscaleMode 不影响其他参数
TAA.SetUpscaleMode("bilinear")  -- 复位
local a_before  = TAA.GetBlendAlpha()
local cm_before = TAA.GetClipMode()
local sh_before = TAA.GetSharpness()
local hr_before = TAA.GetHalfResHistory()
local sm_before = TAA.GetSharpenMode()
local ma_before = TAA.GetMotionAdaptive()

TAA.SetUpscaleMode("bicubic")
TAA.SetUpscaleMode("bilinear")
TAA.SetUpscaleMode("bicubic")

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or TAA.GetClipMode()           ~= cm_before
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or TAA.GetHalfResHistory()     ~= hr_before
   or TAA.GetSharpenMode()        ~= sm_before
   or TAA.GetMotionAdaptive()     ~= ma_before then
    fail("UpscaleMode toggle should not affect alpha/clipMode/sharpness/halfRes/sharpenMode/motionAdaptive")
end
pass("UpscaleMode 切换不影响其他参数 (状态独立)")

-- 6.12.7) F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9 八启共存
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(true)
TAA.SetSharpenMode("cas")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(true)
TAA.SetUpscaleMode("bicubic")
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.0) > 1e-4
   or TAA.GetHalfResHistory() ~= true
   or TAA.GetSharpenMode() ~= "cas"
   or math.abs(TAA.GetMotionGamma() - 1.5) > 1e-4
   or TAA.GetMotionAdaptive() ~= true
   or TAA.GetUpscaleMode() ~= "bicubic" then
    fail("F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9 八启共存 failed")
end
pass("Sharpness=0.5 + AntiFlicker=true + ClipMode='variance' + VarianceGamma=1.0 + HalfResHistory=true + SharpenMode='cas' + MotionAdaptive=true + UpscaleMode='bicubic' 八启共存 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)
TAA.SetSharpenMode("unsharp")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(false)
TAA.SetUpscaleMode("bilinear")

-- ============================================================
-- 6.13) Phase F.0.13 — Motion-adaptive sharpness (与 F.0.8 成对)
-- ============================================================

-- 6.13.1) motionAdaptiveSharpness 默认 = false (零回归)
local def_mas = TAA.GetMotionAdaptiveSharpness()
if type(def_mas) ~= "boolean" then fail("GetMotionAdaptiveSharpness should be boolean, got " .. type(def_mas)) end
if def_mas ~= false then fail("Default GetMotionAdaptiveSharpness must be false (零回归)") end
pass("Default MotionAdaptiveSharpness = false (Phase F.0.13 零回归)")

-- 6.13.2) motionSharpness 默认 = 0.1
local def_ms = TAA.GetMotionSharpness()
if type(def_ms) ~= "number" then fail("GetMotionSharpness should be number, got " .. type(def_ms)) end
if math.abs(def_ms - 0.1) > 1e-4 then
    fail("Default GetMotionSharpness must be 0.1, got " .. tostring(def_ms))
end
pass("Default MotionSharpness = 0.1")

-- 6.13.3) round-trip
local ok_mas = TAA.SetMotionAdaptiveSharpness(true)
if ok_mas ~= true then fail("SetMotionAdaptiveSharpness(true) should return true") end
if TAA.GetMotionAdaptiveSharpness() ~= true then fail("Round-trip MotionAdaptiveSharpness=true failed") end
pass("MotionAdaptiveSharpness round-trip true ok")

TAA.SetMotionAdaptiveSharpness(false)
if TAA.GetMotionAdaptiveSharpness() ~= false then fail("Round-trip MotionAdaptiveSharpness=false failed") end
pass("MotionAdaptiveSharpness round-trip false ok")

TAA.SetMotionSharpness(0.8)
if math.abs(TAA.GetMotionSharpness() - 0.8) > 1e-4 then fail("Round-trip MotionSharpness=0.8 failed") end
pass("MotionSharpness round-trip 0.8 ok")

-- 6.13.4) clamp 验证 [0, 2]
TAA.SetMotionSharpness(-1.0)
if math.abs(TAA.GetMotionSharpness() - 0.0) > 1e-4 then
    fail("MotionSharpness clamp lower bound failed (-1 → " .. TAA.GetMotionSharpness() .. ")")
end
pass("MotionSharpness clamp lower bound (-1 → 0) ok")

TAA.SetMotionSharpness(5.0)
if math.abs(TAA.GetMotionSharpness() - 2.0) > 1e-4 then
    fail("MotionSharpness clamp upper bound failed (5 → " .. TAA.GetMotionSharpness() .. ")")
end
pass("MotionSharpness clamp upper bound (5 → 2) ok")

TAA.SetMotionSharpness(0.1)  -- 复位

-- 6.13.5) type-error
local mas_t1 = TAA.SetMotionAdaptiveSharpness(1)
if mas_t1 ~= nil then fail("SetMotionAdaptiveSharpness(1) should return nil") end
pass("MotionAdaptiveSharpness type-error rejected (number)")

local mas_t2 = TAA.SetMotionAdaptiveSharpness("true")
if mas_t2 ~= nil then fail("SetMotionAdaptiveSharpness(string) should return nil") end
pass("MotionAdaptiveSharpness type-error rejected (string)")

local ms_t1 = TAA.SetMotionSharpness("0.5")
if ms_t1 ~= nil then fail("SetMotionSharpness(string) should return nil") end
pass("MotionSharpness type-error rejected (string)")

local ms_t2 = TAA.SetMotionSharpness(true)
if ms_t2 ~= nil then fail("SetMotionSharpness(boolean) should return nil") end
pass("MotionSharpness type-error rejected (boolean)")

-- 6.13.6) 状态独立: 切换不影响其他参数
local a_before  = TAA.GetBlendAlpha()
local sh_before = TAA.GetSharpness()
local sm_before = TAA.GetSharpenMode()
local mg_before = TAA.GetMotionAdaptive()

TAA.SetMotionAdaptiveSharpness(true)
TAA.SetMotionSharpness(0.5)
TAA.SetMotionAdaptiveSharpness(false)
TAA.SetMotionSharpness(0.1)

if math.abs(TAA.GetBlendAlpha()  - a_before)  > 1e-4
   or math.abs(TAA.GetSharpness() - sh_before) > 1e-4
   or TAA.GetSharpenMode()        ~= sm_before
   or TAA.GetMotionAdaptive()     ~= mg_before then
    fail("MotionAdaptiveSharpness/MotionSharpness toggle should not affect alpha/sharpness/sharpenMode/motionAdaptive")
end
pass("MotionAdaptiveSharpness/MotionSharpness 切换不影响其他参数 (状态独立)")

-- 6.13.7) F.0.13 与所有 sharpenMode (unsharp/cas/rcas) 共存
for _, mode in ipairs({"unsharp", "cas", "rcas"}) do
    TAA.SetSharpenMode(mode)
    TAA.SetMotionAdaptiveSharpness(true)
    TAA.SetMotionSharpness(0.2)
    -- 不崩 + state 保持
    if TAA.GetSharpenMode() ~= mode then
        fail("sharpenMode='" .. mode .. "' 与 F.0.13 不共存")
    end
    if TAA.GetMotionAdaptiveSharpness() ~= true then
        fail("F.0.13 in sharpenMode='" .. mode .. "' state lost")
    end
    pass("F.0.13 与 sharpenMode='" .. mode .. "' 共存 ok")
end
TAA.SetSharpenMode("unsharp")
TAA.SetMotionAdaptiveSharpness(false)
TAA.SetMotionSharpness(0.1)

-- 6.13.8) F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9+F.0.12+F.0.13 十启共存
TAA.SetSharpness(0.5)
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(true)
TAA.SetSharpenMode("rcas")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(true)
TAA.SetUpscaleMode("bicubic")
TAA.SetMotionAdaptiveSharpness(true)
TAA.SetMotionSharpness(0.1)
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.0) > 1e-4
   or TAA.GetHalfResHistory() ~= true
   or TAA.GetSharpenMode() ~= "rcas"
   or math.abs(TAA.GetMotionGamma() - 1.5) > 1e-4
   or TAA.GetMotionAdaptive() ~= true
   or TAA.GetUpscaleMode() ~= "bicubic"
   or TAA.GetMotionAdaptiveSharpness() ~= true
   or math.abs(TAA.GetMotionSharpness() - 0.1) > 1e-4 then
    fail("F.0.1+0.2+0.3+0.4+0.5+0.6+0.8+0.9+0.12+0.13 十启共存 failed")
end
pass("十启共存: sharp=0.5 + AF=true + clip=variance + vg=1.0 + halfRes=true + sharpenMode=rcas + mg=1.5 + ma=true + upscale=bicubic + mas=true + ms=0.1 ok")

-- 复位 default
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)
TAA.SetSharpenMode("unsharp")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(false)
TAA.SetUpscaleMode("bilinear")
TAA.SetMotionAdaptiveSharpness(false)
TAA.SetMotionSharpness(0.1)

-- ============================================================
-- 6.14) Phase F.0.14 — Lanczos-2 25-tap 5x5 上采样 (超高画质选项)
-- ============================================================

-- 6.14.1) lanczos round-trip
local lz1 = TAA.SetUpscaleMode("lanczos")
if lz1 ~= true then fail("SetUpscaleMode('lanczos') should return true") end
if TAA.GetUpscaleMode() ~= "lanczos" then
    fail("Round-trip 'lanczos' failed, got '" .. tostring(TAA.GetUpscaleMode()) .. "'")
end
pass("UpscaleMode round-trip 'lanczos' ok (Phase F.0.14)")

-- 6.14.2) lanczos 大小写不敏感
TAA.SetUpscaleMode("LANCZOS")
if TAA.GetUpscaleMode() ~= "lanczos" then
    fail("Case-insensitive 'LANCZOS' failed, got '" .. tostring(TAA.GetUpscaleMode()) .. "'")
end
pass("UpscaleMode case-insensitive 'LANCZOS' → 'lanczos' ok")

TAA.SetUpscaleMode("LanCzOs")
if TAA.GetUpscaleMode() ~= "lanczos" then fail("Case-insensitive 'LanCzOs' failed") end
pass("UpscaleMode case-insensitive 'LanCzOs' → 'lanczos' ok")

-- 6.14.3) bilinear → bicubic → lanczos → bilinear 三 mode 轮转
TAA.SetUpscaleMode("bilinear")
TAA.SetUpscaleMode("bicubic")
if TAA.GetUpscaleMode() ~= "bicubic" then fail("三 mode 轮转 bicubic failed") end
TAA.SetUpscaleMode("lanczos")
if TAA.GetUpscaleMode() ~= "lanczos" then fail("三 mode 轮转 lanczos failed") end
TAA.SetUpscaleMode("bilinear")
if TAA.GetUpscaleMode() ~= "bilinear" then fail("三 mode 轮转 bilinear failed") end
pass("UpscaleMode bilinear → bicubic → lanczos → bilinear 三 mode 轮转 ok")

-- 6.14.4) F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9+F.0.12+F.0.13+F.0.14 十一启共存
TAA.SetSharpness(0.0)                 -- sharpness=0 才能走 lanczos 路径
TAA.SetAntiFlicker(true)
TAA.SetClipMode("variance")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(true)           -- halfRes=true 才能走 lanczos 路径
TAA.SetSharpenMode("rcas")            -- sharpness=0 时 sharpenMode 不生效
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(true)
TAA.SetUpscaleMode("lanczos")          -- F.0.14
TAA.SetMotionAdaptiveSharpness(true)
TAA.SetMotionSharpness(0.1)
if math.abs(TAA.GetSharpness() - 0.0) > 1e-4
   or TAA.GetAntiFlicker() ~= true
   or TAA.GetClipMode() ~= "variance"
   or math.abs(TAA.GetVarianceGamma() - 1.0) > 1e-4
   or TAA.GetHalfResHistory() ~= true
   or TAA.GetSharpenMode() ~= "rcas"
   or math.abs(TAA.GetMotionGamma() - 1.5) > 1e-4
   or TAA.GetMotionAdaptive() ~= true
   or TAA.GetUpscaleMode() ~= "lanczos"
   or TAA.GetMotionAdaptiveSharpness() ~= true
   or math.abs(TAA.GetMotionSharpness() - 0.1) > 1e-4 then
    fail("F.0.1+0.2+0.3+0.4+0.5+0.6+0.8+0.9+0.12+0.13+0.14 十一启共存 failed")
end
pass("十一启共存: sharp=0 + AF=true + clip=variance + vg=1.0 + halfRes=true + sharpenMode=rcas + mg=1.5 + ma=true + upscale=lanczos + mas=true + ms=0.1 ok")

-- 复位 default
TAA.SetSharpness(0.5)
TAA.SetClipMode("ycocg")
TAA.SetVarianceGamma(1.0)
TAA.SetHalfResHistory(false)
TAA.SetSharpenMode("unsharp")
TAA.SetMotionGamma(1.5)
TAA.SetMotionAdaptive(false)
TAA.SetUpscaleMode("bilinear")
TAA.SetMotionAdaptiveSharpness(false)
TAA.SetMotionSharpness(0.1)

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

-- ============================================================
-- 9) Phase F.0.10 — Multi-Instance API (5 fn = Create / Destroy / SetActive / GetActive / GetCount)
-- ============================================================

-- 9.1) 初始 default 状态
local def_active = TAA.GetActiveInstance()
local def_count  = TAA.GetInstanceCount()
if def_active ~= 0 then fail("Default active instance must be 0, got " .. tostring(def_active)) end
if def_count  ~= 1 then fail("Default instance count must be 1, got " .. tostring(def_count)) end
pass("Phase F.0.10 default: active=0, count=1 (零回归)")

-- 9.2) CreateInstance round-trip
local id1 = TAA.CreateInstance()
if id1 ~= 1 then fail("First CreateInstance() must return 1, got " .. tostring(id1)) end
if TAA.GetInstanceCount() ~= 2 then fail("After Create, count must be 2") end
pass("CreateInstance() returns 1, count=2")

local id2 = TAA.CreateInstance()
if id2 ~= 2 then fail("Second CreateInstance() must return 2, got " .. tostring(id2)) end
local id3 = TAA.CreateInstance()
if id3 ~= 3 then fail("Third CreateInstance() must return 3, got " .. tostring(id3)) end
if TAA.GetInstanceCount() ~= 4 then fail("After 3 Create, count must be 4 (含 default)") end
pass("CreateInstance() three round-trips: id=1/2/3, count=4")

-- 9.3) 槽位满 → 返 0
local idFull = TAA.CreateInstance()
if idFull ~= 0 then fail("CreateInstance() when full must return 0, got " .. tostring(idFull)) end
pass("CreateInstance() when MAX_INSTANCES (4) full returns 0")

-- 9.4) SetActiveInstance round-trip
local ok1 = TAA.SetActiveInstance(id1)
if ok1 ~= true then fail("SetActiveInstance(1) should return true") end
if TAA.GetActiveInstance() ~= id1 then fail("After SetActive(1), GetActive must = 1") end
pass("SetActiveInstance(1) round-trip ok")

TAA.SetActiveInstance(id2)
if TAA.GetActiveInstance() ~= id2 then fail("After SetActive(2), GetActive must = 2") end
TAA.SetActiveInstance(0)  -- 切回 default
if TAA.GetActiveInstance() ~= 0 then fail("After SetActive(0), GetActive must = 0") end
pass("SetActiveInstance() 0/1/2 切换 round-trip ok")

-- 9.5) SetActiveInstance 非法 id → nil + err
local invOk, invErr = TAA.SetActiveInstance(99)
if invOk ~= nil then fail("SetActiveInstance(99) should return nil") end
if type(invErr) ~= "string" or not invErr:find("99") then
    fail("SetActiveInstance(99) should err with id mentioned, got '" .. tostring(invErr) .. "'")
end
pass("SetActiveInstance(99) → nil + err")

-- 9.6) SetActiveInstance 类型错 → nil + err
local typeOk = TAA.SetActiveInstance("foo")
if typeOk ~= nil then fail("SetActiveInstance('foo') should return nil") end
pass("SetActiveInstance(string) type-error rejected")

-- 9.7) 两 instance 参数独立: 设置不同 sharpness 验证互不影响
TAA.SetActiveInstance(id1); TAA.SetSharpness(0.3)
TAA.SetActiveInstance(id2); TAA.SetSharpness(1.5)
TAA.SetActiveInstance(id1)
if math.abs(TAA.GetSharpness() - 0.3) > 1e-4 then
    fail("Instance 1 sharpness must be 0.3, got " .. TAA.GetSharpness())
end
TAA.SetActiveInstance(id2)
if math.abs(TAA.GetSharpness() - 1.5) > 1e-4 then
    fail("Instance 2 sharpness must be 1.5, got " .. TAA.GetSharpness())
end
pass("Instance 1 (sharp=0.3) vs Instance 2 (sharp=1.5) state 独立")

-- 9.8) 两 instance 参数独立: 不同 clipMode + sharpenMode
TAA.SetActiveInstance(id1); TAA.SetClipMode("variance"); TAA.SetSharpenMode("rcas")
TAA.SetActiveInstance(id2); TAA.SetClipMode("rgb");      TAA.SetSharpenMode("cas")
TAA.SetActiveInstance(id1)
if TAA.GetClipMode() ~= "variance" or TAA.GetSharpenMode() ~= "rcas" then
    fail("Instance 1 clipMode/sharpenMode 不正确")
end
TAA.SetActiveInstance(id2)
if TAA.GetClipMode() ~= "rgb" or TAA.GetSharpenMode() ~= "cas" then
    fail("Instance 2 clipMode/sharpenMode 不正确")
end
pass("Instance 1 (variance/rcas) vs Instance 2 (rgb/cas) state 独立")

-- 9.9) default instance (0) 未受 instance 1/2 影响
TAA.SetActiveInstance(0)
if math.abs(TAA.GetSharpness() - 0.5) > 1e-4 then
    fail("Default instance sharpness must be 0.5 (零回归), got " .. TAA.GetSharpness())
end
if TAA.GetClipMode() ~= "ycocg" then
    fail("Default instance clipMode must be 'ycocg' (零回归), got '" .. TAA.GetClipMode() .. "'")
end
pass("Default instance state 未受其他 instance 影响 (零回归保障)")

-- 9.10) DestroyInstance round-trip
local destOk = TAA.DestroyInstance(id1)
if destOk ~= true then fail("DestroyInstance(1) should return true") end
if TAA.GetInstanceCount() ~= 3 then fail("After Destroy, count must be 3") end
pass("DestroyInstance(1) round-trip, count=3")

-- 9.11) 销毁后 SetActiveInstance 该 id 应失败
local destSetOk = TAA.SetActiveInstance(id1)
if destSetOk ~= nil then fail("SetActiveInstance(destroyed id 1) should return nil") end
pass("SetActiveInstance(destroyed) → nil + err")

-- 9.12) DestroyInstance(0) 拒绝 (default 不可销毁)
local destZero = TAA.DestroyInstance(0)
if destZero ~= nil then fail("DestroyInstance(0) must return nil (default 不可销毁)") end
pass("DestroyInstance(0) 拒绝 (default 保护)")

-- 9.13) DestroyInstance 非法 id
local destInv = TAA.DestroyInstance(99)
if destInv ~= nil then fail("DestroyInstance(99) should return nil") end
pass("DestroyInstance(99) → nil + err")

-- 9.14) 销毁 active 自动切回 default
TAA.SetActiveInstance(id2)
if TAA.GetActiveInstance() ~= id2 then fail("Pre: active should be 2") end
TAA.DestroyInstance(id2)
if TAA.GetActiveInstance() ~= 0 then
    fail("After DestroyInstance(active), active must auto-revert to 0, got " .. TAA.GetActiveInstance())
end
pass("DestroyInstance(active) 自动切回 default (id=0)")

-- 9.15) 收尾: 销毁剩余 id3
TAA.DestroyInstance(id3)
if TAA.GetInstanceCount() ~= 1 then fail("Final count must be 1 (仅 default), got " .. TAA.GetInstanceCount()) end
pass("Final cleanup: count=1 (仅 default)")

-- 9.16) 创建-销毁-创建: 槽位可复用
local rid1 = TAA.CreateInstance()
if rid1 ~= 1 then fail("Reusable: CreateInstance after cleanup must return 1, got " .. tostring(rid1)) end
TAA.DestroyInstance(rid1)
if TAA.GetInstanceCount() ~= 1 then fail("After re-destroy, count=1") end
pass("槽位复用: Create → Destroy → Create 返同一 id")

-- ============================================================
-- 10) Phase F.0.10.2 — TAA.Process (manual region API)
-- ============================================================
-- headless 下 HDR fbo=0, Process 返 nil + err; 用于验证 Lua 层参数校验链路
print("")
print("--- Phase F.0.10.2 (Process region) ---")

-- 10.1) 0 个参数: 等价于全屏老接口 (HDR 未启用 → nil + err string)
do
    local ok, err = TAA.Process()
    if ok ~= nil then fail("Process() expected nil (HDR off), got " .. tostring(ok)) end
    if type(err) ~= "string" then fail("Process() expected err string, got " .. type(err)) end
    pass("Process() 0 args headless: nil + err (HDR not enabled)")
end

-- 10.2) 4 个 integer 参数: HDR 未启用 → nil + err string
do
    local ok, err = TAA.Process(0, 0, 100, 200)
    if ok ~= nil then fail("Process(0,0,100,200) expected nil (HDR off), got " .. tostring(ok)) end
    if type(err) ~= "string" then fail("Process(0,0,100,200) expected err string") end
    pass("Process(rgnX,Y,W,H) 4 args headless: nil + err")
end

-- 10.3) 部分参数 (1/2/3 个) 拒绝
do
    local ok, err = TAA.Process(0, 0, 100)  -- 3 args 非法
    if ok ~= nil then fail("Process(3 args) expected nil") end
    if not err:find("expected 0 or 4 args") then fail("Process(3 args) wrong err: " .. tostring(err)) end
    pass("Process(3 args) rejected: 'expected 0 or 4 args'")
end

-- 10.4) w<0 / h<0 拒绝
do
    local ok, err = TAA.Process(0, 0, -100, 100)
    if ok ~= nil then fail("Process(w<0) expected nil") end
    if not err:find("w/h must be >= 0") then fail("Process(w<0) wrong err: " .. tostring(err)) end
    pass("Process(w<0) rejected: 'w/h must be >= 0'")

    local ok2, err2 = TAA.Process(0, 0, 100, -100)
    if ok2 ~= nil then fail("Process(h<0) expected nil") end
    pass("Process(h<0) rejected")
end

-- 10.5) 类型错: 非 integer 抛 luaL_error
do
    local ok, err = pcall(TAA.Process, "x", 0, 100, 100)
    if ok then fail("Process('x', ...) should raise (luaL_checkinteger)") end
    pass("Process(non-integer) raises error")
end

-- 10.6) rgnW=0 + rgnH=0 是合法 (全屏路径)
do
    local ok, err = TAA.Process(0, 0, 0, 0)
    -- 等价于全屏 Process(); HDR 未启用 → nil + err (与 10.1 等价)
    if ok ~= nil then fail("Process(0,0,0,0) expected nil (HDR off)") end
    if type(err) ~= "string" then fail("Process(0,0,0,0) expected err string") end
    pass("Process(0,0,0,0) = full-screen path, headless OK (HDR err)")
end

print("")
print("=== Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.8 + F.0.9 + F.0.10 + F.0.10.2 + F.0.12 + F.0.13 + F.0.14 TAA smoke: ALL TESTS PASSED ===")
print("Functions covered: " .. #fn_names .. " / 41 (F.0.10 +5 fn: multi-instance API; F.0.10.2 +1 fn: TAA.Process)")
print("Highlights:")
print("  - default OFF, alpha=0.92, neighborhoodClip=true, jitterEnabled=true, sharpness=0.5, antiFlicker=true, clipMode='ycocg', varianceGamma=1.0, halfResHistory=false, sharpenMode='unsharp', motionGamma=1.5, motionAdaptive=false")
print("  - clamp: BlendAlpha [0, 1], Sharpness [0, 2], VarianceGamma [0, 4], MotionGamma [0, 4]")
print("  - type-error: SetNeighborhoodClip / SetJitterEnabled / SetAntiFlicker / SetHalfResHistory / SetMotionAdaptive reject non-boolean; SetClipMode / SetSharpenMode reject non-string / invalid value; SetVarianceGamma / SetMotionGamma reject non-number")
print("  - Phase F.0.6: 5-tap CAS (AMD FSR1) vs 4-tap unsharp; CAS contrast-adaptive + HDR safe; sharpenMode='cas' -> peak ∈ [-1/8, -1/5] @ sharpness ∈ [0, 1]")
print("  - Phase F.0.8: motion-adaptive γ (UE5 高级), 静止区域=varianceGamma严防ghost / 高速区域lerp到motionGamma宽容防trail; 仅 ClipMode='variance' 生效")
print("  - Phase F.0.9: Catmull-Rom 9-tap bicubic 上采样 (Sigggraph 2018 Filmic SMAA), -50% blur vs bilinear; 仅 sharpness=0 && halfRes=true 生效")
print("  - Phase F.0.12: FSR2 5-tap RCAS (Robust CAS), noise detection (range<1/64 跳过) + edge protection (lobe sqrt 限制); ~+0.03 ms vs unsharp")
print("  - Phase F.0.13: motion-adaptive sharpness, 高速相机运动时 effSharpness lerp 到 motionSharpness (默认 0.1) 减 trail; 与所有 sharpenMode 共存")
print("  - Phase F.0.14: Lanczos-2 25-tap 5x5 上采样 (超高画质); -10% blur vs Catmull-Rom (-55% vs bilinear); ~+0.04 ms; 仅 sharpness=0 && halfRes=true 生效")
print("  - Phase F.0.10: multi-instance (default + 3 user), 各 instance 独立 history RT + 14 参数; split-screen 双人/四人; 默认 active=0 (零回归)")
print("  - Phase F.0.10.2: TAA.Process(rgnX,Y,W,H) 手动 region 触发, 配合 HDR.SetAutoTAA(false) 做真物理 split-screen")
print("  - status: GetFrameCounter [0, 7], GetCurrentJitter in ±0.5 px range")
print("  - coexistence: TAA toggle does not affect SSR Temporal state")
print("  - Phase F.0.1: 4-tap unsharp mask, sharpness=0 走 blit fallback (零 ALU)")
print("  - Phase F.0.4: Karis luma-weighted blend, antiFlicker=false 走 F.0 纯 alpha blend")
print("  - Phase F.0.2: YCoCg AABB clip, clipMode='rgb' 走 F.0 三通道 RGB clip (零 ALU 增量)")
print("  - Phase F.0.3: variance clip = mean ± γσ (Salvi 2016 / UE5), 优于 AABB 的 single-outlier 鲁棒性")
print("  - Phase F.0.5: half-res history RT (w/2,h/2), VRAM -75% (1080p 33.2MB→8.3MB; 4K 132.7MB→33.2MB), 默认 OFF")
