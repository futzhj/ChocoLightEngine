-- ============================================================================
-- ChocoLight Phase E.7 — Lens Flare Demo (callback-model)
-- ============================================================================
-- HDR + Bloom + LensFlare 全链路: ghost + halo + chromatic aberration.
--
-- 控制:
--   F : 切换 LensFlare on/off
--   1/2 : GhostCount -/+ ([0, 8])
--   3/4 : GhostDispersal -/+ (步长 0.05)
--   5/6 : HaloWidth -/+ (步长 0.05)
--   7/8 : ChromaticAberration -/+ (步长 0.001)
--   9/0 : Intensity -/+ (步长 0.05)
--   D : 切换 DistortionEnabled
--   T : 切换 Flare Texture (procedural / 64x64 渐变 LUT)
--   R : reset
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_lens_flare] Light.Graphics 不可用'); print('demo_lens_flare ok (no graphics)'); return end
local HDR, Bloom, LF = Gfx.HDR, Gfx.Bloom, Gfx.LensFlare
if type(HDR) ~= 'table' or type(Bloom) ~= 'table' or type(LF) ~= 'table' then
    print('[demo_lens_flare] HDR/Bloom/LensFlare 子表缺失'); print('demo_lens_flare ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.7 LensFlare demo (callback-model) ====')
print('[demo_lens_flare] Backend             = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_lens_flare] HDR.IsSupported     = ' .. tostring(HDR.IsSupported()))
print('[demo_lens_flare] Bloom.IsSupported   = ' .. tostring(Bloom.IsSupported()))
print('[demo_lens_flare] LensFlare.IsSupported = ' .. tostring(LF.IsSupported()))

if not UI or not UI.Window then
    print('[demo_lens_flare] UI.Window 不可用, 仅 API 探测')
    for _, k in ipairs({'GetThreshold','GetIntensity','GetGhostCount','GetGhostDispersal',
                       'GetHaloWidth','GetChromaticAberration','GetDistortionEnabled'}) do
        print('  LF.' .. k .. '() = ' .. tostring(LF[k] and LF[k]()))
    end
    print('demo_lens_flare ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_lens_flare] Light global 不可用'); print('demo_lens_flare ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

local SPOTS = {
    { x=120, y=140, w=40, h=40, r=4.0, g=0.8, b=0.5 },
    { x=320, y=220, w=60, h=60, r=0.5, g=3.5, b=1.0 },
    { x=520, y=120, w=30, h=30, r=0.5, g=0.6, b=4.5 },
    { x=680, y=280, w=80, h=80, r=3.0, g=2.4, b=0.9 },
    { x=820, y=180, w=40, h=40, r=3.5, g=2.2, b=3.3 },
    { x=220, y=400, w=50, h=50, r=4.0, g=2.8, b=0.5 },
    { x=470, y=420, w=36, h=36, r=0.7, g=4.0, b=3.2 },
    { x=780, y=430, w=64, h=64, r=4.0, g=0.6, b=1.8 },
}

local Demo = Light(Light.UI.Window):New()
local g_hdrEnabled = false
local g_flareImg   = nil   -- 保持引用避免 GC 释放 LF 仍持有的 tex id

function Demo:OnOpen()
    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_lens_flare] HDR.Enable = ' .. tostring(g_hdrEnabled))
    if g_hdrEnabled and not Bloom.IsEnabled() then Bloom.Enable(WIN_W, WIN_H) end
    local ok = g_hdrEnabled and LF.Enable(WIN_W, WIN_H) or false
    print('[demo_lens_flare] LF.Enable  = ' .. tostring(ok))
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
        line(string.format('HDR: %s   Bloom: %s   LensFlare: %s',
            g_hdrEnabled and 'ON' or 'OFF',
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
end

local function build_flare_lut()
    if not (Gfx.Image and Gfx.ImageData) then return nil end
    local W, H = 64, 64
    local bytes = {}
    local cx, cy = (W - 1) * 0.5, (H - 1) * 0.5
    for y = 0, H - 1 do
        for x = 0, W - 1 do
            local dx = (x - cx) / cx; local dy = (y - cy) / cy
            local d  = math.min(1.0, math.sqrt(dx * dx + dy * dy))
            local r = (1.0 - d) * 1.0 + d * 0.3
            local g = (1.0 - d) * 0.9 + d * 0.2
            local b = (1.0 - d) * 0.6 + d * 0.6
            bytes[#bytes + 1] = string.char(
                math.floor(r * 255 + 0.5), math.floor(g * 255 + 0.5),
                math.floor(b * 255 + 0.5), 255)
        end
    end
    return Light(Gfx.Image):New(W, H, table.concat(bytes))
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key == string.byte('F') then
        if LF.IsEnabled() then LF.Disable(); print('[demo] LF OFF')
        else local ok = LF.Enable(WIN_W, WIN_H); print('[demo] LF ' .. (ok and 'ON' or 'OFF (fail)')) end
    elseif key == string.byte('1') then LF.SetGhostCount(LF.GetGhostCount() - 1)
    elseif key == string.byte('2') then LF.SetGhostCount(LF.GetGhostCount() + 1)
    elseif key == string.byte('3') then LF.SetGhostDispersal(clampNum(LF.GetGhostDispersal() - 0.05, 0.0, 2.0))
    elseif key == string.byte('4') then LF.SetGhostDispersal(clampNum(LF.GetGhostDispersal() + 0.05, 0.0, 2.0))
    elseif key == string.byte('5') then LF.SetHaloWidth(clampNum(LF.GetHaloWidth() - 0.05, 0.0, 1.0))
    elseif key == string.byte('6') then LF.SetHaloWidth(clampNum(LF.GetHaloWidth() + 0.05, 0.0, 1.0))
    elseif key == string.byte('7') then LF.SetChromaticAberration(clampNum(LF.GetChromaticAberration() - 0.001, 0.0, 0.02))
    elseif key == string.byte('8') then LF.SetChromaticAberration(clampNum(LF.GetChromaticAberration() + 0.001, 0.0, 0.02))
    elseif key == string.byte('9') then LF.SetIntensity(clampNum(LF.GetIntensity() - 0.05, 0.0))
    elseif key == string.byte('0') then LF.SetIntensity(clampNum(LF.GetIntensity() + 0.05, 0.0))
    elseif key == string.byte('D') then
        LF.SetDistortionEnabled(not LF.GetDistortionEnabled())
        print('[demo] LF distortion = ' .. tostring(LF.GetDistortionEnabled()))
    elseif key == string.byte('T') then
        if LF.GetFlareTextureId() == 0 then
            local img = build_flare_lut()
            if img then
                g_flareImg = img; LF.SetFlareTexture(img)
                print('[demo] Flare texture ON (procedural LUT 64x64, id=' .. tostring(LF.GetFlareTextureId()) .. ')')
            else print('[demo] Flare texture creation failed') end
        else
            LF.SetFlareTexture(nil); g_flareImg = nil
            print('[demo] Flare texture OFF (back to procedural)')
        end
    elseif key == string.byte('R') then
        LF.SetThreshold(1.0); LF.SetIntensity(0.4)
        LF.SetGhostCount(4); LF.SetGhostDispersal(0.4)
        LF.SetHaloWidth(0.5); LF.SetChromaticAberration(0.005)
        LF.SetDistortionEnabled(true)
        LF.SetFlareTexture(nil); g_flareImg = nil
        print('[demo] reset defaults')
    end
end

local function cleanup_demo()
    LF.SetFlareTexture(nil); g_flareImg = nil   -- 提前断引避 LF 悬挂 tex id
    if LF.IsEnabled() then LF.Disable() end
    if Bloom.IsEnabled() then Bloom.Disable() end
    if g_hdrEnabled then HDR.Disable() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.7 - LensFlare Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_lens_flare ok')
