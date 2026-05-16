-- ============================================================================
-- ChocoLight Phase F.0.7 — TAA Compare Demo (8 preset 切换 / A/B 对比)  callback-model
-- ============================================================================
-- 一键切换 8 个 TAA 预设, 直观对比 6 个 Phase 的画质差异.
--   1: TAA OFF (baseline)
--   2: F.0   base       — jitter + reproject + RGB clip + alpha blend
--   3: F.0.1 sharpening — F.0 + 4-tap unsharp mask sharpness=0.5
--   4: F.0.2 YCoCg clip — YCoCg 9-tap clip
--   5: F.0.3 variance   — clip = mean ± γσ (Salvi 2016)
--   6: F.0.4 anti-flick — Karis luma weighted blend
--   7: F.0.5 half-res   — history RT (W/2, H/2)
--   8: ALL (推荐)        — 完整管线
--
-- 控制:
--   1-8 : 切换 8 个 preset
--   R   : 重置 history (Disable + Enable, 重新 stabilize)
--   ESC : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_taa_compare] Light.Graphics 不可用'); print('demo_taa_compare ok (no graphics)'); return end
local HDR, TAA = Gfx.HDR, Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_compare] HDR/TAA 子表缺失'); print('demo_taa_compare ok (subtable missing)'); return
end

print('==== ChocoLight Phase F.0.7 TAA Compare Demo (callback-model) ====')
print('[demo_taa_compare] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_compare] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_compare] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))

if not UI or not UI.Window then
    print('[demo_taa_compare] UI.Window 不可用, 仅 API 探测')
    print('  TAA.GetBlendAlpha     = ' .. tostring(TAA.GetBlendAlpha()))
    print('  TAA.GetClipMode       = ' .. tostring(TAA.GetClipMode and TAA.GetClipMode() or 'n/a'))
    print('  TAA.GetSharpness      = ' .. tostring(TAA.GetSharpness and TAA.GetSharpness() or 'n/a'))
    print('  TAA.GetAntiFlicker    = ' .. tostring(TAA.GetAntiFlicker and TAA.GetAntiFlicker() or 'n/a'))
    print('  TAA.GetVarianceGamma  = ' .. tostring(TAA.GetVarianceGamma and TAA.GetVarianceGamma() or 'n/a'))
    print('  TAA.GetHalfResHistory = ' .. tostring(TAA.GetHalfResHistory and TAA.GetHalfResHistory() or 'n/a'))
    print('demo_taa_compare ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_taa_compare] Light global 不可用'); print('demo_taa_compare ok (no Light global)'); return
end

local WIN_W, WIN_H = 960, 540

-- 几何
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

-- 8 preset 表
local PRESETS = {
    [1] = { name='TAA OFF (baseline)', desc='no TAA -- raw aliasing / firefly', enabled=false },
    [2] = { name='F.0 base', desc='jitter + reproject + RGB AABB clip + alpha blend',
            enabled=true, clipMode='rgb', sharpness=0, antiFlicker=false, halfResHistory=false },
    [3] = { name='F.0.1 sharpening', desc='F.0 + 4-tap unsharp mask sharpness=0.5',
            enabled=true, clipMode='rgb', sharpness=0.5, antiFlicker=false, halfResHistory=false },
    [4] = { name='F.0.2 YCoCg clip', desc='YCoCg AABB clip',
            enabled=true, clipMode='ycocg', sharpness=0.5, antiFlicker=false, halfResHistory=false },
    [5] = { name='F.0.3 variance clip', desc='mean +/- gamma*sigma (Salvi 2016 / UE5)',
            enabled=true, clipMode='variance', sharpness=0.5, antiFlicker=false,
            varianceGamma=1.0, halfResHistory=false },
    [6] = { name='F.0.4 anti-flicker', desc='Karis luma-weighted blend',
            enabled=true, clipMode='variance', sharpness=0.5, antiFlicker=true,
            varianceGamma=1.0, halfResHistory=false },
    [7] = { name='F.0.5 half-res history', desc='history RT (W/2, H/2) -- VRAM -75%',
            enabled=true, clipMode='variance', sharpness=0.5, antiFlicker=true,
            varianceGamma=1.0, halfResHistory=true },
    [8] = { name='ALL (推荐)', desc='full pipeline (variance + AF + sharp 0.8 + halfRes)',
            enabled=true, clipMode='variance', sharpness=0.8, antiFlicker=true,
            varianceGamma=1.0, halfResHistory=true },
}

local STABILIZE_FRAMES = 30
local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

local Demo = Light(Light.UI.Window):New()
local g_cubeMesh, g_barMesh, g_planeMesh = nil, nil, nil
local g_hdrEnabled = false
local g_currentPreset    = 1
local g_stabilizeCounter = 0
local g_cubeAngle = 0.0
local g_barAngle  = 0.0

local function apply_preset(idx)
    local p = PRESETS[idx]; if not p then return false end

    if not p.enabled then
        if TAA.IsEnabled() then TAA.Disable() end
        g_currentPreset = idx; g_stabilizeCounter = 0
        print(string.format('[demo] Preset %d: %s', idx, p.name))
        return true
    end

    if not TAA.IsEnabled() then
        local ok = TAA.Enable(WIN_W, WIN_H)
        if not ok then print('[demo] TAA.Enable failed, preset abort'); return false end
    end
    TAA.SetClipMode(p.clipMode); TAA.SetSharpness(p.sharpness)
    if TAA.SetAntiFlicker then TAA.SetAntiFlicker(p.antiFlicker) end
    if p.varianceGamma and TAA.SetVarianceGamma then TAA.SetVarianceGamma(p.varianceGamma) end
    if TAA.SetHalfResHistory then TAA.SetHalfResHistory(p.halfResHistory) end

    g_currentPreset = idx; g_stabilizeCounter = 0
    print(string.format('[demo] Preset %d: %s -- %s', idx, p.name, p.desc))
    return true
end

function Demo:OnOpen()
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then print('[demo_taa_compare] Mesh.New 不可用'); self:Close(); return end
    local cv, ci = buildCube(0.5)
    local bv, bi = buildCube(1.0)
    local pv, pi = buildPlane()
    g_cubeMesh  = Mesh.New(cv, ci)
    g_barMesh   = Mesh.New(bv, bi)
    g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_barMesh or not g_planeMesh then
        print('[demo_taa_compare] mesh build failed'); self:Close(); return
    end

    g_hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
    print('[demo_taa_compare] HDR.Enable = ' .. tostring(g_hdrEnabled))
    if type(Gfx.SetPerspective) == 'function' then Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0) end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end
    apply_preset(1)
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_cubeAngle = g_cubeAngle + dt * math.rad(30)
    g_barAngle  = g_barAngle  + dt * math.rad(60)
    if g_stabilizeCounter < STABILIZE_FRAMES then g_stabilizeCounter = g_stabilizeCounter + 1 end
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
        local p = PRESETS[g_currentPreset]
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('[Preset %d/8] %s', g_currentPreset, p.name))
        line('Algorithm: ' .. p.desc)
        if p.enabled and TAA.IsEnabled() then
            local cmode = TAA.GetClipMode and TAA.GetClipMode() or 'rgb'
            local cmodeStr = cmode
            if cmode == 'variance' and TAA.GetVarianceGamma then
                cmodeStr = string.format('variance(g=%.2f)', TAA.GetVarianceGamma())
            end
            line(string.format('TAA: ON | alpha=%.2f | clip=%s/%s | jitter=%s',
                TAA.GetBlendAlpha(),
                TAA.GetNeighborhoodClip() and 'ON' or 'OFF',
                cmodeStr,
                TAA.GetJitterEnabled() and 'ON' or 'OFF'))
            local sharp = TAA.GetSharpness and TAA.GetSharpness() or 0
            local af    = TAA.GetAntiFlicker and TAA.GetAntiFlicker() or false
            local hr    = TAA.GetHalfResHistory and TAA.GetHalfResHistory() or false
            line(string.format('Sharpness: %.2f (%s) | AntiFlicker: %s | HalfRes: %s',
                sharp, sharp > 0 and 'sharpen pass' or 'pure blit',
                af and 'ON' or 'OFF',
                hr and 'ON (VRAM -75%)' or 'OFF (full-res)'))
        else
            line('TAA: OFF -- baseline (no jitter / no reproject / no clip)')
            line('Expect: visible aliasing + 1px shimmer + HDR firefly noise')
        end
        local fc = TAA.GetFrameCounter and TAA.GetFrameCounter() or 0
        local stabStr
        if g_stabilizeCounter < STABILIZE_FRAMES and p.enabled then
            local bar = string.rep('=', g_stabilizeCounter) .. string.rep('-', STABILIZE_FRAMES - g_stabilizeCounter)
            stabStr = string.format('History: stabilizing [%s] %d/%d frames', bar, g_stabilizeCounter, STABILIZE_FRAMES)
        elseif p.enabled then
            stabStr = string.format('History: STABLE (>=%d frames after preset switch)', STABILIZE_FRAMES)
        else stabStr = 'History: N/A (TAA OFF)' end
        line(string.format('%s | TAA frame=%d', stabStr, fc))
        line('Keys: 1-8=preset | R=reset history | ESC=exit')
        line('Tip: 切 preset 后等 history STABLE 再观察画质 (~30 frames)')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key >= string.byte('1') and key <= string.byte('8') then
        apply_preset(key - string.byte('0'))
    elseif key == string.byte('R') then
        if TAA.IsEnabled() then TAA.Disable() end
        apply_preset(g_currentPreset)
        print('[demo] Reset history (re-stabilizing 30 frames)')
    end
end

local function cleanup_demo()
    if TAA.IsEnabled() then TAA.Disable() end
    if g_hdrEnabled then HDR.Disable() end
    if g_cubeMesh  then g_cubeMesh:Delete()  end
    if g_barMesh   then g_barMesh:Delete()   end
    if g_planeMesh then g_planeMesh:Delete() end
end

Demo:Open(WIN_W, WIN_H, 'Phase F.0.7 - TAA Compare Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_taa_compare ok')
