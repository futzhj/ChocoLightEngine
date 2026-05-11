-- ============================================================================
-- ChocoLight Phase E.5 — Auto Exposure (Eye Adaptation) demo
-- ============================================================================
-- 演示 AE (动态测量场景平均亮度 + 时间平滑调 HDR exposure).
--
-- 画面布局:
--   左半屏: 暗场景 (背景亮度 0.02, 几个 0.5 亮度色块)
--   右半屏: 亮场景 (背景亮度 1.0, 几个 5.0 亮度色块)
--   D 键切换两个场景占据全屏, 模拟玩家从暗场景进入亮场景.
--
-- 预期效果:
--   AE OFF -> 亮场景过曝刺眼 / 暗场景看不清细节, 静态 manual exposure.
--   AE ON  -> 切换场景时屏幕缓慢自适应:
--             从暗到亮: ~0.5-1s (speedUp=3 EV/sec) 收缩曝光, 高亮区域恢复细节.
--             从亮到暗: ~3-5s (speedDown=1 EV/sec) 放大曝光, 暗区逐渐显现.
--
-- 控制:
--   A       : 切换 AE ON / OFF
--   D       : 切换暗场景 / 亮场景 (用于观察 AE 适应)
--   1 / 2   : SpeedUp -/+ (步长 0.5; clamp [0.1, 20])
--   3 / 4   : SpeedDown -/+ (步长 0.5; clamp [0.1, 20])
--   5 / 6   : TargetEV -/+ (步长 0.5; 用户偏移)
--   R       : 重置默认 (SpeedUp=3, SpeedDown=1, TargetEV=0)
--   ESC     : 退出
--
-- 依赖: Light.UI.Window + Light.Graphics.HDR + Light.Graphics.AutoExposure + Light.Time
-- 后端: 仅 GL33 支持 (AE.IsSupported = false 时 fallback API surface 演示).
-- ============================================================================

-- ==================== 1. 模块加载 ====================

local UI, Gfx, Time
do
    local function safe_require(n)
        local ok, m = pcall(require, n)
        if ok and type(m) == 'table' then return m end
        return nil
    end
    UI   = safe_require('Light.UI')
    Gfx  = safe_require('Light.Graphics')
    Time = safe_require('Light.Time')
end

if not Gfx then
    print('[demo_auto_exposure] Light.Graphics not available')
    print('demo_auto_exposure ok (no graphics)')
    return
end

local HDR = Gfx.HDR
local AE  = Gfx.AutoExposure
if type(HDR) ~= 'table' or type(AE) ~= 'table' then
    print('[demo_auto_exposure] need Light.Graphics.HDR + Light.Graphics.AutoExposure subtables')
    print('demo_auto_exposure ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.5 Auto Exposure demo ====')
print('[demo_ae] Backend         = ' ..
    tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ae] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_ae] AE.IsSupported  = ' .. tostring(AE.IsSupported()))

-- ==================== 2. Headless 探测 ====================

if not UI or not UI.Window then
    print('[demo_ae] UI.Window not available, API probe only')
    print('  AE.IsEnabled            = ' .. tostring(AE.IsEnabled()))
    print('  AE.GetTargetEV          = ' .. tostring(AE.GetTargetEV()))
    print('  AE.GetSpeedUp           = ' .. tostring(AE.GetSpeedUp()))
    print('  AE.GetSpeedDown         = ' .. tostring(AE.GetSpeedDown()))
    print('  AE.GetMinEV             = ' .. tostring(AE.GetMinEV()))
    print('  AE.GetMaxEV             = ' .. tostring(AE.GetMaxEV()))
    print('  AE.GetCurrentEV         = ' .. tostring(AE.GetCurrentEV()))
    print('  AE.GetCurrentExposure   = ' .. tostring(AE.GetCurrentExposure()))
    print('  AE.GetMeasuredLuminance = ' .. tostring(AE.GetMeasuredLuminance()))
    print('  AE.GetAutoEnable        = ' .. tostring(AE.GetAutoEnable()))
    print('demo_auto_exposure ok (headless API check)')
    return
end

-- ==================== 3. 打开窗口 ====================

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, openErr = Window.Open(WIN_W, WIN_H, 'Phase E.5 - Auto Exposure Demo')
if not win then
    print('[demo_ae] Window.Open failed: ' .. tostring(openErr))
    print('demo_auto_exposure ok (no window)')
    return
end

-- ==================== 4. 启用 HDR + AE ====================
-- AE 需 HDR RT 作输入. AE 默认 autoEnable=false, 故须手动 Enable.

local hdrEnabled = false
if HDR.IsSupported() then
    hdrEnabled = HDR.Enable(WIN_W, WIN_H)
    print('[demo_ae] HDR.Enable = ' .. tostring(hdrEnabled))
end

local aeEnabled = false
if hdrEnabled and AE.IsSupported() then
    aeEnabled = AE.Enable(WIN_W, WIN_H)
    print('[demo_ae] AE.Enable  = ' .. tostring(aeEnabled))
end

-- ==================== 5. 主循环 ====================

local lastTime = (Time and Time.GetSeconds and Time.GetSeconds()) or 0

local keyCooldown = {}
local function keyTap(name)
    if win:IsKeyPressed(name) then
        if (keyCooldown[name] or 0) <= 0 then
            keyCooldown[name] = 0.15
            return true
        end
    end
    return false
end

-- "暗场景" / "亮场景" 切换 (D 键)
-- false = split 显示两半 (暗在左, 亮在右), true = 全屏 dark 或 bright
local fullScreenBright = false   -- D 切换: false (split) -> true (亮) -> false (暗) -> ...
local sceneMode = 0   -- 0=split 1=darkOnly 2=brightOnly

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt = now - lastTime
    lastTime = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    win:PollEvents()

    if win:IsKeyPressed('escape') then
        win:Close()
        break
    end

    -- A: 切换 AE
    if keyTap('a') then
        if AE.IsEnabled() then
            AE.Disable()
            print('[demo_ae] AE OFF (manual exposure resumes)')
        else
            if not hdrEnabled then
                hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
            end
            local ok = hdrEnabled and AE.Enable(WIN_W, WIN_H) or false
            print('[demo_ae] AE ' .. (ok and 'ON' or 'OFF (enable failed)'))
        end
    end

    -- D: 切换场景模式 (split / darkOnly / brightOnly)
    if keyTap('d') then
        sceneMode = (sceneMode + 1) % 3
        local name = ({[0]='split', [1]='darkOnly', [2]='brightOnly'})[sceneMode]
        print('[demo_ae] scene -> ' .. name)
    end

    -- 1/2: SpeedUp -/+
    if keyTap('1') then
        AE.SetSpeedUp(AE.GetSpeedUp() - 0.5)
    end
    if keyTap('2') then
        AE.SetSpeedUp(AE.GetSpeedUp() + 0.5)
    end

    -- 3/4: SpeedDown -/+
    if keyTap('3') then
        AE.SetSpeedDown(AE.GetSpeedDown() - 0.5)
    end
    if keyTap('4') then
        AE.SetSpeedDown(AE.GetSpeedDown() + 0.5)
    end

    -- 5/6: TargetEV -/+
    if keyTap('5') then
        AE.SetTargetEV(AE.GetTargetEV() - 0.5)
    end
    if keyTap('6') then
        AE.SetTargetEV(AE.GetTargetEV() + 0.5)
    end

    -- R: 重置默认
    if keyTap('r') then
        AE.SetSpeedUp(3.0)
        AE.SetSpeedDown(1.0)
        AE.SetTargetEV(0.0)
        print('[demo_ae] reset defaults')
    end

    -- 渲染
    win:BeginFrame(0.0, 0.0, 0.0, 1.0)

    -- 决定每个 region 的背景 + sprite 亮度
    -- bgDim=0.02 (约 EV -5)  bgBright=1.0 (约 EV 0)
    -- spriteDim=0.5  spriteBright=5.0 (HDR > 1.0)
    local function fillRect(x, y, w, h, r, g, b)
        Gfx.SetColor(r, g, b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, x, y, 0, w, h, 0)
    end

    if sceneMode == 0 then
        -- split: 左半暗 / 右半亮
        local halfW = WIN_W / 2
        fillRect(0, 0, halfW, WIN_H, 0.02, 0.02, 0.025)
        fillRect(halfW, 0, halfW, WIN_H, 1.0, 1.0, 1.05)
        -- 暗区色块 (亮度 0.5)
        fillRect(60,  120, 80, 80, 0.5, 0.4, 0.5)
        fillRect(180, 260, 80, 80, 0.4, 0.6, 0.5)
        fillRect(80,  380, 100, 80, 0.6, 0.5, 0.4)
        -- 亮区色块 (亮度 5.0, 显著超 1.0)
        fillRect(halfW + 60,  120, 80, 80, 5.0, 4.0, 5.0)
        fillRect(halfW + 180, 260, 80, 80, 4.0, 6.0, 5.0)
        fillRect(halfW + 80,  380, 100, 80, 6.0, 5.0, 4.0)
    elseif sceneMode == 1 then
        -- darkOnly
        fillRect(0, 0, WIN_W, WIN_H, 0.02, 0.02, 0.025)
        fillRect(140, 100, 120, 120, 0.5, 0.4, 0.5)
        fillRect(440, 220, 120, 120, 0.4, 0.6, 0.5)
        fillRect(720, 100, 120, 120, 0.6, 0.5, 0.4)
        fillRect(280, 360, 120, 120, 0.5, 0.5, 0.6)
    else
        -- brightOnly (sceneMode == 2)
        fillRect(0, 0, WIN_W, WIN_H, 1.0, 1.0, 1.05)
        fillRect(140, 100, 120, 120, 5.0, 4.0, 5.0)
        fillRect(440, 220, 120, 120, 4.0, 6.0, 5.0)
        fillRect(720, 100, 120, 120, 6.0, 5.0, 4.0)
        fillRect(280, 360, 120, 120, 5.0, 5.0, 6.0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if win.DrawText then
        local y = 8
        local line = function(s)
            win:DrawText(8, y, s, 1, 1, 1, 1)
            y = y + 16
        end
        local sceneName = ({[0]='split', [1]='darkOnly', [2]='brightOnly'})[sceneMode]
        line(string.format('AE: %s   HDR: %s   Backend: %s   Scene: %s',
            AE.IsEnabled() and 'ON' or 'OFF',
            hdrEnabled and 'ON' or 'OFF',
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'),
            sceneName))
        line(string.format('CurrentEV: %.2f   Exposure: %.3f   MeasuredLogLuma: %.2f',
            AE.GetCurrentEV(), AE.GetCurrentExposure(), AE.GetMeasuredLuminance()))
        line(string.format('TargetEV: %.2f   SpeedUp: %.1f EV/s   SpeedDown: %.1f EV/s',
            AE.GetTargetEV(), AE.GetSpeedUp(), AE.GetSpeedDown()))
        line(string.format('MinEV: %.1f   MaxEV: %.1f   AutoEnable: %s   Supported: %s',
            AE.GetMinEV(), AE.GetMaxEV(),
            tostring(AE.GetAutoEnable()),
            tostring(AE.IsSupported())))
        line('Keys: A=AE  D=scene  1/2=SpeedUp  3/4=SpeedDown  5/6=TargetEV  R=reset  ESC=quit')
    end

    win:EndFrame()
end

-- ==================== 6. 清理 ====================

if AE.IsEnabled() then
    AE.Disable()
end
if hdrEnabled then
    HDR.Disable()
end
print('demo_auto_exposure ok')
