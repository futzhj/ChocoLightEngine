local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local okG, Graphics = pcall(require, "Light.Graphics")
if not okG then fail("require(Light.Graphics) failed: " .. tostring(Graphics)) end

local okImage, Image = pcall(require, "Light.Graphics.Image")
if not okImage then fail("require(Light.Graphics.Image) failed: " .. tostring(Image)) end

local okFont, Font = pcall(require, "Light.Graphics.Font")
if not okFont then fail("require(Light.Graphics.Font) failed: " .. tostring(Font)) end

local okMesh, Mesh = pcall(require, "Light.Graphics.Mesh")
if not okMesh then fail("require(Light.Graphics.Mesh) failed: " .. tostring(Mesh)) end

local okSound, Sound = pcall(require, "Light.Audio.Sound")
if not okSound then fail("require(Light.Audio.Sound) failed: " .. tostring(Sound)) end

local HDR = Graphics.HDR
if type(HDR) ~= "table" then fail("Light.Graphics.HDR missing") end

local surfaces = {
    { Image, "Image", "LoadAsync" },
    { HDR,   "HDR",   "LoadCubeLUTAsync" },
    { HDR,   "HDR",   "LoadHaldLUTAsync" },
    { Font,  "Font",  "LoadAsync" },
    { Mesh,  "Mesh",  "LoadGLTFAsync" },
    { Sound, "Sound", "LoadAsync" },
}

for _, s in ipairs(surfaces) do
    local mod, name, fn = s[1], s[2], s[3]
    if type(mod[fn]) ~= "function" then
        fail(name .. "." .. fn .. " missing or not function (got " .. type(mod[fn]) .. ")")
    end
end
pass("async asset API surface ok")

local futures = {
    Image.LoadAsync("__missing_async_image__.png"),
    HDR.LoadCubeLUTAsync("__missing_async_lut__.cube"),
    HDR.LoadHaldLUTAsync("__missing_async_hald__.png"),
    Font.LoadAsync("__missing_async_font__.ttf", 16),
    Mesh.LoadGLTFAsync("__missing_async_mesh__.gltf", 0),
    Sound.LoadAsync("__missing_async_sound__.wav"),
}

for i, f in ipairs(futures) do
    if type(f) ~= "userdata" then fail("future #" .. i .. " must be userdata, got " .. type(f)) end
    if type(f.IsReady) ~= "function" then fail("future #" .. i .. ":IsReady missing") end
    if type(f.IsError) ~= "function" then fail("future #" .. i .. ":IsError missing") end
    if type(f.Get) ~= "function" then fail("future #" .. i .. ":Get missing") end
    if type(f.GetError) ~= "function" then fail("future #" .. i .. ":GetError missing") end
    if f:IsReady() ~= false then fail("future #" .. i .. " should not be ready for missing resource") end
    if f:IsError() ~= true then fail("future #" .. i .. " should be error for missing resource") end
    local value, err = f:Get()
    if value ~= nil then fail("future #" .. i .. ":Get should return nil value") end
    if type(err) ~= "string" or #err == 0 then fail("future #" .. i .. ":Get should return non-empty err") end
    if type(f:GetError()) ~= "string" then fail("future #" .. i .. ":GetError should return string") end
end
pass("async future userdata error behavior ok")

print("asset_loader_async smoke ok")
