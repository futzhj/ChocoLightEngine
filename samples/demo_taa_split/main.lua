-- ============================================================================
-- ChocoLight Phase F.0.10.1 — TAA Multi-Instance Split Demo (callback-model)
-- ============================================================================
-- 演示 4 个独立 TAA instance 各自持有 history RT, 切 active 不污染 history.
--
-- 控制:
--   0/1/2/3 : 切到 active instance
--   R       : Disable + 重新 apply profile (重置 history)
--   C       : 销毁所有 user instance + 重建
--   ESC     : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_taa_split] Light.Graphics 不可用'); print('demo_taa_split ok (no graphics)'); return end
local HDR, TAA = Gfx.HDR, Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_split] HDR/TAA 子表缺失'); print('demo_taa_split ok (subtable missing)'); return
end

if type(TAA.CreateInstance) ~= 'function' or type(TAA.SetActiveInstance) ~= 'function'
    or type(TAA.GetActiveInstance) ~= 'function' or type(TAA.DestroyInstance) ~= 'function'
    or type(TAA.GetInstanceCount) ~= 'function' then
    print('[demo_taa_split] Phase F.0.10 multi-instance API missing')
    print('demo_taa_split ok (legacy build)'); return
end

print('==== ChocoLight Phase F.0.10.1 TAA Multi-Instance Split Demo (callback-model) ====')
print('[demo_taa_split] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_split] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_split] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))
print('[demo_taa_split] Instance count  = ' .. tostring(TAA.GetInstanceCount()))

if not UI or not UI.Window then
    print('[demo_taa_split] UI.Window 不可用, 仅 API 探测')
    local id1 = TAA.CreateInstance(); local id2 = TAA.CreateInstance(); local id3 = TAA.CreateInstance()
    print(string.format('  CreateInstance x3 -> %s, %s, %s', tostring(id1), tostring(id2), tostring(id3)))
    print('  GetInstanceCount = ' .. tostring(TAA.GetInstanceCount()))
    if id1 then TAA.DestroyInstance(id1) end
    if id2 then TAA.DestroyInstance(id2) end
    if id3 then TAA.DestroyInstance(id3) end
    print('  After cleanup count = ' .. tostring(TAA.GetInstanceCount()))
    print('demo_taa_split ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_taa_split] Light global 不可用'); print('demo_taa_split ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540

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

-- 4 个 instance profile
local PROFILES = {
    [0] = { name='Default (F.0 baseline)', desc='ycocg AABB clip + sharpness=0.5',
        clipMode='ycocg', sharpness=0.5, antiFlicker=true, sharpenMode='unsharp',
        halfResHistory=false, upscaleMode='bilinear', motionAdaptive=false, motionAdaptiveSharpness=false },
    [1] = { name='Instance 1 - Lanczos Upscale (F.0.14)', desc='sharp=0 + halfRes + lanczos 25-tap',
        clipMode='variance', sharpness=0.0, antiFlicker=true, sharpenMode='unsharp',
        halfResHistory=true, upscaleMode='lanczos', motionAdaptive=false, motionAdaptiveSharpness=false },
    [2] = { name='Instance 2 - RCAS Strong Sharpen (F.0.12)', desc='sharp=1.5 + RCAS',
        clipMode='ycocg', sharpness=1.5, antiFlicker=true, sharpenMode='rcas',
        halfResHistory=false, upscaleMode='bilinear', motionAdaptive=false, motionAdaptiveSharpness=false },
    [3] = { name='Instance 3 - Motion Adaptive All (F.0.8/0.13)', desc='variance + motionAdaptive',
        clipMode='variance', sharpness=0.8, antiFlicker=true, sharpenMode='rcas',
        halfResHistory=false, upscaleMode='bilinear', motionAdaptive=true, motionAdaptiveSharpness=true },
}

local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_barMesh, g_planeMesh = nil, nil, nil
local g_hdrEnabled = false
local g_cubeAngle, g_barAngle = 0.0, 0.0
local g_user_ids = {}

local function apply_instance_params(idx)
    local p = PROFILES[idx]; if not p then return false end
    if not TAA.IsEnabled() then
        local ok = TAA.Enable(WIN_W, WIN_H); if not ok then return false end
    end
    TAA.SetClipMode(p.clipMode); TAA.SetSharpness(p.sharpness)
    if TAA.SetAntiFlicker     then TAA.SetAntiFlicker(p.antiFlicker)     end
    if TAA.SetSharpenMode     then TAA.SetSharpenMode(p.sharpenMode)     end
    if TAA.SetHalfResHistory  then TAA.SetHalfResHistory(p.halfResHistory) end
    if TAA.SetUpscaleMode     then TAA.SetUpscaleMode(p.upscaleMode)     end
    if TAA.SetMotionAdaptive  then TAA.SetMotionAdaptive(p.motionAdaptive) end
    if TAA.SetMotionAdaptiveSharpness then TAA.SetMotionAdaptiveSharpness(p.motionAdaptiveSharpness) end
    return true
end

local function create_user_instances()
    g_user_ids = {}
    for i = 1, 3 do
        local id = TAA.CreateInstance()
        if id and id > 0 then
            g_user_ids[i] = id; TAA.SetActiveInstance(id); apply_instance_params(i)
        else
            print(string.format('[demo_taa_split] CreateInstance #%d 失败 (count=%d)', i, TAA.GetInstanceCount()))
        end
    end
    TAA.SetActiveInstance(0); apply_instance_params(0)
    print(string.format('[demo_taa_split] Created %d user instances, count=%d, active=0',
        #g_user_ids, TAA.GetInstanceCount()))
end

local function destroy_user_instances()
    for i = #g_user_ids, 1, -1 do
        if g_user_ids[i] then TAA.DestroyInstance(g_user_ids[i]) end
    end
    g_user_ids = {}; TAA.SetActiveInstance(0)
end

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then print('[demo_taa_split] Mesh.New 不可用'); self:Close(); return end
    local cv, ci = buildCube(0.5); local bv, bi = buildCube(1.0); local pv, pi = buildPlane()
    g_cubeMesh = Mesh.New(cv, ci); g_barMesh = Mesh.New(bv, bi); g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_barMesh or not g_planeMesh then
        print('[demo_taa_split] mesh build failed'); self:Close(); return
    end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_taa_split] HDR.Enable = ' .. tostring(g_hdrEnabled))
    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end
    create_user_instances()
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_cubeAngle = g_cubeAngle + dt * math.rad(30)
    g_barAngle  = g_barAngle  + dt * math.rad(60)
end

function Demo:Draw()
    if type(Gfx.SetCamera) == 'function' then Gfx.SetCamera(0.0, 2.2, 5.0, 0.0, 0.6, 0.0) end

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

    if Gfx.Print then
        local active = TAA.GetActiveInstance()
        local p = PROFILES[active]
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('[Instance %d/%d] %s', active, TAA.GetInstanceCount() - 1, p and p.name or '?'))
        if p then line('Profile: ' .. p.desc) end
        if TAA.IsEnabled() then
            local cmode = TAA.GetClipMode and TAA.GetClipMode() or 'rgb'
            local sharp = TAA.GetSharpness and TAA.GetSharpness() or 0
            local sm    = TAA.GetSharpenMode and TAA.GetSharpenMode() or 'unsharp'
            local hr    = TAA.GetHalfResHistory and TAA.GetHalfResHistory() or false
            local um    = TAA.GetUpscaleMode and TAA.GetUpscaleMode() or 'bilinear'
            local ma    = TAA.GetMotionAdaptive and TAA.GetMotionAdaptive() or false
            local mas   = TAA.GetMotionAdaptiveSharpness and TAA.GetMotionAdaptiveSharpness() or false
            line(string.format('TAA: clip=%s | sharp=%.2f(%s) | halfRes=%s | upscale=%s',
                cmode, sharp, sm, hr and 'ON' or 'OFF', um))
            line(string.format('Motion-Adaptive: gamma=%s | sharpness=%s | jitter=%s',
                ma and 'ON' or 'OFF', mas and 'ON' or 'OFF', TAA.GetJitterEnabled() and 'ON' or 'OFF'))
        else line('TAA: OFF') end
        line(string.format('Instances: count=%d | user_ids=[%s, %s, %s]',
            TAA.GetInstanceCount(),
            tostring(g_user_ids[1] or 'X'), tostring(g_user_ids[2] or 'X'), tostring(g_user_ids[3] or 'X')))
        line('Keys: 0=default | 1/2/3=user instances | R=reset history | C=destroy+recreate | ESC')
        line('Tip: 切 active 不会重置 history (与 demo_taa_compare 切 preset 重置不同) -- F.0.10 核心价值!')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key >= string.byte('0') and key <= string.byte('3') then
        local target = key - string.byte('0')
        if target > 0 and not g_user_ids[target] then
            print(string.format('[demo_taa_split] Instance %d 未创建 (按 C 重建)', target))
        else
            local ok = TAA.SetActiveInstance(target)
            if ok == true then
                local p = PROFILES[target]
                print(string.format('[demo_taa_split] active -> %d (%s)', target, p and p.name or '?'))
            else
                print(string.format('[demo_taa_split] SetActiveInstance(%d) 失败', target))
            end
        end
    elseif key == string.byte('R') then
        local active = TAA.GetActiveInstance()
        if TAA.IsEnabled() then TAA.Disable() end
        apply_instance_params(active)
        print(string.format('[demo_taa_split] Reset instance %d history (re-stabilize)', active))
    elseif key == string.byte('C') then
        destroy_user_instances()
        print(string.format('[demo_taa_split] Destroyed all user instances, count=%d', TAA.GetInstanceCount()))
        create_user_instances()
    end
end

local function cleanup_demo()
    destroy_user_instances()
    if TAA.IsEnabled() then TAA.Disable() end
    if g_hdrEnabled then HDR.Disable() end
    if g_cubeMesh  then g_cubeMesh:Delete()  end
    if g_barMesh   then g_barMesh:Delete()   end
    if g_planeMesh then g_planeMesh:Delete() end
end

Demo:Open(WIN_W, WIN_H, 'Phase F.0.10.1 - TAA Multi-Instance Split Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_taa_split ok')
