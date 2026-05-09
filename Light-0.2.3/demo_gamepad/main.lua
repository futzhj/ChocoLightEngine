-- ChocoLight Sample: Light.Gamepad
--
-- 演示游戏手柄发现 / 按键查询 / 振动 / LED.
-- CI 上无设备时优雅退出.

local Pad = require 'Light.Gamepad'

print("==== Light.Gamepad ====")

local ids = Pad.GetGamepads() or {}
print(string.format("found %d gamepad(s)", #ids))

if #ids == 0 then
    print("(no gamepads connected; plug in an Xbox/PS/SDL controller and rerun)")
    print("\ndemo_gamepad ok (no devices)")
    return
end

-- 打开第一个手柄
local pad, perr = Pad.Open(ids[1])
if not pad then
    print("Pad.Open err:", perr)
    return
end

print(string.format("opened: id=%d name=%s type=%s",
                    Pad.GetID(pad), Pad.GetName(pad), Pad.GetType(pad)))
print("connected:", Pad.IsConnected(pad), "connState:", Pad.GetConnectionState(pad))

local power_state, percent = Pad.GetPowerInfo(pad)
print(string.format("power: %s (%d%%)", power_state, percent))

-- 演示按键 / 摇杆查询 (一次性快照, 真实场景应 per-frame)
print("\nbuttons:")
for _, b in ipairs({"a","b","x","y","start","back","leftshoulder","rightshoulder"}) do
    if Pad.HasButton(pad, b) then
        print(string.format("  %-15s : %s", b, tostring(Pad.GetButton(pad, b))))
    end
end

print("\naxes (range -32768..32767, triggers 0..32767):")
for _, a in ipairs({"leftx","lefty","rightx","righty","lefttrigger","righttrigger"}) do
    if Pad.HasAxis(pad, a) then
        print(string.format("  %-15s : %d", a, Pad.GetAxis(pad, a)))
    end
end

-- 振动 0.5 秒 (低频 50%, 高频 50%)
print("\nrumble 500ms ...")
Pad.Rumble(pad, 32768, 32768, 500)
-- 等待振动结束 (实际应在游戏主循环里, 这里阻塞演示)
local Sys = require 'Light.System'
local end_ms = Sys.GetTickMS() + 600
while Sys.GetTickMS() < end_ms do end

-- 关 LED (PS4/PS5 支持)
Pad.SetLED(pad, 0, 0, 0)

Pad.Close(pad)
print("\ndemo_gamepad ok")
