local Map = require("Light.Plugins.Map")
local FS = require("Light.Filesystem")
local Gfx = Light.Graphics
local Demo = Light(Light.UI.Window):New()

local WIN_W = 1280
local WIN_H = 720
local current = 1
local entries = {}
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

local function log(msg)
    print("[resource_decode_map_01m] " .. tostring(msg))
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

local function load_map(path)
    if not FS.GetPathInfo(path) then
        log("missing asset: " .. path)
        return
    end

    local map, openErr = Map.Open(path)
    if not map then
        log("Map.Open failed: " .. tostring(openErr))
        return
    end

    local preview, previewErr = map:DecodePreview(2048)
    map:Close()
    if not preview then
        log("DecodePreview failed: " .. tostring(previewErr))
        return
    end

    local img, imgErr = make_image(preview)
    if not img then
        log("create image failed: " .. tostring(imgErr))
        return
    end

    entries[#entries + 1] = {
        title = path,
        image = img,
        width = preview.width,
        height = preview.height,
        decodedTiles = preview.decodedTiles or 0,
        totalTiles = preview.totalTiles or 0,
    }
    log(string.format("loaded %s preview=%dx%d tiles=%d/%d", path, preview.width, preview.height, preview.decodedTiles or 0, preview.totalTiles or 0))
end

function Demo:OnOpen()
    local sep = package.config:sub(1, 1)
    local dir = script_dir() .. sep .. "assets"
    load_map(dir .. sep .. "1001.map")
    load_map(dir .. sep .. "1002.map")
    if #entries == 0 then
        log("no assets loaded, run setup.ps1 first")
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
    if key == 49 and entries[1] then
        current = 1
    elseif key == 50 and entries[2] then
        current = 2
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
    local entry = entries[current]
    if not entry then
        return
    end
    Gfx.SetColor(1, 1, 1, 1)
    local scale = math.min((WIN_W - 40) / entry.width, (WIN_H - 40) / entry.height, 1.0)
    local x = (WIN_W - entry.width * scale) * 0.5
    local y = (WIN_H - entry.height * scale) * 0.5
    Gfx.Push()
    Gfx.Translate(x, y, 0)
    Gfx.Scale(scale, scale, 1)
    Gfx.Draw(entry.image, 0, 0)
    Gfx.Pop()
end

Demo:Open(WIN_W, WIN_H, "MAP 0.1M Resource Decode")
while Light.UI.Loop() do
    Light.UI.Resume()
end
