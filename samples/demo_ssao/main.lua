-- ============================================================================
-- ChocoLight Phase E.8 — SSAO Demo (callback-model)
-- ============================================================================
-- 3D PBR 场景 (cube + plane) + 旋转相机 + SSAO toggle.
--
-- 控制:
--   F : 切换 SSAO on/off
--   1/2 : Radius -/+ ([0.05, 5.0])
--   3/4 : Bias -/+ ([0, 0.2])
--   5/6 : Intensity -/+ ([0, 4.0])
--   7/8 : Power -/+ ([0.5, 8.0])
--   B : 切换 BlurEnabled
--   K : 切换 KernelSize (8/16)
--   R : reset
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_ssao] Light.Graphics 不可用'); print('demo_ssao ok (no graphics)'); return end
local HDR, SSAO = Gfx.HDR, Gfx.SSAO
if type(HDR) ~= 'table' or type(SSAO) ~= 'table' then
    print('[demo_ssao] HDR/SSAO 子表缺失'); print('demo_ssao ok (subtable missing)'); return
end

print('==== ChocoLight Phase E.8 SSAO demo (callback-model) ====')
print('[demo_ssao] Backend          = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ssao] HDR.IsSupported  = ' .. tostring(HDR.IsSupported()))
print('[demo_ssao] SSAO.IsSupported = ' .. tostring(SSAO.IsSupported()))

if not UI or not UI.Window then
    print('[demo_ssao] UI.Window 不可用, 仅 API 探测')
    for _, k in ipairs({'GetRadius','GetBias','GetIntensity','GetKernelSize','GetPower','GetBlurEnabled'}) do
        print('  SSAO.' .. k .. '() = ' .. tostring(SSAO[k] and SSAO[k]()))
    end
    print('demo_ssao ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_ssao] Light global 不可用'); print('demo_ssao ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540
local function clampNum(v, lo, hi) if v < lo then return lo end; if hi and v > hi then return hi end; return v end

-- 几何
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
    local v = { -s,0,-s, 0,1,0, 0,0, 0.7,0.7,0.7,1,
                 s,0,-s, 0,1,0, 1,0, 0.7,0.7,0.7,1,
                 s,0, s, 0,1,0, 1,1, 0.7,0.7,0.7,1,
                -s,0, s, 0,1,0, 0,1, 0.7,0.7,0.7,1 }
    return v, { 1, 2, 3, 1, 3, 4 }
end

local SCENE = {
    {x= 0.0,y=0.5,z= 0.0, scale=1.0, color={0.8,0.3,0.3}},
    {x= 2.0,y=0.5,z= 1.5, scale=1.0, color={0.3,0.8,0.3}},
    {x=-2.0,y=0.5,z=-1.5, scale=1.0, color={0.3,0.3,0.8}},
    {x= 1.5,y=0.5,z=-2.0, scale=0.7, color={0.8,0.8,0.3}},
    {x=-1.5,y=0.5,z= 2.0, scale=0.7, color={0.3,0.8,0.8}},
    {x= 0.0,y=1.5,z= 0.0, scale=0.6, color={0.9,0.5,0.3}},
    {x= 3.0,y=0.5,z=-2.5, scale=1.2, color={0.5,0.5,0.9}},
    {x=-3.0,y=0.5,z= 2.5, scale=0.8, color={0.9,0.3,0.9}},
}

local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_planeMesh = nil, nil
local g_hdrEnabled = false
local g_camAngle = 0.0

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_ssao] Gfx.Mesh.New 不可用'); self:Close(); return
    end
    local cv, ci = buildCube(); local pv, pi = buildPlane()
    g_cubeMesh  = Mesh.New(cv, ci)
    g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_planeMesh then
        print('[demo_ssao] mesh build failed'); self:Close(); return
    end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_ssao] HDR.Enable  = ' .. tostring(g_hdrEnabled))
    local ok = g_hdrEnabled and SSAO.Enable(WIN_W, WIN_H) or false
    print('[demo_ssao] SSAO.Enable = ' .. tostring(ok))

    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 1.2)
    end
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_camAngle = g_camAngle + dt * 0.3
end

function Demo:Draw()
    -- 相机围场转
    local cx, cz = math.cos(g_camAngle) * 7.0, math.sin(g_camAngle) * 7.0
    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(cx, 4.0, cz, 0.0, 0.5, 0.0) end

    if g_planeMesh then
        Gfx.Push(); Gfx.SetColor(0.7, 0.7, 0.7, 1.0); g_planeMesh:Draw(0); Gfx.Pop()
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
        line(string.format('HDR: %s   SSAO: %s', g_hdrEnabled and 'ON' or 'OFF', SSAO.IsEnabled() and 'ON' or 'OFF'))
        line(string.format('SSAO: radius=%.2f bias=%.3f intensity=%.2f',
            SSAO.GetRadius(), SSAO.GetBias(), SSAO.GetIntensity()))
        line(string.format('SSAO: power=%.2f kernel=%d blur=%s',
            SSAO.GetPower(), SSAO.GetKernelSize(),
            SSAO.GetBlurEnabled() and 'ON' or 'OFF'))
        line('Keys: F=SSAO 1/2=radius 3/4=bias 5/6=int 7/8=power B=blur K=kernel R=reset ESC')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key == string.byte('F') then
        if SSAO.IsEnabled() then SSAO.Disable(); print('[demo] SSAO OFF')
        else local ok = SSAO.Enable(WIN_W, WIN_H); print('[demo] SSAO ' .. (ok and 'ON' or 'OFF')) end
    elseif key == string.byte('1') then SSAO.SetRadius(clampNum(SSAO.GetRadius() - 0.1, 0.05, 5.0))
    elseif key == string.byte('2') then SSAO.SetRadius(clampNum(SSAO.GetRadius() + 0.1, 0.05, 5.0))
    elseif key == string.byte('3') then SSAO.SetBias(clampNum(SSAO.GetBias() - 0.005, 0.0, 0.2))
    elseif key == string.byte('4') then SSAO.SetBias(clampNum(SSAO.GetBias() + 0.005, 0.0, 0.2))
    elseif key == string.byte('5') then SSAO.SetIntensity(clampNum(SSAO.GetIntensity() - 0.1, 0.0, 4.0))
    elseif key == string.byte('6') then SSAO.SetIntensity(clampNum(SSAO.GetIntensity() + 0.1, 0.0, 4.0))
    elseif key == string.byte('7') then SSAO.SetPower(clampNum(SSAO.GetPower() - 0.25, 0.5, 8.0))
    elseif key == string.byte('8') then SSAO.SetPower(clampNum(SSAO.GetPower() + 0.25, 0.5, 8.0))
    elseif key == string.byte('B') then
        SSAO.SetBlurEnabled(not SSAO.GetBlurEnabled())
        print('[demo] SSAO blur = ' .. tostring(SSAO.GetBlurEnabled()))
    elseif key == string.byte('K') then
        local k = SSAO.GetKernelSize()
        SSAO.SetKernelSize(k == 16 and 8 or 16)
        print('[demo] SSAO kernel = ' .. tostring(SSAO.GetKernelSize()))
    elseif key == string.byte('R') then
        SSAO.SetRadius(0.5); SSAO.SetBias(0.025); SSAO.SetIntensity(1.0)
        SSAO.SetKernelSize(16); SSAO.SetPower(2.0); SSAO.SetBlurEnabled(true)
        print('[demo] reset defaults')
    end
end

local function cleanup_demo()
    if SSAO.IsEnabled() then SSAO.Disable() end
    if g_hdrEnabled then HDR.Disable() end
    if g_cubeMesh  then g_cubeMesh:Delete() end
    if g_planeMesh then g_planeMesh:Delete() end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.8 - SSAO Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_ssao ok')
