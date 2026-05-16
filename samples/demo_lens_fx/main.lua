-- ============================================================================
-- ChocoLight Phase E.6 — Lens Dirt + Streak Demo (callback-model)
-- ============================================================================
-- HDR + Bloom + LensDirt + Streak 全链路.
--
-- 控制:
--   L : 切换 LensDirt
--   K : 切换 Streak
--   1/2 : LD Intensity -/+ (步长 0.1)
--   3/4 : ST Intensity -/+ (步长 0.05)
--   5/6 : ST Length -/+ (步长 0.005)
--   7/8 : ST Iterations -/+
--   H/V/G : Direction horizontal / vertical / diagonal
--   R : reset
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_lens_fx] Light.Graphics 不可用'); print('demo_lens_fx ok (no graphics)'); return end
local HDR, Bloom, LD, ST = Gfx.HDR, Gfx.Bloom, Gfx.LensDirt, Gfx.Streak
if type(HDR) ~= 'table' or type(Bloom) ~= 'table' or type(LD) ~= 'table' or type(ST) ~= 'table' then
    print('[demo_lens_fx] HDR/Bloom/LensDirt/Streak 子表缺失'); print('demo_lens_fx ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.6 LensFx demo (callback-model) ====')
print('[demo_lens_fx] Backend           = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_lens_fx] HDR.IsSupported   = ' .. tostring(HDR.IsSupported()))
print('[demo_lens_fx] Bloom.IsSupported = ' .. tostring(Bloom.IsSupported()))
print('[demo_lens_fx] LD.IsSupported    = ' .. tostring(LD.IsSupported()))
print('[demo_lens_fx] ST.IsSupported    = ' .. tostring(ST.IsSupported()))

if not UI or not UI.Window then
    print('[demo_lens_fx] UI.Window 不可用, 仅 API 探测')
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
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_lens_fx] Light global 不可用'); print('demo_lens_fx ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

local SPOTS = {
    { x=120, y=140, w=40, h=40, r=3.5, g=0.6, b=0.4 },
    { x=300, y=220, w=60, h=60, r=0.4, g=3.0, b=0.8 },
    { x=500, y=120, w=30, h=30, r=0.4, g=0.5, b=4.0 },
    { x=660, y=280, w=80, h=80, r=2.6, g=2.2, b=0.8 },
    { x=800, y=180, w=40, h=40, r=3.2, g=2.0, b=3.0 },
    { x=200, y=400, w=50, h=50, r=3.6, g=2.5, b=0.4 },
    { x=450, y=420, w=36, h=36, r=0.6, g=3.6, b=3.0 },
    { x=760, y=430, w=64, h=64, r=3.6, g=0.5, b=1.6 },
}

local Demo = Light(Light.UI.Window):New()
local g_hdrEnabled = false

function Demo:OnOpen()
    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_lens_fx] HDR.Enable = ' .. tostring(g_hdrEnabled))
    if g_hdrEnabled and not Bloom.IsEnabled() then Bloom.Enable(WIN_W, WIN_H) end
    print('[demo_lens_fx] LD.Enable = ' .. tostring(g_hdrEnabled and LD.Enable() or false))
    print('[demo_lens_fx] ST.Enable = ' .. tostring(g_hdrEnabled and ST.Enable(WIN_W, WIN_H) or false))
end

function Demo:Update(dt) end

function Demo:Draw()
    for _, s in ipairs(SPOTS) do
        Gfx.SetColor(s.r, s.g, s.b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, s.x, s.y, 0, s.w, s.h, 0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    if Gfx.Print then
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        local dx, dy = ST.GetDirection()
        line(string.format('HDR: %s   Bloom: %s   LD: %s   ST: %s',
            g_hdrEnabled and 'ON' or 'OFF',
            Bloom.IsEnabled() and 'ON' or 'OFF',
            LD.IsEnabled() and 'ON' or 'OFF',
            ST.IsEnabled() and 'ON' or 'OFF'))
        line(string.format('LD: intensity=%.2f  dirtTexId=%d', LD.GetIntensity(), LD.GetDirtTextureId()))
        line(string.format('ST: thr=%.2f int=%.2f len=%.3f iter=%d dir=(%.2f,%.2f)',
            ST.GetThreshold(), ST.GetIntensity(), ST.GetLength(), ST.GetIterations(), dx, dy))
        line('Keys: L=LD K=ST 1/2=LDint 3/4=STint 5/6=STlen 7/8=STiter H/V/G=dir R=reset ESC')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key == string.byte('L') then
        if LD.IsEnabled() then LD.Disable(); print('[demo] LD OFF')
        else local ok = LD.Enable(); print('[demo] LD ' .. (ok and 'ON' or 'OFF')) end
    elseif key == string.byte('K') then
        if ST.IsEnabled() then ST.Disable(); print('[demo] ST OFF')
        else local ok = ST.Enable(WIN_W, WIN_H); print('[demo] ST ' .. (ok and 'ON' or 'OFF')) end
    elseif key == string.byte('1') then LD.SetIntensity(clampNum(LD.GetIntensity() - 0.1, 0.0))
    elseif key == string.byte('2') then LD.SetIntensity(clampNum(LD.GetIntensity() + 0.1, 0.0))
    elseif key == string.byte('3') then ST.SetIntensity(clampNum(ST.GetIntensity() - 0.05, 0.0))
    elseif key == string.byte('4') then ST.SetIntensity(clampNum(ST.GetIntensity() + 0.05, 0.0))
    elseif key == string.byte('5') then ST.SetLength(clampNum(ST.GetLength() - 0.005, 0.0, 0.1))
    elseif key == string.byte('6') then ST.SetLength(clampNum(ST.GetLength() + 0.005, 0.0, 0.1))
    elseif key == string.byte('7') then ST.SetIterations(ST.GetIterations() - 1)
    elseif key == string.byte('8') then ST.SetIterations(ST.GetIterations() + 1)
    elseif key == string.byte('H') then ST.SetDirection(1.0, 0.0); print('[demo] ST dir horizontal')
    elseif key == string.byte('V') then ST.SetDirection(0.0, 1.0); print('[demo] ST dir vertical')
    elseif key == string.byte('G') then ST.SetDirection(0.707, 0.707); print('[demo] ST dir diagonal')
    elseif key == string.byte('R') then
        LD.SetIntensity(0.4); ST.SetThreshold(1.0); ST.SetIntensity(0.3)
        ST.SetLength(0.02); ST.SetIterations(5); ST.SetDirection(1.0, 0.0)
        print('[demo] reset defaults')
    end
end

local function cleanup_demo()
    if ST.IsEnabled() then ST.Disable() end
    if LD.IsEnabled() then LD.Disable() end
    if Bloom.IsEnabled() then Bloom.Disable() end
    if g_hdrEnabled then HDR.Disable() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.6 - LensFx Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_lens_fx ok')
