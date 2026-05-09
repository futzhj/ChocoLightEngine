local function resolve(path)
  local node = _G
  local prefix = "_G"

  for part in string.gmatch(path, "[^%.]+") do
    if type(node) ~= "table" then
      return nil, prefix .. " is " .. type(node)
    end

    node = node[part]
    prefix = prefix .. "." .. part
  end

  return node, prefix .. " is " .. type(node)
end

local function check_loaded(path)
  local node, detail = resolve(path)
  assert(type(node) == "table", path .. " missing: " .. detail)
end

local function check_required(path)
  print("checking module", path)
  local ok, err = pcall(require, path)
  assert(ok, "require(" .. path .. ") failed: " .. tostring(err))
  check_loaded(path)
end

check_loaded("Light")
check_loaded("Light.Debug")
check_loaded("Light.Data")
check_loaded("Light.Math")
check_loaded("Light.UI")
check_loaded("Light.Graphics")
check_loaded("Light.AV")
check_loaded("Light.DB")
check_loaded("Light.Network")

check_required("Light.Input")
check_required("Light.Graphics.Particles")
check_required("Light.Graphics.Tilemap")
check_required("Light.Physics.World")
check_required("Light.ECS")
check_required("Light.Scene")
check_required("Light.Graphics.SpriteAnimation")
check_required("Light.Graphics.Shader")
check_required("Light.HotReload")
check_required("Light.UI.Widget")
check_required("Light.Crypto")

print("core_runtime smoke ok")
