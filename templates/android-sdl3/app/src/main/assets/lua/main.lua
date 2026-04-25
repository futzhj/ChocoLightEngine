-- ChocoLight Android Demo
print("ChocoLight Engine v0.3.0 (Android SDL3)")
print("Platform: " .. (jit and jit.os or "Lua 5.1"))

-- 尝试创建窗口并渲染
local ok, err = pcall(function()
    local win = Light(Light.UI.Window):New(800, 600, "ChocoLight")
    if not win then
        print("Window creation failed (headless mode?)")
        return
    end

    Light.UI.Resume(win, {
        Update = function(dt) end,
        Draw = function()
            Light.Graphics.SetColor(0.2, 0.6, 1.0, 1.0)
            Light.Graphics.Rectangle("fill", 100, 100, 200, 150)
            Light.Graphics.SetColor(1.0, 1.0, 1.0, 1.0)
            Light.Graphics.Print("Hello from Android!", 120, 160)
        end,
        OnKey = function(key, action)
            if key == "escape" and action == 1 then
                Light.UI.Close(win)
            end
        end
    })
end)

if not ok then
    print("Error: " .. tostring(err))
end
