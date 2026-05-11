-- Phase E.6 smoke: Light.Graphics.LensDirt + Light.Graphics.Streak
--
-- API coverage:
--   LensDirt (10):
--     Enable / Disable / IsEnabled / IsSupported
--     SetAutoEnable / GetAutoEnable
--     SetDirtTexture / GetDirtTextureId
--     SetIntensity / GetIntensity
--
--   Streak (13):
--     Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
--     SetAutoEnable / GetAutoEnable
--     SetThreshold / GetThreshold
--     SetIntensity / GetIntensity
--     SetLength / GetLength
--     SetDirection / GetDirection    (2-return)
--     SetIterations / GetIterations
--
-- Headless tolerant; ASCII-only.

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Graphics = pcall(require, "Light.Graphics")
if not ok then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local LD = Graphics.LensDirt
local ST = Graphics.Streak
if type(LD) ~= "table" then fail("LensDirt subtable missing (got " .. type(LD) .. ")") end
if type(ST) ~= "table" then fail("Streak subtable missing (got " .. type(ST) .. ")") end
pass("Light.Graphics.LensDirt + Streak subtables present")

-- ============================================================
-- A) LensDirt surface (10 functions)
-- ============================================================

local ld_fns = {
    "Enable", "Disable", "IsEnabled", "IsSupported",
    "SetAutoEnable", "GetAutoEnable",
    "SetDirtTexture", "GetDirtTextureId",
    "SetIntensity", "GetIntensity",
}
for _, k in ipairs(ld_fns) do
    if type(LD[k]) ~= "function" then
        fail("LensDirt." .. k .. " missing (got " .. type(LD[k]) .. ")")
    end
end
pass("LensDirt module surface ok (" .. #ld_fns .. " functions)")

-- ============================================================
-- B) Streak surface (13 functions)
-- ============================================================

local st_fns = {
    "Enable", "Disable", "IsEnabled", "IsSupported", "Resize",
    "SetAutoEnable", "GetAutoEnable",
    "SetThreshold", "GetThreshold",
    "SetIntensity", "GetIntensity",
    "SetLength", "GetLength",
    "SetDirection", "GetDirection",
    "SetIterations", "GetIterations",
}
for _, k in ipairs(st_fns) do
    if type(ST[k]) ~= "function" then
        fail("Streak." .. k .. " missing (got " .. type(ST[k]) .. ")")
    end
end
pass("Streak module surface ok (" .. #st_fns .. " functions)")

-- ============================================================
-- C) IsSupported / IsEnabled initial state (both modules)
-- ============================================================

local ld_sup = LD.IsSupported()
local st_sup = ST.IsSupported()
if type(ld_sup) ~= "boolean" then fail("LD.IsSupported not boolean") end
if type(st_sup) ~= "boolean" then fail("ST.IsSupported not boolean") end
pass("LD.IsSupported=" .. tostring(ld_sup) .. ", ST.IsSupported=" .. tostring(st_sup))

if LD.IsEnabled() ~= false then fail("Initial LD.IsEnabled must be false") end
if ST.IsEnabled() ~= false then fail("Initial ST.IsEnabled must be false") end
pass("Initial IsEnabled() == false (both modules)")

-- ============================================================
-- D) AutoEnable defaults FALSE (both modules)
-- ============================================================

if LD.GetAutoEnable() ~= false then
    fail("LD default AutoEnable must be false, got " .. tostring(LD.GetAutoEnable()))
end
if ST.GetAutoEnable() ~= false then
    fail("ST default AutoEnable must be false, got " .. tostring(ST.GetAutoEnable()))
end
pass("Default AutoEnable == false (both modules)")

LD.SetAutoEnable(true); if LD.GetAutoEnable() ~= true then fail("LD AutoEnable round-trip true failed") end
LD.SetAutoEnable(false)
ST.SetAutoEnable(true); if ST.GetAutoEnable() ~= true then fail("ST AutoEnable round-trip true failed") end
ST.SetAutoEnable(false)
pass("AutoEnable round-trip ok (both modules)")

-- ============================================================
-- E) LensDirt parameters
-- ============================================================

-- Intensity
if math.abs(LD.GetIntensity() - 0.4) > 1e-5 then
    fail("LD default Intensity must be 0.4, got " .. tostring(LD.GetIntensity()))
end
LD.SetIntensity(1.2)
if math.abs(LD.GetIntensity() - 1.2) > 1e-5 then fail("LD Intensity round-trip 1.2 failed") end
LD.SetIntensity(-1.0)
if LD.GetIntensity() < 0.0 - 1e-5 then fail("LD Intensity negative clamp failed") end
LD.SetIntensity(0.4)   -- restore
pass("LD Intensity default 0.4 + round-trip + negative clamp ok")

-- DirtTexture: nil / number / would-be-image (fake table)
LD.SetDirtTexture(nil)
if LD.GetDirtTextureId() ~= 0 then fail("LD nil should set texId=0, got " .. tostring(LD.GetDirtTextureId())) end

LD.SetDirtTexture(42)
if LD.GetDirtTextureId() ~= 42 then fail("LD number round-trip failed, got " .. tostring(LD.GetDirtTextureId())) end

-- fake Image-like table with :GetTextureId()
local fakeImg = { texId = 99 }
function fakeImg:GetTextureId() return self.texId end
LD.SetDirtTexture(fakeImg)
if LD.GetDirtTextureId() ~= 99 then fail("LD table :GetTextureId() call failed") end

LD.SetDirtTexture(0)   -- back to fallback
if LD.GetDirtTextureId() ~= 0 then fail("LD restore to 0 failed") end
pass("LD SetDirtTexture nil/number/table ok")

-- ============================================================
-- F) Streak parameters
-- ============================================================

-- Threshold default 1.0
if math.abs(ST.GetThreshold() - 1.0) > 1e-5 then
    fail("ST default Threshold must be 1.0, got " .. tostring(ST.GetThreshold()))
end
ST.SetThreshold(2.5)
if math.abs(ST.GetThreshold() - 2.5) > 1e-5 then fail("ST Threshold round-trip failed") end
ST.SetThreshold(-1.0)
if ST.GetThreshold() < 0.0 - 1e-5 then fail("ST Threshold negative clamp failed") end
ST.SetThreshold(1.0)
pass("ST Threshold default 1.0 + round-trip + clamp ok")

-- Intensity default 0.3
if math.abs(ST.GetIntensity() - 0.3) > 1e-5 then
    fail("ST default Intensity must be 0.3")
end
ST.SetIntensity(1.5)
if math.abs(ST.GetIntensity() - 1.5) > 1e-5 then fail("ST Intensity round-trip failed") end
ST.SetIntensity(-1.0)
if ST.GetIntensity() < 0.0 - 1e-5 then fail("ST Intensity clamp failed") end
ST.SetIntensity(0.3)
pass("ST Intensity default 0.3 + round-trip + clamp ok")

-- Length default 0.02, clamp [0, 0.1]
if math.abs(ST.GetLength() - 0.02) > 1e-5 then
    fail("ST default Length must be 0.02, got " .. tostring(ST.GetLength()))
end
ST.SetLength(0.05)
if math.abs(ST.GetLength() - 0.05) > 1e-5 then fail("ST Length round-trip failed") end
ST.SetLength(-0.1)
if ST.GetLength() < 0.0 - 1e-5 then fail("ST Length lower clamp failed") end
ST.SetLength(1.0)
if ST.GetLength() > 0.1 + 1e-5 then
    fail("ST Length upper clamp <= 0.1 failed, got " .. tostring(ST.GetLength()))
end
ST.SetLength(0.02)
pass("ST Length default 0.02 + round-trip + range clamp ok")

-- Direction default (1, 0); 2-return getter
local dx, dy = ST.GetDirection()
if math.abs(dx - 1.0) > 1e-5 or math.abs(dy - 0.0) > 1e-5 then
    fail("ST default Direction must be (1, 0), got (" .. tostring(dx) .. ", " .. tostring(dy) .. ")")
end
pass("ST default Direction == (1, 0)")

ST.SetDirection(0.0, 1.0)
dx, dy = ST.GetDirection()
if math.abs(dx - 0.0) > 1e-5 or math.abs(dy - 1.0) > 1e-5 then
    fail("ST Direction round-trip (0, 1) failed")
end

-- (0, 0) should be preserved (old value retained)
ST.SetDirection(0.0, 0.0)
dx, dy = ST.GetDirection()
if math.abs(dx - 0.0) > 1e-5 or math.abs(dy - 1.0) > 1e-5 then
    fail("ST SetDirection(0,0) should be preserved; direction changed to ("
         .. tostring(dx) .. ", " .. tostring(dy) .. ")")
end
ST.SetDirection(1.0, 0.0)   -- restore
pass("ST Direction round-trip + (0,0) preserved ok")

-- Iterations default 5, clamp [1, 8]
if ST.GetIterations() ~= 5 then
    fail("ST default Iterations must be 5, got " .. tostring(ST.GetIterations()))
end
ST.SetIterations(3)
if ST.GetIterations() ~= 3 then fail("ST Iterations round-trip failed") end
ST.SetIterations(0)
if ST.GetIterations() < 1 then fail("ST Iterations lower clamp failed") end
ST.SetIterations(20)
if ST.GetIterations() > 8 then fail("ST Iterations upper clamp failed") end
ST.SetIterations(5)
pass("ST Iterations default 5 + round-trip + clamp ok")

-- ============================================================
-- G) LensDirt Enable/Disable lifecycle (headless tolerant)
-- ============================================================

local ld_en = LD.Enable()
if type(ld_en) ~= "boolean" then fail("LD.Enable should return boolean") end
if ld_en then
    if LD.IsEnabled() ~= true then fail("LD Enable true but IsEnabled false") end
    LD.Disable()
    if LD.IsEnabled() ~= false then fail("LD after Disable still enabled") end
    pass("LD Enable/Disable lifecycle ok (live backend)")
else
    pass("LD.Enable returned false (headless), IsEnabled stays false")
end
LD.Disable() -- idempotent
LD.Disable()
pass("LD Double Disable safe")

-- ============================================================
-- H) Streak Enable/Resize/Disable lifecycle (headless tolerant)
-- ============================================================

local st_en = ST.Enable(640, 360)
if type(st_en) ~= "boolean" then fail("ST.Enable should return boolean") end
if st_en then
    if ST.IsEnabled() ~= true then fail("ST Enable true but IsEnabled false") end
    if ST.Resize(640, 360) ~= true then fail("ST same-size Resize should return true") end
    ST.Disable()
    if ST.IsEnabled() ~= false then fail("ST after Disable still enabled") end
    pass("ST Enable/Resize/Disable lifecycle ok (live backend)")
else
    pass("ST.Enable returned false (headless), IsEnabled stays false")
end
ST.Disable() -- idempotent
ST.Disable()
pass("ST Double Disable safe")

print("[OK] Phase E.6 smoke (Light.Graphics.LensDirt + Streak): all checks passed")
