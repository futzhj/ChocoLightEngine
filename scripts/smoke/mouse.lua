-- Phase AH smoke: Light.Mouse (SDL_mouse.h, polling subset)
-- ASCII-only.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Mouse")
if not ok then fail("require(Light.Mouse) failed: " .. tostring(mod)) end

-- 1) 7 fns
local fn_names = {
    "HasMouse","GetMice","GetMouseNameForID",
    "GetMouseState","GetGlobalMouseState","GetRelativeMouseState",
    "CaptureMouse",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Mouse." .. k .. " missing") end
end
pass("Light.Mouse module ok (" .. #fn_names .. " fns)")

-- 2) Constants - button indexes and masks
assert(mod.BUTTON_LEFT == 1, "BUTTON_LEFT should be 1")
assert(mod.BUTTON_MIDDLE == 2)
assert(mod.BUTTON_RIGHT == 3)
assert(mod.BUTTON_X1 == 4)
assert(mod.BUTTON_X2 == 5)
-- masks: SDL3 defines BUTTON_LMASK = 1u << (LEFT-1) = 1
assert(mod.BUTTON_MASK_LEFT == 1)
assert(mod.BUTTON_MASK_MIDDLE == 2)
assert(mod.BUTTON_MASK_RIGHT == 4)
assert(mod.BUTTON_MASK_X1 == 8)
assert(mod.BUTTON_MASK_X2 == 16)
pass("Constants ok (5 BUTTON_* indices, 5 BUTTON_MASK_*)")

-- 3) HasMouse / GetMice
local has = mod.HasMouse()
assert(type(has) == "boolean")
local mice = mod.GetMice()
assert(type(mice) == "table")
pass("HasMouse=" .. tostring(has) .. ", GetMice count=" .. #mice)

for i, id in ipairs(mice) do
    local n = mod.GetMouseNameForID(id)
    assert(n == nil or type(n) == "string")
    if i == 1 then
        pass("First mouse name: " .. tostring(n))
    end
end

-- 4) State queries return (buttons, x, y) without crashing
local b1, x1, y1 = mod.GetMouseState()
assert(type(b1) == "number" and type(x1) == "number" and type(y1) == "number",
       "GetMouseState must return 3 numbers")
assert(b1 >= 0)
pass("GetMouseState ok: buttons=" .. b1 .. ", pos=(" .. x1 .. "," .. y1 .. ")")

local b2, x2, y2 = mod.GetGlobalMouseState()
assert(type(b2) == "number" and type(x2) == "number" and type(y2) == "number")
pass("GetGlobalMouseState ok: buttons=" .. b2 .. ", pos=(" .. x2 .. "," .. y2 .. ")")

-- Relative resets internally each call. Two consecutive calls should give
-- ~0 delta (no input on CI), but we don't assert exact zero - just type.
local b3, dx, dy = mod.GetRelativeMouseState()
assert(type(b3) == "number" and type(dx) == "number" and type(dy) == "number")
pass("GetRelativeMouseState ok: buttons=" .. b3 .. ", delta=(" .. dx .. "," .. dy .. ")")

-- 5) CaptureMouse: enable then disable. CI without focus may fail; tolerate.
local cap_ok, cap_err = mod.CaptureMouse(true)
mod.CaptureMouse(false)  -- always release
if cap_ok then
    pass("CaptureMouse enable/disable ok")
else
    pass("CaptureMouse not allowed in this env (expected on headless CI): "
         .. tostring(cap_err))
end

print("mouse smoke ok")
