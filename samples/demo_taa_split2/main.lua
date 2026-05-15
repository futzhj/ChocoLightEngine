-- ============================================================================
-- ChocoLight Phase F.0.10.4 — True Physical Split-Screen with Independent
--                              TAA + Bloom + SSR + MotionBlur (per-region)
-- ============================================================================
-- F.0.10.2 交付了双 TAA instance 同帧 split-screen。
-- F.0.10.3 交付了 Bloom / SSR / MotionBlur 的 region 化 (3 个 .Process(rgn) Lua API).
-- F.0.10.4 (本 demo) 把 F.0.10.3 的成果同屏站台化: 双 player 各自不同后处理 profile.
--
-- 双 player 后处理对比 profile (视觉差异明显):
--   Player 1 (左, "电影感冲击"): RCAS 强锐 + 强 Bloom + 中速 SSR + 强 MotionBlur
--   Player 2 (右, "高清细腻"):   Lanczos halfRes + 轻 Bloom + 高质 temporal SSR + 无 MotionBlur
--
-- F.0.10.3 新增 API 起作用点:
--   1) HDR.SetAutoBloom(false)                                -- 关 EndScene 自动全屏 Bloom
--   2) HDR.SetAutoSSR(false)                                  -- 关 EndScene 自动全屏 SSR
--   3) HDR.SetAutoMotionBlur(false)                           -- 关 EndScene 自动全屏 MB
--   4) Bloom.Process(rgnX, rgnY, rgnW, rgnH)                  -- 手动 region Bloom
--   5) SSR.Process(rgnX, rgnY, rgnW, rgnH)                    -- 手动 region SSR
--   6) MotionBlur.Process(rgnX, rgnY, rgnW, rgnH)             -- 手动 region MB
--
-- F.0.10.2 已有 API:
--   HDR.SetAutoTAA(false) + TAA.Process(rgn) + Gfx.SetViewport + TAA.SetActiveInstance
--
-- 每帧流程 (per region):
--   HDR.BeginScene()                          -- BeginFrame 自动调 (全屏清 HDR fbo)
--
--   -- Player 1 (左半屏, x=0, w=HALF_W)
--   Gfx.SetViewport(0, 0, HALF_W, WIN_H)
--   TAA.SetActiveInstance(p1_id); TAA.ApplyJitter()
--   drawScene(player1_camera)                 -- raster into HDR fbo 左半区域
--   Bloom.Process(0, 0, HALF_W, WIN_H)        -- 1❶ Bloom (顺序同 HDR.EndScene auto 路径)
--   SSR.Process(0, 0, HALF_W, WIN_H)          -- 2❷ SSR
--   MotionBlur.Process(0, 0, HALF_W, WIN_H)   -- 3❸ MotionBlur
--   TAA.Process(0, 0, HALF_W, WIN_H)          -- 4❹ TAA history (双 instance 隔离)
--
--   -- Player 2 (右半屏, x=HALF_W, w=HALF_W) — 同理, 但 profile 不同
--   ...
--
--   Gfx.SetViewport(0, 0, WIN_W, WIN_H)       -- 复位 (HDR.EndScene tonemap 仍全屏)
--   HDR.EndScene()                            -- 仅 tonemap (auto-Bloom/SSR/MB/TAA 均 off)
--
-- 控制:
--   R             : 重置两个 instance history
--   ESC           : 退出
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
    print('[demo_taa_split2] Light.Graphics not available')
    print('demo_taa_split2 ok (no graphics)')
    return
end

local HDR = Gfx.HDR
local TAA = Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_split2] need HDR + TAA subtables')
    print('demo_taa_split2 ok (subtable missing)')
    return
end

-- 检查 Phase F.0.10.2 新 API 完整可用
local function api_missing(t, name)
    if type(t[name]) ~= 'function' then return true end
    return false
end

-- F.0.10.2 基础 API 依赖
if api_missing(TAA, 'CreateInstance')
or api_missing(TAA, 'SetActiveInstance')
or api_missing(TAA, 'Process')                  -- F.0.10.2 新增
or api_missing(HDR, 'SetAutoTAA')               -- F.0.10.2 新增
or api_missing(Gfx, 'SetViewport') then         -- F.0.10.2 Phase 1
    print('[demo_taa_split2] Phase F.0.10.2 API missing (need TAA.{CreateInstance, SetActiveInstance, Process}, HDR.SetAutoTAA, Graphics.SetViewport)')
    print('demo_taa_split2 ok (legacy build, F.0.10.2 not available)')
    return
end

-- F.0.10.3 新 API 依赖 (Bloom/SSR/MB region 化) — 未启时降级成 F.0.10.2 纯 TAA 演示
local Bloom = Gfx.Bloom
local SSR   = Gfx.SSR
local MB    = Gfx.MotionBlur
local hasF10_3 = (type(Bloom) == 'table') and (type(SSR) == 'table') and (type(MB) == 'table')
             and (not api_missing(Bloom, 'Process'))
             and (not api_missing(SSR,   'Process'))
             and (not api_missing(MB,    'Process'))
             and (not api_missing(HDR, 'SetAutoBloom'))
             and (not api_missing(HDR, 'SetAutoSSR'))
             and (not api_missing(HDR, 'SetAutoMotionBlur'))
if hasF10_3 then
    print('[demo_taa_split2] Phase F.0.10.3 API ready (Bloom/SSR/MB region + HDR.SetAuto*)')
else
    print('[demo_taa_split2] Phase F.0.10.3 API not full (demo 降级为 F.0.10.2 纯 TAA)')
end

print('==== ChocoLight Phase F.0.10.4 True Physical Split-Screen Demo ====')
print('  (TAA + Bloom + SSR + MotionBlur all per-region with different profiles)')
print('[demo_taa_split2] Backend          = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_split2] HDR.IsSupported  = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_split2] TAA.IsSupported  = ' .. tostring(TAA.IsSupported()))
print('[demo_taa_split2] Initial autoTAA  = ' .. tostring(HDR.GetAutoTAA()))

-- ============================================================================
-- Headless API 探针: 在 Window 不可用 / Window.Open 失败时都跑 (CI smoke 路径)
-- F.0.10.4 拓展: 补 Bloom/SSR/MB region API 探针
-- ============================================================================
local function run_headless_api_probe()
    print('[demo_taa_split2] running headless API probe (no window)')

    -- F.0.10.2 检查 HDR.SetAutoTAA round-trip
    HDR.SetAutoTAA(false)
    if HDR.GetAutoTAA() ~= false then
        print('  FAIL: SetAutoTAA(false) round-trip failed')
    else
        print('  PASS: HDR.SetAutoTAA(false) round-trip ok')
    end
    HDR.SetAutoTAA(true)

    -- F.0.10.2 TAA.Process 在 HDR 未启用时返 nil + err
    local ok, err = TAA.Process(0, 0, 100, 100)
    if ok == nil and type(err) == 'string' then
        print('  PASS: TAA.Process(region) headless returns nil + err: ' .. err)
    else
        print('  FAIL: TAA.Process expected nil + err, got ' .. tostring(ok))
    end

    -- 创建/销毁 2 个 instance 验证 multi-instance API 仍工作
    local id1 = TAA.CreateInstance()
    local id2 = TAA.CreateInstance()
    print(string.format('  CreateInstance x2 -> %s, %s', tostring(id1), tostring(id2)))
    if id1 then TAA.DestroyInstance(id1) end
    if id2 then TAA.DestroyInstance(id2) end

    -- F.0.10.4 新增: Bloom / SSR / MB region API 探针
    if hasF10_3 then
        -- HDR.SetAutoBloom round-trip
        HDR.SetAutoBloom(false)
        if HDR.GetAutoBloom() ~= false then print('  FAIL: SetAutoBloom round-trip')
        else                                print('  PASS: HDR.SetAutoBloom(false) round-trip ok') end
        HDR.SetAutoBloom(true)

        -- HDR.SetAutoSSR round-trip
        HDR.SetAutoSSR(false)
        if HDR.GetAutoSSR() ~= false then print('  FAIL: SetAutoSSR round-trip')
        else                              print('  PASS: HDR.SetAutoSSR(false) round-trip ok') end
        HDR.SetAutoSSR(true)

        -- HDR.SetAutoMotionBlur round-trip
        HDR.SetAutoMotionBlur(false)
        if HDR.GetAutoMotionBlur() ~= false then print('  FAIL: SetAutoMotionBlur round-trip')
        else                                     print('  PASS: HDR.SetAutoMotionBlur(false) round-trip ok') end
        HDR.SetAutoMotionBlur(true)

        -- Bloom/SSR/MB.Process 在 HDR 未启时返 nil + err
        local function probe_process(name, fn)
            local r, e = fn(0, 0, 100, 100)
            if r == nil and type(e) == 'string' then
                print(string.format('  PASS: %s.Process(region) headless returns nil + err: %s', name, e))
            else
                print(string.format('  FAIL: %s.Process expected nil + err, got %s', name, tostring(r)))
            end
        end
        probe_process('Bloom', Bloom.Process)
        probe_process('SSR',   SSR.Process)
        probe_process('MB',    MB.Process)
    else
        print('  SKIP: F.0.10.3 API not present (legacy build)')
    end
end

if not UI or not UI.Window then
    run_headless_api_probe()
    print('demo_taa_split2 ok (headless API check, no UI.Window)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 1280, 540
local pok, win, err = pcall(function()
    return Window.Open(WIN_W, WIN_H, 'Phase F.0.10.4 - True Physical Split-Screen: TAA+Bloom+SSR+MB per-region')
end)
if not pok then
    -- Window.Open 抛异常 (常因 no GL context, CI headless 环境典型情况)
    print('[demo_taa_split2] Window.Open raised: ' .. tostring(win))
    run_headless_api_probe()
    print('demo_taa_split2 ok (no GL context, fallback API probe)')
    return
end
if not win then
    print('[demo_taa_split2] Window.Open returned nil: ' .. tostring(err))
    run_headless_api_probe()
    print('demo_taa_split2 ok (no window, fallback API probe)')
    return
end

local HALF_W = math.floor(WIN_W / 2)

-- ============================================================================
-- 几何 (与 demo_taa_split 一致, 方便视觉对比)
-- ============================================================================
local function buildCube(s)
    s = s or 0.5
    local v, idx = {}, {}
    local function addQuad(p1, p2, p3, p4, n)
        local base = #v / 12
        for _, p in ipairs({p1, p2, p3, p4}) do
            v[#v+1]=p[1]; v[#v+1]=p[2]; v[#v+1]=p[3]
            v[#v+1]=n[1]; v[#v+1]=n[2]; v[#v+1]=n[3]
            v[#v+1]=0;    v[#v+1]=0
            v[#v+1]=1;    v[#v+1]=1; v[#v+1]=1; v[#v+1]=1
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
    local v = {
        -s, 0, -s,  0, 1, 0,  0, 0,  0.04, 0.04, 0.05, 1,
         s, 0, -s,  0, 1, 0,  1, 0,  0.04, 0.04, 0.05, 1,
         s, 0,  s,  0, 1, 0,  1, 1,  0.04, 0.04, 0.05, 1,
        -s, 0,  s,  0, 1, 0,  0, 1,  0.04, 0.04, 0.05, 1,
    }
    return v, {1, 2, 3, 1, 3, 4}
end

local cubeMesh, barMesh, planeMesh
do
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_taa_split2] Gfx.Mesh.New not available')
        win:Close()
        return
    end
    local cv, ci = buildCube(0.5)
    local bv, bi = buildCube(1.0)
    local pv, pi = buildPlane()
    cubeMesh   = Mesh.New(cv, ci)
    barMesh    = Mesh.New(bv, bi)
    planeMesh  = Mesh.New(pv, pi)
    if not cubeMesh or not barMesh or not planeMesh then
        print('[demo_taa_split2] mesh build failed')
        win:Close()
        return
    end
end

-- ============================================================================
-- HDR + TAA 初始化
-- ============================================================================
local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_taa_split2] HDR.Enable = ' .. tostring(hdrEnabled))

-- F.0.10.2 关键: 关 auto-TAA, 让本 demo 手动 TAA.Process 控时序
HDR.SetAutoTAA(false)
print('[demo_taa_split2] HDR.SetAutoTAA(false) -- 手动控 TAA (双 instance per-region)')

-- F.0.10.3 关键: 关 auto-Bloom/SSR/MB, 让本 demo 手动 Process(rgn) 控时序 (双 player 不同 profile)
-- 任一启用失败都自动降级 (老路径全屏 auto)
local bloomEnabled, ssrEnabled, mbEnabled = false, false, false
if hasF10_3 and hdrEnabled then
    -- Bloom: 开启 + 关 auto (后续每帧手动 region Process)
    if Bloom.IsSupported() and Bloom.Enable(WIN_W, WIN_H) then
        bloomEnabled = true
        HDR.SetAutoBloom(false)
        print('[demo_taa_split2] Bloom enabled + SetAutoBloom(false) -- 手动 region Bloom')
    end
    -- SSR: 开启 + 关 auto
    if SSR.IsSupported() and SSR.Enable(WIN_W, WIN_H) then
        ssrEnabled = true
        HDR.SetAutoSSR(false)
        print('[demo_taa_split2] SSR enabled + SetAutoSSR(false) -- 手动 region SSR')
    end
    -- MotionBlur: 开启 + 关 auto (默认 autoEnable=false, 但显式关掉避免日后默认变更影响)
    if MB.IsSupported() and MB.Enable(WIN_W, WIN_H) then
        mbEnabled = true
        HDR.SetAutoMotionBlur(false)
        print('[demo_taa_split2] MotionBlur enabled + SetAutoMotionBlur(false) -- 手动 region MB')
    end
end

if type(Gfx.SetPerspective) == 'function' then
    -- aspect ratio = HALF_W / WIN_H (因为每半屏是独立 viewport, 各自 16:9 比例的一半)
    Gfx.SetPerspective(60, HALF_W / WIN_H, 0.1, 100.0)
end
if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
if type(Gfx.SetDirectionalLight) == 'function' then
    Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
end

-- 创建 2 个 TAA instance (player 1 = id1, player 2 = id2)
local p1_id = TAA.CreateInstance()
local p2_id = TAA.CreateInstance()
print(string.format('[demo_taa_split2] Created TAA instances: p1=%s, p2=%s', tostring(p1_id), tostring(p2_id)))
if not p1_id or not p2_id then
    print('[demo_taa_split2] TAA.CreateInstance failed, fallback to single')
    if p1_id then TAA.DestroyInstance(p1_id) end
    if p2_id then TAA.DestroyInstance(p2_id) end
    HDR.SetAutoTAA(true)
    win:Close()
    return
end

-- 每个 instance 启用 history RT (full scene size; region 写入由 scissor 限制)
-- Profile: player 1 用强 sharpen (RCAS), player 2 用 Lanczos 高画质上采样
local function setup_p1()
    TAA.SetActiveInstance(p1_id)
    TAA.Enable(WIN_W, WIN_H)
    TAA.SetClipMode('ycocg')
    TAA.SetSharpness(1.2)
    TAA.SetSharpenMode('rcas')           -- F.0.12 RCAS 强锐化
    TAA.SetAntiFlicker(true)
end
local function setup_p2()
    TAA.SetActiveInstance(p2_id)
    TAA.Enable(WIN_W, WIN_H)
    TAA.SetClipMode('variance')
    TAA.SetVarianceGamma(1.0)
    TAA.SetSharpness(0.0)                -- sharp=0 + halfRes=true → Lanczos 路径
    TAA.SetSharpenMode('unsharp')
    TAA.SetHalfResHistory(true)
    TAA.SetUpscaleMode('lanczos')        -- F.0.14 高画质上采样
    TAA.SetAntiFlicker(true)
end
setup_p1()
setup_p2()
TAA.SetActiveInstance(0)                  -- 切回 default 备用

-- ============================================================================
-- F.0.10.4 — 双 player 后处理 profile (注意: Bloom/SSR/MB 是 *全局* 状态,
-- 不像 TAA 那样有 multi-instance, 所以本 demo 通过"每帧切换 profile"实现差异化:
--   切到 player 1 渲染前 -> apply_player1_postfx_profile()
--   切到 player 2 渲染前 -> apply_player2_postfx_profile()
-- 后处理参数切换 < 1us, 不影响性能)
-- ============================================================================
local function apply_p1_postfx_profile()
    -- Player 1: 电影感冲击 — 强 Bloom + 中速 SSR + 强 MotionBlur
    if bloomEnabled then
        Bloom.SetIntensity(1.5)          -- 强辉光
        Bloom.SetThreshold(0.8)          -- 低阈值: 中等亮度也起辉
        Bloom.SetRadius(1.5)             -- 大半径 (更扩散)
    end
    if ssrEnabled then
        SSR.SetIntensity(0.6)            -- 中等强度反射
        if SSR.SetTemporalEnabled then SSR.SetTemporalEnabled(false) end  -- 关 temporal (老化感)
    end
    if mbEnabled then
        MB.SetStrength(0.8)              -- 强 motion blur (电影感运动模糊)
        MB.SetSampleCount(12)            -- 高样本数 (减 artifact)
    end
end
local function apply_p2_postfx_profile()
    -- Player 2: 高清细腻 — 轻 Bloom + 高质 temporal SSR + 无 MotionBlur
    if bloomEnabled then
        Bloom.SetIntensity(0.4)          -- 轻辉光 (突出 cube/bar 细节)
        Bloom.SetThreshold(1.5)          -- 高阈值: 仅最亮区起辉
        Bloom.SetRadius(0.8)             -- 小半径 (更集中)
    end
    if ssrEnabled then
        SSR.SetIntensity(1.0)            -- 强反射
        if SSR.SetTemporalEnabled then SSR.SetTemporalEnabled(true)  end  -- 开 temporal 降噪
    end
    if mbEnabled then
        MB.SetStrength(0.0)              -- 关 motion blur (静止感)
    end
end

-- ============================================================================
-- 主循环
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

local cubeAngle, barAngle = 0.0, 0.0

local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

-- 公用场景绘制 (双视角共享同一场景, 仅相机不同)
local function drawScene()
    if planeMesh then
        Gfx.Push(); Gfx.SetColor(0.04, 0.04, 0.05, 1.0); planeMesh:Draw(0); Gfx.Pop()
    end
    -- 中央旋转金色 cube
    Gfx.Push()
    Gfx.Translate(0.0, 0.6, 0.0)
    Gfx.Rotate(math.deg(cubeAngle), 0, 1, 0)
    Gfx.Scale(1.2, 1.2, 1.2)
    Gfx.SetColor(1.0, 0.9, 0.7, 1.0)
    cubeMesh:Draw(0)
    Gfx.Pop()
    -- 8 根彩虹薄棒
    local R = 2.5
    for i = 1, 8 do
        local theta = barAngle + (i - 1) * math.pi * 2.0 / 8.0
        local bx = math.cos(theta) * R
        local bz = math.sin(theta) * R
        local c = BAR_COLORS[i]
        Gfx.Push()
        Gfx.Translate(bx, 0.6, bz)
        Gfx.Rotate(math.deg(theta) + 90, 0, 1, 0)
        Gfx.Scale(0.04, 1.2, 0.04)
        Gfx.SetColor(c[1], c[2], c[3], 1.0)
        barMesh:Draw(0)
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)
end

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt  = now - lastTime
    lastTime  = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    cubeAngle = cubeAngle + dt * math.rad(30)
    barAngle  = barAngle  + dt * math.rad(60)

    win:PollEvents()
    if win:IsKeyPressed('escape') then win:Close(); break end

    -- R: 重置两 instance history (Disable + re-Enable + re-apply)
    if keyTap('r') then
        TAA.SetActiveInstance(p1_id); if TAA.IsEnabled() then TAA.Disable() end; setup_p1()
        TAA.SetActiveInstance(p2_id); if TAA.IsEnabled() then TAA.Disable() end; setup_p2()
        TAA.SetActiveInstance(0)
        print('[demo_taa_split2] Reset both instance history (R pressed)')
    end

    -- ========== 渲染 ==========
    win:BeginFrame(0.02, 0.02, 0.03, 1.0)

    -- HDR.BeginScene 已被 BeginFrame 自动调 (全屏清屏到 HDR fbo)

    -- ===== Player 1 (左半屏) =====
    Gfx.SetViewport(0, 0, HALF_W, WIN_H)
    TAA.SetActiveInstance(p1_id)
    TAA.ApplyJitter()                    -- 应用 instance 1 的 jitter
    if type(Gfx.SetCamera) == 'function' then
        -- player 1: 略高+远的视角 (展示 overhead)
        Gfx.SetCamera(2.5, 3.0, 5.5, 0.0, 0.6, 0.0)
    end
    drawScene()
    -- F.0.10.4 — 后处理流水线 region 化 (顺序与 HDR.EndScene auto 路径一致):
    --   Bloom -> SSR -> MotionBlur -> TAA
    if bloomEnabled or ssrEnabled or mbEnabled then
        apply_p1_postfx_profile()
        if bloomEnabled then Bloom.Process(0, 0, HALF_W, WIN_H)      end
        if ssrEnabled   then SSR.Process(0, 0, HALF_W, WIN_H)        end
        if mbEnabled    then MB.Process(0, 0, HALF_W, WIN_H)         end
    end
    TAA.Process(0, 0, HALF_W, WIN_H)    -- region TAA: 仅更新左半 instance 1 history

    -- ===== Player 2 (右半屏) =====
    Gfx.SetViewport(HALF_W, 0, HALF_W, WIN_H)
    TAA.SetActiveInstance(p2_id)
    TAA.ApplyJitter()                    -- 应用 instance 2 的 jitter (与 p1 独立)
    if type(Gfx.SetCamera) == 'function' then
        -- player 2: 低角度近景 (展示 ground-level)
        Gfx.SetCamera(-3.5, 1.0, 3.0, 0.0, 0.8, 0.0)
    end
    drawScene()
    -- F.0.10.4 — 后处理 region 化, p2 profile (高清细腻, 顺序同上)
    if bloomEnabled or ssrEnabled or mbEnabled then
        apply_p2_postfx_profile()
        if bloomEnabled then Bloom.Process(HALF_W, 0, HALF_W, WIN_H) end
        if ssrEnabled   then SSR.Process(HALF_W, 0, HALF_W, WIN_H)   end
        if mbEnabled    then MB.Process(HALF_W, 0, HALF_W, WIN_H)    end
    end
    TAA.Process(HALF_W, 0, HALF_W, WIN_H) -- region TAA: 仅更新右半 instance 2 history

    -- 复位全屏 viewport, HDR.EndScene 仅做 tonemap (auto-Bloom/SSR/MB/TAA 已全部 off)
    Gfx.SetViewport(0, 0, WIN_W, WIN_H)
    TAA.SetActiveInstance(0)              -- 切回 default 避免污染下次

    -- ========== HUD ==========
    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        line('===== Phase F.0.10.4 True Physical Split-Screen Demo =====')
        line('  (TAA + Bloom + SSR + MotionBlur all per-region)')
        line(string.format('Window: %dx%d | Half: %dx%d', WIN_W, WIN_H, HALF_W, WIN_H))
        line(string.format('  TAA      : auto=%s, instances=%d',
            tostring(HDR.GetAutoTAA()), TAA.GetInstanceCount()))
        if hasF10_3 then
            line(string.format('  Bloom    : enabled=%s, autoBloom=%s',
                tostring(bloomEnabled), tostring(HDR.GetAutoBloom())))
            line(string.format('  SSR      : enabled=%s, autoSSR=%s',
                tostring(ssrEnabled), tostring(HDR.GetAutoSSR())))
            line(string.format('  Motion B.: enabled=%s, autoMB=%s',
                tostring(mbEnabled), tostring(HDR.GetAutoMotionBlur())))
        else
            line('  [F.0.10.3 API unavailable, falling back to TAA-only demo]')
        end
        line('')
        line(string.format('P1 (LEFT,  id=%d): RCAS sharp + STRONG bloom + mid SSR + STRONG MB', p1_id))
        line(string.format('P2 (RIGHT, id=%d): Lanczos     + light bloom  + temporal SSR + NO MB', p2_id))
        line('Keys: R = reset both history | ESC = quit')
        -- 右半 banner (突出 split-screen 边界)
        win:DrawText(HALF_W + 8, 8, '[P2: hi-quality side]', 0.5, 1, 0.5, 1)
    end

    win:EndFrame()
end

-- ============================================================================
-- 清理 (反向, 顺序: TAA 实例 -> 4 个 auto 开关复位 -> 4 个 Disable -> mesh)
-- ============================================================================
TAA.SetActiveInstance(0)
if p1_id then TAA.DestroyInstance(p1_id) end
if p2_id then TAA.DestroyInstance(p2_id) end

-- 复位 4 个 auto 开关到默认 (避免其他 demo 受影响)
HDR.SetAutoTAA(true)
if hasF10_3 then
    HDR.SetAutoBloom(true)
    HDR.SetAutoSSR(true)
    HDR.SetAutoMotionBlur(true)
end

-- 反向 Disable (TAA -> MB -> SSR -> Bloom -> HDR)
if TAA.IsEnabled() then TAA.Disable() end
if mbEnabled     then MB.Disable()    end
if ssrEnabled    then SSR.Disable()   end
if bloomEnabled  then Bloom.Disable() end
if hdrEnabled    then HDR.Disable()   end

if cubeMesh      then cubeMesh:Delete()  end
if barMesh       then barMesh:Delete()   end
if planeMesh     then planeMesh:Delete() end
print('demo_taa_split2 ok')
