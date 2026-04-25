-- ChocoLight iOS Demo
print("ChocoLight Engine v0.3.0 (iOS SDL3)")

local ok, err = pcall(function()
    local win = Light(Light.UI.Window):New(800, 600, "ChocoLight")
    if not win then
        print("Window creation failed")
        return
    end

    Light.UI.Resume(win, {
        Update = function(dt) end,
        Draw = function()
            Light.Graphics.SetColor(0.2, 0.6, 1.0, 1.0)
            Light.Graphics.Rectangle("fill", 100, 100, 200, 150)
            Light.Graphics.SetColor(1.0, 1.0, 1.0, 1.0)
            Light.Graphics.Print("Hello from iOS!", 120, 160)
        end,
    })
end)

if not ok then
    print("Error: " .. tostring(err))
end
