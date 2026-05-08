-- ChocoLight Sample: Light.Tray (Phase I / I.2 / I.3)
--
-- 完整演示: 创建托盘 -> 设图标 -> 菜单(按钮+复选+分隔+子菜单)
--           -> 注册 Lua callback (Phase I.3) -> 主循环.
--
-- 在 Web/Android/iOS 等 dummy 后端上 Create 返回 nil, 优雅退出.

local Tray = require 'Light.Tray'
local Sys  = require 'Light.System'

print("==== Light.Tray ====")

local tray, err = Tray.Create("ChocoLight Tray Demo")
if not tray then
    print("Tray.Create returned nil (no platform support): " .. tostring(err))
    print("\ndemo_tray ok (skipped - dummy backend)")
    return
end

-- 设置图标 (传入存在的 PNG 路径; 此处仅演示 API)
local ICON_PATH = "samples/demo_tray/icon.png"
local i_ok, i_err = Tray.SetIconFromFile(tray, ICON_PATH)
if i_ok then
    print("icon set from " .. ICON_PATH)
else
    print("icon load skipped: " .. tostring(i_err))
end

local menu = Tray.GetMenu(tray)
if not menu then
    print("Tray.GetMenu failed; aborting")
    Tray.Destroy(tray)
    return
end

-- 状态共享
local running   = true
local auto_save = true

-- "Open" 按钮 + Lua callback
local btn_open = Tray.AddButton(menu, "Open Window")
Tray.SetClickCallback(btn_open, function(count)
    print(string.format("[callback] Open clicked %d time(s)", count))
end)

-- 复选框 + callback
local cb_auto = Tray.AddCheckbox(menu, "Auto Save", auto_save)
Tray.SetClickCallback(cb_auto, function(_)
    auto_save = not auto_save
    Tray.SetEntryChecked(cb_auto, auto_save)
    print("[callback] auto_save = " .. tostring(auto_save))
end)

Tray.AddSeparator(menu)

-- 子菜单
local _, sub = Tray.AddSubmenu(menu, "Help")
if sub then
    local btn_about = Tray.AddButton(sub, "About")
    Tray.SetClickCallback(btn_about, function()
        print("[callback] ChocoLight Tray Demo - github.com/futzhj/ChocoLightEngine")
    end)
end

Tray.AddSeparator(menu)

-- "Quit" 按钮关闭主循环
local btn_quit = Tray.AddButton(menu, "Quit")
Tray.SetClickCallback(btn_quit, function()
    print("[callback] quit requested")
    running = false
end)

print("Tray ready. Right-click the system tray icon. Demo will exit after 30s without click.")

-- 主循环 (~ 60Hz, max 30s 自动退出)
local end_time = Sys.GetTickMS() + 30 * 1000
while running and Sys.GetTickMS() < end_time do
    Tray.Update()
    local n = Tray.PollCallbacks()
    if n > 0 then
        print(string.format("dispatched %d callback(s) this frame", n))
    end
    -- ChocoLight 主循环里此处通常 sleep ~16ms; 演示用 busy wait
    local s = Sys.GetTickMS() + 16
    while Sys.GetTickMS() < s do end
end

-- 清理: 取消所有 callback ref 后销毁
Tray.SetClickCallback(btn_open, nil)
Tray.SetClickCallback(cb_auto, nil)
Tray.SetClickCallback(btn_quit, nil)
Tray.Destroy(tray)

print("\ndemo_tray ok")
