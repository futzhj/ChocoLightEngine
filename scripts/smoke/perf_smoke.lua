-- perf_smoke.lua
-- Phase A 性能模块烟雾测试 (无窗口环境)
-- 验证: 渲染/批渲染相关模块 API 表已注册到 Lua 全局命名空间
-- 实际性能验证需在带 GL context 的环境中运行 samples/perf_benchmark/

local function fail(message)
  error(message, 2)
end

local function assert_type(value, expected, label)
  if type(value) ~= expected then
    fail(label .. ": expected type " .. expected .. ", got " .. type(value))
  end
end

local function assert_function(value, label)
  if type(value) ~= "function" then
    fail(label .. ": expected function, got " .. type(value))
  end
end

local function require_table(name)
  local ok, module_or_error = pcall(require, name)
  if not ok then
    fail("require(" .. name .. ") failed: " .. tostring(module_or_error))
  end
  assert_type(module_or_error, "table", name)
  return module_or_error
end

-- ==================== 渲染核心模块 ====================

local Graphics = Light.Graphics
assert_type(Graphics, "table", "Light.Graphics")
assert_function(Graphics.Draw, "Light.Graphics.Draw")
assert_function(Graphics.DrawQuad, "Light.Graphics.DrawQuad")
assert_function(Graphics.DrawSprite, "Light.Graphics.DrawSprite")
assert_function(Graphics.Print, "Light.Graphics.Print")
assert_function(Graphics.Line, "Light.Graphics.Line")
assert_function(Graphics.Triangle, "Light.Graphics.Triangle")
assert_function(Graphics.Rectangle, "Light.Graphics.Rectangle")
assert_function(Graphics.Quad, "Light.Graphics.Quad")
assert_function(Graphics.Circle, "Light.Graphics.Circle")
assert_function(Graphics.Push, "Light.Graphics.Push")
assert_function(Graphics.Pop, "Light.Graphics.Pop")
assert_function(Graphics.SetColor, "Light.Graphics.SetColor")

-- ==================== 批渲染受益模块 ====================

local Particles = require_table("Light.Graphics.Particles")
assert_type(Particles, "table", "Light.Graphics.Particles 表已注册")

local Tilemap = require_table("Light.Graphics.Tilemap")
assert_type(Tilemap, "table", "Light.Graphics.Tilemap 表已注册")

-- ==================== Phase A8: GetBackendName 预占位 ====================
-- (Phase B9 真正实现; Phase A 阶段允许该函数缺失)
if type(Graphics.GetBackendName) == "function" then
  print("perf_smoke: Light.Graphics.GetBackendName already present (Phase B9 ahead of schedule)")
end

-- ==================== Phase A 模块路径检查 ====================

local function check_loaded(path)
  local node = _G
  for part in string.gmatch(path, "[^%.]+") do
    if type(node) ~= "table" then
      fail(path .. " not reachable: " .. type(node))
    end
    node = node[part]
  end
  if type(node) ~= "table" then
    fail(path .. " missing or not a table: " .. type(node))
  end
end

check_loaded("Light.Graphics.Particles")
check_loaded("Light.Graphics.Tilemap")
check_loaded("Light.Graphics.SpriteAnimation")
check_loaded("Light.Graphics.Shader")

print("perf_smoke ok")
