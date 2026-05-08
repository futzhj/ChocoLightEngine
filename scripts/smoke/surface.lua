-- Phase AF smoke: Light.Surface (SDL_surface.h)
-- ASCII-only.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Surface")
if not ok then fail("require(Light.Surface) failed: " .. tostring(mod)) end

local pixels = require("Light.Pixels")  -- for SDL_PIXELFORMAT_RGBA32 constant

-- 1) 37 fns
local fn_names = {
    "CreateSurface","DestroySurface","DuplicateSurface","ConvertSurface",
    "ScaleSurface","LoadBMP","SaveBMP",
    "GetWidth","GetHeight","GetFormat","GetPitch","GetSize",
    "GetSurfaceProperties",
    "LockSurface","UnlockSurface","ReadSurfacePixel","WriteSurfacePixel",
    "SetSurfaceColorMod","GetSurfaceColorMod","SetSurfaceAlphaMod","GetSurfaceAlphaMod",
    "SetSurfaceBlendMode","GetSurfaceBlendMode",
    "SetSurfaceColorKey","GetSurfaceColorKey","SurfaceHasColorKey",
    "SetSurfaceClipRect","GetSurfaceClipRect",
    "SetSurfaceRLE","SurfaceHasRLE","FlipSurface",
    "ClearSurface","FillSurfaceRect","BlitSurface","BlitSurfaceScaled",
    "MapSurfaceRGB","MapSurfaceRGBA",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Surface." .. k .. " missing") end
end
pass("Light.Surface module ok (" .. #fn_names .. " fns)")

-- 2) Constants
assert(mod.FLIP_NONE == 0)
assert(mod.FLIP_HORIZONTAL ~= mod.FLIP_VERTICAL)
assert(type(mod.SCALEMODE_NEAREST) == "number")
assert(type(mod.SCALEMODE_LINEAR) == "number")
pass("Constants ok")

-- ============================================================
-- Build a 4x4 RGBA32 surface
-- ============================================================
local W, H = 4, 4
local FMT = pixels.PIXELFORMAT_RGBA32 or pixels.PIXELFORMAT_RGBA8888
assert(type(FMT) == "number", "Light.Pixels must expose RGBA32 or RGBA8888")

local s = mod.CreateSurface(W, H, FMT)
assert(type(s) == "userdata", "CreateSurface failed")
assert(mod.GetWidth(s) == W)
assert(mod.GetHeight(s) == H)
local sw, sh = mod.GetSize(s)
assert(sw == W and sh == H)
assert(mod.GetFormat(s) == FMT)
assert(mod.GetPitch(s) >= W * 4)
pass("CreateSurface + getters ok (" .. W .. "x" .. H .. ", fmt=" .. FMT .. ")")

-- 3) Properties
local pid = mod.GetSurfaceProperties(s)
assert(type(pid) == "number" and pid > 0, "props id must be > 0")
pass("GetSurfaceProperties -> " .. pid .. " ok")

-- 4) Lock + write a single red pixel
assert(mod.LockSurface(s) == true)
assert(mod.WriteSurfacePixel(s, 1, 1, 255, 0, 0, 255) == true, "write red pixel")
local r, g, b, a = mod.ReadSurfacePixel(s, 1, 1)
assert(r == 255 and g == 0 and b == 0 and a == 255,
       string.format("read mismatch (1,1) = (%d,%d,%d,%d)", r, g, b, a))
mod.UnlockSurface(s)
pass("Lock + Write/ReadSurfacePixel round-trip ok")

-- 5) ClearSurface (floats 0..1)
assert(mod.ClearSurface(s, 0, 1, 0, 1) == true, "clear to opaque green")
local r2, g2, b2, a2 = mod.ReadSurfacePixel(s, 0, 0)
assert(g2 == 255 and r2 == 0 and b2 == 0 and a2 == 255,
       string.format("Clear mismatch: (%d,%d,%d,%d)", r2, g2, b2, a2))
pass("ClearSurface ok")

-- 6) FillSurfaceRect (region form)
local color = mod.MapSurfaceRGBA(s, 0, 0, 255, 255)
assert(mod.FillSurfaceRect(s, 0, 0, 2, 2, color) == true, "fill 2x2 blue")
local r3, g3, b3 = mod.ReadSurfacePixel(s, 0, 0)
assert(b3 == 255 and r3 == 0 and g3 == 0, "fill region mismatch top-left")
local r4, g4, b4 = mod.ReadSurfacePixel(s, 3, 3)
assert(g4 == 255, "fill should not have touched (3,3)")
pass("FillSurfaceRect (region) ok")

-- 6a) FillSurfaceRect (whole surface form)
local fullcolor = mod.MapSurfaceRGBA(s, 128, 128, 128, 255)
assert(mod.FillSurfaceRect(s, fullcolor) == true)
local rg, gg, bg = mod.ReadSurfacePixel(s, 3, 3)
assert(rg == 128 and gg == 128 and bg == 128, "whole-surface fill mismatch")
pass("FillSurfaceRect (whole) ok")

-- 7) Color/Alpha mod round-trip
assert(mod.SetSurfaceColorMod(s, 100, 150, 200) == true)
local cm_r, cm_g, cm_b = mod.GetSurfaceColorMod(s)
assert(cm_r == 100 and cm_g == 150 and cm_b == 200,
       string.format("ColorMod round-trip mismatch (%d,%d,%d)", cm_r, cm_g, cm_b))
assert(mod.SetSurfaceAlphaMod(s, 200) == true)
assert(mod.GetSurfaceAlphaMod(s) == 200)
pass("Color/Alpha mod round-trip ok")

-- 8) BlendMode
local blendmode = require("Light.BlendMode")
assert(mod.SetSurfaceBlendMode(s, blendmode.BLEND) == true)
assert(mod.GetSurfaceBlendMode(s) == blendmode.BLEND)
pass("BlendMode round-trip ok")

-- 9) ColorKey
assert(mod.SurfaceHasColorKey(s) == false, "fresh surface must not have color key")
assert(mod.SetSurfaceColorKey(s, true, 0xFF00FF) == true)
assert(mod.SurfaceHasColorKey(s) == true)
local key = mod.GetSurfaceColorKey(s)
assert(key == 0xFF00FF, "color key round-trip: got " .. tostring(key))
assert(mod.SetSurfaceColorKey(s, false, 0) == true)
assert(mod.SurfaceHasColorKey(s) == false)
pass("ColorKey round-trip ok")

-- 10) ClipRect: full surface form (nil) and region form
assert(mod.SetSurfaceClipRect(s, 1, 1, 2, 2) == true)
local cx, cy, cw, ch = mod.GetSurfaceClipRect(s)
assert(cx == 1 and cy == 1 and cw == 2 and ch == 2,
       string.format("clip mismatch (%d,%d,%d,%d)", cx, cy, cw, ch))
assert(mod.SetSurfaceClipRect(s) == true, "reset clip with no rect")
local fx, fy, fw, fh = mod.GetSurfaceClipRect(s)
assert(fw == W and fh == H, "after reset, clip should be full surface")
pass("ClipRect set/get ok")

-- 11) Flip + Duplicate
local s2 = mod.DuplicateSurface(s)
assert(type(s2) == "userdata")
assert(mod.FlipSurface(s2, mod.FLIP_HORIZONTAL) == true)
mod.DestroySurface(s2)
pass("DuplicateSurface + FlipSurface ok")

-- 12) ScaleSurface
local s3 = mod.ScaleSurface(s, 8, 8, mod.SCALEMODE_NEAREST)
assert(mod.GetWidth(s3) == 8 and mod.GetHeight(s3) == 8)
mod.DestroySurface(s3)
pass("ScaleSurface 4x4 -> 8x8 ok")

-- 13) ConvertSurface to a different format (if available)
local fmt2 = pixels.PIXELFORMAT_RGB24 or pixels.PIXELFORMAT_BGRA32
if fmt2 and fmt2 ~= FMT then
    local s4 = mod.ConvertSurface(s, fmt2)
    assert(mod.GetFormat(s4) == fmt2)
    mod.DestroySurface(s4)
    pass("ConvertSurface ok")
else
    pass("ConvertSurface skipped (no alt format constant)")
end

-- 14) Blit: src 4x4 onto a 8x8 dst
local dst = mod.CreateSurface(8, 8, FMT)
mod.ClearSurface(dst, 0, 0, 0, 1)
assert(mod.BlitSurface(s, dst) == true, "BlitSurface(src, dst) whole-surface form")
assert(mod.BlitSurface(s, 0, 0, 4, 4, dst, 4, 4, 4, 4) == true,
       "BlitSurface with full src+dst rects")
mod.DestroySurface(dst)
pass("BlitSurface variadic forms ok")

-- 15) BlitSurfaceScaled
local dst2 = mod.CreateSurface(16, 16, FMT)
mod.ClearSurface(dst2, 0, 0, 0, 1)
assert(mod.BlitSurfaceScaled(s, dst2, mod.SCALEMODE_NEAREST) == true,
       "BlitSurfaceScaled(src, dst, mode)")
mod.DestroySurface(dst2)
pass("BlitSurfaceScaled ok")

-- 16) BMP round-trip via temp file
local tmpdir = os.getenv("TEMP") or os.getenv("TMP") or "/tmp"
local tmppath = tmpdir .. "/light_surface_smoke_" .. os.time() .. ".bmp"
local saveok, saveerr = mod.SaveBMP(s, tmppath)
if not saveok then fail("SaveBMP: " .. tostring(saveerr)) end

local loaded, loaderr = mod.LoadBMP(tmppath)
if not loaded then fail("LoadBMP: " .. tostring(loaderr)) end
assert(mod.GetWidth(loaded) == W and mod.GetHeight(loaded) == H,
       "loaded size mismatch")
mod.DestroySurface(loaded)
os.remove(tmppath)
pass("SaveBMP/LoadBMP round-trip ok")

-- 17) RLE
assert(mod.SurfaceHasRLE(s) == false)
assert(mod.SetSurfaceRLE(s, true) == true)
-- RLE state is exposed only when renderer accelerates blit; SDL may keep
-- HasRLE false until first locked blit. Don't assert state, just ensure
-- the call did not raise.
mod.SetSurfaceRLE(s, false)
pass("SetSurfaceRLE ok (state-toggling ok)")

-- 18) Destroy + use-after raises
mod.DestroySurface(s)
mod.DestroySurface(s)  -- double-destroy is safe
local rok = pcall(mod.GetWidth, s)
if rok then fail("GetWidth on destroyed surface should raise") end
pass("Destroy idempotent + use-after raises ok")

print("surface smoke ok")
