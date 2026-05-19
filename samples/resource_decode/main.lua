local TCP = require("Light.Plugins.TCP")
local Map = require("Light.Plugins.Map")

local Gfx = Light.Graphics
local Demo = Light(Light.UI.Window):New()

local WIN_W = 1280
local WIN_H = 720
local current = 1
local entries = {}
local messages = {}

local function exists(path)
    local f = io.open(path, "rb")
    if f then
        f:close()
        return true
    end
    return false
end

local function add_message(msg)
    messages[#messages + 1] = tostring(msg)
    print("[resource_decode] " .. tostring(msg))
end

local function make_image(decoded)
    if not decoded or type(decoded.rgba) ~= "string" then
        return nil
    end
    if #decoded.rgba ~= decoded.width * decoded.height * 4 then
        return nil
    end
    return Light(Gfx.Image):New(decoded.width, decoded.height, decoded.rgba)
end

local function add_entry(title, decoded)
    local img = make_image(decoded)
    if not img then
        add_message("failed to create image: " .. title)
        return
    end
    entries[#entries + 1] = {
        title = title,
        image = img,
        width = decoded.width,
        height = decoded.height,
        decodedTiles = decoded.decodedTiles,
        totalTiles = decoded.totalTiles,
    }
end

local function load_tcp(path)
    if not exists(path) then
        add_message("skip TCP asset: " .. path)
        return
    end
    local tcp, err = TCP.Open(path)
    if not tcp then
        add_message("TCP.Open failed: " .. tostring(err))
        return
    end
    local atlas, atlasErr = tcp:DecodeAtlas()
    if atlas then
        add_entry("TCP atlas: " .. path, atlas)
    else
        add_message("TCP.DecodeAtlas failed: " .. tostring(atlasErr))
    end
    tcp:Close()
end

local function load_map(path)
    if not exists(path) then
        add_message("skip MAP asset: " .. path)
        return
    end
    local map, err = Map.Open(path)
    if not map then
        add_message("Map.Open failed: " .. tostring(err))
        return
    end
    local preview, previewErr = map:DecodePreview(2048)
    if preview then
        add_entry("MAP preview: " .. path, preview)
    else
        add_message("Map.DecodePreview failed: " .. tostring(previewErr))
    end
    map:Close()
end

function Demo:OnOpen()
    local sep = package.config:sub(1, 1)
    local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"

    add_message("keys: 1=TCP 2=1001.map 3=mx1001 4=mx1002 ESC=quit")
    load_tcp(assets .. sep .. "鸿鸣.tcp")
    load_map(assets .. sep .. "1001.map")
    load_map(assets .. sep .. "mx_map" .. sep .. "1001.map")
    load_map(assets .. sep .. "mx_map" .. sep .. "1002.map")

    if #entries == 0 then
        add_message("no decodable assets loaded")
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then
        return
    end
    if key == 256 then
        self:Close()
        return
    end
    if key >= 49 and key <= 52 then
        local idx = key - 48
        if entries[idx] then
            current = idx
        end
    end
end

function Demo:Update(dt)
end

function Demo:Draw()
    Gfx.SetColor(1, 1, 1, 1)
    local entry = entries[current]
    if entry and entry.image then
        local scale = math.min((WIN_W - 40) / entry.width, (WIN_H - 120) / entry.height, 1.0)
        Gfx.Push()
        Gfx.Translate(20, 80, 0)
        Gfx.Scale(scale, scale, 1)
        Gfx.Draw(entry.image, 0, 0)
        Gfx.Pop()
    end

end

Demo:Open(WIN_W, WIN_H, "Resource Decode Demo")

while Light.UI.Loop() do
    Light.UI.Resume()
end
