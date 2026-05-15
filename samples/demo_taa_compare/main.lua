-- ============================================================================
-- ChocoLight Phase F.0.7 — TAA Compare Demo (8 preset 切换 / A/B 对比)
-- ============================================================================
-- 一键切换 8 个 TAA 预设配置 (从 OFF → 完整管线), 直观对比 6 个 Phase 的画质差异.
--   * Preset 1: TAA OFF (baseline, 看原始 aliasing / firefly)
--   * Preset 2: F.0   base       — jitter + reproject + RGB AABB clip + alpha blend
--   * Preset 3: F.0.1 sharpening — F.0 + 4-tap unsharp mask sharpness=0.5
--   * Preset 4: F.0.2 YCoCg clip — YCoCg 空间 9-tap clip (色彩边缘鲁棒)
--   * Preset 5: F.0.3 variance   — clip = mean ± γσ (Salvi 2016 / UE5)
--   * Preset 6: F.0.4 anti-flick — Karis luma weighted blend (压制 firefly)
--   * Preset 7: F.0.5 half-res   — history RT (W/2, H/2), VRAM -75%
--   * Preset 8: ALL (推荐)        — 完整管线 (移动 4K 推荐配置)
--
-- 渐进式叠加: 每个 preset 在前一个基础上加一个特性,
-- 让用户清晰看到每个 Phase 的边际贡献.
--
-- 场景: 中央旋转高对比 cube + 8 根围绕薄棒 + 黑色地面.
--   - 中央 cube: HDR 高光 + 自转 30 deg/s → ghosting + firefly 触发
--   - 薄棒: 1px 厚度, 围绕中心旋转 60 deg/s → aliasing + trail 触发
--
-- 控制:
--   1-8     : 切换 8 个 preset
--   R       : 重置 history (Disable + Enable, 适合 reset 后开始 stabilize)
--   ESC     : 退出
--
-- 注意: TAA 是连续帧累积; 切 preset 后需等 ~30 帧 history 稳定才能公平对比.
--       HUD 显示 "stabilizing N/30" 进度条.
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
    print('[demo_taa_compare] Light.Graphics not available')
    print('demo_taa_compare ok (no graphics)')
    return
end

local HDR = Gfx.HDR
local TAA = Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_compare] need HDR + TAA subtables (Phase E.3 + Phase F.0)')
    print('demo_taa_compare ok (subtable missing)')
    return
end

print('==== ChocoLight Phase F.0.7 TAA Compare Demo ====')
print('[demo_taa_compare] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_compare] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_compare] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))

-- Headless 模式: 输出 API 状态后退出 (CI smoke 模式)
if not UI or not UI.Window then
    print('[demo_taa_compare] UI.Window not available, API probe only')
    print('  TAA.GetBlendAlpha       = ' .. tostring(TAA.GetBlendAlpha()))
    print('  TAA.GetClipMode         = ' .. tostring(TAA.GetClipMode and TAA.GetClipMode() or 'n/a'))
    print('  TAA.GetSharpness        = ' .. tostring(TAA.GetSharpness and TAA.GetSharpness() or 'n/a'))
    print('  TAA.GetAntiFlicker      = ' .. tostring(TAA.GetAntiFlicker and TAA.GetAntiFlicker() or 'n/a'))
    print('  TAA.GetVarianceGamma    = ' .. tostring(TAA.GetVarianceGamma and TAA.GetVarianceGamma() or 'n/a'))
    print('  TAA.GetHalfResHistory   = ' .. tostring(TAA.GetHalfResHistory and TAA.GetHalfResHistory() or 'n/a'))
    print('demo_taa_compare ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local pok, win, err = pcall(function() return Window.Open(WIN_W, WIN_H, 'Phase F.0.7 - TAA Compare Demo') end)
if not pok then
    print('[demo_taa_compare] Window.Open raised: ' .. tostring(win))
    print('demo_taa_compare ok (no window)')
    return
end
if not win then
    print('[demo_taa_compare] Window.Open returned nil: ' .. tostring(err))
    print('demo_taa_compare ok (no window)')
    return
end

-- ============================================================================
-- 1. 几何体: cube + thin bar + plane (高对比测试场景)
-- ============================================================================

-- vertex format: pos3 + normal3 + uv2 + color4 = 12 floats
local function buildCube(s)
    s = s or 0.5
    local v = {}
    local idx = {}
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

-- plane (XZ 黑色地面, 反衬 HDR 高光)
local function buildPlane()
    local s = 6.0
    local v = {
        -s, 0, -s,  0, 1, 0,  0, 0,  0.04, 0.04, 0.05, 1,
         s, 0, -s,  0, 1, 0,  1, 0,  0.04, 0.04, 0.05, 1,
         s, 0,  s,  0, 1, 0,  1, 1,  0.04, 0.04, 0.05, 1,
        -s, 0,  s,  0, 1, 0,  0, 1,  0.04, 0.04, 0.05, 1,
    }
    local idx = {1, 2, 3, 1, 3, 4}
    return v, idx
end

local cubeMesh, barMesh, planeMesh
do
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_taa_compare] Gfx.Mesh.New not available')
        win:Close()
        print('demo_taa_compare ok (no mesh api)')
        return
    end
    local cv, ci = buildCube(0.5)            -- 中央立方体 1×1×1
    local bv, bi = buildCube(1.0)            -- 薄棒原型 (sx 缩放后 1×1×1)
    local pv, pi = buildPlane()
    cubeMesh   = Mesh.New(cv, ci)
    barMesh    = Mesh.New(bv, bi)
    planeMesh  = Mesh.New(pv, pi)
    if not cubeMesh or not barMesh or not planeMesh then
        print('[demo_taa_compare] mesh build failed')
        win:Close()
        return
    end
end

-- ============================================================================
-- 2. HDR + TAA 启动 + camera + 灯光
-- ============================================================================

local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_taa_compare] HDR.Enable = ' .. tostring(hdrEnabled))

if type(Gfx.SetPerspective) == 'function' then
    Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0)
end
if type(Gfx.SetDepthTest) == 'function' then
    Gfx.SetDepthTest(true)
end
if type(Gfx.SetDirectionalLight) == 'function' then
    -- 强 HDR 光 (intensity 5) + 暖白 → 触发金色 cube 高光 firefly
    Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
end

-- ============================================================================
-- 3. 8 个 TAA Preset 表 (渐进式叠加)
-- ============================================================================

-- 每个 preset 描述: name / desc(算法说明) / TAA 参数集
-- 未启 TAA 时 enabled=false; 否则提供完整参数 (clipMode/sharpness/antiFlicker/varianceGamma/halfResHistory)
local PRESETS = {
    [1] = {
        name = 'TAA OFF (baseline)',
        desc = 'no TAA -- raw aliasing / firefly / 1px shimmer',
        enabled = false,
    },
    [2] = {
        name = 'F.0 base',
        desc = 'jitter + reproject + RGB AABB clip + alpha blend',
        enabled = true,
        clipMode = 'rgb',
        sharpness = 0,
        antiFlicker = false,
        halfResHistory = false,
    },
    [3] = {
        name = 'F.0.1 sharpening',
        desc = 'F.0 + 4-tap unsharp mask sharpness=0.5',
        enabled = true,
        clipMode = 'rgb',
        sharpness = 0.5,
        antiFlicker = false,
        halfResHistory = false,
    },
    [4] = {
        name = 'F.0.2 YCoCg clip',
        desc = 'YCoCg AABB clip (色彩边缘鲁棒, 减 ghosting)',
        enabled = true,
        clipMode = 'ycocg',
        sharpness = 0.5,
        antiFlicker = false,
        halfResHistory = false,
    },
    [5] = {
        name = 'F.0.3 variance clip',
        desc = 'mean +/- gamma*sigma (Salvi 2016 / UE5 default)',
        enabled = true,
        clipMode = 'variance',
        sharpness = 0.5,
        antiFlicker = false,
        varianceGamma = 1.0,
        halfResHistory = false,
    },
    [6] = {
        name = 'F.0.4 anti-flicker',
        desc = 'Karis luma-weighted blend (压制 HDR firefly)',
        enabled = true,
        clipMode = 'variance',
        sharpness = 0.5,
        antiFlicker = true,
        varianceGamma = 1.0,
        halfResHistory = false,
    },
    [7] = {
        name = 'F.0.5 half-res history',
        desc = 'history RT (W/2, H/2) -- VRAM -75%, TAA pass -75% pixels',
        enabled = true,
        clipMode = 'variance',
        sharpness = 0.5,
        antiFlicker = true,
        varianceGamma = 1.0,
        halfResHistory = true,
    },
    [8] = {
        name = 'ALL (推荐)',
        desc = '完整管线 (variance + AF + sharp 0.8 + halfRes) -- 移动 4K 推荐',
        enabled = true,
        clipMode = 'variance',
        sharpness = 0.8,
        antiFlicker = true,
        varianceGamma = 1.0,
        halfResHistory = true,
    },
}

-- ============================================================================
-- 4. apply_preset(idx) — 切换 TAA 配置, reset stabilization 计数
-- ============================================================================

local STABILIZE_FRAMES = 30                    -- TAA history alpha=0.92 收敛大约 30 帧
local currentPreset    = 1
local stabilizeCounter = 0                     -- 切 preset 后递增, 到 30 显示 stabilized

-- apply: 按 preset 表设 TAA 状态. 处理 enabled=false 时 Disable, 否则 Enable + Set 全部参数
local function apply_preset(idx)
    local p = PRESETS[idx]
    if not p then return false end

    -- 步骤 1: enabled=false → Disable
    if not p.enabled then
        if TAA.IsEnabled() then TAA.Disable() end
        currentPreset = idx
        stabilizeCounter = 0
        print(string.format('[demo] Preset %d: %s', idx, p.name))
        return true
    end

    -- 步骤 2: enabled=true → 确保 Enable 后再设参数
    if not TAA.IsEnabled() then
        local ok = TAA.Enable(WIN_W, WIN_H)
        if not ok then
            print('[demo] TAA.Enable failed, preset abort')
            return false
        end
    end

    TAA.SetClipMode(p.clipMode)
    TAA.SetSharpness(p.sharpness)
    if TAA.SetAntiFlicker then TAA.SetAntiFlicker(p.antiFlicker) end
    if p.varianceGamma and TAA.SetVarianceGamma then
        TAA.SetVarianceGamma(p.varianceGamma)
    end
    if TAA.SetHalfResHistory then TAA.SetHalfResHistory(p.halfResHistory) end

    currentPreset = idx
    stabilizeCounter = 0
    print(string.format('[demo] Preset %d: %s -- %s', idx, p.name, p.desc))
    return true
end

-- 启动默认 preset = 1 (OFF, baseline 让用户先看 raw aliasing)
apply_preset(1)

-- ============================================================================
-- 5. 主循环
-- ============================================================================

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

-- 中央立方体自转 + 围绕薄棒公转角
local cubeAngle = 0.0
local barAngle  = 0.0

-- 8 根围绕薄棒颜色 (彩虹色, 增加色彩对比触发 YCoCg / variance clip 差异)
local BAR_COLORS = {
    {1.0, 0.2, 0.2},    -- 红
    {1.0, 0.6, 0.2},    -- 橙
    {1.0, 1.0, 0.2},    -- 黄
    {0.2, 1.0, 0.2},    -- 绿
    {0.2, 1.0, 1.0},    -- 青
    {0.2, 0.5, 1.0},    -- 蓝
    {0.7, 0.2, 1.0},    -- 紫
    {1.0, 0.2, 0.7},    -- 粉
}

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt  = now - lastTime
    lastTime  = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    cubeAngle = cubeAngle + dt * math.rad(30)         -- 中央 cube 自转 30 deg/s
    barAngle  = barAngle  + dt * math.rad(60)         -- 薄棒围绕 60 deg/s (高速 → trail)

    win:PollEvents()
    if win:IsKeyPressed('escape') then win:Close(); break end

    -- 数字键 1-8: 切 preset
    for i = 1, 8 do
        if keyTap(tostring(i)) then
            apply_preset(i)
        end
    end

    -- R: 重置 history (Disable + Enable, 适合彻底 clear ghosting 重新开始 stabilize)
    if keyTap('r') then
        if TAA.IsEnabled() then TAA.Disable() end
        apply_preset(currentPreset)
        print('[demo] Reset history (re-stabilizing 30 frames)')
    end

    -- 推进 stabilization 计数 (每 frame +1, clamp 到 STABILIZE_FRAMES)
    if stabilizeCounter < STABILIZE_FRAMES then
        stabilizeCounter = stabilizeCounter + 1
    end

    -- ========== 渲染 ==========

    win:BeginFrame(0.02, 0.02, 0.03, 1.0)            -- 黑底 → 反衬 HDR 高光

    -- 静态相机 (eye=(0, 2.2, 5), at=(0, 0.6, 0)), 让用户专注观察 TAA 对运动物处理
    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(0.0, 2.2, 5.0, 0.0, 0.6, 0.0)
    end

    -- 黑色地面
    if planeMesh then
        Gfx.Push()
        Gfx.SetColor(0.04, 0.04, 0.05, 1.0)
        planeMesh:Draw(0)
        Gfx.Pop()
    end

    -- 中央旋转金色 cube (HDR 高光 → firefly 触发器)
    Gfx.Push()
    Gfx.Translate(0.0, 0.6, 0.0)
    Gfx.Rotate(math.deg(cubeAngle), 0, 1, 0)
    Gfx.Scale(1.2, 1.2, 1.2)
    Gfx.SetColor(1.0, 0.9, 0.7, 1.0)                  -- 暖金色
    cubeMesh:Draw(0)
    Gfx.Pop()

    -- 8 根围绕薄棒 (1px 厚度 → aliasing + trail 触发器)
    -- 半径 2.5, Y=0.6, 颜色彩虹, 旋转 60 deg/s
    local R = 2.5
    for i = 1, 8 do
        local theta = barAngle + (i - 1) * math.pi * 2.0 / 8.0
        local bx = math.cos(theta) * R
        local bz = math.sin(theta) * R
        local c = BAR_COLORS[i]
        Gfx.Push()
        Gfx.Translate(bx, 0.6, bz)
        Gfx.Rotate(math.deg(theta) + 90, 0, 1, 0)     -- 切线方向
        Gfx.Scale(0.04, 1.2, 0.04)                    -- 1×1 单位立方体缩成细薄棒 (4cm × 1.2m × 4cm)
        Gfx.SetColor(c[1], c[2], c[3], 1.0)
        barMesh:Draw(0)
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- ========== HUD ==========
    if win.DrawText then
        local p   = PRESETS[currentPreset]
        local y   = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end

        -- 1) Preset 索引 + 名 (突出)
        line(string.format('[Preset %d/8] %s', currentPreset, p.name))

        -- 2) algorithm 一行说明
        line('Algorithm: ' .. p.desc)

        -- 3) TAA 当前参数 (反映实际 GetXXX 值, 不依赖 preset 表)
        if p.enabled and TAA.IsEnabled() then
            local cmode  = TAA.GetClipMode and TAA.GetClipMode() or 'rgb'
            local cmodeStr = cmode
            if cmode == 'variance' and TAA.GetVarianceGamma then
                cmodeStr = string.format('variance(g=%.2f)', TAA.GetVarianceGamma())  -- 'g' = γ ASCII fallback (HUD 字体限制)
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
                sharp,
                sharp > 0 and 'sharpen pass' or 'pure blit',
                af and 'ON' or 'OFF',
                hr and 'ON (VRAM -75%)' or 'OFF (full-res)'))
        else
            line('TAA: OFF -- baseline (no jitter / no reproject / no clip)')
            line('Expect: visible aliasing + 1px shimmer + HDR firefly noise')
        end

        -- 4) history stabilization 进度 (告诉用户切 preset 后等 30 帧再公平对比)
        local fc = TAA.GetFrameCounter and TAA.GetFrameCounter() or 0
        local stabStr
        if stabilizeCounter < STABILIZE_FRAMES and p.enabled then
            local bar = string.rep('=', stabilizeCounter) .. string.rep('-', STABILIZE_FRAMES - stabilizeCounter)
            stabStr = string.format('History: stabilizing [%s] %d/%d frames', bar, stabilizeCounter, STABILIZE_FRAMES)
        elseif p.enabled then
            stabStr = string.format('History: STABLE (>=%d frames after preset switch)', STABILIZE_FRAMES)
        else
            stabStr = 'History: N/A (TAA OFF)'
        end
        line(string.format('%s | TAA frame=%d', stabStr, fc))

        -- 5) Keys 帮助
        line('Keys: 1-8=preset | R=reset history | ESC=exit')
        line('Tip: 切 preset 后等 history STABLE 再观察画质 (~30 frames)')
    end

    win:EndFrame()
end

-- ============================================================================
-- 6. 反向清理
-- ============================================================================

if TAA.IsEnabled() then TAA.Disable() end
if hdrEnabled then HDR.Disable() end
if cubeMesh  then cubeMesh:Delete()  end
if barMesh   then barMesh:Delete()   end
if planeMesh then planeMesh:Delete() end
print('demo_taa_compare ok')
