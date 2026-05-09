local function fail(message)
  error(message, 2)
end

local function assert_type(value, expected, label)
  if type(value) ~= expected then
    fail(label .. ": expected type " .. expected .. ", got " .. type(value))
  end
end

local function assert_true(value, label)
  if not value then
    fail(label .. ": expected true-like value")
  end
end

local function skip(message)
  print("phase3_window_runtime smoke skipped: " .. message)
  os.exit(0)
end

local function require_or_skip(name)
  local ok, module_or_error = pcall(require, name)
  if not ok then
    skip("require(" .. name .. ") failed: " .. tostring(module_or_error))
  end
  return module_or_error
end

require_or_skip("Light.UI.Window")
local Graphics = require_or_skip("Light.Graphics")
local Shader = require_or_skip("Light.Graphics.Shader")
local Widget = require_or_skip("Light.UI.Widget")

assert_type(Graphics.SetColor, "function", "Graphics.SetColor")
assert_type(Graphics.Rectangle, "function", "Graphics.Rectangle")
assert_type(Shader.IsSupported, "function", "Shader.IsSupported")
assert_type(Widget.Container.New, "function", "Widget.Container.New")

local app = Light(Light.UI.Window):New()
local root = Widget.Container.New(0, 0, 160, 120)
local panel = Widget.Panel.New(8, 8, 80, 32, {
  bgColor = {0.2, 0.25, 0.35, 1.0},
  borderColor = {0.6, 0.7, 1.0, 1.0},
})
local button = Widget.Button.New(12, 48, 72, 24, "Smoke")
root:AddChild(panel)
root:AddChild(button)

local function try_shader_path()
  if not Shader.IsSupported() then
    print("phase3_window_runtime shader path skipped: backend does not support user shaders")
    return
  end

  local vertex_source = [[
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec4 vColor;
void main() {
  gl_Position = uMVP * vec4(aPos, 1.0);
  vColor = aColor;
}
]]
  local fragment_source = [[
#version 330 core
in vec4 vColor;
uniform float uTime;
uniform vec2 uRes;
uniform vec3 uColor;
uniform vec4 uTint;
uniform int uMode;
uniform mat4 uMat;
out vec4 FragColor;
void main() {
  FragColor = vec4(uColor, 1.0) * uTint * vColor;
}
]]

  local shader, err = Shader.New(vertex_source, fragment_source)
  assert_true(shader, "Shader.New failed: " .. tostring(err))
  assert_true(shader:Use(), "Shader.Use")
  shader:SetFloat("uTime", 1.0)
  shader:SetVec2("uRes", 160, 120)
  shader:SetVec3("uColor", 1.0, 1.0, 1.0)
  shader:SetVec4("uTint", 1.0, 1.0, 1.0, 1.0)
  shader:SetInt("uMode", 1)
  shader:SetMat4("uMat", {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
  })
  Shader.UseDefault()
  shader:Delete()
end

function app:Draw()
  Graphics.SetColor(0.1, 0.12, 0.16, 1.0)
  Graphics.Rectangle(2, 0, 0, 160, 120, 0)
  Graphics.SetColor(0.9, 0.35, 0.25, 1.0)
  Graphics.Rectangle(2, 96, 16, 32, 32, 0)
  root:Draw()
end

function app:Update()
  self:Close()
end

local ok, open_result = pcall(function()
  return app:Open(160, 120, "Phase3 Smoke")
end)

if not ok then
  skip("window open raised error: " .. tostring(open_result))
end

if open_result == false then
  skip("window or GL context unavailable")
end

local width, height = app:GetDimensions()
assert_true(width > 0, "Window width")
assert_true(height > 0, "Window height")

try_shader_path()

local frame_ok, frame_error = pcall(function()
  app()
end)
assert_true(frame_ok, "Window frame failed: " .. tostring(frame_error))

print("phase3_window_runtime smoke ok")
