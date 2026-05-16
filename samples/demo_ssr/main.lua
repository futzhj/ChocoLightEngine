-- ============================================================================
-- ChocoLight Phase E.9~E.18 + F.0~F.0.14 — SSR + Velocity + MotionBlur + TAA Demo (callback-model)
-- ============================================================================
-- 反射场景沙盒 (plane + 7 cube + 旋转相机), 演示完整后处理链路.
--
-- 控制 (基础 SSR):
--   F : 切 SSR     1/2 : MaxSteps -/+    3/4 : StepSize -/+    5/6 : Thickness -/+
--   7/8 : Intensity -/+   -/= : MaxDistance -/+   [/] : EdgeFade -/+
-- Phase E.10 Blur:
--   B : 切 Blur    9/0 : BlurRadius -/+
-- Phase E.11 Bilateral:
--   V : 切 Bilateral    ,/. : BlurDepthSigma -/+
-- Phase E.12 Temporal:
--   T : 切 Temporal    U/I : Alpha -/+    N : 切 Rejection 0/1
-- Phase E.14/E.18 Velocity:
--   K : 切 Velocity Dilation    L : 切 Velocity Format rg16f/rg8
--   ] : 切 Dilation HalfRes    \ : 切 Dilation AutoSkip
-- Phase E.15~17 MotionBlur:
--   M : 切 MotionBlur    ; : 切 Mode    [ : 切 MB HalfRes
-- Phase F.0~14 TAA:
--   Y : 切 TAA    J : 切 Jitter    H : Sharpness 循环
--   G : 切 AntiFlicker    X : 切 HalfResHistory    Z : 切 SharpenMode
--   Q : 切 MotionAdaptive    O : 切 MotionAdaptiveSharpness    P : 切 UpscaleMode
--   C : TAA multi-instance lifecycle 演示
-- 通用:
--   R : reset    ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_ssr] Light.Graphics 不可用'); print('demo_ssr ok (no graphics)'); return end
local HDR, SSR = Gfx.HDR, Gfx.SSR
local MotionBlur = Gfx.MotionBlur
local TAA = Gfx.TAA
if type(HDR) ~= 'table' or type(SSR) ~= 'table' then
    print('[demo_ssr] HDR/SSR 子表缺失'); print('demo_ssr ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.9 SSR demo (callback-model) ====')
print('[demo_ssr] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ssr] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_ssr] SSR.IsSupported = ' .. tostring(SSR.IsSupported()))

if not UI or not UI.Window then
    print('[demo_ssr] UI.Window 不可用, 仅 API 探测')
    for _, k in ipairs({'GetMaxSteps','GetStepSize','GetThickness','GetMaxDistance',
                       'GetIntensity','GetEdgeFade','GetReflectionTexId'}) do
        print('  SSR.' .. k .. '() = ' .. tostring(SSR[k] and SSR[k]()))
    end
    print('demo_ssr ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_ssr] Light global 不可用'); print('demo_ssr ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

-- 几何 (复用 SSAO demo 模板)
local function buildCube()
    local s = 0.5; local v, idx = {}, {}
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

local SCENE = {
    {x= 0.0,y=0.6,z= 0.0, scale=1.2, color={1.0,0.4,0.4}},
    {x= 2.5,y=0.5,z= 1.5, scale=1.0, color={0.4,1.0,0.4}},
    {x=-2.5,y=0.5,z=-1.5, scale=1.0, color={0.4,0.4,1.0}},
    {x= 1.5,y=0.4,z=-2.5, scale=0.8, color={1.0,1.0,0.4}},
    {x=-1.5,y=0.4,z= 2.5, scale=0.8, color={0.4,1.0,1.0}},
    {x= 3.5,y=0.5,z=-3.0, scale=1.0, color={1.0,0.5,1.0}},
    {x=-3.5,y=0.5,z= 3.0, scale=1.0, color={1.0,0.7,0.3}},
}

local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_planeMesh = nil, nil
local g_hdrEnabled = false
local g_camAngle   = 0.0
local g_camHeight  = 3.5

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then print('[demo_ssr] Gfx.Mesh.New 不可用'); self:Close(); return end
    local cv, ci = buildCube(); local pv, pi = buildPlane()
    g_cubeMesh  = Mesh.New(cv, ci)
    g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_planeMesh then print('[demo_ssr] mesh build failed'); self:Close(); return end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_ssr] HDR.Enable = ' .. tostring(g_hdrEnabled))
    local ssrOk = g_hdrEnabled and SSR.Enable(WIN_W, WIN_H) or false
    print('[demo_ssr] SSR.Enable = ' .. tostring(ssrOk))
    if ssrOk then print('[demo_ssr] Reflection tex id = ' .. tostring(SSR.GetReflectionTexId())) end

    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 1.5)
    end
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_camAngle = g_camAngle + dt * 0.25
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
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('HDR: %s   SSR: %s   reflectTex=%d',
            g_hdrEnabled and 'ON' or 'OFF', SSR.IsEnabled() and 'ON' or 'OFF', SSR.GetReflectionTexId()))
        line(string.format('SSR: steps=%d step=%.2f thick=%.2f',
            SSR.GetMaxSteps(), SSR.GetStepSize(), SSR.GetThickness()))
        line(string.format('SSR: maxDist=%.0f intensity=%.2f edgeFade=%.2f',
            SSR.GetMaxDistance(), SSR.GetIntensity(), SSR.GetEdgeFade()))
        line(string.format('SSR Blur: %s  radius=%.2f  Bilateral=%s  sigma=%.0f',
            SSR.GetBlurEnabled() and 'ON' or 'OFF', SSR.GetBlurRadius(),
            SSR.GetBilateralEnabled() and 'ON' or 'OFF', SSR.GetBlurDepthSigma()))
        line(string.format('Temporal: %s | alpha=%.2f | reject=%d (%s)',
            SSR.GetTemporalEnabled() and 'ON' or 'OFF', SSR.GetTemporalAlpha(),
            SSR.GetRejectionMode(),
            SSR.GetRejectionMode() == 1 and 'neighborhood' or 'depth'))
        local dilateHalfRes  = HDR.GetVelocityDilationHalfRes  and HDR.GetVelocityDilationHalfRes()  or false
        local dilateAutoSkip = HDR.GetVelocityDilationAutoSkip and HDR.GetVelocityDilationAutoSkip() or false
        line(string.format('Velocity: %s | dilation=%s (halfRes=%s, autoSkip=%s)',
            HDR.GetVelocityFormat(), HDR.GetVelocityDilation() and 'ON' or 'OFF',
            dilateHalfRes and 'ON' or 'OFF', dilateAutoSkip and 'ON' or 'OFF'))
        if MotionBlur then
            local modeNames = { [0] = 'combined', [1] = 'camera_only', [2] = 'object_only' }
            local m = MotionBlur.GetMode and MotionBlur.GetMode() or 0
            local hr = MotionBlur.GetHalfRes and MotionBlur.GetHalfRes() or false
            line(string.format('MotionBlur: %s | mode=%d (%s) | halfRes=%s | strength=%.2f | samples=%d',
                MotionBlur.IsEnabled() and 'ON' or 'OFF',
                m, modeNames[m] or '?', hr and 'ON' or 'OFF',
                MotionBlur.GetStrength(), MotionBlur.GetSampleCount()))
        end
        if TAA then
            local jx, jy = TAA.GetCurrentJitter()
            line(string.format('TAA: %s | alpha=%.2f | clip=%s | sharp=%.2f/%s | frame=%d (jx=%.3f jy=%.3f)',
                TAA.IsEnabled() and 'ON' or 'OFF', TAA.GetBlendAlpha(),
                TAA.GetNeighborhoodClip() and 'ON' or 'OFF',
                TAA.GetSharpness and TAA.GetSharpness() or 0,
                TAA.GetSharpenMode and TAA.GetSharpenMode() or '?',
                TAA.GetFrameCounter(), jx, jy))
        end
        line('Keys: F=SSR B=Blur V=Bilateral T=Temporal U/I=alpha N=reject K=Dilation L=Format ' ..
             'M=MotionBlur ;=Mode Y=TAA J=Jitter H=Sharp G=AF X=HalfRes Z=Sharpen Q=MotionAdapt ' ..
             'O=MotAdaSharp P=Upscale C=instance R=reset ESC')
    end
end

local function getKeyChar(k)
    if k >= 32 and k <= 126 then return string.char(k) end
    return nil
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end

    -- 短路 ESC
    if key == 256 then self:Close(); return end

    -- SSR 基础参数
    if     key == string.byte('F') then
        if SSR.IsEnabled() then SSR.Disable(); print('[demo] SSR OFF')
        else local ok = SSR.Enable(WIN_W, WIN_H); print('[demo] SSR ' .. (ok and 'ON' or 'OFF (fail)')) end
    elseif key == string.byte('1') then SSR.SetMaxSteps(math.max(8, SSR.GetMaxSteps() - 8)); print('[demo] MaxSteps = ' .. SSR.GetMaxSteps())
    elseif key == string.byte('2') then SSR.SetMaxSteps(math.min(128, SSR.GetMaxSteps() + 8)); print('[demo] MaxSteps = ' .. SSR.GetMaxSteps())
    elseif key == string.byte('3') then SSR.SetStepSize(clampNum(SSR.GetStepSize() - 0.05, 0.01, 1.0))
    elseif key == string.byte('4') then SSR.SetStepSize(clampNum(SSR.GetStepSize() + 0.05, 0.01, 1.0))
    elseif key == string.byte('5') then SSR.SetThickness(clampNum(SSR.GetThickness() - 0.1, 0.01, 5.0))
    elseif key == string.byte('6') then SSR.SetThickness(clampNum(SSR.GetThickness() + 0.1, 0.01, 5.0))
    elseif key == string.byte('7') then SSR.SetIntensity(clampNum(SSR.GetIntensity() - 0.1, 0.0, 2.0))
    elseif key == string.byte('8') then SSR.SetIntensity(clampNum(SSR.GetIntensity() + 0.1, 0.0, 2.0))
    elseif key == 45 then SSR.SetMaxDistance(clampNum(SSR.GetMaxDistance() - 10, 1, 1000))                 -- '-'
    elseif key == 61 then SSR.SetMaxDistance(clampNum(SSR.GetMaxDistance() + 10, 1, 1000))                 -- '='
    -- Phase E.10 Blur
    elseif key == string.byte('B') then
        local v = not SSR.GetBlurEnabled(); SSR.SetBlurEnabled(v)
        print('[demo] SSR Blur ' .. (v and 'ON' or 'OFF'))
    elseif key == string.byte('9') then SSR.SetBlurRadius(clampNum(SSR.GetBlurRadius() - 0.25, 0.5, 4.0))
    elseif key == string.byte('0') then SSR.SetBlurRadius(clampNum(SSR.GetBlurRadius() + 0.25, 0.5, 4.0))
    -- Phase E.11 Bilateral
    elseif key == string.byte('V') then
        local b = not SSR.GetBilateralEnabled(); SSR.SetBilateralEnabled(b)
        print('[demo] SSR Bilateral ' .. (b and 'ON' or 'OFF'))
    elseif key == 44 then SSR.SetBlurDepthSigma(clampNum(SSR.GetBlurDepthSigma() - 25.0, 50.0, 500.0))     -- ','
    elseif key == 46 then SSR.SetBlurDepthSigma(clampNum(SSR.GetBlurDepthSigma() + 25.0, 50.0, 500.0))     -- '.'
    -- Phase E.12 Temporal
    elseif key == string.byte('T') then
        local te = not SSR.GetTemporalEnabled(); SSR.SetTemporalEnabled(te)
        print('[demo] SSR Temporal ' .. (te and 'ON' or 'OFF'))
    elseif key == string.byte('U') then SSR.SetTemporalAlpha(clampNum(SSR.GetTemporalAlpha() - 0.02, 0.5, 0.99))
    elseif key == string.byte('I') then SSR.SetTemporalAlpha(clampNum(SSR.GetTemporalAlpha() + 0.02, 0.5, 0.99))
    elseif key == string.byte('N') then
        local rm = 1 - SSR.GetRejectionMode(); SSR.SetRejectionMode(rm)
        print('[demo] RejectionMode = ' .. tostring(rm))
    -- Phase E.14 Velocity Dilation
    elseif key == string.byte('K') then
        local d = not HDR.GetVelocityDilation(); HDR.SetVelocityDilation(d)
        print('[demo] Velocity Dilation ' .. (d and 'ON' or 'OFF'))
    elseif key == string.byte('L') then
        local cur = HDR.GetVelocityFormat()
        local nxt = (cur == 'rg16f') and 'rg8' or 'rg16f'
        local ok = HDR.SetVelocityFormat(nxt)
        print('[demo] Velocity Format ' .. cur .. ' -> ' .. nxt .. ' (' .. (ok and 'ok' or 'failed') .. ')')
    -- Phase E.15 MotionBlur
    elseif key == string.byte('M') then
        if MotionBlur then
            if MotionBlur.IsEnabled() then MotionBlur.Disable(); print('[demo] MotionBlur OFF')
            else local ok = MotionBlur.Enable(WIN_W, WIN_H); print('[demo] MotionBlur ' .. (ok and 'ON' or 'OFF (fail)')) end
        end
    elseif key == 59 then                                                                              -- ';'
        if MotionBlur then
            local cur = MotionBlur.GetMode(); local nxt = (cur + 1) % 3
            MotionBlur.SetMode(nxt); print('[demo] MotionBlur Mode -> ' .. nxt)
        end
    -- Phase E.18.1/E.18.2 — ] / \
    elseif key == 93 then                                                                              -- ']'
        if HDR.SetVelocityDilationHalfRes then
            local cur = HDR.GetVelocityDilationHalfRes()
            HDR.SetVelocityDilationHalfRes(not cur)
            print('[demo] Velocity Dilation HalfRes -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == 92 then                                                                              -- '\'
        if HDR.SetVelocityDilationAutoSkip then
            local cur = HDR.GetVelocityDilationAutoSkip()
            HDR.SetVelocityDilationAutoSkip(not cur)
            print('[demo] Velocity Dilation AutoSkip -> ' .. (cur and 'OFF' or 'ON'))
        end
    -- Phase F.0 TAA
    elseif key == string.byte('Y') then
        if TAA then
            if TAA.IsEnabled() then TAA.Disable(); print('[demo] TAA OFF')
            else
                local ok = TAA.Enable(WIN_W, WIN_H)
                print('[demo] TAA Enable -> ' .. (ok and 'ON' or 'FAILED'))
            end
        end
    elseif key == string.byte('J') then
        if TAA and TAA.IsEnabled() then
            local cur = TAA.GetJitterEnabled(); TAA.SetJitterEnabled(not cur)
            print('[demo] TAA Jitter -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == string.byte('H') then
        if TAA and TAA.IsEnabled() and TAA.SetSharpness then
            local cur = TAA.GetSharpness(); local nxt = cur + 0.1
            if nxt > 2.0 + 1e-3 then nxt = 0.0 end
            TAA.SetSharpness(nxt)
            print(string.format('[demo] TAA Sharpness: %.2f -> %.2f', cur, TAA.GetSharpness()))
        end
    elseif key == string.byte('G') then
        if TAA and TAA.IsEnabled() and TAA.SetAntiFlicker then
            local cur = TAA.GetAntiFlicker(); TAA.SetAntiFlicker(not cur)
            print('[demo] TAA AntiFlicker -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == string.byte('X') then
        if TAA and TAA.IsEnabled() and TAA.SetHalfResHistory then
            local cur = TAA.GetHalfResHistory(); TAA.SetHalfResHistory(not cur)
            print('[demo] TAA HalfResHistory -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == string.byte('Z') then
        if TAA and TAA.IsEnabled() and TAA.SetSharpenMode then
            local cur = TAA.GetSharpenMode()
            local cycle = { unsharp = 'cas', cas = 'rcas', rcas = 'unsharp' }
            local nxt = cycle[cur] or 'unsharp'
            TAA.SetSharpenMode(nxt); print('[demo] TAA SharpenMode: ' .. cur .. ' -> ' .. nxt)
        end
    elseif key == string.byte('Q') then
        if TAA and TAA.IsEnabled() and TAA.SetMotionAdaptive then
            local cur = TAA.GetMotionAdaptive(); TAA.SetMotionAdaptive(not cur)
            print('[demo] TAA MotionAdaptive -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == string.byte('O') then
        if TAA and TAA.IsEnabled() and TAA.SetMotionAdaptiveSharpness then
            local cur = TAA.GetMotionAdaptiveSharpness(); TAA.SetMotionAdaptiveSharpness(not cur)
            print('[demo] TAA MotionAdaptiveSharpness -> ' .. (cur and 'OFF' or 'ON'))
        end
    elseif key == string.byte('P') then
        if TAA and TAA.IsEnabled() and TAA.SetUpscaleMode then
            local cur = TAA.GetUpscaleMode()
            local nxt = (cur == 'bilinear') and 'bicubic' or (cur == 'bicubic') and 'lanczos' or 'bilinear'
            TAA.SetUpscaleMode(nxt); print('[demo] TAA UpscaleMode: ' .. cur .. ' -> ' .. nxt)
        end
    elseif key == string.byte('C') then
        -- Phase F.0.10 multi-instance lifecycle 演示
        if TAA and TAA.CreateInstance then
            local count = TAA.GetInstanceCount(); local active = TAA.GetActiveInstance()
            if count == 1 then
                local id1, id2, id3 = TAA.CreateInstance(), TAA.CreateInstance(), TAA.CreateInstance()
                if id1 > 0 and id2 > 0 and id3 > 0 then
                    TAA.SetActiveInstance(id1); TAA.SetSharpness(0.3); TAA.SetClipMode('rgb');      TAA.SetSharpenMode('unsharp')
                    TAA.SetActiveInstance(id2); TAA.SetSharpness(1.5); TAA.SetClipMode('ycocg');    TAA.SetSharpenMode('cas')
                    TAA.SetActiveInstance(id3); TAA.SetSharpness(0.8); TAA.SetClipMode('variance'); TAA.SetSharpenMode('rcas')
                    TAA.SetActiveInstance(0)
                    print('[demo] TAA F.0.10: 已创建 3 instance, count=' .. TAA.GetInstanceCount())
                end
            elseif active < TAA.GetInstanceCount() - 1 then
                TAA.SetActiveInstance(active + 1)
                print(string.format('[demo] TAA F.0.10: active %d -> %d', active, active + 1))
            else
                for i = 3, 1, -1 do TAA.DestroyInstance(i) end
                print('[demo] TAA F.0.10: 销毁全部, count=' .. TAA.GetInstanceCount())
            end
        end
    elseif key == string.byte('R') then
        SSR.SetMaxSteps(64); SSR.SetStepSize(0.1); SSR.SetThickness(0.5)
        SSR.SetMaxDistance(50.0); SSR.SetIntensity(0.7); SSR.SetEdgeFade(0.1)
        SSR.SetBlurEnabled(false); SSR.SetBlurRadius(1.5)
        SSR.SetBilateralEnabled(true); SSR.SetBlurDepthSigma(200.0)
        SSR.SetTemporalEnabled(true); SSR.SetTemporalAlpha(0.9); SSR.SetRejectionMode(1)
        print('[demo] reset defaults')
    end
end

local function cleanup_demo()
    if TAA and TAA.IsEnabled() then TAA.Disable() end
    if MotionBlur and MotionBlur.IsEnabled() then MotionBlur.Disable() end
    if SSR.IsEnabled() then SSR.Disable() end
    if g_hdrEnabled then HDR.Disable() end
    if g_cubeMesh  then g_cubeMesh:Delete() end
    if g_planeMesh then g_planeMesh:Delete() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.9 - SSR Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_ssr ok')
