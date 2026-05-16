-- ============================================================================
-- ChocoLight Phase E.5 — Auto Exposure (Eye Adaptation) Demo (callback-model)
-- ============================================================================
-- 演示 AE: 动态测量场景平均亮度 + 时间平滑调 HDR exposure.
--
-- 控制:
--   A : 切换 AE ON / OFF
--   D : 切换 split / darkOnly / brightOnly 场景
--   1/2 : SpeedUp -/+ (步长 0.5)
--   3/4 : SpeedDown -/+ (步长 0.5)
--   5/6 : TargetEV -/+ (步长 0.5)
--   R : 重置默认 (SpeedUp=3, SpeedDown=1, TargetEV=0)
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_auto_exposure] Light.Graphics 不可用'); print('demo_auto_exposure ok (no graphics)'); return end
local HDR, AE = Gfx.HDR, Gfx.AutoExposure
if type(HDR) ~= 'table' or type(AE) ~= 'table' then
    print('[demo_auto_exposure] HDR/AutoExposure 子表缺失'); print('demo_auto_exposure ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.5 Auto Exposure demo (callback-model) ====')
print('[demo_ae] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ae] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_ae] AE.IsSupported  = ' .. tostring(AE.IsSupported()))

if not UI or not UI.Window then
    print('[demo_ae] UI.Window 不可用, 仅 API 探测')
    for _, k in ipairs({'IsEnabled','GetTargetEV','GetSpeedUp','GetSpeedDown','GetMinEV','GetMaxEV',
                       'GetCurrentEV','GetCurrentExposure','GetMeasuredLuminance','GetAutoEnable'}) do
        print('  AE.' .. k .. '() = ' .. tostring(AE[k] and AE[k]()))
    end
    print('demo_auto_exposure ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_ae] Light global 不可用'); print('demo_auto_exposure ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local SCENE_NAMES = { [0]='split', [1]='darkOnly', [2]='brightOnly' }

local Demo = Light(Light.UI.Window):New()
local g_hdrEnabled = false
local g_sceneMode  = 0     -- 0=split 1=darkOnly 2=brightOnly

function Demo:OnOpen()
    if HDR.IsSupported() then
        g_hdrEnabled = HDR.Enable(WIN_W, WIN_H)
        print('[demo_ae] HDR.Enable = ' .. tostring(g_hdrEnabled))
    end
    if g_hdrEnabled and AE.IsSupported() then
        local ok = AE.Enable(WIN_W, WIN_H)
        print('[demo_ae] AE.Enable  = ' .. tostring(ok))
    end
end

function Demo:Update(dt) end

local function fillRect(x, y, w, h, r, g, b)
    Gfx.SetColor(r, g, b, 1.0); Gfx.Rectangle(Gfx.FillMode, x, y, 0, w, h, 0)
end

function Demo:Draw()
    if g_sceneMode == 0 then
        -- split: 左暗右亮
        local halfW = WIN_W / 2
        fillRect(0, 0, halfW, WIN_H, 0.02, 0.02, 0.025)
        fillRect(halfW, 0, halfW, WIN_H, 1.0, 1.0, 1.05)
        fillRect(60,  120, 80, 80, 0.5, 0.4, 0.5)
        fillRect(180, 260, 80, 80, 0.4, 0.6, 0.5)
        fillRect(80,  380, 100, 80, 0.6, 0.5, 0.4)
        fillRect(halfW + 60,  120, 80, 80, 5.0, 4.0, 5.0)
        fillRect(halfW + 180, 260, 80, 80, 4.0, 6.0, 5.0)
        fillRect(halfW + 80,  380, 100, 80, 6.0, 5.0, 4.0)
    elseif g_sceneMode == 1 then
        fillRect(0, 0, WIN_W, WIN_H, 0.02, 0.02, 0.025)
        fillRect(140, 100, 120, 120, 0.5, 0.4, 0.5)
        fillRect(440, 220, 120, 120, 0.4, 0.6, 0.5)
        fillRect(720, 100, 120, 120, 0.6, 0.5, 0.4)
        fillRect(280, 360, 120, 120, 0.5, 0.5, 0.6)
    else
        fillRect(0, 0, WIN_W, WIN_H, 1.0, 1.0, 1.05)
        fillRect(140, 100, 120, 120, 5.0, 4.0, 5.0)
        fillRect(440, 220, 120, 120, 4.0, 6.0, 5.0)
        fillRect(720, 100, 120, 120, 6.0, 5.0, 4.0)
        fillRect(280, 360, 120, 120, 5.0, 5.0, 6.0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    if Gfx.Print then
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('AE: %s   HDR: %s   Backend: %s   Scene: %s',
            AE.IsEnabled() and 'ON' or 'OFF',
            g_hdrEnabled and 'ON' or 'OFF',
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'),
            SCENE_NAMES[g_sceneMode]))
        line(string.format('CurrentEV: %.2f   Exposure: %.3f   MeasuredLogLuma: %.2f',
            AE.GetCurrentEV(), AE.GetCurrentExposure(), AE.GetMeasuredLuminance()))
        line(string.format('TargetEV: %.2f   SpeedUp: %.1f EV/s   SpeedDown: %.1f EV/s',
            AE.GetTargetEV(), AE.GetSpeedUp(), AE.GetSpeedDown()))
        line(string.format('MinEV: %.1f   MaxEV: %.1f   AutoEnable: %s   Supported: %s',
            AE.GetMinEV(), AE.GetMaxEV(), tostring(AE.GetAutoEnable()), tostring(AE.IsSupported())))
        line('Keys: A=AE  D=scene  1/2=SpeedUp  3/4=SpeedDown  5/6=TargetEV  R=reset  ESC=quit')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key == string.byte('A') then
        if AE.IsEnabled() then AE.Disable(); print('[demo_ae] AE OFF (manual exposure resumes)')
        else
            if not g_hdrEnabled then g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false end
            local ok = g_hdrEnabled and AE.Enable(WIN_W, WIN_H) or false
            print('[demo_ae] AE ' .. (ok and 'ON' or 'OFF (enable failed)'))
        end
    elseif key == string.byte('D') then
        g_sceneMode = (g_sceneMode + 1) % 3
        print('[demo_ae] scene -> ' .. SCENE_NAMES[g_sceneMode])
    elseif key == string.byte('1') then AE.SetSpeedUp(AE.GetSpeedUp() - 0.5)
    elseif key == string.byte('2') then AE.SetSpeedUp(AE.GetSpeedUp() + 0.5)
    elseif key == string.byte('3') then AE.SetSpeedDown(AE.GetSpeedDown() - 0.5)
    elseif key == string.byte('4') then AE.SetSpeedDown(AE.GetSpeedDown() + 0.5)
    elseif key == string.byte('5') then AE.SetTargetEV(AE.GetTargetEV() - 0.5)
    elseif key == string.byte('6') then AE.SetTargetEV(AE.GetTargetEV() + 0.5)
    elseif key == string.byte('R') then
        AE.SetSpeedUp(3.0); AE.SetSpeedDown(1.0); AE.SetTargetEV(0.0)
        print('[demo_ae] reset defaults')
    end
end

local function cleanup_demo()
    if AE.IsEnabled() then AE.Disable() end
    if g_hdrEnabled then HDR.Disable() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.5 - Auto Exposure Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_auto_exposure ok')
