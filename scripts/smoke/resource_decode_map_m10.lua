local Map = require("Light.Plugins.Map")

local function check(cond, msg)
    if not cond then
        error("[resource_decode_map_m10] " .. tostring(msg), 2)
    end
end

local function exists(path)
    local f = io.open(path, "rb")
    if f then
        f:close()
        return true
    end
    return false
end

local sep = package.config:sub(1, 1)
local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"
local path = assets .. sep .. "1001.map"

check(type(Map.Open) == "function", "Map.Open missing")

if not exists(path) then
    print("[resource_decode_map_m10] skip asset: " .. path)
    return
end

local map, err = Map.Open(path)
check(map ~= nil, "Map.Open failed: " .. tostring(err))

local info = map:GetInfo()
check(type(info) == "table", "GetInfo must return table")
check(info.kind == "M1.0", "kind must be M1.0")
check(info.width > 0 and info.height > 0, "dimensions invalid")
check(info.tileWidth > 0 and info.tileHeight > 0, "tile size invalid")
check(info.tileCount > 0, "tileCount must be > 0")

local preview = map:DecodePreview(2048)
check(type(preview) == "table", "DecodePreview must return table")
check(type(preview.rgba) == "string", "preview rgba must be string")
check(preview.width > 0 and preview.height > 0, "preview dimensions invalid")
check(preview.totalTiles == info.tileCount, "preview totalTiles mismatch")
check(preview.decodedTiles > 0, "decodedTiles must be > 0")
check(preview.decodedTiles <= preview.totalTiles, "decodedTiles exceeds totalTiles")
check(#preview.rgba == preview.width * preview.height * 4, "preview rgba length mismatch")

map:Close()
print("[resource_decode_map_m10] PASS")
