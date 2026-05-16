-- ============================================================================
-- ChocoLight Phase F.0.10.7 — True Physical Split-Screen Demo (callback-model)
-- ============================================================================
-- 双 player 后处理 + tonemap profile, 同帧 split-screen TAA + Bloom + SSR + MB + Tonemap.
--
-- Player 1 (左, "黄昏电影感"):  RCAS + 强 Bloom + 中 SSR + 强 MB + ACES exp=1.5 暖调
-- Player 2 (右, "冷夜高清"):    Lanczos + 轻 Bloom + temporal SSR + 无 MB + Uncharted2 exp=0.6 冷调
--
-- 控制:
--   R   : 重置两个 instance history
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_taa_split2] Light.Graphics 不可用'); print('demo_taa_split2 ok (no graphics)'); return end
local HDR, TAA = Gfx.HDR, Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_split2] HDR/TAA 子表缺失'); print('demo_taa_split2 ok (subtable missing)'); return
end

local function api_missing(t, name) return type(t[name]) ~= 'function' end

-- F.0.10.2 基础 API 必需
if api_missing(TAA, 'CreateInstance') or api_missing(TAA, 'SetActiveInstance')
    or api_missing(TAA, 'Process') or api_missing(HDR, 'SetAutoTAA')
    or api_missing(Gfx, 'SetViewport') then
    print('[demo_taa_split2] Phase F.0.10.2 API missing')
    print('demo_taa_split2 ok (legacy build)'); return
end

local Bloom = Gfx.Bloom; local SSR = Gfx.SSR; local MB = Gfx.MotionBlur
local hasF10_3 = (type(Bloom) == 'table') and (type(SSR) == 'table') and (type(MB) == 'table')
             and (not api_missing(Bloom, 'Process')) and (not api_missing(SSR, 'Process')) and (not api_missing(MB, 'Process'))
             and (not api_missing(HDR, 'SetAutoBloom')) and (not api_missing(HDR, 'SetAutoSSR'))
             and (not api_missing(HDR, 'SetAutoMotionBlur'))
local hasF10_6 = (not api_missing(HDR, 'SetAutoTonemap')) and (not api_missing(HDR, 'GetAutoTonemap'))
             and (not api_missing(HDR, 'Tonemap'))
local hasF10_8 = (not api_missing(HDR, 'CreateLUT3D')) and (not api_missing(HDR, 'DeleteLUT3D'))
             and (not api_missing(HDR, 'SetGradingLUT'))
local hasF10_8_1 = not api_missing(HDR, 'LoadCubeLUT')
local hasF10_8_2 = not api_missing(HDR, 'LoadHaldLUT')
local hasF10_8_3 = (not api_missing(HDR, 'WatchLUT')) and (not api_missing(HDR, 'PollLUTReloads'))

print('==== ChocoLight Phase F.0.10.7 True Physical Split-Screen Demo (callback-model) ====')
print('[demo_taa_split2] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_split2] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_split2] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))
print('[demo_taa_split2] Initial autoTAA = ' .. tostring(HDR.GetAutoTAA()))

-- ============================================================================
-- Headless API probe (CI 兼容路径)
-- ============================================================================
local function run_headless_api_probe()
    print('[demo_taa_split2] running headless API probe (no window)')

    HDR.SetAutoTAA(false)
    if HDR.GetAutoTAA() ~= false then print('  FAIL: SetAutoTAA round-trip')
    else                              print('  PASS: HDR.SetAutoTAA(false) round-trip ok') end
    HDR.SetAutoTAA(true)

    local ok, err = TAA.Process(0, 0, 100, 100)
    if ok == nil and type(err) == 'string' then
        print('  PASS: TAA.Process(region) headless returns nil + err: ' .. err)
    else
        print('  FAIL: TAA.Process expected nil + err, got ' .. tostring(ok))
    end

    local id1, id2 = TAA.CreateInstance(), TAA.CreateInstance()
    print(string.format('  CreateInstance x2 -> %s, %s', tostring(id1), tostring(id2)))
    if id1 then TAA.DestroyInstance(id1) end
    if id2 then TAA.DestroyInstance(id2) end

    if hasF10_3 then
        HDR.SetAutoBloom(false); print('  PASS: HDR.SetAutoBloom round-trip = ' .. tostring(HDR.GetAutoBloom() == false))
        HDR.SetAutoBloom(true)
        HDR.SetAutoSSR(false); print('  PASS: HDR.SetAutoSSR round-trip = ' .. tostring(HDR.GetAutoSSR() == false))
        HDR.SetAutoSSR(true)
        HDR.SetAutoMotionBlur(false); print('  PASS: HDR.SetAutoMotionBlur round-trip = ' .. tostring(HDR.GetAutoMotionBlur() == false))
        HDR.SetAutoMotionBlur(true)
        local function probe_process(name, fn)
            local r, e = fn(0, 0, 100, 100)
            if r == nil and type(e) == 'string' then
                print('  PASS: ' .. name .. '.Process(region) headless: ' .. e)
            else print('  FAIL: ' .. name .. '.Process expected nil + err') end
        end
        probe_process('Bloom', Bloom.Process)
        probe_process('SSR',   SSR.Process)
        probe_process('MB',    MB.Process)
    else print('  SKIP: F.0.10.3 API not present') end

    if hasF10_6 then
        HDR.SetAutoTonemap(false); print('  PASS: HDR.SetAutoTonemap round-trip = ' .. tostring(HDR.GetAutoTonemap() == false))
        HDR.SetAutoTonemap(true)
        local r, e = HDR.Tonemap(0, 0, 100, 100)
        if r == nil and type(e) == 'string' then print('  PASS: HDR.Tonemap(rgn) headless: ' .. e) end
    else print('  SKIP: F.0.10.6 API not present') end

    if hasF10_8 then
        local r1, e1 = HDR.CreateLUT3D(2, '\0\0\0\0\0\0\0\0')
        if r1 == nil and type(e1) == 'string' then print('  PASS: HDR.CreateLUT3D(size=2) rejected: ' .. e1) end
        HDR.SetGradingLUT(0, 0.0)
        if HDR.GetGradingLUTId() == 0 then print('  PASS: HDR.SetGradingLUT(0, 0) round-trip ok') end
    else print('  SKIP: F.0.10.8 API not present') end

    if hasF10_8_1 then
        local r1, e1 = HDR.LoadCubeLUT('definitely_not_exist.cube')
        if r1 == nil and type(e1) == 'string' and e1:find('file read failed') then
            print('  PASS: HDR.LoadCubeLUT(missing file) rejected: ' .. e1)
        end
    else print('  SKIP: F.0.10.8.1 API not present') end

    if hasF10_8_2 then
        local r1, e1 = HDR.LoadHaldLUT('definitely_not_exist.png')
        if r1 == nil and type(e1) == 'string' and e1:find('stbi_load failed') then
            print('  PASS: HDR.LoadHaldLUT(missing file) rejected: ' .. e1)
        end
    else print('  SKIP: F.0.10.8.2 API not present') end

    if hasF10_8_3 then
        if HDR.GetLUTHotReload() == true then print('  PASS: HDR.GetLUTHotReload default = true') end
        local r, e = HDR.WatchLUT('definitely_not_exist.cube')
        if r == nil and type(e) == 'string' then print('  PASS: HDR.WatchLUT(missing) rejected') end
        if HDR.PollLUTReloads() == 0 then print('  PASS: HDR.PollLUTReloads(empty) returns 0') end
    else print('  SKIP: F.0.10.8.3 API not present') end

    -- F.0.10.9 multi-instance HDR
    if HDR.CreateInstance and HDR.DestroyInstance and HDR.SetActiveInstance
       and HDR.GetActiveInstance and HDR.GetInstanceCount then
        local cnt0 = HDR.GetInstanceCount()
        local id = HDR.CreateInstance()
        if id == 1 and HDR.GetInstanceCount() == cnt0 + 1 then
            HDR.SetActiveInstance(id); local act = HDR.GetActiveInstance()
            HDR.SetActiveInstance(0); HDR.DestroyInstance(id)
            if act == id and HDR.GetInstanceCount() == cnt0 then
                print('  PASS: F.0.10.9 Multi-Instance round-trip ok')
            end
        end
    else print('  SKIP: F.0.10.9 Multi-Instance API not present') end
end

if not UI or not UI.Window then
    run_headless_api_probe()
    print('demo_taa_split2 ok (headless API check, no UI.Window)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_taa_split2] Light global 不可用')
    run_headless_api_probe()
    print('demo_taa_split2 ok (no Light global)')
    return
end

-- ============================================================================
-- 几何 + 全局常量
-- ============================================================================
local WIN_W, WIN_H = 1280, 540
local HALF_W = math.floor(WIN_W / 2)

local function buildCube(s)
    s = s or 0.5; local v, idx = {}, {}
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
    local s = 6.0
    local v = { -s,0,-s, 0,1,0, 0,0, 0.04,0.04,0.05,1,
                 s,0,-s, 0,1,0, 1,0, 0.04,0.04,0.05,1,
                 s,0, s, 0,1,0, 1,1, 0.04,0.04,0.05,1,
                -s,0, s, 0,1,0, 0,1, 0.04,0.04,0.05,1 }
    return v, { 1, 2, 3, 1, 3, 4 }
end

local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

-- per-region tonemap params
local P1_TM_PARAMS = { exposure=1.5, gamma=2.2, tonemap='aces' }
local P2_TM_PARAMS = { exposure=0.6, gamma=2.4, tonemap='uncharted2' }

-- ============================================================================
-- Demo 类
-- ============================================================================
local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_barMesh, g_planeMesh = nil, nil, nil
local g_hdrEnabled    = false
local g_bloomEnabled  = false
local g_ssrEnabled    = false
local g_mbEnabled     = false
local g_tonemapPerRegion = false
local g_p1_id, g_p2_id   = nil, nil
local g_cubeAngle, g_barAngle = 0.0, 0.0

local function setup_p1()
    TAA.SetActiveInstance(g_p1_id); TAA.Enable(WIN_W, WIN_H)
    TAA.SetClipMode('ycocg'); TAA.SetSharpness(1.2); TAA.SetSharpenMode('rcas'); TAA.SetAntiFlicker(true)
end
local function setup_p2()
    TAA.SetActiveInstance(g_p2_id); TAA.Enable(WIN_W, WIN_H)
    TAA.SetClipMode('variance'); TAA.SetVarianceGamma(1.0)
    TAA.SetSharpness(0.0); TAA.SetSharpenMode('unsharp')
    TAA.SetHalfResHistory(true); TAA.SetUpscaleMode('lanczos'); TAA.SetAntiFlicker(true)
end

local function apply_p1_postfx_profile()
    if g_bloomEnabled then
        Bloom.SetIntensity(1.5); Bloom.SetThreshold(0.8); Bloom.SetRadius(1.5)
    end
    if g_ssrEnabled then
        SSR.SetIntensity(0.6)
        if SSR.SetTemporalEnabled then SSR.SetTemporalEnabled(false) end
    end
    if g_mbEnabled then MB.SetStrength(0.8); MB.SetSampleCount(12) end
end
local function apply_p2_postfx_profile()
    if g_bloomEnabled then
        Bloom.SetIntensity(0.4); Bloom.SetThreshold(1.5); Bloom.SetRadius(0.8)
    end
    if g_ssrEnabled then
        SSR.SetIntensity(1.0)
        if SSR.SetTemporalEnabled then SSR.SetTemporalEnabled(true) end
    end
    if g_mbEnabled then MB.SetStrength(0.0) end
end

local function drawScene()
    if g_planeMesh then
        Gfx.Push(); Gfx.SetColor(0.04, 0.04, 0.05, 1.0); g_planeMesh:Draw(0); Gfx.Pop()
    end
    Gfx.Push()
    Gfx.Translate(0.0, 0.6, 0.0); Gfx.Rotate(math.deg(g_cubeAngle), 0, 1, 0)
    Gfx.Scale(1.2, 1.2, 1.2); Gfx.SetColor(1.0, 0.9, 0.7, 1.0)
    if g_cubeMesh then g_cubeMesh:Draw(0) end
    Gfx.Pop()
    local R = 2.5
    for i = 1, 8 do
        local theta = g_barAngle + (i - 1) * math.pi * 2.0 / 8.0
        local bx, bz = math.cos(theta) * R, math.sin(theta) * R
        local c = BAR_COLORS[i]
        Gfx.Push()
        Gfx.Translate(bx, 0.6, bz); Gfx.Rotate(math.deg(theta) + 90, 0, 1, 0)
        Gfx.Scale(0.04, 1.2, 0.04); Gfx.SetColor(c[1], c[2], c[3], 1.0)
        if g_barMesh then g_barMesh:Draw(0) end
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)
end

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then print('[demo_taa_split2] Mesh.New 不可用'); self:Close(); return end
    local cv, ci = buildCube(0.5); local bv, bi = buildCube(1.0); local pv, pi = buildPlane()
    g_cubeMesh = Mesh.New(cv, ci); g_barMesh = Mesh.New(bv, bi); g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_barMesh or not g_planeMesh then
        print('[demo_taa_split2] mesh build failed'); self:Close(); return
    end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_taa_split2] HDR.Enable = ' .. tostring(g_hdrEnabled))

    HDR.SetAutoTAA(false)
    print('[demo_taa_split2] HDR.SetAutoTAA(false)')

    if hasF10_3 and g_hdrEnabled then
        if Bloom.IsSupported() and Bloom.Enable(WIN_W, WIN_H) then
            g_bloomEnabled = true; HDR.SetAutoBloom(false)
            print('[demo_taa_split2] Bloom enabled, autoBloom=false')
        end
        if SSR.IsSupported() and SSR.Enable(WIN_W, WIN_H) then
            g_ssrEnabled = true; HDR.SetAutoSSR(false)
            print('[demo_taa_split2] SSR enabled, autoSSR=false')
        end
        if MB.IsSupported() and MB.Enable(WIN_W, WIN_H) then
            g_mbEnabled = true; HDR.SetAutoMotionBlur(false)
            print('[demo_taa_split2] MotionBlur enabled, autoMotionBlur=false')
        end
    end

    if hasF10_6 and g_hdrEnabled then
        HDR.SetAutoTonemap(false); g_tonemapPerRegion = true
        print('[demo_taa_split2] HDR.SetAutoTonemap(false) -- per-region tonemap')
    end

    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, HALF_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end

    g_p1_id = TAA.CreateInstance(); g_p2_id = TAA.CreateInstance()
    print(string.format('[demo_taa_split2] Created TAA instances: p1=%s, p2=%s',
        tostring(g_p1_id), tostring(g_p2_id)))
    if not g_p1_id or not g_p2_id then
        print('[demo_taa_split2] TAA.CreateInstance failed, abort'); self:Close(); return
    end
    setup_p1(); setup_p2()
    TAA.SetActiveInstance(0)
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_cubeAngle = g_cubeAngle + dt * math.rad(30)
    g_barAngle  = g_barAngle  + dt * math.rad(60)
end

function Demo:Draw()
    if not g_p1_id or not g_p2_id then return end

    -- ===== Player 1 (左半屏) =====
    Gfx.SetViewport(0, 0, HALF_W, WIN_H)
    TAA.SetActiveInstance(g_p1_id); TAA.ApplyJitter()
    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(2.5, 3.0, 5.5, 0.0, 0.6, 0.0) end
    drawScene()
    if g_bloomEnabled or g_ssrEnabled or g_mbEnabled then
        apply_p1_postfx_profile()
        if g_bloomEnabled then Bloom.Process(0, 0, HALF_W, WIN_H) end
        if g_ssrEnabled   then SSR.Process(0, 0, HALF_W, WIN_H)   end
        if g_mbEnabled    then MB.Process(0, 0, HALF_W, WIN_H)    end
    end
    TAA.Process(0, 0, HALF_W, WIN_H)

    -- ===== Player 2 (右半屏) =====
    Gfx.SetViewport(HALF_W, 0, HALF_W, WIN_H)
    TAA.SetActiveInstance(g_p2_id); TAA.ApplyJitter()
    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(-3.5, 1.0, 3.0, 0.0, 0.8, 0.0) end
    drawScene()
    if g_bloomEnabled or g_ssrEnabled or g_mbEnabled then
        apply_p2_postfx_profile()
        if g_bloomEnabled then Bloom.Process(HALF_W, 0, HALF_W, WIN_H) end
        if g_ssrEnabled   then SSR.Process(HALF_W, 0, HALF_W, WIN_H)   end
        if g_mbEnabled    then MB.Process(HALF_W, 0, HALF_W, WIN_H)    end
    end
    TAA.Process(HALF_W, 0, HALF_W, WIN_H)

    Gfx.SetViewport(0, 0, WIN_W, WIN_H)
    TAA.SetActiveInstance(0)

    -- per-region tonemap
    if g_tonemapPerRegion then
        HDR.Tonemap(0,      0, HALF_W, WIN_H, P1_TM_PARAMS)
        HDR.Tonemap(HALF_W, 0, HALF_W, WIN_H, P2_TM_PARAMS)
    end

    if Gfx.Print then
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line('===== Phase F.0.10.7 True Physical Split-Screen Demo =====')
        line('  (TAA + Bloom + SSR + MotionBlur + Tonemap all per-region)')
        line(string.format('Window: %dx%d | Half: %dx%d', WIN_W, WIN_H, HALF_W, WIN_H))
        line(string.format('  TAA      : auto=%s, instances=%d', tostring(HDR.GetAutoTAA()), TAA.GetInstanceCount()))
        if hasF10_3 then
            line(string.format('  Bloom    : enabled=%s, autoBloom=%s', tostring(g_bloomEnabled), tostring(HDR.GetAutoBloom())))
            line(string.format('  SSR      : enabled=%s, autoSSR=%s', tostring(g_ssrEnabled), tostring(HDR.GetAutoSSR())))
            line(string.format('  Motion B.: enabled=%s, autoMB=%s', tostring(g_mbEnabled), tostring(HDR.GetAutoMotionBlur())))
        end
        if hasF10_6 then
            line(string.format('  Tonemap  : per-region=%s, autoTonemap=%s',
                tostring(g_tonemapPerRegion), tostring(HDR.GetAutoTonemap())))
        end
        line('')
        line(string.format('P1 (LEFT,  id=%s): RCAS + STRONG bloom + mid SSR + STRONG MB + ACES exp=1.5 黄昏', tostring(g_p1_id)))
        line(string.format('P2 (RIGHT, id=%s): Lanczos + light bloom + temporal SSR + NO MB + Uncharted2 exp=0.6 冷夜', tostring(g_p2_id)))
        line('Keys: R = reset both history | ESC = quit')
        Gfx.SetColor(0.5, 0.7, 1.0, 1.0)
        Gfx.Print('[P2: cool-dark hi-quality]', HALF_W + 8, 8, 0)
        Gfx.SetColor(1, 1, 1, 1)
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key == string.byte('R') then
        TAA.SetActiveInstance(g_p1_id); if TAA.IsEnabled() then TAA.Disable() end; setup_p1()
        TAA.SetActiveInstance(g_p2_id); if TAA.IsEnabled() then TAA.Disable() end; setup_p2()
        TAA.SetActiveInstance(0)
        print('[demo_taa_split2] Reset both instance history (R pressed)')
    end
end

local function cleanup_demo()
    TAA.SetActiveInstance(0)
    if g_p1_id then TAA.DestroyInstance(g_p1_id) end
    if g_p2_id then TAA.DestroyInstance(g_p2_id) end
    -- 复位 auto 开关
    HDR.SetAutoTAA(true)
    if hasF10_3 then HDR.SetAutoBloom(true); HDR.SetAutoSSR(true); HDR.SetAutoMotionBlur(true) end
    if hasF10_6 then HDR.SetAutoTonemap(true) end
    -- 反向 Disable
    if TAA.IsEnabled()  then TAA.Disable()  end
    if g_mbEnabled      then MB.Disable()    end
    if g_ssrEnabled     then SSR.Disable()   end
    if g_bloomEnabled   then Bloom.Disable() end
    if g_hdrEnabled     then HDR.Disable()   end
    if g_cubeMesh   then g_cubeMesh:Delete()  end
    if g_barMesh    then g_barMesh:Delete()   end
    if g_planeMesh  then g_planeMesh:Delete() end
end

Demo:Open(WIN_W, WIN_H, 'Phase F.0.10.7 - True Physical Split-Screen (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_taa_split2 ok')
