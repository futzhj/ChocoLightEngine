-- ============================================================================
-- ChocoLight Phase E.9+E.10+E.11 — SSR (Screen-Space Reflection) demo
-- ============================================================================
-- 金属反射场景: plane 地面 + 多色 cube + 旋转相机 + SSR toggle + Blur toggle + Bilateral.
--   * 显式 SetPerspective + SetCamera + SetDepthTest(true)
--   * G-buffer normal MRT 由 HDR pipeline 提供 (Phase E.8.x)
--   * SSR linear ray march (64 step) 写入反射 RT, additive composite 入 HDR
--   * Phase E.10: 可选 half-res Gaussian blur (5-tap separable) 模糊反射
--   * Phase E.11: 可选 depth-aware bilateral 权重门控，消除跨深度边 leak
--   * 反射效果最强: 摄像机斜俯视地面 (法线向上, 反射方向命中周围 cube)
--
-- 控制:
--   F        : 切换 SSR on/off
--   1 / 2    : MaxSteps      -/+  (步长 8,    范围 [8, 128])
--   3 / 4    : StepSize      -/+  (步长 0.05, 范围 [0.01, 1.0])
--   5 / 6    : Thickness     -/+  (步长 0.1,  范围 [0.01, 5.0])
--   7 / 8    : Intensity     -/+  (步长 0.1,  范围 [0.0, 2.0])
--   - / =    : MaxDistance   -/+  (步长 10,   范围 [1, 1000])
--   [ / ]    : EdgeFade      -/+  (步长 0.05, 范围 [0.0, 0.5])
--   B        : 切换 SSR Blur on/off            (Phase E.10)
--   9 / 0    : BlurRadius    -/+  (步长 0.25, 范围 [0.5, 4.0])  (Phase E.10)
--   V        : 切换 Bilateral on/off           (Phase E.11; off = E.10 Gaussian)
--   , / .    : BlurDepthSigma -/+ (步长 25,   范围 [50, 500])    (Phase E.11)
--   T        : 切换 Temporal on/off           (Phase E.12; off = E.11 行为)
--   U / I    : TemporalAlpha -/+ (步长 0.02, 范围 [0.5, 0.99])  (Phase E.12)
--   N        : 切换 RejectionMode 0/1         (Phase E.12; 1=neighborhood clip)
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
    print('[demo_ssr] Light.Graphics not available')
    print('demo_ssr ok (no graphics)')
    return
end

local HDR = Gfx.HDR
local SSR = Gfx.SSR
-- Phase E.15 — MotionBlur 是可选模块 (老版本可能没有), 不强制要求
local MotionBlur = Gfx.MotionBlur
if type(HDR) ~= 'table' or type(SSR) ~= 'table' then
    print('[demo_ssr] need HDR + SSR subtables')
    print('demo_ssr ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.9 SSR demo ====')
print('[demo_ssr] Backend       = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ssr] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_ssr] SSR.IsSupported = ' .. tostring(SSR.IsSupported()))

if not UI or not UI.Window then
    print('[demo_ssr] UI.Window not available, API probe only')
    print('  SSR.GetMaxSteps      = ' .. tostring(SSR.GetMaxSteps()))
    print('  SSR.GetStepSize      = ' .. tostring(SSR.GetStepSize()))
    print('  SSR.GetThickness     = ' .. tostring(SSR.GetThickness()))
    print('  SSR.GetMaxDistance   = ' .. tostring(SSR.GetMaxDistance()))
    print('  SSR.GetIntensity     = ' .. tostring(SSR.GetIntensity()))
    print('  SSR.GetEdgeFade      = ' .. tostring(SSR.GetEdgeFade()))
    print('  SSR.GetReflectionTexId = ' .. tostring(SSR.GetReflectionTexId()))
    print('demo_ssr ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
-- ChocoLight Window 是 OOP 表; pcall 防御不同环境下的调用约定差异 + headless 优雅退出
local pok, win, err = pcall(function() return Window.Open(WIN_W, WIN_H, 'Phase E.9 - SSR Demo') end)
if not pok then
    print('[demo_ssr] Window.Open raised error: ' .. tostring(win))
    print('demo_ssr ok (no window)')
    return
end
if not win then
    print('[demo_ssr] Window.Open returned nil: ' .. tostring(err))
    print('demo_ssr ok (no window)')
    return
end

-- ============================================================================
-- 1. 生成几何体: plane (地面, 反射面) + 多个 cube (反射源)
-- ============================================================================

-- vertex format: pos3 + normal3 + uv2 + color4 = 12 floats (与 SSAO demo 同)
local function buildCube()
    local s = 0.5
    local v = {}
    local idx = {}
    local function addQuad(p1, p2, p3, p4, n)
        local base = #v / 12
        for _, p in ipairs({p1, p2, p3, p4}) do
            for i = 1, 3 do v[#v + 1] = p[i] end
            for i = 1, 3 do v[#v + 1] = n[i] end
            v[#v + 1] = 0; v[#v + 1] = 0
            v[#v + 1] = 1; v[#v + 1] = 1
            v[#v + 1] = 1; v[#v + 1] = 1
        end
        idx[#idx + 1] = base + 1; idx[#idx + 1] = base + 2; idx[#idx + 1] = base + 3
        idx[#idx + 1] = base + 1; idx[#idx + 1] = base + 3; idx[#idx + 1] = base + 4
    end
    addQuad({ s,-s,-s}, { s, s,-s}, { s, s, s}, { s,-s, s}, { 1, 0, 0})
    addQuad({-s,-s, s}, {-s, s, s}, {-s, s,-s}, {-s,-s,-s}, {-1, 0, 0})
    addQuad({-s, s,-s}, {-s, s, s}, { s, s, s}, { s, s,-s}, { 0, 1, 0})
    addQuad({-s,-s, s}, {-s,-s,-s}, { s,-s,-s}, { s,-s, s}, { 0,-1, 0})
    addQuad({-s,-s, s}, { s,-s, s}, { s, s, s}, {-s, s, s}, { 0, 0, 1})
    addQuad({ s,-s,-s}, {-s,-s,-s}, {-s, s,-s}, { s, s,-s}, { 0, 0,-1})
    return v, idx
end

-- plane XZ 面朝上 (反射面), 大小 10x10
local function buildPlane()
    local s = 5.0
    local v = {
        -s, 0, -s,  0, 1, 0,  0, 0,  0.5, 0.5, 0.55, 1,
         s, 0, -s,  0, 1, 0,  1, 0,  0.5, 0.5, 0.55, 1,
         s, 0,  s,  0, 1, 0,  1, 1,  0.5, 0.5, 0.55, 1,
        -s, 0,  s,  0, 1, 0,  0, 1,  0.5, 0.5, 0.55, 1,
    }
    local idx = {1, 2, 3, 1, 3, 4}
    return v, idx
end

local cubeMesh = nil
local planeMesh = nil
do
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_ssr] Gfx.Mesh.New not available, skipping 3D scene')
        win:Close()
        print('demo_ssr ok (no mesh api)')
        return
    end
    local cv, ci = buildCube()
    local pv, pi = buildPlane()
    local m1, e1 = Mesh.New(cv, ci)
    local m2, e2 = Mesh.New(pv, pi)
    if not m1 then print('[demo_ssr] cube mesh failed: ' .. tostring(e1)); win:Close(); return end
    if not m2 then print('[demo_ssr] plane mesh failed: ' .. tostring(e2)); win:Close(); return end
    cubeMesh = m1
    planeMesh = m2
    print(string.format('[demo_ssr] cube: %d verts / %d idx; plane: %d verts / %d idx',
        m1:GetVertexCount(), m1:GetIndexCount(), m2:GetVertexCount(), m2:GetIndexCount()))
end

-- ============================================================================
-- 2. HDR + SSR Enable + camera
-- ============================================================================

local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_ssr] HDR.Enable = ' .. tostring(hdrEnabled))

local ssrEnabled = hdrEnabled and SSR.Enable(WIN_W, WIN_H) or false
print('[demo_ssr] SSR.Enable = ' .. tostring(ssrEnabled))
if ssrEnabled then
    print('[demo_ssr] Reflection tex id = ' .. tostring(SSR.GetReflectionTexId()))
end

-- 3D camera (透视投影 + LookAt)
if type(Gfx.SetPerspective) == 'function' then
    Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0)
end
if type(Gfx.SetDepthTest) == 'function' then
    Gfx.SetDepthTest(true)
end

-- 主灯光 (PBR direction light)
if type(Gfx.SetDirectionalLight) == 'function' then
    Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 1.5)
end

-- ============================================================================
-- 3. 主循环 + 键位
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

local function clampNum(v, lo, hi)
    if v < lo then return lo end
    if hi and v > hi then return hi end
    return v
end

-- 3D 场景: 7 个 cube + 1 plane (亮色, 反射明显)
local SCENE = {
    {x =  0.0, y = 0.6, z =  0.0, scale = 1.2, color = {1.0, 0.4, 0.4}},
    {x =  2.5, y = 0.5, z =  1.5, scale = 1.0, color = {0.4, 1.0, 0.4}},
    {x = -2.5, y = 0.5, z = -1.5, scale = 1.0, color = {0.4, 0.4, 1.0}},
    {x =  1.5, y = 0.4, z = -2.5, scale = 0.8, color = {1.0, 1.0, 0.4}},
    {x = -1.5, y = 0.4, z =  2.5, scale = 0.8, color = {0.4, 1.0, 1.0}},
    {x =  3.5, y = 0.5, z = -3.0, scale = 1.0, color = {1.0, 0.5, 1.0}},
    {x = -3.5, y = 0.5, z =  3.0, scale = 1.0, color = {1.0, 0.7, 0.3}},
}

local camAngle = 0.0
local camHeight = 3.5   -- 较低俯角, 让反射更明显

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt = now - lastTime
    lastTime = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    camAngle = camAngle + dt * 0.25

    win:PollEvents()
    if win:IsKeyPressed('escape') then win:Close(); break end

    if keyTap('f') then
        if SSR.IsEnabled() then SSR.Disable(); print('[demo] SSR OFF')
        else local ok = SSR.Enable(WIN_W, WIN_H); print('[demo] SSR ' .. (ok and 'ON' or 'OFF (fail)')) end
    end

    -- 1/2: MaxSteps (8 step 增量)
    if keyTap('1') then SSR.SetMaxSteps(math.max(8, SSR.GetMaxSteps() - 8)); print('[demo] MaxSteps = ' .. SSR.GetMaxSteps()) end
    if keyTap('2') then SSR.SetMaxSteps(math.min(128, SSR.GetMaxSteps() + 8)); print('[demo] MaxSteps = ' .. SSR.GetMaxSteps()) end

    -- 3/4: StepSize
    if keyTap('3') then SSR.SetStepSize(clampNum(SSR.GetStepSize() - 0.05, 0.01, 1.0)) end
    if keyTap('4') then SSR.SetStepSize(clampNum(SSR.GetStepSize() + 0.05, 0.01, 1.0)) end

    -- 5/6: Thickness
    if keyTap('5') then SSR.SetThickness(clampNum(SSR.GetThickness() - 0.1, 0.01, 5.0)) end
    if keyTap('6') then SSR.SetThickness(clampNum(SSR.GetThickness() + 0.1, 0.01, 5.0)) end

    -- 7/8: Intensity
    if keyTap('7') then SSR.SetIntensity(clampNum(SSR.GetIntensity() - 0.1, 0.0, 2.0)) end
    if keyTap('8') then SSR.SetIntensity(clampNum(SSR.GetIntensity() + 0.1, 0.0, 2.0)) end

    -- - / =: MaxDistance (注: '-' 在 SDL key name 中是 'minus', '=' 是 'equals')
    if keyTap('minus')  then SSR.SetMaxDistance(clampNum(SSR.GetMaxDistance() - 10, 1, 1000)) end
    if keyTap('equals') then SSR.SetMaxDistance(clampNum(SSR.GetMaxDistance() + 10, 1, 1000)) end

    -- [ / ]: EdgeFade
    if keyTap('[') then SSR.SetEdgeFade(clampNum(SSR.GetEdgeFade() - 0.05, 0.0, 0.5)) end
    if keyTap(']') then SSR.SetEdgeFade(clampNum(SSR.GetEdgeFade() + 0.05, 0.0, 0.5)) end

    -- Phase E.10 — B: 切换 SSR Blur on/off
    if keyTap('b') then
        local v = not SSR.GetBlurEnabled()
        SSR.SetBlurEnabled(v)
        print('[demo] SSR Blur ' .. (v and 'ON' or 'OFF') .. ' (radius=' .. string.format('%.2f', SSR.GetBlurRadius()) .. ')')
    end

    -- Phase E.10 — 9/0: BlurRadius
    if keyTap('9') then
        SSR.SetBlurRadius(clampNum(SSR.GetBlurRadius() - 0.25, 0.5, 4.0))
        print('[demo] BlurRadius = ' .. string.format('%.2f', SSR.GetBlurRadius()))
    end
    if keyTap('0') then
        SSR.SetBlurRadius(clampNum(SSR.GetBlurRadius() + 0.25, 0.5, 4.0))
        print('[demo] BlurRadius = ' .. string.format('%.2f', SSR.GetBlurRadius()))
    end

    -- Phase E.11 — V: 切换 Bilateral
    if keyTap('v') then
        local b = not SSR.GetBilateralEnabled()
        SSR.SetBilateralEnabled(b)
        print('[demo] SSR Bilateral ' .. (b and 'ON' or 'OFF') .. ' (sigma=' .. string.format('%.0f', SSR.GetBlurDepthSigma()) .. ')')
    end

    -- Phase E.11 — ,/.: BlurDepthSigma
    if keyTap(',') then
        SSR.SetBlurDepthSigma(clampNum(SSR.GetBlurDepthSigma() - 25.0, 50.0, 500.0))
        print('[demo] BlurDepthSigma = ' .. string.format('%.0f', SSR.GetBlurDepthSigma()))
    end
    if keyTap('.') then
        SSR.SetBlurDepthSigma(clampNum(SSR.GetBlurDepthSigma() + 25.0, 50.0, 500.0))
        print('[demo] BlurDepthSigma = ' .. string.format('%.0f', SSR.GetBlurDepthSigma()))
    end

    -- Phase E.12 — T: 切换 Temporal
    if keyTap('t') then
        local te = not SSR.GetTemporalEnabled()
        SSR.SetTemporalEnabled(te)
        print('[demo] SSR Temporal ' .. (te and 'ON' or 'OFF') ..
              ' (alpha=' .. string.format('%.2f', SSR.GetTemporalAlpha()) ..
              ', reject=' .. tostring(SSR.GetRejectionMode()) .. ')')
    end

    -- Phase E.12 — U/I: TemporalAlpha
    if keyTap('u') then
        SSR.SetTemporalAlpha(clampNum(SSR.GetTemporalAlpha() - 0.02, 0.5, 0.99))
        print('[demo] TemporalAlpha = ' .. string.format('%.3f', SSR.GetTemporalAlpha()))
    end
    if keyTap('i') then
        SSR.SetTemporalAlpha(clampNum(SSR.GetTemporalAlpha() + 0.02, 0.5, 0.99))
        print('[demo] TemporalAlpha = ' .. string.format('%.3f', SSR.GetTemporalAlpha()))
    end

    -- Phase E.12 — N: 切换 RejectionMode (0=current-depth, 1=neighborhood)
    if keyTap('n') then
        local rm = 1 - SSR.GetRejectionMode()
        SSR.SetRejectionMode(rm)
        print('[demo] RejectionMode = ' .. tostring(rm) ..
              ' (' .. (rm == 1 and 'neighborhood-clip' or 'current-depth') .. ')')
    end

    -- Phase E.14 — K: 切 velocity dilation
    if keyTap('k') then
        local d = not HDR.GetVelocityDilation()
        HDR.SetVelocityDilation(d)
        print('[demo] Velocity Dilation ' .. (d and 'ON' or 'OFF'))
    end

    -- Phase E.14 — L: 切 velocity 格式 rg16f <-> rg8 (会重建 HDR RT)
    if keyTap('l') then
        local cur = HDR.GetVelocityFormat()
        local nxt = (cur == 'rg16f') and 'rg8' or 'rg16f'
        local ok = HDR.SetVelocityFormat(nxt)
        print('[demo] Velocity Format ' .. cur .. ' -> ' .. nxt ..
              ' (' .. (ok and 'ok' or 'failed') .. ')')
    end

    -- Phase E.15 — M: 切 Motion Blur ON/OFF
    if MotionBlur and keyTap('m') then
        if MotionBlur.IsEnabled() then
            MotionBlur.Disable()
            print('[demo] MotionBlur OFF')
        else
            local ok = MotionBlur.Enable(WIN_W, WIN_H)
            print('[demo] MotionBlur ' .. (ok and 'ON' or 'OFF (fail)'))
        end
    end

    -- Phase E.16 — ;: 循环切 motion blur mode (0→1→2→0)
    if MotionBlur and keyTap(';') then
        local names = { [0] = 'combined', [1] = 'camera_only', [2] = 'object_only' }
        local cur = MotionBlur.GetMode()
        local nxt = (cur + 1) % 3
        MotionBlur.SetMode(nxt)
        print(string.format('[demo] MotionBlur Mode: %s (%d) -> %s (%d)',
            names[cur], cur, names[nxt], nxt))
    end

    -- Phase E.17 — [: 切 half-res motion blur ON/OFF
    if MotionBlur and MotionBlur.SetHalfRes and keyTap('[') then
        local cur = MotionBlur.GetHalfRes()
        MotionBlur.SetHalfRes(not cur)
        print(string.format('[demo] MotionBlur HalfRes: %s -> %s',
            cur and 'ON' or 'OFF', (not cur) and 'ON' or 'OFF'))
    end

    -- Phase E.18.1 — ]: 切 dilation pass 半分辨率 ON/OFF (与 [ motion blur halfRes 对称)
    if HDR.SetVelocityDilationHalfRes and keyTap(']') then
        local cur = HDR.GetVelocityDilationHalfRes()
        HDR.SetVelocityDilationHalfRes(not cur)
        print(string.format('[demo] Velocity Dilation HalfRes: %s -> %s (VRAM %s / dilation pass %s)',
            cur and 'ON' or 'OFF', (not cur) and 'ON' or 'OFF',
            (not cur) and '-75%' or 'baseline',
            (not cur) and '+4x' or 'baseline'))
    end

    -- Phase E.18.2 — \: 切 dilation pass autoSkip ON/OFF (单消费者 SSR Temporal 场景自动跳过)
    if HDR.SetVelocityDilationAutoSkip and keyTap('\\') then
        local cur = HDR.GetVelocityDilationAutoSkip()
        HDR.SetVelocityDilationAutoSkip(not cur)
        print(string.format('[demo] Velocity Dilation AutoSkip: %s -> %s (%s)',
            cur and 'ON' or 'OFF', (not cur) and 'ON' or 'OFF',
            (not cur) and 'SSR-only single consumer auto-skipped' or 'always run dilation pass'))
    end

    -- Phase F.0 — Y: 切 TAA 主管线 ON/OFF (与 SSR Temporal 共存，推荐手动关 SSR Temporal)
    local TAA = Gfx.TAA
    if TAA and keyTap('y') then
        if TAA.IsEnabled() then
            TAA.Disable()
            print('[demo] TAA OFF')
        else
            local w, h = win:GetSize()
            local ok = TAA.Enable(w, h)
            print(string.format('[demo] TAA Enable(%d, %d) -> %s (alpha=%.2f, clip=%s, jitter=%s)',
                w, h, ok and 'ON' or 'FAILED',
                TAA.GetBlendAlpha(),
                TAA.GetNeighborhoodClip() and 'ON' or 'OFF',
                TAA.GetJitterEnabled() and 'ON' or 'OFF'))
            if ok and SSR.GetTemporalEnabled() then
                print('[demo] WARN: SSR Temporal 仍启用, 反射会被 temporal 两次 (考虑 SSR.SetTemporalEnabled(false))')
            end
        end
    end

    -- Phase F.0 — J: 切 TAA jitter ON/OFF (关掉则退化为纯时序 stability filter, 无 super-sampling)
    if TAA and TAA.IsEnabled() and keyTap('j') then
        local cur = TAA.GetJitterEnabled()
        TAA.SetJitterEnabled(not cur)
        print(string.format('[demo] TAA Jitter: %s -> %s (%s)',
            cur and 'ON' or 'OFF', (not cur) and 'ON' or 'OFF',
            (not cur) and 'sub-pixel super-sampling' or 'pure temporal stability (no SS)'))
    end

    -- Phase F.0.1 — H: 调节 TAA sharpness (±0.1, clamp [0, 2], 默认 0.5)
    --   sharpness=0 走纯 blit fallback (零 ALU); > 0 启用 4-tap unsharp mask
    if TAA and TAA.IsEnabled() and TAA.SetSharpness and keyTap('h') then
        local cur = TAA.GetSharpness()
        local nxt = cur + 0.1
        if nxt > 2.0 + 1e-3 then nxt = 0.0 end   -- 环状回 0 (方便演示 sharpness=0 路径)
        TAA.SetSharpness(nxt)
        print(string.format('[demo] TAA Sharpness: %.2f -> %.2f (%s)',
            cur, TAA.GetSharpness(),
            TAA.GetSharpness() > 0 and '4-tap unsharp mask' or 'pure blit (zero ALU)'))
    end

    -- Phase F.0.4 — G: 切 TAA anti-flicker (Karis luma-weighted blend) ON/OFF
    --   off → F.0 纯 alpha blend (低亮部行为完全等价); on → Karis 压制 firefly 闪烁
    if TAA and TAA.IsEnabled() and TAA.SetAntiFlicker and keyTap('g') then
        local cur = TAA.GetAntiFlicker()
        TAA.SetAntiFlicker(not cur)
        print(string.format('[demo] TAA AntiFlicker: %s -> %s (%s)',
            cur and 'ON' or 'OFF',
            TAA.GetAntiFlicker() and 'ON' or 'OFF',
            TAA.GetAntiFlicker() and 'Karis luma weighting (压制 firefly)' or 'pure alpha blend (F.0 行为)'))
    end

    -- R: reset 默认
    if keyTap('r') then
        SSR.SetMaxSteps(64); SSR.SetStepSize(0.1); SSR.SetThickness(0.5)
        SSR.SetMaxDistance(50.0); SSR.SetIntensity(0.7); SSR.SetEdgeFade(0.1)
        SSR.SetBlurEnabled(false); SSR.SetBlurRadius(1.5)         -- Phase E.10
        SSR.SetBilateralEnabled(true); SSR.SetBlurDepthSigma(200.0)  -- Phase E.11
        SSR.SetTemporalEnabled(true); SSR.SetTemporalAlpha(0.9); SSR.SetRejectionMode(1)  -- Phase E.12
        print('[demo] reset defaults')
    end

    -- ========== 渲染 ==========
    win:BeginFrame(0.08, 0.10, 0.14, 1.0)

    -- 相机围绕场景慢转
    local cx = math.cos(camAngle) * 8.0
    local cz = math.sin(camAngle) * 8.0
    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(cx, camHeight, cz, 0.0, 0.5, 0.0)
    end

    -- 地面 (反射面)
    if planeMesh then
        Gfx.Push()
        Gfx.SetColor(0.5, 0.5, 0.55, 1.0)
        planeMesh:Draw(0)
        Gfx.Pop()
    end

    -- 7 个立方体
    for _, c in ipairs(SCENE) do
        Gfx.Push()
        Gfx.Translate(c.x, c.y, c.z)
        Gfx.Scale(c.scale, c.scale, c.scale)
        Gfx.SetColor(c.color[1], c.color[2], c.color[3], 1.0)
        cubeMesh:Draw(0)
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        line(string.format('HDR: %s   SSR: %s   reflectTex=%d',
            hdrEnabled and 'ON' or 'OFF',
            SSR.IsEnabled() and 'ON' or 'OFF',
            SSR.GetReflectionTexId()))
        line(string.format('SSR: steps=%d step=%.2f thick=%.2f',
            SSR.GetMaxSteps(), SSR.GetStepSize(), SSR.GetThickness()))
        line(string.format('SSR: maxDist=%.0f intensity=%.2f edgeFade=%.2f',
            SSR.GetMaxDistance(), SSR.GetIntensity(), SSR.GetEdgeFade()))
        line(string.format('SSR Blur: %s  radius=%.2f  Bilateral=%s  sigma=%.0f',
            SSR.GetBlurEnabled() and 'ON' or 'OFF',
            SSR.GetBlurRadius(),
            SSR.GetBilateralEnabled() and 'ON' or 'OFF',
            SSR.GetBlurDepthSigma()))
        line(string.format('Temporal: %s | alpha=%.2f | reject=%d (%s)',
            SSR.GetTemporalEnabled() and 'ON' or 'OFF',
            SSR.GetTemporalAlpha(),
            SSR.GetRejectionMode(),
            SSR.GetRejectionMode() == 1 and 'neighborhood' or 'depth'))
        -- Phase E.13/E.14: velocity buffer 与 Temporal SSR 联动；显示 dilation/format 状态
        -- Phase E.18: dilation=ON 时 HDR EndScene 内做独立 9-tap pass，SSR/MB 共享 dilatedTex
        -- Phase E.18.1: dilation pass 可半分辨率 (VRAM -75%, perf +4x)
        -- Phase E.18.2: autoSkip 单消费者 SSR Temporal 场景跳过 dilation pass
        local dilateHalfRes = HDR.GetVelocityDilationHalfRes and HDR.GetVelocityDilationHalfRes() or false
        local dilateAutoSkip = HDR.GetVelocityDilationAutoSkip and HDR.GetVelocityDilationAutoSkip() or false
        line(string.format('Velocity: %s | dilation=%s (E.18 shared, halfRes=%s, autoSkip=%s) | reproj=%s',
            HDR.GetVelocityFormat(),
            HDR.GetVelocityDilation() and 'ON' or 'OFF',
            dilateHalfRes and 'ON' or 'OFF',
            dilateAutoSkip and 'ON' or 'OFF',
            SSR.GetTemporalEnabled() and 'velocity-first' or 'idle (Temporal OFF)'))
        -- Phase E.15+E.16+E.17: MotionBlur 状态 (可选模块)
        if MotionBlur then
            local modeNames = { [0] = 'combined', [1] = 'camera_only', [2] = 'object_only' }
            local m = MotionBlur.GetMode and MotionBlur.GetMode() or 0
            local hr = MotionBlur.GetHalfRes and MotionBlur.GetHalfRes() or false
            line(string.format('MotionBlur: %s | mode=%d (%s) | halfRes=%s | strength=%.2f | samples=%d',
                MotionBlur.IsEnabled() and 'ON' or 'OFF',
                m, modeNames[m] or '?',
                hr and 'ON' or 'OFF',
                MotionBlur.GetStrength(),
                MotionBlur.GetSampleCount()))
        end
        -- Phase F.0+F.0.1+F.0.2+F.0.4: TAA 主管线状态 (jitter + frame counter + alpha + clip + sharpness + antiFlicker + clipMode)
        local TAAhud = Gfx.TAA
        if TAAhud then
            local jx, jy = TAAhud.GetCurrentJitter()
            local sharp  = TAAhud.GetSharpness and TAAhud.GetSharpness() or 0
            local af     = TAAhud.GetAntiFlicker and TAAhud.GetAntiFlicker() or false
            local cmode  = TAAhud.GetClipMode and TAAhud.GetClipMode() or 'rgb'  -- Phase F.0.2
            line(string.format('TAA: %s | alpha=%.2f | clip=%s/%s | jitter=%s | sharp=%.2f (%s) | AF=%s | frame=%d (jx=%.3f jy=%.3f)',
                TAAhud.IsEnabled() and 'ON' or 'OFF',
                TAAhud.GetBlendAlpha(),
                TAAhud.GetNeighborhoodClip() and 'ON' or 'OFF',
                cmode,                                   -- Phase F.0.2: rgb / ycocg
                TAAhud.GetJitterEnabled() and 'ON' or 'OFF',
                sharp,
                sharp > 0 and 'sharpen pass' or 'pure blit',
                af and 'ON' or 'OFF',                    -- Phase F.0.4
                TAAhud.GetFrameCounter(),
                jx, jy))
        end
        line('Keys: F=SSR B=Blur V=Bilateral T=Temporal 9/0=radius ,/.=sigma U/I=alpha N=reject K=Dilation L=Format M=MotionBlur ;=Mode [=MBHalfRes ]=DilHalfRes \\=AutoSkip Y=TAA J=TAAjitter H=TAAsharp G=TAAAF R=reset ESC')
    end

    win:EndFrame()
end

-- ============================================================================
-- 4. 反向清理
-- ============================================================================

if Gfx.TAA and Gfx.TAA.IsEnabled() then Gfx.TAA.Disable() end
if MotionBlur and MotionBlur.IsEnabled() then MotionBlur.Disable() end
if SSR.IsEnabled() then SSR.Disable() end
if hdrEnabled then HDR.Disable() end
if cubeMesh then cubeMesh:Delete() end
if planeMesh then planeMesh:Delete() end
print('demo_ssr ok')
