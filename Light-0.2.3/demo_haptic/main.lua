-- ChocoLight Sample: Light.Haptic (Phase K)
--
-- 演示触觉反馈 (FFB 设备 / 鼠标 haptic / 力反馈方向盘).
-- 与 Light.Gamepad.Rumble 不同: Haptic 针对独立 haptic 子系统.
-- CI/无设备环境优雅降级.

local Haptic = require 'Light.Haptic'
local Sys    = require 'Light.System'

print("==== Light.Haptic ====")

if not Haptic.Init() then
    print("Haptic.Init failed (typical on Web/mobile/headless)")
    print("\ndemo_haptic ok (no haptic subsystem)")
    return
end
print("Haptic.Init ok")

print("IsMouseHaptic = " .. tostring(Haptic.IsMouseHaptic()))

local list = Haptic.GetHaptics() or {}
print(string.format("\nfound %d haptic device(s):", #list))
for i, h in ipairs(list) do
    print(string.format("  [%d] id=%d name=%s", i, h.id, h.name))
end

if #list == 0 and not Haptic.IsMouseHaptic() then
    print("\n(no haptic devices; plug in a FFB wheel/joystick or mouse with haptic)")
    Haptic.Quit()
    print("\ndemo_haptic ok (no devices)")
    return
end

-- 优先打开第一个 haptic 设备, 没有就用 mouse
local dev
if #list > 0 then
    dev = Haptic.Open(list[1].id)
    print("\nopened haptic device: " .. (Haptic.GetName(dev) or "?"))
elseif Haptic.IsMouseHaptic() then
    dev = Haptic.OpenFromMouse()
    print("\nopened mouse haptic")
end

if not dev then
    print("Haptic.Open failed")
    Haptic.Quit()
    return
end

-- 设备能力
print(string.format("features bitmask:      0x%08X", Haptic.GetFeatures(dev) or 0))
print(string.format("max effects:           %d",   Haptic.GetMaxEffects(dev) or 0))
print(string.format("max effects playing:   %d",   Haptic.GetMaxEffectsPlaying(dev) or 0))
print(string.format("num axes:              %d",   Haptic.GetNumAxes(dev) or 0))
print(string.format("rumble supported:      %s",   tostring(Haptic.RumbleSupported(dev))))

-- 简易 rumble: 50% 强度, 800ms
if Haptic.RumbleSupported(dev) then
    if Haptic.InitRumble(dev) then
        Haptic.SetGain(dev, 100)
        print("\nplaying rumble 50% for 800ms ...")
        Haptic.PlayRumble(dev, 0.5, 800)

        -- 等待结束 (实际场景应在游戏主循环里)
        local end_ms = Sys.GetTickMS() + 900
        while Sys.GetTickMS() < end_ms do end

        Haptic.StopRumble(dev)
        print("rumble stopped")
    else
        print("InitRumble failed")
    end
end

Haptic.Close(dev)
Haptic.Quit()
print("\ndemo_haptic ok")
