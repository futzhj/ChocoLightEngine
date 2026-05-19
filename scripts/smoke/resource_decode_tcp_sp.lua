local TCP = require("Light.Plugins.TCP")
local FS = require("Light.Filesystem")

local function check(cond, msg)
    if not cond then
        error("[resource_decode_tcp_sp] " .. tostring(msg), 2)
    end
end

local function exists(path)
    return FS.GetPathInfo(path) ~= nil
end

local sep = package.config:sub(1, 1)
local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"
local path = assets .. sep .. "鸿鸣.tcp"

check(type(TCP.Open) == "function", "TCP.Open missing")

if not exists(path) then
    print("[resource_decode_tcp_sp] skip asset: " .. path)
    return
end

local tcp, err = TCP.Open(path)
check(tcp ~= nil, "TCP.Open failed: " .. tostring(err))

local info = tcp:GetInfo()
check(type(info) == "table", "GetInfo must return table")
check(info.kind == "SP", "kind must be SP")
check(info.frameCount > 0, "frameCount must be > 0")
check(info.width > 0 and info.height > 0, "logical size invalid")

local frame = tcp:DecodeFrame(0)
check(type(frame) == "table", "DecodeFrame must return table")
check(type(frame.rgba) == "string", "frame rgba must be string")
check(#frame.rgba == frame.width * frame.height * 4, "frame rgba length mismatch")

local atlas = tcp:DecodeAtlas()
check(type(atlas) == "table", "DecodeAtlas must return table")
check(type(atlas.rgba) == "string", "atlas rgba must be string")
check(#atlas.rgba == atlas.width * atlas.height * 4, "atlas rgba length mismatch")

tcp:Close()
print("[resource_decode_tcp_sp] PASS")
