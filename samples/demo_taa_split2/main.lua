-- ============================================================================
-- ChocoLight Phase F.0.10.2 — True Physical Split-Screen with Independent TAA
-- ============================================================================
-- 演示**真物理 split-screen**: 同一帧内左半屏渲染 player 1, 右半屏渲染 player 2,
-- 双 TAA instance 各自独立 history, 真正的同帧双 TAA (vs F.0.10.1 demo_taa_split 是
-- timeline cycle).
--
-- 实现要点 (F.0.10.2 新增 API):
--   1) HDR.SetAutoTAA(false)               -- 关 EndScene 自动 TAA
--   2) Graphics.SetViewport(x, y, w, h)    -- 限制 raster 到子区域 (双 viewport 关键)
--   3) TAA.Process(rgnX, rgnY, rgnW, rgnH) -- 手动 region TAA process (per-instance)
--   4) TAA.SetActiveInstance(id) + ApplyJitter() — 双 instance jitter 独立
--
-- 每帧流程:
--   HDR.BeginScene()                                  -- 全屏清屏 (HDR fbo)
--
--   -- Player 1 (左半屏)
--   Graphics.SetViewport(0, 0, W/2, H)
--   TAA.SetActiveInstance(1); TAA.ApplyJitter()       -- 用 instance 1 的 jitter
--   Gfx.SetCamera(player1_eye, player1_at)
--   -- ... draw scene from player 1 view ...
--   TAA.Process(0, 0, W/2, H)                          -- scissor 限定 instance 1 history 仅左半
--
--   -- Player 2 (右半屏)
--   Graphics.SetViewport(W/2, 0, W/2, H)
--   TAA.SetActiveInstance(2); TAA.ApplyJitter()       -- 用 instance 2 的 jitter
--   Gfx.SetCamera(player2_eye, player2_at)
--   -- ... draw scene from player 2 view ...
--   TAA.Process(W/2, 0, W/2, H)                        -- scissor 限定 instance 2 history 仅右半
--
--   Graphics.SetViewport(0, 0, W, H)                   -- 复位全屏 (HDR.EndScene 全屏 tonemap)
--   HDR.EndScene()                                     -- bloom + tonemap (auto-TAA 已关)
--
-- 控制:
--   方向键 / WASD : 移动 player 1 / player 2 视角 (待后续扩展)
--   R             : 重置两个 instance history (Disable + Enable)
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

if api_missing(TAA, 'CreateInstance')
or api_missing(TAA, 'SetActiveInstance')
or api_missing(TAA, 'Process')                  -- F.0.10.2 新增
or api_missing(HDR, 'SetAutoTAA')               -- F.0.10.2 新增
or api_missing(Gfx, 'SetViewport') then         -- F.0.10.2 Phase 1
    print('[demo_taa_split2] Phase F.0.10.2 API missing (need TAA.{CreateInstance, SetActiveInstance, Process}, HDR.SetAutoTAA, Graphics.SetViewport)')
    print('demo_taa_split2 ok (legacy build, F.0.10.2 not available)')
    return
end

print('==== ChocoLight Phase F.0.10.2 True Physical Split-Screen Demo ====')
print('[demo_taa_split2] Backend          = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_split2] HDR.IsSupported  = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_split2] TAA.IsSupported  = ' .. tostring(TAA.IsSupported()))
print('[demo_taa_split2] Initial autoTAA  = ' .. tostring(HDR.GetAutoTAA()))

-- ============================================================================
-- Headless API 探针: 在 Window 不可用 / Window.Open 失败时都跑 (CI smoke 路径)
-- ============================================================================
local function run_headless_api_probe()
    print('[demo_taa_split2] running headless API probe (no window)')

    -- 检查 HDR.SetAutoTAA round-trip
    HDR.SetAutoTAA(false)
    if HDR.GetAutoTAA() ~= false then
        print('  FAIL: SetAutoTAA(false) round-trip failed')
    else
        print('  PASS: HDR.SetAutoTAA(false) round-trip ok')
    end
    HDR.SetAutoTAA(true)

    -- TAA.Process 在 HDR 未启用时返 nil + err
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
end

if not UI or not UI.Window then
    run_headless_api_probe()
    print('demo_taa_split2 ok (headless API check, no UI.Window)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 1280, 540
local pok, win, err = pcall(function()
    return Window.Open(WIN_W, WIN_H, 'Phase F.0.10.2 - True Physical Split-Screen TAA Demo')
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
    TAA.Process(HALF_W, 0, HALF_W, WIN_H) -- region TAA: 仅更新右半 instance 2 history

    -- 复位全屏 viewport, HDR.EndScene 走 bloom + 全屏 tonemap (auto-TAA 已 off)
    Gfx.SetViewport(0, 0, WIN_W, WIN_H)
    TAA.SetActiveInstance(0)              -- 切回 default 避免污染下次

    -- ========== HUD ==========
    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        line('===== Phase F.0.10.2 True Physical Split-Screen Demo =====')
        line(string.format('Window: %dx%d | Half: %dx%d', WIN_W, WIN_H, HALF_W, WIN_H))
        line(string.format('Player 1 (LEFT, id=%d):  RCAS sharpen=1.2 (F.0.12 strong)', p1_id))
        line(string.format('Player 2 (RIGHT, id=%d): Lanczos halfRes (F.0.14 hi-quality upscale)', p2_id))
        line(string.format('HDR.GetAutoTAA = %s (false = manual region TAA per frame)',
            tostring(HDR.GetAutoTAA())))
        line(string.format('Instance count = %d (default + p1 + p2)', TAA.GetInstanceCount()))
        -- 右半文字 (展示 split-screen 边界)
        win:DrawText(HALF_W + 8, 8, '[P2 viewport: Lanczos high-quality]', 0.5, 1, 0.5, 1)
        line('Keys: R = reset both history | ESC = quit')
    end

    win:EndFrame()
end

-- ============================================================================
-- 清理 (反向)
-- ============================================================================
TAA.SetActiveInstance(0)
if p1_id then TAA.DestroyInstance(p1_id) end
if p2_id then TAA.DestroyInstance(p2_id) end
HDR.SetAutoTAA(true)                      -- 复位默认 (避免其他 demo 受影响)
if TAA.IsEnabled() then TAA.Disable() end
if hdrEnabled    then HDR.Disable()  end
if cubeMesh      then cubeMesh:Delete()  end
if barMesh       then barMesh:Delete()   end
if planeMesh     then planeMesh:Delete() end
print('demo_taa_split2 ok')
