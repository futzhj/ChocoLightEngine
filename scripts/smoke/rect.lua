-- Phase AB smoke: Light.Rect (SDL_rect.h)
--
-- ASCII-only. Pure math, no SDL_Init dependency.
--
-- Convention: rectangles are (x, y, w, h); points are (x, y).

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, mod = pcall(require, "Light.Rect")
if not ok then fail("require(Light.Rect) failed: " .. tostring(mod)) end

-- 1) 17 fns
local fn_names = {
    "PointInRect", "RectEmpty", "RectsEqual",
    "HasRectIntersection", "GetRectIntersection", "GetRectUnion",
    "GetRectEnclosingPoints", "GetRectAndLineIntersection",
    "PointInRectFloat", "RectEmptyFloat", "RectsEqualFloat", "RectsEqualEpsilon",
    "HasRectIntersectionFloat", "GetRectIntersectionFloat", "GetRectUnionFloat",
    "GetRectEnclosingPointsFloat", "GetRectAndLineIntersectionFloat",
}
for _, k in ipairs(fn_names) do
    if type(mod[k]) ~= "function" then fail("Light.Rect." .. k .. " missing") end
end
pass("Light.Rect module ok (" .. #fn_names .. " functions)")

-- ============================================================
-- INTEGER suite
-- ============================================================

-- 2) PointInRect: corner inclusive on top-left, exclusive on bottom-right
assert(mod.PointInRect(5, 5,  0, 0, 10, 10) == true,  "(5,5) in [0,0,10,10] must be true")
assert(mod.PointInRect(0, 0,  0, 0, 10, 10) == true,  "(0,0) is the inclusive corner")
assert(mod.PointInRect(10, 10, 0, 0, 10, 10) == false, "(10,10) must be on the exclusive edge -> false")
assert(mod.PointInRect(-1, 5,  0, 0, 10, 10) == false, "(-1,5) is outside")
pass("PointInRect ok (4 cases)")

-- 3) RectEmpty: w <= 0 or h <= 0 means empty
assert(mod.RectEmpty(0, 0, 0, 0) == true,  "0x0 is empty")
assert(mod.RectEmpty(0, 0, -1, 5) == true,  "negative w is empty")
assert(mod.RectEmpty(0, 0, 1, 1) == false, "1x1 is not empty")
pass("RectEmpty ok (3 cases)")

-- 4) RectsEqual
assert(mod.RectsEqual(0, 0, 10, 10, 0, 0, 10, 10) == true)
assert(mod.RectsEqual(0, 0, 10, 10, 0, 0, 10, 11) == false)
pass("RectsEqual ok")

-- 5) HasRectIntersection
assert(mod.HasRectIntersection(0, 0, 10, 10,  5, 5, 10, 10) == true,
       "overlapping rects must intersect")
assert(mod.HasRectIntersection(0, 0, 10, 10, 100,100,10,10) == false,
       "disjoint rects must not intersect")
pass("HasRectIntersection ok")

-- 6) GetRectIntersection: overlapping returns intersection
local rx, ry, rw, rh = mod.GetRectIntersection(0, 0, 10, 10, 5, 5, 10, 10)
assert(rx == 5 and ry == 5 and rw == 5 and rh == 5,
       string.format("intersection mismatch: got (%s,%s,%s,%s)", rx, ry, rw, rh))
local nilret = mod.GetRectIntersection(0, 0, 10, 10, 100, 100, 10, 10)
assert(nilret == nil, "disjoint must return nil")
pass("GetRectIntersection ok")

-- 7) GetRectUnion
local ux, uy, uw, uh = mod.GetRectUnion(0, 0, 10, 10, 20, 20, 5, 5)
assert(ux == 0 and uy == 0 and uw == 25 and uh == 25,
       string.format("union mismatch: got (%s,%s,%s,%s)", ux, uy, uw, uh))
pass("GetRectUnion ok")

-- 8) GetRectEnclosingPoints (no clip)
local ex, ey, ew, eh = mod.GetRectEnclosingPoints({1, 1, 5, 7, 3, 3})
-- Encloses min..max+1 in SDL semantics: bounds = (1,1) to (5,7)
assert(ex == 1 and ey == 1 and ew == 5 and eh == 7,
       string.format("enclosing mismatch: got (%s,%s,%s,%s)", ex, ey, ew, eh))
pass("GetRectEnclosingPoints (no clip) ok")

-- 8a) GetRectEnclosingPoints with clip discarding all points
local nilenc = mod.GetRectEnclosingPoints({100, 100}, 0, 0, 10, 10)
assert(nilenc == nil, "all points outside clip must yield nil")
pass("GetRectEnclosingPoints (clip filters all) ok")

-- 8b) Boundary: odd-length table raises
local rok = pcall(mod.GetRectEnclosingPoints, {1, 2, 3})
if rok then fail("odd-length point list should raise") end
pass("GetRectEnclosingPoints(odd len) raises ok")

-- 9) GetRectAndLineIntersection: clipping a line by a rect
-- rect = (0,0,10,10); line from (-5,5) to (15,5) should clip to (0,5)-(9,5).
local nx1, ny1, nx2, ny2 = mod.GetRectAndLineIntersection(0, 0, 10, 10, -5, 5, 15, 5)
assert(nx1 == 0 and ny1 == 5 and nx2 == 9 and ny2 == 5,
       string.format("line-rect mismatch: got (%s,%s)-(%s,%s)", nx1, ny1, nx2, ny2))
local nilret2 = mod.GetRectAndLineIntersection(0, 0, 10, 10, 100, 100, 200, 200)
assert(nilret2 == nil, "line outside rect must return nil")
pass("GetRectAndLineIntersection ok")

-- ============================================================
-- FLOAT suite
-- ============================================================

-- 10) PointInRectFloat
assert(mod.PointInRectFloat(0.5, 0.5,  0.0, 0.0, 1.0, 1.0) == true)
-- SDL_PointInRectFloat treats the rect as a half-open region in floats too;
-- exactly on the right/bottom edge is "inside" because it uses <=.
assert(mod.PointInRectFloat(2.0, 2.0,  0.0, 0.0, 1.0, 1.0) == false)
pass("PointInRectFloat ok")

-- 11) RectEmptyFloat
assert(mod.RectEmptyFloat(0, 0, 0, 0) == true)
assert(mod.RectEmptyFloat(0, 0, 1, 1) == false)
pass("RectEmptyFloat ok")

-- 12) RectsEqualFloat: bitwise equal
assert(mod.RectsEqualFloat(1.0, 2.0, 3.0, 4.0, 1.0, 2.0, 3.0, 4.0) == true)
assert(mod.RectsEqualFloat(1.0, 2.0, 3.0, 4.0, 1.0, 2.0, 3.0, 4.5) == false)
pass("RectsEqualFloat ok")

-- 13) RectsEqualEpsilon: tolerant compare
assert(mod.RectsEqualEpsilon(1.0, 2.0, 3.0, 4.0,
                             1.0001, 2.0001, 3.0001, 4.0001, 0.001) == true,
       "rects within epsilon must be equal")
assert(mod.RectsEqualEpsilon(1.0, 2.0, 3.0, 4.0,
                             1.5, 2.0, 3.0, 4.0, 0.001) == false,
       "rects beyond epsilon must differ")
pass("RectsEqualEpsilon ok")

-- 14) HasRectIntersectionFloat
assert(mod.HasRectIntersectionFloat(0,0,10,10,  5,5,10,10) == true)
assert(mod.HasRectIntersectionFloat(0,0,1,1,    100,100,1,1) == false)
pass("HasRectIntersectionFloat ok")

-- 15) GetRectIntersectionFloat
local frx, fry, frw, frh = mod.GetRectIntersectionFloat(0,0,10,10, 5,5,10,10)
assert(math.abs(frx - 5) < 1e-5 and math.abs(fry - 5) < 1e-5 and
       math.abs(frw - 5) < 1e-5 and math.abs(frh - 5) < 1e-5,
       string.format("float intersection mismatch: (%s,%s,%s,%s)", frx, fry, frw, frh))
pass("GetRectIntersectionFloat ok")

-- 16) GetRectUnionFloat
local fux, fuy, fuw, fuh = mod.GetRectUnionFloat(0,0,10,10, 20,20,5,5)
assert(math.abs(fux - 0) < 1e-5 and math.abs(fuy - 0) < 1e-5 and
       math.abs(fuw - 25) < 1e-5 and math.abs(fuh - 25) < 1e-5,
       string.format("float union mismatch: (%s,%s,%s,%s)", fux, fuy, fuw, fuh))
pass("GetRectUnionFloat ok")

-- 17) GetRectEnclosingPointsFloat (no clip)
local fex, fey, few, feh = mod.GetRectEnclosingPointsFloat({0.5, 0.5,  3.5, 5.5,  2.0, 2.0})
-- min (0.5, 0.5) max (3.5, 5.5) -> w/h = max - min for floats (SDL_FRect)
assert(math.abs(fex - 0.5) < 1e-5 and math.abs(fey - 0.5) < 1e-5,
       "float enclosing origin mismatch")
assert(few > 0 and feh > 0, "float enclosing must yield positive size")
pass("GetRectEnclosingPointsFloat ok")

-- 18) GetRectAndLineIntersectionFloat
local fnx1, fny1, fnx2, fny2 =
    mod.GetRectAndLineIntersectionFloat(0, 0, 10, 10, -5.0, 5.0, 15.0, 5.0)
assert(fnx1 ~= nil and fny1 == 5.0,
       "float line clip y must be 5; got " .. tostring(fny1))
pass("GetRectAndLineIntersectionFloat ok")

-- ============================================================
-- Boundary
-- ============================================================

-- 19) Missing args raise
local rb = pcall(mod.PointInRect, 5, 5, 0, 0)  -- missing w, h
if rb then fail("PointInRect with missing args should raise") end
pass("PointInRect(missing args) raises ok")

print("rect smoke ok")
