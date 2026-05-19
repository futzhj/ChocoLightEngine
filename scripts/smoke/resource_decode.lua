local function check(cond, msg)
    if not cond then
        error("[resource_decode] " .. tostring(msg), 2)
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

local TCP = require("Light.Plugins.TCP")
local Map = require("Light.Plugins.Map")

check(type(TCP.Open) == "function", "TCP.Open missing")
check(type(Map.Open) == "function", "Map.Open missing")

local sep = package.config:sub(1, 1)
local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"

local tcpPath = assets .. sep .. "鸿鸣.tcp"
if exists(tcpPath) then
    local tcp, err = TCP.Open(tcpPath)
    check(tcp ~= nil, "TCP.Open failed: " .. tostring(err))

    local info = tcp:GetInfo()
    check(type(info) == "table", "TCP.GetInfo must return table")
    check(info.kind == "SP", "TCP kind must be SP")
    check(info.frameCount > 0, "TCP frameCount must be > 0")

    local atlas = tcp:DecodeAtlas()
    check(type(atlas) == "table", "TCP.DecodeAtlas must return table")
    check(type(atlas.rgba) == "string", "TCP atlas rgba must be string")
    check(#atlas.rgba == atlas.width * atlas.height * 4, "TCP atlas rgba length mismatch")
    tcp:Close()
else
    print("[resource_decode] skip TCP asset: " .. tcpPath)
end

local mapPath = assets .. sep .. "mx_map" .. sep .. "1001.map"
if exists(mapPath) then
    local map, err = Map.Open(mapPath)
    check(map ~= nil, "Map.Open failed: " .. tostring(err))

    local info = map:GetInfo()
    check(type(info) == "table", "Map.GetInfo must return table")
    check(info.width > 0 and info.height > 0, "Map dimensions invalid")
    check(info.tileCount > 0, "Map tileCount must be > 0")

    local preview = map:DecodePreview(2048)
    check(type(preview) == "table", "Map.DecodePreview must return table")
    check(type(preview.rgba) == "string", "Map preview rgba must be string")
    check(preview.decodedTiles == preview.totalTiles, "Map decoded tile count mismatch")
    check(#preview.rgba == preview.width * preview.height * 4, "Map preview rgba length mismatch")
    map:Close()
else
    print("[resource_decode] skip MAP asset: " .. mapPath)
end

print("[resource_decode] PASS")
