-- Phase R smoke: Light.Pixels (SDL_pixels.h)
--
-- ASCII-only per Windows CI convention.
-- Pure SDL pixel-format machinery; no SDL_Init required.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Pixels")
if not ok then fail("require(Light.Pixels) failed: " .. tostring(mod)) end

-- 1) 12 fns
for _, k in ipairs({
    "GetPixelFormatName", "GetMasksForPixelFormat", "GetPixelFormatForMasks",
    "GetPixelFormatDetails",
    "MapRGB", "MapRGBA", "GetRGB", "GetRGBA",
    "CreatePalette", "SetPaletteColors", "DestroyPalette", "PaletteSize",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Pixels." .. k .. " missing") end
end
pass("Light.Pixels module ok (12 functions)")

-- 2) Sample of constants - existence + sane numeric values
for _, k in ipairs({
    "PIXELFORMAT_UNKNOWN", "PIXELFORMAT_RGBA8888", "PIXELFORMAT_ARGB8888",
    "PIXELFORMAT_RGB565", "PIXELFORMAT_INDEX8", "PIXELFORMAT_RGB24",
    "PIXELFORMAT_YV12", "PIXELFORMAT_IYUV", "PIXELFORMAT_YUY2",
    "COLORSPACE_UNKNOWN", "COLORSPACE_SRGB", "COLORSPACE_SRGB_LINEAR",
    "COLORSPACE_HDR10", "COLORSPACE_RGB_DEFAULT", "COLORSPACE_YUV_DEFAULT",
}) do
    if type(mod[k]) ~= "number" then fail("constant " .. k .. " missing or non-number") end
end
assert(mod.PIXELFORMAT_UNKNOWN == 0, "UNKNOWN must be 0")
assert(mod.COLORSPACE_UNKNOWN  == 0, "COLORSPACE_UNKNOWN must be 0")
pass("Light.Pixels constants ok (sample of 30+ pixel formats / 10 colorspaces)")

-- 3) GetPixelFormatName round trip
assert(mod.GetPixelFormatName(mod.PIXELFORMAT_RGBA8888) == "SDL_PIXELFORMAT_RGBA8888",
       "name(RGBA8888) wrong: " .. tostring(mod.GetPixelFormatName(mod.PIXELFORMAT_RGBA8888)))
assert(mod.GetPixelFormatName(mod.PIXELFORMAT_UNKNOWN) == "SDL_PIXELFORMAT_UNKNOWN",
       "name(UNKNOWN) wrong")
pass("GetPixelFormatName ok")

-- 4) GetMasksForPixelFormat - RGBA8888 has known masks on a little-endian
--    machine. SDL3 RGBA8888 layout is byte-order agnostic but BPP must be 32.
local masks, merr = mod.GetMasksForPixelFormat(mod.PIXELFORMAT_RGBA8888)
if not masks then fail("GetMasksForPixelFormat(RGBA8888) failed: " .. tostring(merr)) end
assert(masks.bpp == 32, "RGBA8888 bpp must be 32, got " .. tostring(masks.bpp))
-- Each channel mask must be non-zero and disjoint
assert(masks.Rmask ~= 0 and masks.Gmask ~= 0 and masks.Bmask ~= 0 and masks.Amask ~= 0,
       "all 4 RGBA channel masks must be non-zero")
pass(string.format("GetMasksForPixelFormat(RGBA8888) ok: bpp=%d Rmask=0x%X Amask=0x%X",
                   masks.bpp, masks.Rmask, masks.Amask))

-- 5) GetPixelFormatForMasks - reverse round trip from masks -> format
local back = mod.GetPixelFormatForMasks(masks.bpp, masks.Rmask, masks.Gmask, masks.Bmask, masks.Amask)
assert(back == mod.PIXELFORMAT_RGBA8888,
       string.format("masks -> format round-trip: expected RGBA8888 (%d), got %d",
                     mod.PIXELFORMAT_RGBA8888, back))
pass("GetPixelFormatForMasks round-trip ok")

-- 6) GetPixelFormatDetails - must contain bits/shifts/masks
local d, derr = mod.GetPixelFormatDetails(mod.PIXELFORMAT_RGBA8888)
if not d then fail("GetPixelFormatDetails failed: " .. tostring(derr)) end
assert(d.format == mod.PIXELFORMAT_RGBA8888, "details.format mismatch")
assert(d.bits_per_pixel == 32 and d.bytes_per_pixel == 4, "RGBA8888 bpp/Bpp")
assert(d.Rbits == 8 and d.Gbits == 8 and d.Bbits == 8 and d.Abits == 8,
       "RGBA8888 each channel 8 bits")
pass("GetPixelFormatDetails ok")

-- 8) MapRGBA / GetRGBA round-trip on RGBA8888 (lossless 8-bit channels)
local px = mod.MapRGBA(mod.PIXELFORMAT_RGBA8888, 0xAA, 0xBB, 0xCC, 0xDD)
assert(type(px) == "number", "MapRGBA must return number")
local r, g, b, a = mod.GetRGBA(px, mod.PIXELFORMAT_RGBA8888)
assert(r == 0xAA and g == 0xBB and b == 0xCC and a == 0xDD,
       string.format("RGBA8888 round-trip mismatch: got %d/%d/%d/%d", r, g, b, a))
pass(string.format("MapRGBA + GetRGBA round-trip ok (px=0x%08X)", px))

-- 9) MapRGB / GetRGB on RGBA8888 - alpha should default to 255
local px2 = mod.MapRGB(mod.PIXELFORMAT_RGBA8888, 0x10, 0x20, 0x30)
local r2, g2, b2, a2 = mod.GetRGBA(px2, mod.PIXELFORMAT_RGBA8888)
assert(r2 == 0x10 and g2 == 0x20 and b2 == 0x30 and a2 == 0xFF,
       string.format("MapRGB alpha must be 255: got a=%d", a2))
pass("MapRGB ok (alpha defaults to 255)")

-- 10) Channel clamping: -1 -> 0, 999 -> 255
local pxc = mod.MapRGBA(mod.PIXELFORMAT_RGBA8888, -1, 999, 128, 0)
local rc, gc, bc, ac = mod.GetRGBA(pxc, mod.PIXELFORMAT_RGBA8888)
assert(rc == 0 and gc == 255 and bc == 128 and ac == 0,
       string.format("clamp wrong: %d/%d/%d/%d", rc, gc, bc, ac))
pass("8-bit channel clamping ok")

-- 11) Lossy format: RGB565 truncates 8-bit channels to 5/6/5 bits
local px565 = mod.MapRGB(mod.PIXELFORMAT_RGB565, 0xFF, 0xFF, 0xFF)
local r565, g565, b565 = mod.GetRGB(px565, mod.PIXELFORMAT_RGB565)
-- Each channel decoded back will be 0xFF (saturated) or near 0xFF
assert(r565 >= 0xF8 and g565 >= 0xFC and b565 >= 0xF8,
       string.format("RGB565 white round-trip too lossy: %d/%d/%d", r565, g565, b565))
pass(string.format("RGB565 lossy round-trip ok (white -> %d/%d/%d)", r565, g565, b565))

-- 12) Palette CRUD - INDEX8 round-trip via palette
local pal, perr = mod.CreatePalette(256)
if not pal then fail("CreatePalette(256) failed: " .. tostring(perr)) end
assert(mod.PaletteSize(pal) == 256, "palette size must be 256")

-- Set first 3 entries to known colors (named-key form)
local set_ok, set_err = mod.SetPaletteColors(pal, {
    { r = 0xFF, g = 0x00, b = 0x00, a = 0xFF },  -- index 0 = red
    { r = 0x00, g = 0xFF, b = 0x00, a = 0xFF },  -- index 1 = green
    { r = 0x00, g = 0x00, b = 0xFF, a = 0xFF },  -- index 2 = blue
}, 0)
assert(set_ok, "SetPaletteColors named keys failed: " .. tostring(set_err))

-- Also test positional form at offset 10
local set_ok2, set_err2 = mod.SetPaletteColors(pal, {
    { 0x80, 0x80, 0x80 },  -- index 10 = grey, alpha defaults to 255
}, 10)
assert(set_ok2, "SetPaletteColors positional form failed: " .. tostring(set_err2))
pass("SetPaletteColors (named + positional) ok")

-- INDEX8 MapRGBA on an exact palette entry: SDL searches for nearest color.
local px_idx0 = mod.MapRGBA(mod.PIXELFORMAT_INDEX8, 0xFF, 0x00, 0x00, 0xFF, pal)
assert(px_idx0 == 0, "MapRGBA(INDEX8, red) must match palette[0]: got " .. tostring(px_idx0))
local px_idx1 = mod.MapRGBA(mod.PIXELFORMAT_INDEX8, 0x00, 0xFF, 0x00, 0xFF, pal)
assert(px_idx1 == 1, "MapRGBA(INDEX8, green) must match palette[1]: got " .. tostring(px_idx1))
pass("INDEX8 MapRGBA via palette ok")

-- GetRGBA(INDEX8, 0, palette) should give back red
local pr, pg, pb, pa = mod.GetRGBA(0, mod.PIXELFORMAT_INDEX8, pal)
assert(pr == 0xFF and pg == 0 and pb == 0 and pa == 0xFF,
       string.format("INDEX8 GetRGBA palette[0]: %d/%d/%d/%d", pr, pg, pb, pa))
pass("INDEX8 GetRGBA via palette ok")

mod.DestroyPalette(pal)
pass("DestroyPalette ok")

-- DestroyPalette(nil) should be a no-op silently
mod.DestroyPalette(nil)
pass("DestroyPalette(nil) no-op ok")

-- 13) Boundaries
local b_ok = pcall(mod.CreatePalette, 0)
-- CreatePalette(0) returns (nil, err); pcall succeeds.
local h, e = mod.CreatePalette(0)
assert(h == nil and e ~= nil, "CreatePalette(0) must return nil, err")
pass("CreatePalette(0) boundary ok: " .. tostring(e))

local h2, e2 = mod.CreatePalette(-5)
assert(h2 == nil and e2 ~= nil, "CreatePalette(-5) must return nil, err")
pass("CreatePalette(-5) boundary ok: " .. tostring(e2))

local err_pal_ok = pcall(mod.PaletteSize, "not a palette")
if err_pal_ok then fail("PaletteSize(string) should raise") end
pass("PaletteSize(non-handle) raises ok")

print("pixels smoke ok")
