-- ============================================================================
-- ChocoLight Phase E.4 — Bloom Demo (callback-model)
-- ============================================================================
-- 演示 Bloom 后处理: 亮度提取 + 多尺度高斯模糊金字塔 + 加性合成.
--
-- 控制:
--   B : 切换 Bloom ON / OFF
--   1/2 : Threshold -/+ (步长 0.1)
--   3/4 : Intensity -/+ (步长 0.1)
--   5/6 : Radius -/+ (步长 0.05, [0, 1])
--   7/8 : Levels -/+ ([2, 8], 下次 Enable/Resize 生效)
--   R : 重置 (Thr=1.0 Int=0.8 Rad=0.7 Lv=5)
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_bloom] Light.Graphics 不可用'); print('demo_bloom ok (no graphics)'); return end
local HDR, Bloom = Gfx.HDR, Gfx.Bloom
if type(HDR) ~= 'table' or type(Bloom) ~= 'table' then
    print('[demo_bloom] HDR / Bloom 子表缺失'); print('demo_bloom ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.4 Bloom demo (callback-model) ====')
print('[demo_bloom] Backend           = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_bloom] HDR.IsSupported   = ' .. tostring(HDR.IsSupported()))
print('[demo_bloom] Bloom.IsSupported = ' .. tostring(Bloom.IsSupported()))

-- Headless probe
if not UI or not UI.Window then
    print('[demo_bloom] UI.Window 不可用, 仅 API 探测')
    print('  Bloom.IsEnabled    = ' .. tostring(Bloom.IsEnabled()))
    print('  Bloom.GetThreshold = ' .. tostring(Bloom.GetThreshold()))
    print('  Bloom.GetIntensity = ' .. tostring(Bloom.GetIntensity()))
    print('  Bloom.GetRadius    = ' .. tostring(Bloom.GetRadius()))
    print('  Bloom.GetLevels    = ' .. tostring(Bloom.GetLevels()))
    print('demo_bloom ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_bloom] Light global 不可用'); print('demo_bloom ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

-- 亮点阵列 (顶点色 > 1.0, HDR ON 时辉光明显)
local SPOTS = {
    { x=120, y=120, w=40, h=40, r=3.5, g=0.6, b=0.4 },
    { x=280, y=200, w=60, h=60, r=0.5, g=3.0, b=0.8 },
    { x=480, y=100, w=30, h=30, r=0.4, g=0.6, b=3.8 },
    { x=620, y=280, w=80, h=80, r=2.5, g=2.2, b=0.8 },
    { x=800, y=160, w=40, h=40, r=3.0, g=2.0, b=3.0 },
    { x=180, y=360, w=50, h=50, r=3.5, g=2.5, b=0.4 },
    { x=380, y=400, w=36, h=36, r=0.6, g=3.6, b=3.0 },
    { x=720, y=420, w=64, h=64, r=3.6, g=0.5, b=1.6 },
}

local Demo = Light(Light.UI.Window):New()
local g_hdrEnabled = false

function Demo:OnOpen()
    if HDR.IsSupported() then
        g_hdrEnabled = HDR.Enable(WIN_W, WIN_H)
        print('[demo_bloom] HDR.Enable = ' .. tostring(g_hdrEnabled))
    end
    if g_hdrEnabled then
        print('[demo_bloom] Bloom.IsEnabled (auto) = ' .. tostring(Bloom.IsEnabled()))
    end
end

function Demo:Update(dt) end

function Demo:Draw()
    -- 暗背景网格
    Gfx.SetColor(0.04, 0.04, 0.06, 1.0)
    for gx = 0, WIN_W, 40 do Gfx.Rectangle(Gfx.FillMode, gx, 0, 0, 1, WIN_H, 0) end
    for gy = 0, WIN_H, 40 do Gfx.Rectangle(Gfx.FillMode, 0, gy, 0, WIN_W, 1, 0) end

    -- 亮点
    for _, s in ipairs(SPOTS) do
        Gfx.SetColor(s.r, s.g, s.b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, s.x, s.y, 0, s.w, s.h, 0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if Gfx.Print then
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('HDR: %s   Bloom: %s   Supported: HDR=%s Bloom=%s   Backend: %s',
            g_hdrEnabled and 'ON' or 'OFF',
            Bloom.IsEnabled() and 'ON' or 'OFF',
            tostring(HDR.IsSupported()),
            tostring(Bloom.IsSupported()),
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?')))
        line(string.format('Threshold: %.2f   Intensity: %.2f   Radius: %.2f   Levels: %d (auto=%s)',
            Bloom.GetThreshold(), Bloom.GetIntensity(),
            Bloom.GetRadius(), Bloom.GetLevels(),
            tostring(Bloom.GetAutoEnable())))
        line('Keys: B=Bloom  1/2=Threshold  3/4=Intensity  5/6=Radius  7/8=Levels  R=reset  ESC=quit')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()                                                                -- ESC
    elseif key == string.byte('B') then
        if Bloom.IsEnabled() then Bloom.Disable(); print('[demo_bloom] Bloom OFF')
        else
            if not g_hdrEnabled then g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false end
            local ok = g_hdrEnabled and Bloom.Enable(WIN_W, WIN_H) or false
            print('[demo_bloom] Bloom ' .. (ok and 'ON' or 'OFF (enable failed)'))
        end
    elseif key == string.byte('1') then Bloom.SetThreshold(clampNum(Bloom.GetThreshold() - 0.1, 0.0))
    elseif key == string.byte('2') then Bloom.SetThreshold(clampNum(Bloom.GetThreshold() + 0.1, 0.0))
    elseif key == string.byte('3') then Bloom.SetIntensity(clampNum(Bloom.GetIntensity() - 0.1, 0.0))
    elseif key == string.byte('4') then Bloom.SetIntensity(clampNum(Bloom.GetIntensity() + 0.1, 0.0))
    elseif key == string.byte('5') then Bloom.SetRadius(clampNum(Bloom.GetRadius() - 0.05, 0.0, 1.0))
    elseif key == string.byte('6') then Bloom.SetRadius(clampNum(Bloom.GetRadius() + 0.05, 0.0, 1.0))
    elseif key == string.byte('7') then Bloom.SetLevels(Bloom.GetLevels() - 1); print('[demo_bloom] Levels -> ' .. Bloom.GetLevels())
    elseif key == string.byte('8') then Bloom.SetLevels(Bloom.GetLevels() + 1); print('[demo_bloom] Levels -> ' .. Bloom.GetLevels())
    elseif key == string.byte('R') then
        Bloom.SetThreshold(1.0); Bloom.SetIntensity(0.8); Bloom.SetRadius(0.7); Bloom.SetLevels(5)
        if Bloom.IsEnabled() then Bloom.Resize(WIN_W, WIN_H) end
        print('[demo_bloom] reset defaults')
    end
end

local function cleanup_demo()
    if Bloom.IsEnabled() then Bloom.Disable() end
    if g_hdrEnabled then HDR.Disable() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.4 - Bloom Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_bloom ok')
