local Map = require("Light.Plugins.Map")
local FS = require("Light.Filesystem")
local Gfx = Light.Graphics
local Demo = Light(Light.UI.Window):New()

local WIN_W = 1280
local WIN_H = 720
local image = nil
local imageW = 1
local imageH = 1
local elapsed = 0
local autoClose = tonumber(os.getenv("LIGHT_RESOURCE_DECODE_AUTOCLOSE") or "")

local function script_dir()
    local src = debug.getinfo(1, "S").source
    if src:sub(1, 1) == "@" then
        src = src:sub(2)
    end
    local idx = src:match("^.*()[/\\]")
    if idx then
        return src:sub(1, idx - 1)
    end
    return "."
end

local function fail(msg)
    print("[resource_decode_map_m10] " .. tostring(msg))
end

local function make_image(decoded)
    if not decoded or type(decoded.rgba) ~= "string" then
        return nil, "decoded rgba missing"
    end
    if #decoded.rgba ~= decoded.width * decoded.height * 4 then
        return nil, "decoded rgba length mismatch"
    end
    return Light(Gfx.Image):New(decoded.width, decoded.height, decoded.rgba)
end

function Demo:OnOpen()
    local sep = package.config:sub(1, 1)
    local path = script_dir() .. sep .. "assets" .. sep .. "1001.map"
    if not FS.GetPathInfo(path) then
        fail("missing asset, run setup.ps1 first: " .. path)
        return
    end

    local map, openErr = Map.Open(path)
    if not map then
        fail("Map.Open failed: " .. tostring(openErr))
        return
    end

    local preview, previewErr = map:DecodePreview(2048)
    map:Close()
    if not preview then
        fail("DecodePreview failed: " .. tostring(previewErr))
        return
    end

    local img, imgErr = make_image(preview)
    if not img then
        fail("create image failed: " .. tostring(imgErr))
        return
    end

    image = img
    imageW = preview.width
    imageH = preview.height
    print(string.format("[resource_decode_map_m10] loaded %s preview=%dx%d tiles=%d/%d", path, imageW, imageH, preview.decodedTiles or 0, preview.totalTiles or 0))
end

function Demo:OnKey(key, scancode, action, mods)
    if action == 1 and key == 256 then
        self:Close()
    end
end

function Demo:Update(dt)
    if autoClose then
        elapsed = elapsed + dt
        if elapsed >= autoClose then
            self:Close()
        end
    end
end

function Demo:Draw()
    if not image then
        return
    end
    Gfx.SetColor(1, 1, 1, 1)
    local scale = math.min((WIN_W - 40) / imageW, (WIN_H - 40) / imageH, 1.0)
    local x = (WIN_W - imageW * scale) * 0.5
    local y = (WIN_H - imageH * scale) * 0.5
    Gfx.Push()
    Gfx.Translate(x, y, 0)
    Gfx.Scale(scale, scale, 1)
    Gfx.Draw(image, 0, 0)
    Gfx.Pop()
end

Demo:Open(WIN_W, WIN_H, "MAP M1.0 Resource Decode")
while Light.UI.Loop() do
    Light.UI.Resume()
end
