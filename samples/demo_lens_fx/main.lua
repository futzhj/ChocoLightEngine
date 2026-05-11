-- ============================================================================
-- ChocoLight Phase E.6 — Lens Dirt + Streak (镜头后处理) demo
-- ============================================================================
-- 演示 Lens Dirt (镜头脏污) + Streak (anamorphic flare 横向条纹光晕).
--
-- 画面: 黑底 + 多个 HDR 亮点, Bloom + LensDirt + Streak 全链路:
--   * Bloom   : 亮点扩散模糊 (Phase E.4)
--   * LensDirt: bloom × dirt 纹理 (Phase E.6) — 模拟脏镜头
--   * Streak  : 亮点横向 anamorphic 条纹 (Phase E.6) — 电影感
--
-- 控制:
--   L       : 切换 LensDirt on/off
--   K       : 切换 Streak on/off
--   1 / 2   : LensDirt Intensity -/+ (0.1)
--   3 / 4   : Streak Intensity -/+ (0.05)
--   5 / 6   : Streak Length -/+ (0.005)
--   7 / 8   : Streak Iterations -/+
--   H / V / G : Streak Direction horizontal / vertical / diagonal
--   R       : reset 所有参数
--   ESC     : 退出
--
-- 依赖: HDR + Bloom + LensDirt + Streak + UI.Window + Time
-- ============================================================================

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
    print('[demo_lens_fx] Light.Graphics not available')
    print('demo_lens_fx ok (no graphics)')
    return
end

local HDR   = Gfx.HDR
local Bloom = Gfx.Bloom
local LD    = Gfx.LensDirt
local ST    = Gfx.Streak
if type(HDR) ~= 'table' or type(Bloom) ~= 'table'
    or type(LD) ~= 'table' or type(ST) ~= 'table' then
    print('[demo_lens_fx] need HDR + Bloom + LensDirt + Streak subtables')
    print('demo_lens_fx ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.6 LensFx demo ====')
print('[demo_lens_fx] Backend           = ' ..
    tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_lens_fx] HDR.IsSupported   = ' .. tostring(HDR.IsSupported()))
print('[demo_lens_fx] Bloom.IsSupported = ' .. tostring(Bloom.IsSupported()))
print('[demo_lens_fx] LD.IsSupported    = ' .. tostring(LD.IsSupported()))
print('[demo_lens_fx] ST.IsSupported    = ' .. tostring(ST.IsSupported()))

if not UI or not UI.Window then
    print('[demo_lens_fx] UI.Window not available, API probe only')
    print('  LD.IsEnabled     = ' .. tostring(LD.IsEnabled()))
    print('  LD.GetIntensity  = ' .. tostring(LD.GetIntensity()))
    print('  ST.IsEnabled     = ' .. tostring(ST.IsEnabled()))
    print('  ST.GetThreshold  = ' .. tostring(ST.GetThreshold()))
    print('  ST.GetIntensity  = ' .. tostring(ST.GetIntensity()))
    print('  ST.GetLength     = ' .. tostring(ST.GetLength()))
    local dx, dy = ST.GetDirection()
    print('  ST.GetDirection  = (' .. tostring(dx) .. ', ' .. tostring(dy) .. ')')
    print('  ST.GetIterations = ' .. tostring(ST.GetIterations()))
    print('demo_lens_fx ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, err = Window.Open(WIN_W, WIN_H, 'Phase E.6 - LensFx Demo')
if not win then
    print('[demo_lens_fx] Window.Open failed: ' .. tostring(err))
    print('demo_lens_fx ok (no window)')
    return
end

-- 启用 HDR + Bloom (LensDirt 需要 Bloom 输入)
local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_lens_fx] HDR.Enable = ' .. tostring(hdrEnabled))
if hdrEnabled and not Bloom.IsEnabled() then
    Bloom.Enable(WIN_W, WIN_H)   -- 若 autoEnable=false 时手动启
end

-- 启用 LensDirt + Streak
local ldEnabled = hdrEnabled and LD.Enable() or false
local stEnabled = hdrEnabled and ST.Enable(WIN_W, WIN_H) or false
print('[demo_lens_fx] LD.Enable = ' .. tostring(ldEnabled))
print('[demo_lens_fx] ST.Enable = ' .. tostring(stEnabled))

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

local function clampNum(v, lo, hi)
    if v < lo then return lo end
    if hi and v > hi then return hi end
    return v
end

-- HDR 亮点阵列 (R/G/B 可 > 1.0)
local SPOTS = {
    { x = 120, y = 140, w = 40, h = 40, r = 3.5, g = 0.6, b = 0.4 },
    { x = 300, y = 220, w = 60, h = 60, r = 0.4, g = 3.0, b = 0.8 },
    { x = 500, y = 120, w = 30, h = 30, r = 0.4, g = 0.5, b = 4.0 },
    { x = 660, y = 280, w = 80, h = 80, r = 2.6, g = 2.2, b = 0.8 },
    { x = 800, y = 180, w = 40, h = 40, r = 3.2, g = 2.0, b = 3.0 },
    { x = 200, y = 400, w = 50, h = 50, r = 3.6, g = 2.5, b = 0.4 },
    { x = 450, y = 420, w = 36, h = 36, r = 0.6, g = 3.6, b = 3.0 },
    { x = 760, y = 430, w = 64, h = 64, r = 3.6, g = 0.5, b = 1.6 },
}

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
        win:Close(); break
    end

    -- L: 切换 LensDirt
    if keyTap('l') then
        if LD.IsEnabled() then LD.Disable(); print('[demo] LD OFF')
        else local ok = LD.Enable(); print('[demo] LD ' .. (ok and 'ON' or 'OFF (fail)')) end
    end

    -- K: 切换 Streak
    if keyTap('k') then
        if ST.IsEnabled() then ST.Disable(); print('[demo] ST OFF')
        else local ok = ST.Enable(WIN_W, WIN_H); print('[demo] ST ' .. (ok and 'ON' or 'OFF (fail)')) end
    end

    -- 1/2: LD Intensity
    if keyTap('1') then LD.SetIntensity(clampNum(LD.GetIntensity() - 0.1, 0.0)) end
    if keyTap('2') then LD.SetIntensity(clampNum(LD.GetIntensity() + 0.1, 0.0)) end

    -- 3/4: ST Intensity
    if keyTap('3') then ST.SetIntensity(clampNum(ST.GetIntensity() - 0.05, 0.0)) end
    if keyTap('4') then ST.SetIntensity(clampNum(ST.GetIntensity() + 0.05, 0.0)) end

    -- 5/6: ST Length
    if keyTap('5') then ST.SetLength(clampNum(ST.GetLength() - 0.005, 0.0, 0.1)) end
    if keyTap('6') then ST.SetLength(clampNum(ST.GetLength() + 0.005, 0.0, 0.1)) end

    -- 7/8: ST Iterations
    if keyTap('7') then ST.SetIterations(ST.GetIterations() - 1) end
    if keyTap('8') then ST.SetIterations(ST.GetIterations() + 1) end

    -- H / V / G: Direction
    if keyTap('h') then ST.SetDirection(1.0, 0.0); print('[demo] ST dir horizontal') end
    if keyTap('v') then ST.SetDirection(0.0, 1.0); print('[demo] ST dir vertical')   end
    if keyTap('g') then ST.SetDirection(0.707, 0.707); print('[demo] ST dir diagonal') end

    -- R: reset
    if keyTap('r') then
        LD.SetIntensity(0.4)
        ST.SetThreshold(1.0)
        ST.SetIntensity(0.3)
        ST.SetLength(0.02)
        ST.SetIterations(5)
        ST.SetDirection(1.0, 0.0)
        print('[demo] reset defaults')
    end

    -- 渲染
    win:BeginFrame(0.0, 0.0, 0.0, 1.0)
    for _, s in ipairs(SPOTS) do
        Gfx.SetColor(s.r, s.g, s.b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, s.x, s.y, 0, s.w, s.h, 0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        local dx, dy = ST.GetDirection()
        line(string.format('HDR: %s   Bloom: %s   LD: %s   ST: %s',
            hdrEnabled and 'ON' or 'OFF',
            Bloom.IsEnabled() and 'ON' or 'OFF',
            LD.IsEnabled() and 'ON' or 'OFF',
            ST.IsEnabled() and 'ON' or 'OFF'))
        line(string.format('LD: intensity=%.2f  dirtTexId=%d',
            LD.GetIntensity(), LD.GetDirtTextureId()))
        line(string.format('ST: thr=%.2f int=%.2f len=%.3f iter=%d dir=(%.2f,%.2f)',
            ST.GetThreshold(), ST.GetIntensity(), ST.GetLength(),
            ST.GetIterations(), dx, dy))
        line('Keys: L=LD K=ST 1/2=LDint 3/4=STint 5/6=STlen 7/8=STiter H/V/G=dir R=reset ESC')
    end

    win:EndFrame()
end

-- 清理 (反向顺序)
if ST.IsEnabled() then ST.Disable() end
if LD.IsEnabled() then LD.Disable() end
if Bloom.IsEnabled() then Bloom.Disable() end
if hdrEnabled then HDR.Disable() end
print('demo_lens_fx ok')
