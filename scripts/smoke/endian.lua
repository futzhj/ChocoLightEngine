-- Phase P smoke: Light.Endian (SDL_endian.h + SDL_bits.h)
--
-- ASCII-only per Windows CI convention.
-- Pure CPU functions; no SDL_Init dependency.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Endian")
if not ok then fail("require(Light.Endian) failed: " .. tostring(mod)) end

-- 1) 14 fns
for _, k in ipairs({
    "Swap16", "Swap32", "Swap64", "SwapFloat",
    "Swap16LE", "Swap32LE", "Swap64LE", "SwapFloatLE",
    "Swap16BE", "Swap32BE", "Swap64BE", "SwapFloatBE",
    "MostSignificantBitIndex32", "HasExactlyOneBitSet32",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Endian." .. k .. " missing") end
end
pass("Light.Endian module ok (14 functions)")

-- 2) constants
assert(mod.LIL_ENDIAN == 1234, "LIL_ENDIAN must be 1234")
assert(mod.BIG_ENDIAN == 4321, "BIG_ENDIAN must be 4321")
assert(mod.BYTE_ORDER == mod.LIL_ENDIAN or mod.BYTE_ORDER == mod.BIG_ENDIAN,
       "BYTE_ORDER must equal LIL_ENDIAN or BIG_ENDIAN, got " .. tostring(mod.BYTE_ORDER))
assert(mod.FLOAT_WORD_ORDER == mod.LIL_ENDIAN or mod.FLOAT_WORD_ORDER == mod.BIG_ENDIAN,
       "FLOAT_WORD_ORDER must equal LIL_ENDIAN or BIG_ENDIAN")
assert(type(mod.IS_LITTLE_ENDIAN) == "boolean", "IS_LITTLE_ENDIAN must be boolean")
assert(mod.IS_LITTLE_ENDIAN == (mod.BYTE_ORDER == mod.LIL_ENDIAN),
       "IS_LITTLE_ENDIAN must agree with BYTE_ORDER")
pass(string.format("Light.Endian constants ok (BYTE_ORDER=%d IS_LE=%s)",
                   mod.BYTE_ORDER, tostring(mod.IS_LITTLE_ENDIAN)))

-- 3) Unconditional swaps - bit-pattern checks against known values
assert(mod.Swap16(0x1234) == 0x3412, "Swap16(0x1234)")
assert(mod.Swap16(0xFF00) == 0x00FF, "Swap16(0xFF00)")
assert(mod.Swap16(0x0000) == 0x0000, "Swap16(0)")
assert(mod.Swap16(0xFFFF) == 0xFFFF, "Swap16(0xFFFF) palindrome")

assert(mod.Swap32(0x12345678) == 0x78563412, "Swap32(0x12345678)")
assert(mod.Swap32(0xDEADBEEF) == 0xEFBEADDE, "Swap32(0xDEADBEEF)")
assert(mod.Swap32(0x00000000) == 0x00000000, "Swap32(0)")
assert(mod.Swap32(0xFFFFFFFF) == 0xFFFFFFFF, "Swap32(all 1) palindrome")
pass("Swap16 / Swap32 ok")

-- Swap64 - lua_Number is double (53-bit mantissa). Most byte-swap outputs
-- of a non-zero u64 land above 2^53 and would lose precision. Strategy:
--   * 0 round-trips trivially
--   * Swap64(Swap64(x)) == x for any x within 2^53 (pre-swap fits in
--     low bits, post-swap also fits because we swap back)
--   * One explicit small case: Swap64(0x10000) = 0x10000 bytes are
--     [00 00 00 00 00 01 00 00] -> reversed [00 00 01 00 00 00 00 00]
--     = 0x0000010000000000 = 2^40 = 1099511627776 (safely < 2^53).
assert(mod.Swap64(0) == 0, "Swap64(0)")
assert(mod.Swap64(0x10000) == 0x0000010000000000,
       "Swap64(0x10000) must equal 2^40, got " .. tostring(mod.Swap64(0x10000)))

-- round-trip tests: input AND swapped output both must fit in 2^53.
-- That means the input's set bytes must avoid the lowest byte (so the
-- swapped result avoids the highest byte). Values used here all swap to
-- something <= 2^48.
for _, v in ipairs({ 0x100, 0x10000, 0x1000000, 0x100000000 }) do
    local rt = mod.Swap64(mod.Swap64(v))
    assert(rt == v, "Swap64 round-trip " .. tostring(v) .. " -> " .. tostring(rt))
end
pass("Swap64 round-trip + small-case ok")

-- SwapFloat - swap and swap back must round-trip
local f0 = 1.5
local f1 = mod.SwapFloat(mod.SwapFloat(f0))
assert(f0 == f1, "SwapFloat round-trip 1.5: got " .. tostring(f1))
local f2 = mod.SwapFloat(mod.SwapFloat(-3.14159))
assert(math.abs(f2 - (-3.14159)) < 1e-5, "SwapFloat round-trip -pi: got " .. tostring(f2))
local f3 = mod.SwapFloat(mod.SwapFloat(0.0))
assert(f3 == 0.0, "SwapFloat round-trip 0: got " .. tostring(f3))
pass("SwapFloat round-trip ok")

-- 4) LE / BE forms
-- Identity check: on the host's native order, Swap*<order> is a no-op
local function check_native_noop(label, fn, val, expect_noop)
    local got = fn(val)
    if expect_noop then
        assert(got == val, label .. " on native order must be noop, got " .. tostring(got))
    else
        -- foreign order should swap
        if val ~= 0 and val ~= 0xFFFF and val ~= 0xFFFFFFFF then
            assert(got ~= val, label .. " on foreign order must swap, got " .. tostring(got))
        end
    end
end

local is_le = mod.IS_LITTLE_ENDIAN
check_native_noop("Swap16LE(0x1234)", mod.Swap16LE, 0x1234, is_le)
check_native_noop("Swap32LE(0x12345678)", mod.Swap32LE, 0x12345678, is_le)
check_native_noop("Swap16BE(0x1234)", mod.Swap16BE, 0x1234, not is_le)
check_native_noop("Swap32BE(0x12345678)", mod.Swap32BE, 0x12345678, not is_le)
pass("Swap*LE / Swap*BE host-order behaviour ok")

-- LE/BE composition: Swap16LE(Swap16BE(x)) == Swap16(x) regardless of host
local x = 0xABCD
local lebe = mod.Swap16LE(mod.Swap16BE(x))
assert(lebe == mod.Swap16(x), "LE(BE(x)) == Swap(x) invariant for u16")

local y = 0x12345678
assert(mod.Swap32LE(mod.Swap32BE(y)) == mod.Swap32(y), "LE(BE(x)) == Swap(x) invariant for u32")
pass("Swap*LE / Swap*BE invariants ok")

-- SwapFloatLE / SwapFloatBE applied twice IS round-trip:
--   On LE host: BE = swap, BE(BE(g)) = swap(swap(g)) = g; LE = noop, LE(LE(g)) = g.
--   On BE host: roles reverse; same conclusion.
-- Mixing LE and BE does NOT round-trip (one branch swaps once, the other zero or twice).
local g = 2.71828
local h_le = mod.SwapFloatLE(mod.SwapFloatLE(g))
local h_be = mod.SwapFloatBE(mod.SwapFloatBE(g))
assert(math.abs(h_le - g) < 1e-5, "SwapFloatLE(SwapFloatLE(g)) round-trip: " .. tostring(h_le))
assert(math.abs(h_be - g) < 1e-5, "SwapFloatBE(SwapFloatBE(g)) round-trip: " .. tostring(h_be))
pass("SwapFloatLE / SwapFloatBE round-trip ok")

-- 5) Negative input -> mask into unsigned
-- -1 as u32 = 0xFFFFFFFF, swap is itself.
assert(mod.Swap32(-1) == 0xFFFFFFFF, "negative -1 must mask to 0xFFFFFFFF")
assert(mod.Swap16(-1) == 0xFFFF,     "negative -1 must mask to 0xFFFF")
pass("negative input masking ok")

-- 6) MostSignificantBitIndex32
assert(mod.MostSignificantBitIndex32(0)          == -1, "msb(0) = -1")
assert(mod.MostSignificantBitIndex32(1)          == 0,  "msb(1) = 0")
assert(mod.MostSignificantBitIndex32(2)          == 1,  "msb(2) = 1")
assert(mod.MostSignificantBitIndex32(0x80000000) == 31, "msb(0x80000000) = 31")
assert(mod.MostSignificantBitIndex32(0xFFFFFFFF) == 31, "msb(0xFFFFFFFF) = 31")
assert(mod.MostSignificantBitIndex32(0x10)       == 4,  "msb(16) = 4")
pass("MostSignificantBitIndex32 ok")

-- 7) HasExactlyOneBitSet32
assert(mod.HasExactlyOneBitSet32(0)          == false, "0 has 0 bits set")
assert(mod.HasExactlyOneBitSet32(1)          == true,  "1 has exactly 1 bit")
assert(mod.HasExactlyOneBitSet32(2)          == true,  "2 has exactly 1 bit")
assert(mod.HasExactlyOneBitSet32(3)          == false, "3 has 2 bits")
assert(mod.HasExactlyOneBitSet32(0x80000000) == true,  "0x80000000 has exactly 1 bit")
assert(mod.HasExactlyOneBitSet32(0xFFFFFFFF) == false, "all 1s has 32 bits")
pass("HasExactlyOneBitSet32 ok")

print("endian smoke ok")
