-- Phase E.1.4 smoke: Light.Lighting2D (2D forward multi-light state)
--
-- API coverage:
--   SetEnabled / IsEnabled
--   SetAmbient / GetAmbient
--   AddPointLight / AddSpotLight (incl. 16-light cap)
--   UpdateLight (incl. invalid id / partial update)
--   RemoveLight / ClearLights (idempotency)
--   GetLightCount / GetMaxLights
--   Constants: MAX_LIGHTS / TYPE_POINT / TYPE_SPOT
--
-- Pure API test, no GPU dependency. CI verifies via both
-- 'lightc -p' syntax check and Windows 'light.exe' runtime smoke.
-- ASCII-only (matches existing smoke style).

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Lighting2D")
if not ok then fail("require(Light.Lighting2D) failed: " .. tostring(mod)) end

-- ============================================================
-- 1) Module surface: 11 functions + 3 constants
-- ============================================================

local fn_names = {
    "SetEnabled", "IsEnabled",
    "SetAmbient", "GetAmbient",
    "AddPointLight", "AddSpotLight",
    "UpdateLight", "RemoveLight", "ClearLights",
    "GetLightCount", "GetMaxLights",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then
        fail("Light.Lighting2D." .. k .. " missing or not a function")
    end
end
pass("Light.Lighting2D module surface ok (" .. #fn_names .. " functions)")

assert(mod.MAX_LIGHTS == 16, "MAX_LIGHTS must be 16, got " .. tostring(mod.MAX_LIGHTS))
assert(mod.TYPE_POINT == 1, "TYPE_POINT must be 1, got " .. tostring(mod.TYPE_POINT))
assert(mod.TYPE_SPOT  == 2, "TYPE_SPOT must be 2, got " .. tostring(mod.TYPE_SPOT))
pass("Constants ok (MAX_LIGHTS=16, TYPE_POINT=1, TYPE_SPOT=2)")

-- ============================================================
-- 2) Enabled / Ambient
-- ============================================================

mod.SetEnabled(false)
assert(mod.IsEnabled() == false, "SetEnabled(false) did not stick")
mod.SetEnabled(true)
assert(mod.IsEnabled() == true,  "SetEnabled(true) did not stick")
pass("SetEnabled / IsEnabled ok")

mod.SetAmbient(0.1, 0.2, 0.3)
local ar, ag, ab = mod.GetAmbient()
local function approx(a, b) return math.abs(a - b) < 1e-5 end
assert(approx(ar, 0.1) and approx(ag, 0.2) and approx(ab, 0.3),
       "GetAmbient mismatch: " .. tostring(ar) .. "," .. tostring(ag) .. "," .. tostring(ab))
pass("SetAmbient / GetAmbient ok")

-- ============================================================
-- 3) Clear baseline: GetLightCount must be 0 after clear
-- ============================================================

mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights should zero count")
pass("ClearLights baseline ok")

-- ============================================================
-- 4) AddPointLight + GetLightCount basic
-- ============================================================

local id1 = mod.AddPointLight({ x = 100, y = 200, color = {r=1, g=0, b=0}, range = 150, intensity = 1.5 })
assert(type(id1) == "number" and id1 >= 1 and id1 <= 16,
       "AddPointLight returned invalid id: " .. tostring(id1))
assert(mod.GetLightCount() == 1, "GetLightCount should be 1")
pass("AddPointLight first id=" .. tostring(id1))

-- ============================================================
-- 5) AddSpotLight with full fields
-- ============================================================

local id2 = mod.AddSpotLight({
    x = 50, y = 50, dirX = 1, dirY = 0,
    color = {r=0, g=1, b=0}, range = 200,
    innerAngle = 15, outerAngle = 30, intensity = 2.0,
})
assert(type(id2) == "number", "AddSpotLight returned non-number: " .. tostring(id2))
assert(id2 ~= id1, "Add returned duplicate id")
assert(mod.GetLightCount() == 2, "GetLightCount should be 2")
pass("AddSpotLight id=" .. tostring(id2))

-- ============================================================
-- 6) UpdateLight partial: change only intensity, expect ok=true
-- ============================================================

local ok1 = mod.UpdateLight(id1, { intensity = 3.0 })
assert(ok1 == true, "UpdateLight valid id should return true, got " .. tostring(ok1))
pass("UpdateLight partial field ok")

-- Update with full table also returns true
local ok2 = mod.UpdateLight(id2, {
    x = 60, y = 70, dirX = 0, dirY = 1,
    color = {r=0.5, g=0.5, b=0.5}, range = 180,
    innerAngle = 10, outerAngle = 25, intensity = 1.0,
})
assert(ok2 == true, "UpdateLight full table failed")
pass("UpdateLight full table ok")

-- ============================================================
-- 7) UpdateLight invalid id -> false
-- ============================================================

assert(mod.UpdateLight(0,   {x=0}) == false, "UpdateLight(0) should return false")
assert(mod.UpdateLight(99,  {x=0}) == false, "UpdateLight(99) should return false")
assert(mod.UpdateLight(-1,  {x=0}) == false, "UpdateLight(-1) should return false")
pass("UpdateLight invalid id rejected (id=0/99/-1)")

-- ============================================================
-- 8) RemoveLight + idempotency
-- ============================================================

mod.RemoveLight(id1)
assert(mod.GetLightCount() == 1, "After Remove, count should be 1")
-- Idempotent: removing same id again is no-op
mod.RemoveLight(id1)
assert(mod.GetLightCount() == 1, "Double Remove should be no-op")
-- Removing invalid id is no-op
mod.RemoveLight(99)
mod.RemoveLight(0)
assert(mod.GetLightCount() == 1, "Remove invalid id should be no-op")
pass("RemoveLight + idempotency ok")

-- UpdateLight on removed slot -> false
assert(mod.UpdateLight(id1, {x=0}) == false,
       "UpdateLight on removed slot should return false")
pass("UpdateLight on removed slot rejected")

-- ============================================================
-- 9) 16-light cap: fill all slots, 17th returns nil
-- ============================================================

mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights reset failed")

local ids = {}
for i = 1, 16 do
    local id = mod.AddPointLight({ x = i * 10, y = i * 10 })
    assert(type(id) == "number" and id >= 1 and id <= 16,
           "AddPointLight #" .. i .. " failed: " .. tostring(id))
    ids[i] = id
end
assert(mod.GetLightCount() == 16, "After 16 adds, count should be 16")
pass("Filled all 16 slots")

-- 17th must fail with nil
local id17, err17 = mod.AddPointLight({ x = 0, y = 0 })
assert(id17 == nil, "17th AddPointLight should return nil, got " .. tostring(id17))
assert(type(err17) == "string", "17th AddPointLight should return error message")
pass("17th AddPointLight returns nil + err: " .. tostring(err17))

local id17s, err17s = mod.AddSpotLight({ x = 0, y = 0, dirX = 1, dirY = 0 })
assert(id17s == nil, "17th AddSpotLight should return nil")
pass("17th AddSpotLight returns nil")

-- ============================================================
-- 10) Remove + re-Add slot reuse
-- ============================================================

mod.RemoveLight(ids[5])
assert(mod.GetLightCount() == 15, "After Remove, count should be 15")
local newId = mod.AddPointLight({ x = 999, y = 999 })
assert(type(newId) == "number", "Add after Remove should succeed")
assert(mod.GetLightCount() == 16, "After re-Add, count should be 16 again")
pass("Slot reuse after Remove ok (new id=" .. tostring(newId) .. ")")

-- ============================================================
-- 11) ClearLights resets all slots, preserves ambient
-- ============================================================

mod.SetAmbient(0.4, 0.5, 0.6)
mod.ClearLights()
assert(mod.GetLightCount() == 0, "ClearLights should zero count")

local cr, cg, cb = mod.GetAmbient()
assert(approx(cr, 0.4) and approx(cg, 0.5) and approx(cb, 0.6),
       "ClearLights must NOT clear ambient, got " .. tostring(cr))
pass("ClearLights preserves ambient")

-- Can add again after Clear
local idAfter = mod.AddPointLight({ x = 1, y = 1 })
assert(type(idAfter) == "number", "Add after Clear should succeed")
pass("Add after Clear works (id=" .. tostring(idAfter) .. ")")

-- ============================================================
-- 12) Edge: AddPointLight with empty table uses defaults
-- ============================================================

mod.ClearLights()
local idDefault = mod.AddPointLight({})  -- all fields default
assert(type(idDefault) == "number", "AddPointLight({}) should succeed with defaults")
pass("AddPointLight with empty table uses defaults")

-- ============================================================
-- 13) Edge: AddSpotLight with zero direction -> normalized to (1,0)
-- ============================================================

local idZeroDir = mod.AddSpotLight({ x = 0, y = 0, dirX = 0, dirY = 0 })
assert(type(idZeroDir) == "number", "AddSpotLight zero-dir should still succeed (fallback)")
pass("AddSpotLight zero-direction handled (fallback to unit vector)")

-- ============================================================
-- Final cleanup
-- ============================================================

mod.ClearLights()
mod.SetAmbient(0, 0, 0)
mod.SetEnabled(true)
assert(mod.GetLightCount() == 0, "Final clear failed")

print("==== Light.Lighting2D smoke DONE ====")
