local Map = require("Light.Plugins.Map")

local function check(cond, msg)
    if not cond then
        error("[resource_decode_map_01m] " .. tostring(msg), 2)
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

local function verify_map(path)
    if not exists(path) then
        print("[resource_decode_map_01m] skip asset: " .. path)
        return false
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
    return true
end

check(type(Map.Open) == "function", "Map.Open missing")

local sep = package.config:sub(1, 1)
local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"
local decoded = 0

if verify_map(assets .. sep .. "mx_map" .. sep .. "1001.map") then
    decoded = decoded + 1
end
if verify_map(assets .. sep .. "mx_map" .. sep .. "1002.map") then
    decoded = decoded + 1
end

if decoded == 0 then
    print("[resource_decode_map_01m] no mx_map assets decoded")
end
print("[resource_decode_map_01m] PASS")
