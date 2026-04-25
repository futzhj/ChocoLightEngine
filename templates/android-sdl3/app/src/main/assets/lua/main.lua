-- ChocoLight Android Demo (最小可运行脚本)
print("ChocoLight Engine v0.3.0 (Android SDL3)")

-- 诊断 Light 模块
print("Light type: " .. type(Light))
if type(Light) == "table" then
  for k, v in pairs(Light) do
    print("  Light." .. tostring(k) .. " = " .. type(v))
  end
  if Light.UI then
    print("Light.UI type: " .. type(Light.UI))
    for k, v in pairs(Light.UI) do
      print("  Light.UI." .. tostring(k) .. " = " .. type(v))
    end
  else
    print("Light.UI is nil!")
  end
end

local HelloWindow = Light(Light.UI.Window):New()

function HelloWindow:OnOpen()
  print("Window opened!")
end

function HelloWindow:Update(dt)
end

function HelloWindow:Draw()
  -- 蓝色矩形
  Light.Graphics.Rectangle(
    Light.Graphics.FillMode,
    100, 100, 0,
    200, 150, 0,
    0, 0, 0,
    1, 1, 1,
    0, 0, 0
  )

  -- 圆形
  Light.Graphics.Circle(
    Light.Graphics.LineMode,
    400, 300, 0,
    50,
    16,
    0, 0, 0,
    1, 1, 1,
    0, 0, 0
  )
end

function HelloWindow:OnKey(key, scanCode, action, mods)
  print("Key:", key, scanCode, action, mods)
end

HelloWindow:Open(800, 600, "ChocoLight")
HelloWindow:SetVSync(true)

while Light.UI.Loop() do
  Light.UI.Resume()
end
