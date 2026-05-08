-- Phase S smoke: Light.BlendMode (SDL_blendmode.h)
--
-- ASCII-only per Windows CI convention.
-- Pure bit-pack; no SDL_Init required.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.BlendMode")
if not ok then fail("require(Light.BlendMode) failed: " .. tostring(mod)) end

-- 1) 1 fn
if type(mod.Compose) ~= "function" then fail("Light.BlendMode.Compose missing") end
pass("Light.BlendMode module ok (1 function)")

-- 2) Preset constants - exact SDL_blendmode.h bit values
assert(mod.NONE                 == 0x00000000, "NONE must be 0x00000000, got "    .. tostring(mod.NONE))
assert(mod.BLEND                == 0x00000001, "BLEND must be 0x00000001")
assert(mod.BLEND_PREMULTIPLIED  == 0x00000010, "BLEND_PREMULTIPLIED must be 0x00000010")
assert(mod.ADD                  == 0x00000002, "ADD must be 0x00000002")
assert(mod.ADD_PREMULTIPLIED    == 0x00000020, "ADD_PREMULTIPLIED must be 0x00000020")
assert(mod.MOD                  == 0x00000004, "MOD must be 0x00000004")
assert(mod.MUL                  == 0x00000008, "MUL must be 0x00000008")
assert(mod.INVALID              == 0x7FFFFFFF, "INVALID must be 0x7FFFFFFF")
pass("Light.BlendMode preset constants ok (8)")

-- 3) Operation constants (1..5)
assert(mod.OP_ADD          == 0x1, "OP_ADD")
assert(mod.OP_SUBTRACT     == 0x2, "OP_SUBTRACT")
assert(mod.OP_REV_SUBTRACT == 0x3, "OP_REV_SUBTRACT")
assert(mod.OP_MINIMUM      == 0x4, "OP_MINIMUM")
assert(mod.OP_MAXIMUM      == 0x5, "OP_MAXIMUM")
pass("Light.BlendMode operation constants ok (5)")

-- 4) Factor constants (1..10)
assert(mod.FACTOR_ZERO                == 0x1,  "FACTOR_ZERO")
assert(mod.FACTOR_ONE                 == 0x2,  "FACTOR_ONE")
assert(mod.FACTOR_SRC_COLOR           == 0x3,  "FACTOR_SRC_COLOR")
assert(mod.FACTOR_ONE_MINUS_SRC_COLOR == 0x4,  "FACTOR_ONE_MINUS_SRC_COLOR")
assert(mod.FACTOR_SRC_ALPHA           == 0x5,  "FACTOR_SRC_ALPHA")
assert(mod.FACTOR_ONE_MINUS_SRC_ALPHA == 0x6,  "FACTOR_ONE_MINUS_SRC_ALPHA")
assert(mod.FACTOR_DST_COLOR           == 0x7,  "FACTOR_DST_COLOR")
assert(mod.FACTOR_ONE_MINUS_DST_COLOR == 0x8,  "FACTOR_ONE_MINUS_DST_COLOR")
assert(mod.FACTOR_DST_ALPHA           == 0x9,  "FACTOR_DST_ALPHA")
assert(mod.FACTOR_ONE_MINUS_DST_ALPHA == 0xA,  "FACTOR_ONE_MINUS_DST_ALPHA")
pass("Light.BlendMode factor constants ok (10)")

-- 5) Compose - reproduce SDL_BLENDMODE_BLEND algebra:
--    dstRGB = (srcRGB * srcA) + (dstRGB * (1-srcA))
--    dstA   = srcA            + (dstA   * (1-srcA))
-- which is (SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD, ONE, ONE_MINUS_SRC_ALPHA, ADD).
-- Custom-composed modes are NOT bit-equal to the BLEND preset (SDL stores
-- the custom recipe in the high bits), but the returned value must be:
--   * not equal to INVALID
--   * distinct from any preset
--   * stable across two identical Compose calls
local blend_custom = mod.Compose(
    mod.FACTOR_SRC_ALPHA, mod.FACTOR_ONE_MINUS_SRC_ALPHA, mod.OP_ADD,
    mod.FACTOR_ONE,       mod.FACTOR_ONE_MINUS_SRC_ALPHA, mod.OP_ADD
)
assert(type(blend_custom) == "number", "Compose must return number")
assert(blend_custom ~= mod.INVALID, "Compose(BLEND recipe) must not be INVALID, got " .. tostring(blend_custom))
pass(string.format("Compose(BLEND recipe) ok: 0x%08X", blend_custom))

-- Stability: same recipe -> same value
local blend_custom2 = mod.Compose(
    mod.FACTOR_SRC_ALPHA, mod.FACTOR_ONE_MINUS_SRC_ALPHA, mod.OP_ADD,
    mod.FACTOR_ONE,       mod.FACTOR_ONE_MINUS_SRC_ALPHA, mod.OP_ADD
)
assert(blend_custom == blend_custom2, "Compose must be deterministic")
pass("Compose determinism ok")

-- Distinctness: different recipe -> different value
local add_custom = mod.Compose(
    mod.FACTOR_SRC_ALPHA, mod.FACTOR_ONE, mod.OP_ADD,
    mod.FACTOR_ZERO,      mod.FACTOR_ONE, mod.OP_ADD
)
assert(add_custom ~= blend_custom,
       string.format("Compose(ADD recipe) must differ from Compose(BLEND recipe)"))
pass(string.format("Compose(ADD recipe) ok: 0x%08X", add_custom))

-- 6) Boundary: wrong-arity + non-numeric arg raise lua error
local err_ok = pcall(mod.Compose, 1, 2, 3, 4, 5)  -- 5 args, need 6
if err_ok then fail("Compose(5 args) should raise") end
pass("Compose(5 args) boundary raises ok")

local err_ok2 = pcall(mod.Compose, 1, 2, 3, 4, 5, "not a number")
if err_ok2 then fail("Compose(str last arg) should raise") end
pass("Compose(str arg) boundary raises ok")

print("blendmode smoke ok")
