-- ChocoLight iOS Demo (最小可运行脚本)
print("ChocoLight Engine v0.3.0 (iOS SDL3)")

local HelloWindow = Light(Light.UI.Window):New()

function HelloWindow:OnOpen()
  print("Window opened!")
end

function HelloWindow:Update(dt)
end

function HelloWindow:Draw()
  Light.Graphics.Rectangle(
    Light.Graphics.FillMode,
    100, 100, 0,
    200, 150, 0,
    0, 0, 0,
    1, 1, 1,
    0, 0, 0
  )

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
