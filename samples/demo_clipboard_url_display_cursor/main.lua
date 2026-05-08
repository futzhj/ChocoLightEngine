-- ChocoLight Sample: Light.Clipboard + Light.URL + Light.Display + Light.Cursor
--
-- 桌面集成 4 件套. CI 上多数 fn 优雅降级返回空.

local Clip    = require 'Light.Clipboard'
local URL     = require 'Light.URL'
local Display = require 'Light.Display'
local Cursor  = require 'Light.Cursor'

print("==== Light.Clipboard ====")

local prev_text = Clip.GetText() or ""
print("clipboard text (before): " .. prev_text:sub(1, 40))

if Clip.SetText("ChocoLight rocks!") then
    print("after SetText:           " .. (Clip.GetText() or ""))
    Clip.SetText(prev_text)  -- 还原
end

print("HasText():               " .. tostring(Clip.HasText()))

print("\n==== Light.URL ====")
-- URL.Open 会启动外部应用. Demo 不真启动, 仅注释示范:
local DEMO_LAUNCH = false
if DEMO_LAUNCH then
    URL.Open("https://github.com/futzhj/ChocoLightEngine")
    print("opened browser")
else
    print("(URL demo skipped; set DEMO_LAUNCH=true to open browser)")
end

print("\n==== Light.Display ====")

local displays = Display.GetDisplays() or {}
print(string.format("found %d display(s)", #displays))
for i, id in ipairs(displays) do
    local b = Display.GetBounds(id) or {}
    local name = Display.GetName(id) or "?"
    local scale = Display.GetContentScale(id) or 1.0
    print(string.format("  [%d] id=%d name=%s bounds=(%d,%d %dx%d) scale=%.2f",
                        i, id, name, b.x or 0, b.y or 0, b.w or 0, b.h or 0, scale))
end

local primary = Display.GetPrimary()
if primary then print("primary display: id=" .. primary) end

print("\n==== Light.Cursor ====")
-- 系统光标: 创建 + 显示 + 销毁
local hand, herr = Cursor.CreateSystem("hand")
if hand then
    print("created system cursor 'hand'")
    -- Cursor.Set(hand)  -- 真正切换需要窗口
    Cursor.Destroy(hand)
    print("destroyed cursor")
else
    print("Cursor.CreateSystem err:", herr)
end

print("Cursor.IsVisible():     " .. tostring(Cursor.IsVisible()))

print("\ndemo_clipboard_url_display_cursor ok")
