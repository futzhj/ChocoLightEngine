-- Phase E.15+E.16+E.17 smoke: Light.Graphics.MotionBlur (velocity-driven motion blur surface)
--
-- API coverage (15 functions):
--   Lifecycle 5: Enable / Disable / IsEnabled / IsSupported / Resize
--   AutoEnable 2: SetAutoEnable / GetAutoEnable          (default false)
--   Params     4: SetStrength / GetStrength (clamp [0, 4])
--                 SetSampleCount / GetSampleCount (clamp [1, 32])
--   Phase E.16 2: SetMode / GetMode (default 0; clamp [0, 2])
--                 0=combined / 1=camera_only / 2=object_only
--   Phase E.17 2: SetHalfRes / GetHalfRes (default false)
--                 true=half-res (VRAM -75%, perf ~4x)
--
-- Headless guard: same as hdr.lua. Enable() MUST either
--   (a) return false cleanly when no GL ctx (typical) OR
--   (b) return true if host already has GL ctx.
-- All Set/Get round-trip + clamp must work regardless of Enable state.
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local MB = Graphics.MotionBlur
if type(MB) ~= "table" then
    fail("Light.Graphics.MotionBlur missing or not a table (got " .. type(MB) .. ")")
end
pass("Light.Graphics.MotionBlur subtable present")

-- ============================================================
-- 1) Module surface: 11 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetStrength", "GetStrength",
    "SetSampleCount", "GetSampleCount",
    "SetMode", "GetMode",                       -- Phase E.16
    "SetHalfRes", "GetHalfRes",                 -- Phase E.17
}
for _, k in ipairs(fn_names) do
    if type(MB[k]) ~= "function" then
        fail("Light.Graphics.MotionBlur." .. k .. " missing or not a function (got " .. type(MB[k]) .. ")")
    end
end
pass("Light.Graphics.MotionBlur module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) Initial state probes (IsSupported / IsEnabled)
-- ============================================================

local is_sup = MB.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = MB.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("IsEnabled() = false initially")

-- ============================================================
-- 3) Default values round-trip
-- ============================================================

local function approx(a, b) return math.abs(a - b) < 1e-4 end

-- AutoEnable default = false
local ae0 = MB.GetAutoEnable()
if type(ae0) ~= "boolean" then
    fail("GetAutoEnable should return boolean, got " .. type(ae0))
end
if ae0 ~= false then
    fail("Default GetAutoEnable() must be false, got " .. tostring(ae0))
end
pass("GetAutoEnable() default = false")

-- Strength default = 1.0
local s0 = MB.GetStrength()
if type(s0) ~= "number" then
    fail("GetStrength should return number, got " .. type(s0))
end
if not approx(s0, 1.0) then
    fail("Default GetStrength() must be 1.0, got " .. tostring(s0))
end
pass("GetStrength() default = 1.0")

-- SampleCount default = 8
local n0 = MB.GetSampleCount()
if type(n0) ~= "number" then     -- lua_pushinteger 在 Lumen 中可能映射为 number
    fail("GetSampleCount should return number/integer, got " .. type(n0))
end
if math.floor(n0 + 0.5) ~= 8 then
    fail("Default GetSampleCount() must be 8, got " .. tostring(n0))
end
pass("GetSampleCount() default = 8")

-- ============================================================
-- 4) Set/Get round-trip
-- ============================================================

MB.SetAutoEnable(true)
if MB.GetAutoEnable() ~= true then
    fail("SetAutoEnable(true) round-trip failed")
end
MB.SetAutoEnable(false)
if MB.GetAutoEnable() ~= false then
    fail("SetAutoEnable(false) round-trip failed")
end
pass("SetAutoEnable / GetAutoEnable round-trip ok")

MB.SetStrength(2.5)
if not approx(MB.GetStrength(), 2.5) then
    fail("SetStrength(2.5) round-trip failed (got " .. tostring(MB.GetStrength()) .. ")")
end
MB.SetStrength(1.0)        -- 复位
pass("SetStrength / GetStrength round-trip ok")

MB.SetSampleCount(16)
if MB.GetSampleCount() ~= 16 then
    fail("SetSampleCount(16) round-trip failed (got " .. tostring(MB.GetSampleCount()) .. ")")
end
MB.SetSampleCount(8)        -- 复位
pass("SetSampleCount / GetSampleCount round-trip ok")

-- ============================================================
-- 5) Clamp behavior (strength [0, 4], sampleCount [1, 32])
-- ============================================================

MB.SetStrength(-1.0)
if not approx(MB.GetStrength(), 0.0) then
    fail("SetStrength(-1) should clamp to 0, got " .. tostring(MB.GetStrength()))
end
pass("SetStrength clamp lower bound (0)")

MB.SetStrength(99.0)
if not approx(MB.GetStrength(), 4.0) then
    fail("SetStrength(99) should clamp to 4, got " .. tostring(MB.GetStrength()))
end
pass("SetStrength clamp upper bound (4)")

MB.SetStrength(1.0)         -- 复位

MB.SetSampleCount(0)
if MB.GetSampleCount() ~= 1 then
    fail("SetSampleCount(0) should clamp to 1, got " .. tostring(MB.GetSampleCount()))
end
pass("SetSampleCount clamp lower bound (1)")

MB.SetSampleCount(99)
if MB.GetSampleCount() ~= 32 then
    fail("SetSampleCount(99) should clamp to 32, got " .. tostring(MB.GetSampleCount()))
end
pass("SetSampleCount clamp upper bound (32)")

MB.SetSampleCount(8)        -- 复位

-- ============================================================
-- 6) Enable / Disable cycle (headless friendly)
-- ============================================================

-- Enable 在 headless 下可能返 false (无 GL ctx) 或 true (有 ctx). 都接受.
local r = MB.Enable(640, 480)
if type(r) ~= "boolean" then
    fail("Enable should return boolean, got " .. type(r))
end
if r and not MB.IsEnabled() then
    fail("Enable returned true but IsEnabled() = false")
end
pass("Enable(640, 480) returns boolean (= " .. tostring(r) .. ")")

-- Resize 同样允许 headless 失败
local r2 = MB.Resize(800, 600)
if type(r2) ~= "boolean" then
    fail("Resize should return boolean, got " .. type(r2))
end
pass("Resize(800, 600) returns boolean (= " .. tostring(r2) .. ")")

-- Disable 必须 idempotent (无论 Enable 是否成功)
MB.Disable()
if MB.IsEnabled() ~= false then
    fail("After Disable, IsEnabled() must be false, got " .. tostring(MB.IsEnabled()))
end
pass("Disable() -> IsEnabled() = false")

-- 二次 Disable idempotent
MB.Disable()
pass("Double Disable() idempotent")

-- ============================================================
-- 7) Phase E.16 — Mode default / round-trip / clamp
-- ============================================================

-- 默认 mode = 0 (combined, 与 Phase E.15 行为一致)
local mode0 = MB.GetMode()
if type(mode0) ~= "number" then
    fail("GetMode should return number/integer, got " .. type(mode0))
end
if math.floor(mode0 + 0.5) ~= 0 then
    fail("Default GetMode() must be 0 (combined), got " .. tostring(mode0))
end
pass("GetMode() default = 0 (combined)")

-- round-trip 1 (camera_only)
MB.SetMode(1)
if math.floor(MB.GetMode() + 0.5) ~= 1 then
    fail("SetMode(1) round-trip failed (got " .. tostring(MB.GetMode()) .. ")")
end

-- round-trip 2 (object_only)
MB.SetMode(2)
if math.floor(MB.GetMode() + 0.5) ~= 2 then
    fail("SetMode(2) round-trip failed (got " .. tostring(MB.GetMode()) .. ")")
end
pass("SetMode / GetMode round-trip ok (1=camera_only, 2=object_only)")

-- clamp 下界
MB.SetMode(-5)
if math.floor(MB.GetMode() + 0.5) ~= 0 then
    fail("SetMode(-5) should clamp to 0, got " .. tostring(MB.GetMode()))
end
pass("SetMode clamp lower bound (0)")

-- clamp 上界
MB.SetMode(99)
if math.floor(MB.GetMode() + 0.5) ~= 2 then
    fail("SetMode(99) should clamp to 2, got " .. tostring(MB.GetMode()))
end
pass("SetMode clamp upper bound (2)")

MB.SetMode(0)               -- 复位为 combined

-- ============================================================
-- 8) Phase E.17 — HalfRes default / round-trip / no-op
-- ============================================================

-- 默认 false (full-res, 与 Phase E.15/E.16 一致)
local hr0 = MB.GetHalfRes()
if type(hr0) ~= "boolean" then
    fail("GetHalfRes should return boolean, got " .. type(hr0))
end
if hr0 ~= false then
    fail("Default GetHalfRes() must be false, got " .. tostring(hr0))
end
pass("GetHalfRes() default = false")

-- round-trip true → false
MB.SetHalfRes(true)
if MB.GetHalfRes() ~= true then
    fail("SetHalfRes(true) round-trip failed")
end
MB.SetHalfRes(false)
if MB.GetHalfRes() ~= false then
    fail("SetHalfRes(false) round-trip failed")
end
pass("SetHalfRes / GetHalfRes round-trip ok")

-- SetHalfRes 不应损坏 IsEnabled 状态类型 (headless 友好)
MB.SetHalfRes(true)
if type(MB.IsEnabled()) ~= "boolean" then
    fail("SetHalfRes 不应影响 IsEnabled 返回类型")
end
MB.SetHalfRes(false)        -- 复位
pass("SetHalfRes does not corrupt IsEnabled state")

-- ============================================================
-- Final summary
-- ============================================================

print("")
print("=== Light.Graphics.MotionBlur smoke OK (Phase E.15+E.16+E.17) ===")
