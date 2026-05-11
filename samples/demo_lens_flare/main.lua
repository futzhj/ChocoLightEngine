-- ============================================================================
-- ChocoLight Phase E.7 — Lens Flare (Ghost + Halo + Chromatic Aberration) demo
-- ============================================================================
-- HDR + Bloom + LensFlare 全链路:
--   * Bloom    : 亮点扩散模糊 (Phase E.4)
--   * LensFlare: ghost (反向投射光圈) + halo (环形光晕) + chromatic aberration
--
-- 控制:
--   F        : 切换 LensFlare on/off
--   1 / 2    : GhostCount   -/+ (步长 1, 范围 [0, 8])
--   3 / 4    : GhostDispersal -/+ (步长 0.05)
--   5 / 6    : HaloWidth    -/+ (步长 0.05)
--   7 / 8    : ChromaticAberration -/+ (步长 0.001)
--   9 / 0    : Intensity    -/+ (步长 0.05)
--   D        : 切换 DistortionEnabled (色差开关)
--   T        : 切换 Flare Texture (Phase E.7.4: procedural <-> 程序生成 LUT)
--   R        : reset 所有参数到默认
--   ESC      : 退出
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
    print('[demo_lens_flare] Light.Graphics not available')
    print('demo_lens_flare ok (no graphics)')
    return
end

local HDR   = Gfx.HDR
local Bloom = Gfx.Bloom
local LF    = Gfx.LensFlare
if type(HDR) ~= 'table' or type(Bloom) ~= 'table' or type(LF) ~= 'table' then
    print('[demo_lens_flare] need HDR + Bloom + LensFlare subtables')
    print('demo_lens_flare ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.7 LensFlare demo ====')
print('[demo_lens_flare] Backend             = ' ..
    tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_lens_flare] HDR.IsSupported     = ' .. tostring(HDR.IsSupported()))
print('[demo_lens_flare] Bloom.IsSupported   = ' .. tostring(Bloom.IsSupported()))
print('[demo_lens_flare] LensFlare.IsSupported = ' .. tostring(LF.IsSupported()))

if not UI or not UI.Window then
    print('[demo_lens_flare] UI.Window not available, API probe only')
    print('  LF.GetThreshold            = ' .. tostring(LF.GetThreshold()))
    print('  LF.GetIntensity            = ' .. tostring(LF.GetIntensity()))
    print('  LF.GetGhostCount           = ' .. tostring(LF.GetGhostCount()))
    print('  LF.GetGhostDispersal       = ' .. tostring(LF.GetGhostDispersal()))
    print('  LF.GetHaloWidth            = ' .. tostring(LF.GetHaloWidth()))
    print('  LF.GetChromaticAberration  = ' .. tostring(LF.GetChromaticAberration()))
    print('  LF.GetDistortionEnabled    = ' .. tostring(LF.GetDistortionEnabled()))
    print('demo_lens_flare ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, err = Window.Open(WIN_W, WIN_H, 'Phase E.7 - LensFlare Demo')
if not win then
    print('[demo_lens_flare] Window.Open failed: ' .. tostring(err))
    print('demo_lens_flare ok (no window)')
    return
end

local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_lens_flare] HDR.Enable   = ' .. tostring(hdrEnabled))
if hdrEnabled and not Bloom.IsEnabled() then
    Bloom.Enable(WIN_W, WIN_H)
end

local lfEnabled = hdrEnabled and LF.Enable(WIN_W, WIN_H) or false
print('[demo_lens_flare] LF.Enable    = ' .. tostring(lfEnabled))

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

-- HDR 亮点阵列
local SPOTS = {
    { x = 120, y = 140, w = 40, h = 40, r = 4.0, g = 0.8, b = 0.5 },
    { x = 320, y = 220, w = 60, h = 60, r = 0.5, g = 3.5, b = 1.0 },
    { x = 520, y = 120, w = 30, h = 30, r = 0.5, g = 0.6, b = 4.5 },
    { x = 680, y = 280, w = 80, h = 80, r = 3.0, g = 2.4, b = 0.9 },
    { x = 820, y = 180, w = 40, h = 40, r = 3.5, g = 2.2, b = 3.3 },
    { x = 220, y = 400, w = 50, h = 50, r = 4.0, g = 2.8, b = 0.5 },
    { x = 470, y = 420, w = 36, h = 36, r = 0.7, g = 4.0, b = 3.2 },
    { x = 780, y = 430, w = 64, h = 64, r = 4.0, g = 0.6, b = 1.8 },
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
    if win:IsKeyPressed('escape') then win:Close(); break end

    if keyTap('f') then
        if LF.IsEnabled() then LF.Disable(); print('[demo] LF OFF')
        else local ok = LF.Enable(WIN_W, WIN_H); print('[demo] LF ' .. (ok and 'ON' or 'OFF (fail)')) end
    end

    -- 1/2: GhostCount
    if keyTap('1') then LF.SetGhostCount(LF.GetGhostCount() - 1) end
    if keyTap('2') then LF.SetGhostCount(LF.GetGhostCount() + 1) end

    -- 3/4: GhostDispersal
    if keyTap('3') then LF.SetGhostDispersal(clampNum(LF.GetGhostDispersal() - 0.05, 0.0, 2.0)) end
    if keyTap('4') then LF.SetGhostDispersal(clampNum(LF.GetGhostDispersal() + 0.05, 0.0, 2.0)) end

    -- 5/6: HaloWidth
    if keyTap('5') then LF.SetHaloWidth(clampNum(LF.GetHaloWidth() - 0.05, 0.0, 1.0)) end
    if keyTap('6') then LF.SetHaloWidth(clampNum(LF.GetHaloWidth() + 0.05, 0.0, 1.0)) end

    -- 7/8: ChromaticAberration
    if keyTap('7') then LF.SetChromaticAberration(clampNum(LF.GetChromaticAberration() - 0.001, 0.0, 0.02)) end
    if keyTap('8') then LF.SetChromaticAberration(clampNum(LF.GetChromaticAberration() + 0.001, 0.0, 0.02)) end

    -- 9/0: Intensity
    if keyTap('9') then LF.SetIntensity(clampNum(LF.GetIntensity() - 0.05, 0.0)) end
    if keyTap('0') then LF.SetIntensity(clampNum(LF.GetIntensity() + 0.05, 0.0)) end

    -- D: DistortionEnabled
    if keyTap('d') then
        LF.SetDistortionEnabled(not LF.GetDistortionEnabled())
        print('[demo] LF distortion = ' .. tostring(LF.GetDistortionEnabled()))
    end

    -- T: Phase E.7.4 — 切换 Flare Texture (procedural / 内置渐变 LUT)
    if keyTap('t') then
        if LF.GetFlareTextureId() == 0 then
            -- 启用：生成一张 64x64 渐变彩虹 LUT (中心暖色 + 边缘冷色)
            if Gfx.Image and Gfx.ImageData then
                local W, H = 64, 64
                local bytes = {}
                local cx, cy = (W - 1) * 0.5, (H - 1) * 0.5
                for y = 0, H - 1 do
                    for x = 0, W - 1 do
                        local dx = (x - cx) / cx
                        local dy = (y - cy) / cy
                        local d  = math.min(1.0, math.sqrt(dx * dx + dy * dy))
                        -- 中心黄白 (1.0, 0.9, 0.6) -> 边缘暗紫 (0.3, 0.2, 0.6)
                        local r = (1.0 - d) * 1.0 + d * 0.3
                        local g = (1.0 - d) * 0.9 + d * 0.2
                        local b = (1.0 - d) * 0.6 + d * 0.6
                        bytes[#bytes + 1] = string.char(
                            math.floor(r * 255 + 0.5),
                            math.floor(g * 255 + 0.5),
                            math.floor(b * 255 + 0.5),
                            255)
                    end
                end
                local rgba = table.concat(bytes)
                local img  = Light(Gfx.Image):New(W, H, rgba)
                if img then
                    -- demo 持有 img 引用避免 GC; LF 仅存 tex id
                    _G.__demo_flare_img = img
                    LF.SetFlareTexture(img)
                    print('[demo] Flare texture ON (procedural LUT 64x64, id=' ..
                        tostring(LF.GetFlareTextureId()) .. ')')
                else
                    print('[demo] Flare texture creation failed')
                end
            else
                print('[demo] Light.Graphics.Image not available, skip flare texture')
            end
        else
            LF.SetFlareTexture(nil)
            _G.__demo_flare_img = nil
            print('[demo] Flare texture OFF (back to procedural)')
        end
    end

    -- R: reset
    if keyTap('r') then
        LF.SetThreshold(1.0); LF.SetIntensity(0.4)
        LF.SetGhostCount(4); LF.SetGhostDispersal(0.4)
        LF.SetHaloWidth(0.5); LF.SetChromaticAberration(0.005)
        LF.SetDistortionEnabled(true)
        LF.SetFlareTexture(nil)
        _G.__demo_flare_img = nil
        print('[demo] reset defaults')
    end

    win:BeginFrame(0.0, 0.0, 0.0, 1.0)
    for _, s in ipairs(SPOTS) do
        Gfx.SetColor(s.r, s.g, s.b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, s.x, s.y, 0, s.w, s.h, 0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        line(string.format('HDR: %s   Bloom: %s   LensFlare: %s',
            hdrEnabled and 'ON' or 'OFF',
            Bloom.IsEnabled() and 'ON' or 'OFF',
            LF.IsEnabled() and 'ON' or 'OFF'))
        line(string.format('LF: thr=%.2f int=%.2f  ghost(%d, %.2f)  halo=%.2f',
            LF.GetThreshold(), LF.GetIntensity(),
            LF.GetGhostCount(), LF.GetGhostDispersal(),
            LF.GetHaloWidth()))
        line(string.format('LF: chroma=%.3f distortion=%s  flareTex=%s',
            LF.GetChromaticAberration(),
            LF.GetDistortionEnabled() and 'ON' or 'OFF',
            LF.GetFlareTextureId() ~= 0 and 'LUT(' .. LF.GetFlareTextureId() .. ')' or 'fallback'))
        line('Keys: F=LF 1/2=ghost# 3/4=disp 5/6=halo 7/8=chroma 9/0=int D=distort T=flareTex R=reset ESC')
    end

    win:EndFrame()
end

-- 反向清理
LF.SetFlareTexture(nil)        -- Phase E.7.4: 提前断引避免 LF 持悬挂 tex id
_G.__demo_flare_img = nil
if LF.IsEnabled() then LF.Disable() end
if Bloom.IsEnabled() then Bloom.Disable() end
if hdrEnabled then HDR.Disable() end
print('demo_lens_flare ok')
