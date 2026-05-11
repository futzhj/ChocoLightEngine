-- Phase E.3 smoke: Light.Graphics.HDR (HDR + ACES tonemapping surface)
--
-- API coverage:
--   Enable / Disable / IsEnabled / IsSupported / Resize
--   SetExposure / GetExposure / SetGamma / GetGamma
--   GetSceneTexture
--
-- Headless guard: no GL context at smoke stage, Enable() MUST either
--   (a) return false cleanly (typical) OR
--   (b) return true if the host already has a GL ctx (some runners).
-- In case (a) subsequent Disable/IsEnabled must still work (idempotency).
--
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local HDR = Graphics.HDR
if type(HDR) ~= "table" then
    fail("Light.Graphics.HDR missing or not a table (got " .. type(HDR) .. ")")
end
pass("Light.Graphics.HDR subtable present")

-- ============================================================
-- 1) Module surface: 10 functions
-- ============================================================

local fn_names = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetExposure", "GetExposure", "SetGamma", "GetGamma",
    "GetSceneTexture",
    -- Phase E.3.4
    "SetTonemapper", "GetTonemapper",
}
for _, k in ipairs(fn_names) do
    if type(HDR[k]) ~= "function" then
        fail("Light.Graphics.HDR." .. k .. " missing or not a function (got " .. type(HDR[k]) .. ")")
    end
end
pass("Light.Graphics.HDR module surface ok (" .. #fn_names .. " functions)")

-- ============================================================
-- 2) IsSupported / IsEnabled — initial state probes
-- ============================================================

local is_sup = HDR.IsSupported()
if type(is_sup) ~= "boolean" then
    fail("IsSupported should return boolean, got " .. type(is_sup))
end
pass("IsSupported() returns boolean (value=" .. tostring(is_sup) .. ")")

local is_en = HDR.IsEnabled()
if type(is_en) ~= "boolean" then
    fail("IsEnabled should return boolean, got " .. type(is_en))
end
if is_en ~= false then
    fail("Initial IsEnabled() must be false, got " .. tostring(is_en))
end
pass("IsEnabled() = false initially")

-- ============================================================
-- 3) Exposure / Gamma getter/setter round-trip
-- ============================================================

local function approx(a, b) return math.abs(a - b) < 1e-4 end

local exp0 = HDR.GetExposure()
if type(exp0) ~= "number" then
    fail("GetExposure should return number, got " .. type(exp0))
end
if not approx(exp0, 1.0) then
    fail("Initial exposure must be 1.0, got " .. tostring(exp0))
end
pass("GetExposure() default = 1.0")

HDR.SetExposure(2.5)
if not approx(HDR.GetExposure(), 2.5) then
    fail("SetExposure(2.5) did not stick: " .. tostring(HDR.GetExposure()))
end
HDR.SetExposure(0.5)
if not approx(HDR.GetExposure(), 0.5) then
    fail("SetExposure(0.5) did not stick: " .. tostring(HDR.GetExposure()))
end
HDR.SetExposure(1.0)  -- restore
pass("SetExposure / GetExposure round-trip ok")

local gam0 = HDR.GetGamma()
if type(gam0) ~= "number" then
    fail("GetGamma should return number, got " .. type(gam0))
end
if not approx(gam0, 2.2) then
    fail("Initial gamma must be 2.2, got " .. tostring(gam0))
end
pass("GetGamma() default = 2.2")

HDR.SetGamma(1.8)
if not approx(HDR.GetGamma(), 1.8) then
    fail("SetGamma(1.8) did not stick: " .. tostring(HDR.GetGamma()))
end
HDR.SetGamma(2.4)
if not approx(HDR.GetGamma(), 2.4) then
    fail("SetGamma(2.4) did not stick: " .. tostring(HDR.GetGamma()))
end
HDR.SetGamma(2.2)  -- restore
pass("SetGamma / GetGamma round-trip ok")

-- ============================================================
-- 4) GetSceneTexture — 0 when not enabled
-- ============================================================

local tex0 = HDR.GetSceneTexture()
if type(tex0) ~= "number" then
    fail("GetSceneTexture should return number/integer, got " .. type(tex0))
end
if tex0 ~= 0 then
    fail("GetSceneTexture() must be 0 when HDR disabled, got " .. tostring(tex0))
end
pass("GetSceneTexture() = 0 when disabled")

-- ============================================================
-- 5) Enable / Disable — headless robustness
-- ============================================================
--
-- Smoke runs with no GL context (light.exe invokes main.lua before Window.Open).
-- Enable(w, h) must not crash; it is allowed to return false.
-- If it does return true (some host already opened a GL ctx), Disable must
-- also work without errors.

local ok_en, ret_en = pcall(HDR.Enable, 256, 256)
if not ok_en then
    fail("HDR.Enable(256, 256) raised error: " .. tostring(ret_en))
end
if type(ret_en) ~= "boolean" then
    fail("HDR.Enable should return boolean, got " .. type(ret_en))
end
pass("HDR.Enable(256, 256) returned " .. tostring(ret_en) .. " cleanly (no throw)")

-- Enabled flag must match Enable() return value
if HDR.IsEnabled() ~= ret_en then
    fail("IsEnabled() = " .. tostring(HDR.IsEnabled()) .. " but Enable returned " .. tostring(ret_en))
end
pass("IsEnabled() tracks Enable() result")

-- Disable must always be callable (no-op if never enabled)
local ok_dis, err_dis = pcall(HDR.Disable)
if not ok_dis then
    fail("HDR.Disable() raised error: " .. tostring(err_dis))
end
if HDR.IsEnabled() ~= false then
    fail("IsEnabled() must be false after Disable(), got " .. tostring(HDR.IsEnabled()))
end
pass("HDR.Disable() always safe; IsEnabled() = false after")

-- Double Disable: idempotent
local ok_dis2 = pcall(HDR.Disable)
if not ok_dis2 then fail("double Disable() raised error") end
pass("Double Disable() idempotent")

-- Resize on disabled: must not crash, must return false (can't resize what doesn't exist)
local ok_rs, ret_rs = pcall(HDR.Resize, 512, 512)
if not ok_rs then
    fail("HDR.Resize(512, 512) on disabled state raised error: " .. tostring(ret_rs))
end
if type(ret_rs) ~= "boolean" then
    fail("HDR.Resize should return boolean, got " .. type(ret_rs))
end
pass("HDR.Resize on disabled returned " .. tostring(ret_rs) .. " cleanly")

-- ============================================================
-- 6) Bad params — argument error or false
-- ============================================================

-- Invalid size: 0 or negative. Current impl returns false cleanly.
local _, r1 = pcall(HDR.Enable, 0, 0)
if r1 == true then fail("Enable(0, 0) must not succeed") end
local _, r2 = pcall(HDR.Enable, -1, 256)
if r2 == true then fail("Enable(-1, 256) must not succeed") end
pass("Enable bad params rejected (w<=0 or h<=0)")

-- GetSceneTexture after failed Enable: still 0
if HDR.GetSceneTexture() ~= 0 then
    fail("GetSceneTexture() must be 0 after failed Enable, got " .. tostring(HDR.GetSceneTexture()))
end
pass("GetSceneTexture() stays 0 after failed Enable")

-- ============================================================
-- 7) Phase E.3.4 — Tonemapper operator (按字符串名)
-- ============================================================

-- 7.1 默认 operator = "aces"
local tm0 = HDR.GetTonemapper()
if type(tm0) ~= "string" then
    fail("GetTonemapper should return string, got " .. type(tm0))
end
if tm0 ~= "aces" then
    fail("Default tonemapper must be 'aces', got '" .. tostring(tm0) .. "'")
end
pass("GetTonemapper() default = 'aces'")

-- 7.2 4 个合法 operator 往返
local valid = { "aces", "reinhard", "uncharted2", "linear" }
for _, name in ipairs(valid) do
    HDR.SetTonemapper(name)
    local got = HDR.GetTonemapper()
    if got ~= name then
        fail("SetTonemapper('" .. name .. "') did not stick: got '" .. tostring(got) .. "'")
    end
end
pass("SetTonemapper / GetTonemapper round-trip ok (4 operators)")

-- 7.3 大小写无关
HDR.SetTonemapper("ACES")
if HDR.GetTonemapper() ~= "aces" then
    fail("Case-insensitive 'ACES' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("Reinhard")
if HDR.GetTonemapper() ~= "reinhard" then
    fail("Case-insensitive 'Reinhard' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("UnCharTeD2")
if HDR.GetTonemapper() ~= "uncharted2" then
    fail("Case-insensitive 'UnCharTeD2' failed: got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
pass("Case-insensitive SetTonemapper ok (ACES / Reinhard / UnCharTeD2)")

-- 7.4 无效名回退 ACES
HDR.SetTonemapper("not_a_real_operator")
if HDR.GetTonemapper() ~= "aces" then
    fail("Unknown name should fallback to 'aces', got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
HDR.SetTonemapper("")
if HDR.GetTonemapper() ~= "aces" then
    fail("Empty name should fallback to 'aces', got '" .. tostring(HDR.GetTonemapper()) .. "'")
end
pass("Unknown / empty tonemapper name falls back to 'aces'")

-- 7.5 SetTonemapper 不抛 (bad-type arg 由 luaL_checkstring 处理, 抛 lua error)
local ok_bad = pcall(HDR.SetTonemapper)   -- 缺参数
if ok_bad then fail("SetTonemapper() with no arg must error") end
local ok_bad2 = pcall(HDR.SetTonemapper, 123)  -- 数字 (lua 自动转字符串 -> 未知 -> aces)
if not ok_bad2 then fail("SetTonemapper(123) lua-auto-stringified should not error") end
pass("SetTonemapper arg validation ok (nil arg errors; number stringified)")

-- 恢复默认
HDR.SetTonemapper("aces")

-- ============================================================
-- Done
-- ============================================================

print("[Phase E.3] Light.Graphics.HDR smoke PASS (12 functions)")
