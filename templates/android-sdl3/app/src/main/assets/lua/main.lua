-- ChocoLight Android Demo
print("ChocoLight Engine v0.3.0 (Android SDL3)")
print("Platform: " .. (jit and jit.os or "Lua 5.1"))

-- 列出 Light 模块可用的 API
print("=== Light API ===")
if Light then
    for k, v in pairs(Light) do
        print("  Light." .. tostring(k) .. " = " .. type(v))
    end
else
    print("  Light module not found!")
end

-- 简单事件循环保持 App 运行
print("Entering main loop...")
local running = true
while running do
    local event = Light.PollEvent and Light.PollEvent()
    if event == "quit" then
        running = false
    end
    -- 简单延时避免 CPU 满载
    if Light.Sleep then Light.Sleep(16) end
end
print("Done.")
