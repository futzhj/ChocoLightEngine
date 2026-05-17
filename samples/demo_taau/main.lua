-- ============================================================================
-- ChocoLight Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦演示
-- ============================================================================
-- DLSS / FSR2 风格: render 在低分辨率, output 在高分辨率, TAA 累积上采样.
--
-- 控制:
--   Y      : 切 TAAU (默认 OFF, 零回归 F.0 行为)
--   1      : Performance preset (renderScale = 0.5)
--   2      : Balanced preset    (renderScale = 0.667)
--   3      : Quality preset     (renderScale = 0.75)
--   4      : Native preset      (renderScale = 1.0)
--   -/=    : RenderScale -/+ 0.05 (与 preset 自动同步)
--   T      : 切 TAA (基础, 必须 ON 才能见到 TAAU 效果)
--   J      : 切 Jitter
--   H      : Sharpness 循环 (0/0.5/1.0/1.5/2.0)
--   Z      : SharpenMode 循环 ("unsharp"/"cas"/"rcas")
--   X      : 切 HalfResHistory (Q5: TAAU 启用时强制关; 演示自动仲裁)
--   R      : reset 全部为默认
--   ESC    : 退出
-- ============================================================================

local function safe_require(n)
    local ok, m = pcall(require, n)
    if ok and type(m) == 'table' then return m end
    return nil
end

local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')
if not Gfx then print('[demo_taau] Light.Graphics 不可用'); print('demo_taau ok (no graphics)'); return end

local HDR, TAA = Gfx.HDR, Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taau] HDR/TAA 子表缺失'); print('demo_taau ok (subtable missing)'); return
end

print('==== ChocoLight Phase F.1 TAAU Demo ====')
print('[demo_taau] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taau] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_taau] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))

if not UI or not UI.Window then
    print('[demo_taau] UI.Window 不可用, 仅 API 探测')
    print('  TAA.GetTAAUEnabled()  = ' .. tostring(TAA.GetTAAUEnabled()))
    print('  TAA.GetRenderScale()  = ' .. tostring(TAA.GetRenderScale()))
    print('  TAA.GetUpscalePreset()= ' .. tostring(TAA.GetUpscalePreset()))
    print('demo_taau ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_taau] Light global 不可用'); print('demo_taau ok (no Light global)'); return
end

local WIN_W, WIN_H = 1280, 720
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

local function buildCube()
    local s = 0.5
    local v, idx = {}, {}
    local function addQuad(p1, p2, p3, p4, n)
        local base = #v / 12
        for _, p in ipairs({p1, p2, p3, p4}) do
            v[#v+1]=p[1]; v[#v+1]=p[2]; v[#v+1]=p[3]
            v[#v+1]=n[1]; v[#v+1]=n[2]; v[#v+1]=n[3]
            v[#v+1]=0; v[#v+1]=0
            v[#v+1]=1; v[#v+1]=1; v[#v+1]=1; v[#v+1]=1
        end
        idx[#idx+1]=base+1; idx[#idx+1]=base+2; idx[#idx+1]=base+3
        idx[#idx+1]=base+1; idx[#idx+1]=base+3; idx[#idx+1]=base+4
    end
    addQuad({ s,-s,-s},{ s, s,-s},{ s, s, s},{ s,-s, s},{ 1, 0, 0})
    addQuad({-s,-s, s},{-s, s, s},{-s, s,-s},{-s,-s,-s},{-1, 0, 0})
    addQuad({-s, s,-s},{-s, s, s},{ s, s, s},{ s, s,-s},{ 0, 1, 0})
    addQuad({-s,-s, s},{-s,-s,-s},{ s,-s,-s},{ s,-s, s},{ 0,-1, 0})
    addQuad({-s,-s, s},{ s,-s, s},{ s, s, s},{-s, s, s},{ 0, 0, 1})
    addQuad({ s,-s,-s},{-s,-s,-s},{-s, s,-s},{ s, s,-s},{ 0, 0,-1})
    return v, idx
end

local function buildPlane()
    local s = 5.0
    local v = { -s,0,-s, 0,1,0, 0,0, 0.5,0.5,0.55,1,
                 s,0,-s, 0,1,0, 1,0, 0.5,0.5,0.55,1,
                 s,0, s, 0,1,0, 1,1, 0.5,0.5,0.55,1,
                -s,0, s, 0,1,0, 0,1, 0.5,0.5,0.55,1 }
    return v, { 1, 2, 3, 1, 3, 4 }
end

-- 高频几何场景 (棋盘格 + 多个旋转小立方体), 让 TAAU 上采样质量差异可见
local SCENE = {
    {x= 0.0,y=0.6,z= 0.0, scale=1.2, color={1.0,0.4,0.4}},
    {x= 2.5,y=0.4,z= 1.5, scale=0.6, color={0.4,1.0,0.4}},
    {x=-2.5,y=0.4,z=-1.5, scale=0.6, color={0.4,0.4,1.0}},
    {x= 1.5,y=0.3,z=-2.5, scale=0.5, color={1.0,1.0,0.4}},
    {x=-1.5,y=0.3,z= 2.5, scale=0.5, color={0.4,1.0,1.0}},
    {x= 3.5,y=0.4,z=-3.0, scale=0.6, color={1.0,0.5,1.0}},
    {x=-3.5,y=0.4,z= 3.0, scale=0.6, color={1.0,0.7,0.3}},
    {x= 0.0,y=0.3,z=-4.0, scale=0.5, color={0.8,0.8,1.0}},
    {x= 0.0,y=0.3,z= 4.0, scale=0.5, color={1.0,0.8,0.8}},
}

local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_planeMesh = nil, nil
local g_hdrEnabled = false
local g_camAngle   = 0.0
local g_camHeight  = 3.0

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_taau] Gfx.Mesh.New 不可用'); self:Close(); return
    end
    local cv, ci = buildCube(); local pv, pi = buildPlane()
    g_cubeMesh  = Mesh.New(cv, ci)
    g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_planeMesh then print('[demo_taau] mesh build failed'); self:Close(); return end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_taau] HDR.Enable = ' .. tostring(g_hdrEnabled))
    if g_hdrEnabled and TAA.IsSupported() then
        local taaOk = TAA.Enable(WIN_W, WIN_H)
        print('[demo_taau] TAA.Enable = ' .. tostring(taaOk))
    end

    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 1.5)
    end
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_camAngle = g_camAngle + dt * 0.20
end

function Demo:Draw()
    local cx, cz = math.cos(g_camAngle) * 8.0, math.sin(g_camAngle) * 8.0
    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(cx, g_camHeight, cz, 0.0, 0.5, 0.0) end

    if g_planeMesh then
        Gfx.Push(); Gfx.SetColor(0.5, 0.5, 0.55, 1.0); g_planeMesh:Draw(0); Gfx.Pop()
    end
    if g_cubeMesh then
        for _, c in ipairs(SCENE) do
            Gfx.Push()
            Gfx.Translate(c.x, c.y, c.z); Gfx.Scale(c.scale, c.scale, c.scale)
            Gfx.SetColor(c.color[1], c.color[2], c.color[3], 1.0)
            g_cubeMesh:Draw(0); Gfx.Pop()
        end
    end
    Gfx.SetColor(1, 1, 1, 1)

    if Gfx.Print then
        local y = 8
        local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end

        line(string.format('HDR: %s   TAA: %s   Jitter: %s',
            g_hdrEnabled and 'ON' or 'OFF',
            TAA.IsEnabled() and 'ON' or 'OFF',
            TAA.GetJitterEnabled() and 'ON' or 'OFF'))

        -- F.1 TAAU 状态
        local taauOn   = TAA.GetTAAUEnabled()
        local scale    = TAA.GetRenderScale()
        local preset   = TAA.GetUpscalePreset()
        local rw, rh   = TAA.GetRenderResolution()
        local ow, oh   = TAA.GetOutputResolution()
        line(string.format('TAAU: %s   preset=%s   scale=%.3f',
            taauOn and 'ON' or 'OFF', preset, scale))
        line(string.format('Render: %dx%d   Output: %dx%d   (ratio %.0f%%)',
            rw, rh, ow, oh,
            (ow > 0) and (100.0 * rw / ow) or 100.0))

        line(string.format('Sharpen: %s   mode=%s   HalfResHistory=%s',
            (TAA.GetSharpness() > 0) and string.format('%.2f', TAA.GetSharpness()) or 'OFF',
            TAA.GetSharpenMode(), TAA.GetHalfResHistory() and 'ON' or 'OFF'))

        -- Phase F.1.1: MipBias 状态 (auto 模式下应等于 log2(scale)-0.7)
        if TAA.GetMipBias then
            line(string.format('MipBias: %.3f   auto=%s',
                TAA.GetMipBias(), TAA.GetAutoMipBias() and 'ON' or 'OFF'))
        end

        local jx, jy = TAA.GetCurrentJitter()
        line(string.format('Jitter: frame=%d  jx=%.3f  jy=%.3f (in %s pixel)',
            TAA.GetFrameCounter(), jx, jy, taauOn and 'render' or 'output'))

        line('')
        line('Keys: Y=TAAU 1/2/3/4=preset(perf/bal/qual/nat) -/= RenderScale')
        line('      T=TAA J=Jitter H=Sharp Z=SharpenMode X=HalfRes B=AutoMipBias')
        line('      E=ScreenshotEXR M=RecordMP4toggle R=reset ESC')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close(); return end   -- ESC

    if key == string.byte('Y') then
        local newOn = not TAA.GetTAAUEnabled()
        local ok = TAA.SetTAAUEnabled(newOn)
        print(string.format('[demo_taau] SetTAAUEnabled(%s) -> %s', tostring(newOn), tostring(ok)))
        if ok then
            local rw, rh = TAA.GetRenderResolution()
            local ow, oh = TAA.GetOutputResolution()
            print(string.format('  Render=%dx%d  Output=%dx%d  Scale=%.3f',
                rw, rh, ow, oh, TAA.GetRenderScale()))
        end

    elseif key == string.byte('1') then
        TAA.SetUpscalePreset('performance')
        print('[demo_taau] Preset=performance (scale=0.5)')
    elseif key == string.byte('2') then
        TAA.SetUpscalePreset('balanced')
        print('[demo_taau] Preset=balanced (scale=0.667)')
    elseif key == string.byte('3') then
        TAA.SetUpscalePreset('quality')
        print('[demo_taau] Preset=quality (scale=0.75)')
    elseif key == string.byte('4') then
        TAA.SetUpscalePreset('native')
        print('[demo_taau] Preset=native (scale=1.0)')

    elseif key == 45 then    -- '-'
        TAA.SetRenderScale(clampNum(TAA.GetRenderScale() - 0.05, 0.5, 1.0))
        print(string.format('[demo_taau] RenderScale = %.3f  (preset=%s)',
            TAA.GetRenderScale(), TAA.GetUpscalePreset()))
    elseif key == 61 then    -- '='
        TAA.SetRenderScale(clampNum(TAA.GetRenderScale() + 0.05, 0.5, 1.0))
        print(string.format('[demo_taau] RenderScale = %.3f  (preset=%s)',
            TAA.GetRenderScale(), TAA.GetUpscalePreset()))

    elseif key == string.byte('T') then
        if TAA.IsEnabled() then
            TAA.Disable(); print('[demo_taau] TAA OFF')
        else
            local ok = TAA.Enable(WIN_W, WIN_H); print('[demo_taau] TAA ' .. (ok and 'ON' or 'OFF (fail)'))
        end
    elseif key == string.byte('J') then
        TAA.SetJitterEnabled(not TAA.GetJitterEnabled())
        print('[demo_taau] Jitter = ' .. (TAA.GetJitterEnabled() and 'ON' or 'OFF'))

    elseif key == string.byte('H') then
        local cur = TAA.GetSharpness()
        local steps = { 0.0, 0.5, 1.0, 1.5, 2.0 }
        local nextIdx = 1
        for i, v in ipairs(steps) do
            if math.abs(cur - v) < 0.01 then nextIdx = (i % #steps) + 1; break end
        end
        TAA.SetSharpness(steps[nextIdx])
        print(string.format('[demo_taau] Sharpness = %.2f', steps[nextIdx]))

    elseif key == string.byte('Z') then
        local modes = { 'unsharp', 'cas', 'rcas' }
        local cur = TAA.GetSharpenMode()
        local nextIdx = 1
        for i, m in ipairs(modes) do
            if m == cur then nextIdx = (i % #modes) + 1; break end
        end
        TAA.SetSharpenMode(modes[nextIdx])
        print('[demo_taau] SharpenMode = ' .. modes[nextIdx])

    elseif key == string.byte('X') then
        local newOn = not TAA.GetHalfResHistory()
        TAA.SetHalfResHistory(newOn)
        -- 提示 Q5 仲裁
        if TAA.GetTAAUEnabled() and newOn then
            print('[demo_taau] HalfResHistory 与 TAAU 互斥, Q5 仲裁应已自动关 TAAU 或 HalfRes')
        end
        print('[demo_taau] HalfResHistory = ' .. (TAA.GetHalfResHistory() and 'ON' or 'OFF'))

    elseif key == string.byte('B') then
        -- Phase F.1.1: 切 autoMipBias (调试: 关后纹理可能变糊, 开后随 renderScale 自动锐化)
        if TAA.SetAutoMipBias then
            TAA.SetAutoMipBias(not TAA.GetAutoMipBias())
            print(string.format('[demo_taau] AutoMipBias = %s, current bias = %.3f',
                TAA.GetAutoMipBias() and 'ON' or 'OFF', TAA.GetMipBias()))
        end

    elseif key == string.byte('E') then
        -- Phase F.0.11.5: ScreenshotEXR (HDR 高精度 OpenEXR, half-float ZIP 压缩)
        if Gfx.ScreenshotEXR then
            local ok, err = Gfx.ScreenshotEXR('taau_screenshot.exr')
            if ok then
                print('[demo_taau] ScreenshotEXR -> taau_screenshot.exr (half-float, ZIP)')
            else
                print('[demo_taau] ScreenshotEXR failed: ' .. tostring(err))
            end
        end

    elseif key == string.byte('M') then
        -- Phase F.0.11.6: 切 MP4 录屏 (开始/结束)
        if Gfx.RecordMP4 and Gfx.GetRecordMode and Gfx.IsRecording then
            if Gfx.IsRecording() then
                local n = Gfx.StopRecord()
                print(string.format('[demo_taau] StopRecord: %d frames written to taau_record.mp4', n))
            else
                local ok, err = Gfx.RecordMP4('taau_record.mp4', { fps = 30, bitrate = 5000000 })
                if ok then
                    print('[demo_taau] RecordMP4 started -> taau_record.mp4 (30 fps, 5 Mbps); 再按 M 停止')
                else
                    print('[demo_taau] RecordMP4 failed: ' .. tostring(err))
                end
            end
        end

    elseif key == string.byte('R') then
        TAA.SetTAAUEnabled(false)
        TAA.SetRenderScale(1.0)
        TAA.SetSharpness(0.5)
        TAA.SetSharpenMode('unsharp')
        TAA.SetJitterEnabled(true)
        TAA.SetHalfResHistory(false)
        if TAA.SetAutoMipBias then TAA.SetAutoMipBias(true) end
        print('[demo_taau] reset to defaults')
    end
end

Light(Demo):Open()
print('demo_taau ok')
