-- ============================================================================
-- ChocoLight Phase F.0.10.9.2 — Multi-HDR-Instance Demo (Main + PIP)
-- ============================================================================
-- F.0.10.9 把 HDRRenderer 改成 multi-instance; 本 demo 真 GL 环境演示其核心能力:
--
--   Main 1600x900 HDR fbo (instance 0)        : warm LUT, exp=1.2, ACES
--   PIP  480x270  HDR fbo (instance pipId)    : cool LUT, exp=0.6, Uncharted2
--
-- 两个 fbo 真不同分辨率 (无法用 region 模拟), 每帧独立 Begin/Draw/End/Tonemap.
-- 验证 F.0.10.9 multi-instance + F.0.10.9.1 state 隔离 + F.0.10.9.x.1 LUT 跨 instance 同步.
--
-- 帧流程 (在 Window:__call 内, BeginScene(0) 已自动调):
--   1. Main:   drawScene → HDR.EndScene() → HDR.Tonemap(0,0,W,H) 主屏 warm
--   2. PIP:    SetActive(pip) → BeginScene → drawScene → EndScene → Tonemap 角落 cool
--   3. SetActive(0)  -- 切回防 EndFrame 自动 EndScene(pip) 二次跑
--
-- 控制 (GLFW 键码): L=toggle LUT  E=toggle main exposure  R=toggle PIP rotate  ESC=quit
-- ============================================================================

-- ============================================================================
-- 0. require + 模块可用性检测
-- ============================================================================
local function safe_require(n)
    local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil
end
local UI   = safe_require('Light.UI')
local Gfx  = safe_require('Light.Graphics')

if not Gfx then
    print('[demo_multi_hdr_pip] Light.Graphics not available')
    print('demo_multi_hdr_pip ok (no graphics)')
    return
end

local HDR = Gfx.HDR
if type(HDR) ~= 'table' then
    print('[demo_multi_hdr_pip] need HDR subtable')
    print('demo_multi_hdr_pip ok (subtable missing)')
    return
end

-- F.0.10.9 multi-instance API 完整性检测
local function api_missing(t, name) return type(t[name]) ~= 'function' end
if api_missing(HDR, 'CreateInstance') or api_missing(HDR, 'DestroyInstance')
or api_missing(HDR, 'SetActiveInstance') or api_missing(HDR, 'GetActiveInstance')
or api_missing(HDR, 'GetInstanceCount') then
    print('[demo_multi_hdr_pip] Phase F.0.10.9 multi-instance API missing')
    print('demo_multi_hdr_pip ok (legacy build, F.0.10.9 not available)')
    return
end

print('==== ChocoLight Phase F.0.10.9.2 Multi-HDR-Instance Demo (Main + PIP) ====')
print('[demo_multi_hdr_pip] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_multi_hdr_pip] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))

-- ============================================================================
-- 1. Headless API probe (无 GL ctx / Window 不可用时跑, CI smoke 路径)
-- ============================================================================
local function run_headless_api_probe()
    print('[demo_multi_hdr_pip] running headless API probe (no window)')

    -- 1) 默认 count=1, active=0
    if HDR.GetInstanceCount() ~= 1 or HDR.GetActiveInstance() ~= 0 then
        print('  FAIL: default state'); return
    end
    print('  PASS: default state count=1, active=0')

    -- 2) Create + SetActive + Get round-trip
    local pipId = HDR.CreateInstance()
    if pipId ~= 1 then
        print('  FAIL: CreateInstance expect=1, got ' .. tostring(pipId)); return
    end
    HDR.SetActiveInstance(pipId)
    if HDR.GetActiveInstance() ~= pipId then
        print('  FAIL: SetActiveInstance round-trip'); return
    end
    print('  PASS: CreateInstance + SetActive round-trip (pipId=' .. pipId .. ')')

    -- 3) Per-instance state 隔离 (exposure/gamma/tonemap)
    HDR.SetActiveInstance(0)
    HDR.SetExposure(1.2); HDR.SetGamma(2.2); HDR.SetTonemapper('aces')
    HDR.SetActiveInstance(pipId)
    HDR.SetExposure(0.6); HDR.SetGamma(2.4); HDR.SetTonemapper('uncharted2')
    HDR.SetActiveInstance(0)
    local me, mg, mt = HDR.GetExposure(), HDR.GetGamma(), HDR.GetTonemapper()
    HDR.SetActiveInstance(pipId)
    local pe, pg, pt = HDR.GetExposure(), HDR.GetGamma(), HDR.GetTonemapper()
    if math.abs(me - 1.2) < 1e-3 and math.abs(mg - 2.2) < 1e-3 and mt == 'aces'
       and math.abs(pe - 0.6) < 1e-3 and math.abs(pg - 2.4) < 1e-3 and pt == 'uncharted2' then
        print('  PASS: per-instance state 隔离 ok (exp/gamma/tonemap)')
    else
        print(string.format('  FAIL: state mismatch main=(%.2f,%.2f,%s) pip=(%.2f,%.2f,%s)', me, mg, mt, pe, pg, pt))
    end

    -- 4) Per-instance LUT id 隔离 (mock id)
    HDR.SetActiveInstance(0);     HDR.SetGradingLUT(11111, 0.8)
    HDR.SetActiveInstance(pipId); HDR.SetGradingLUT(22222, 0.8)
    HDR.SetActiveInstance(0); local mlut = HDR.GetGradingLUTId()
    HDR.SetActiveInstance(pipId); local plut = HDR.GetGradingLUTId()
    if mlut == 11111 and plut == 22222 then
        print('  PASS: per-instance LUT id 隔离 (main=11111, pip=22222)')
    else
        print(string.format('  FAIL: LUT id mismatch main=%d pip=%d', mlut, plut))
    end

    -- 5) Cleanup
    HDR.SetActiveInstance(0); HDR.SetGradingLUT(0, 0.0)
    HDR.DestroyInstance(pipId)
    if HDR.GetInstanceCount() == 1 then
        print('  PASS: cleanup ok (count back to 1)')
    else
        print('  FAIL: cleanup count=' .. HDR.GetInstanceCount())
    end
end

-- ============================================================================
-- 2. 检测 UI.Window + Light global; 缺则走 headless probe 兜底
-- ============================================================================
if not UI or not UI.Window then
    run_headless_api_probe()
    print('demo_multi_hdr_pip ok (headless API check, no UI.Window)')
    return
end

-- Light 全局 callable (OOP 框架) — Light(Light.UI.Window):New() 创建子类必需
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_multi_hdr_pip] Light global (OOP framework) not available')
    run_headless_api_probe()
    print('demo_multi_hdr_pip ok (no Light global)')
    return
end

-- ============================================================================
-- 3. 全局常量 + mesh 模板 (复用 demo_taa_split2)
-- ============================================================================
local WIN_W, WIN_H   = 1600, 900      -- 主窗口尺寸
local MAIN_W, MAIN_H = 1600, 900      -- 主屏 HDR fbo (与窗口同)
local PIP_W, PIP_H   = 480,  270      -- PIP HDR fbo (真低分辨率)

-- PIP 在默认 backbuffer 上的显示区域 (右上角, 16:9 缩比)
local PIP_DISP_W = 320
local PIP_DISP_H = 180
local PIP_DISP_X = WIN_W - PIP_DISP_W - 20   -- 右上角, 距右边 20px
local PIP_DISP_Y = WIN_H - PIP_DISP_H - 20   -- 右上角 GL 坐标 = 距底部 20px (Tonemap 用 GL 坐标系)

-- 几何: cube + plane (复用 demo_taa_split2 思路)
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

local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

-- ============================================================================
-- 4. Demo 类 (继承 Light.UI.Window, OOP callback model)
-- ============================================================================
local Demo = Light(Light.UI.Window):New()

-- 共享状态 (用 upvalue 而非 self.xxx, 因 Lua OOP 框架 self 字段可能被框架占用)
local g_pipId        = 0
local g_warmLut      = nil
local g_coolLut      = nil
local g_cubeMesh     = nil
local g_barMesh      = nil
local g_planeMesh    = nil
local g_cubeAngle    = 0.0
local g_barAngle     = 0.0
local g_pipCamAngle  = 0.0
local g_lutOn        = true
local g_mainExpDim   = false
local g_pipRotate    = true
local g_initOk       = false   -- HDR 初始化成功标志 (失败则 Draw 跳过)
local g_logged_setup = false   -- 一次性日志

-- ============================================================================
-- 5. OnOpen: 初始化 mesh + HDR multi-instance + LUT + per-instance state
-- ============================================================================
function Demo:OnOpen()
    print('[demo_multi_hdr_pip] OnOpen: initializing meshes, HDR instances, LUTs')

    -- 5.1 Mesh
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_multi_hdr_pip] Gfx.Mesh.New not available, demo abort'); self:Close(); return
    end
    local cv, ci = buildCube(0.5)
    local bv, bi = buildCube(1.0)
    local pv, pi = buildPlane()
    g_cubeMesh  = Mesh.New(cv, ci)
    g_barMesh   = Mesh.New(bv, bi)
    g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_barMesh or not g_planeMesh then
        print('[demo_multi_hdr_pip] mesh build failed'); self:Close(); return
    end

    -- 5.2 HDR multi-instance
    if not HDR.IsSupported() then
        print('[demo_multi_hdr_pip] HDR not supported on this backend, demo abort'); self:Close(); return
    end
    -- 主屏 (instance 0)
    if not HDR.Enable(MAIN_W, MAIN_H) then
        print('[demo_multi_hdr_pip] Main HDR.Enable failed'); self:Close(); return
    end
    -- PIP (instance pipId)
    g_pipId = HDR.CreateInstance()
    if not g_pipId or g_pipId <= 0 then
        print('[demo_multi_hdr_pip] CreateInstance failed'); self:Close(); return
    end
    HDR.SetActiveInstance(g_pipId)
    if not HDR.Enable(PIP_W, PIP_H) then
        print('[demo_multi_hdr_pip] PIP HDR.Enable failed')
        HDR.DestroyInstance(g_pipId); g_pipId = 0; self:Close(); return
    end
    HDR.SetActiveInstance(0)
    print(string.format('[demo_multi_hdr_pip] Main fbo=%dx%d (id=0) + PIP fbo=%dx%d (id=%d) ready',
          MAIN_W, MAIN_H, PIP_W, PIP_H, g_pipId))

    -- 5.3 关 auto tonemap (两 instance 都手动 tonemap 应用 per-instance LUT)
    HDR.SetActiveInstance(0);       HDR.SetAutoTonemap(false)
    HDR.SetActiveInstance(g_pipId); HDR.SetAutoTonemap(false)
    HDR.SetActiveInstance(0)

    -- 5.4 LUT 加载 (lut id 全 global, 任何 instance 可引用)
    if HDR.LoadCubeLUT then
        g_warmLut = HDR.LoadCubeLUT('samples/demo_multi_hdr_pip/luts/warm_red.cube')
        g_coolLut = HDR.LoadCubeLUT('samples/demo_multi_hdr_pip/luts/cool_blue.cube')
        print(string.format('[demo_multi_hdr_pip] LUT loaded: warm=%s, cool=%s',
              tostring(g_warmLut), tostring(g_coolLut)))
    end

    -- 5.5 Per-instance state setup
    HDR.SetActiveInstance(0)
    HDR.SetExposure(1.2)
    HDR.SetGamma(2.2)
    HDR.SetTonemapper('aces')
    if g_warmLut then HDR.SetGradingLUT(g_warmLut, 0.85) end

    HDR.SetActiveInstance(g_pipId)
    HDR.SetExposure(0.6)
    HDR.SetGamma(2.4)
    HDR.SetTonemapper('uncharted2')
    if g_coolLut then HDR.SetGradingLUT(g_coolLut, 0.85) end

    HDR.SetActiveInstance(0)

    -- 5.6 相机 + 光照
    if type(Gfx.SetPerspective) == 'function' then
        Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0)
    end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end

    g_initOk = true
    print('[demo_multi_hdr_pip] OnOpen: setup ok, entering render loop')
end

-- ============================================================================
-- 6. Update: 每帧动画 + 旋转角度
-- ============================================================================
function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_cubeAngle = g_cubeAngle + dt * math.rad(30)
    g_barAngle  = g_barAngle  + dt * math.rad(60)
    if g_pipRotate then g_pipCamAngle = g_pipCamAngle + dt * math.rad(20) end
end

-- ============================================================================
-- 7. 公用场景绘制 (双视角共享)
-- ============================================================================
local function drawScene()
    if g_planeMesh then
        Gfx.Push(); Gfx.SetColor(0.04, 0.04, 0.05, 1.0); g_planeMesh:Draw(0); Gfx.Pop()
    end
    -- 中央旋转金色 cube
    Gfx.Push()
    Gfx.Translate(0.0, 0.6, 0.0)
    Gfx.Rotate(math.deg(g_cubeAngle), 0, 1, 0)
    Gfx.Scale(1.2, 1.2, 1.2)
    Gfx.SetColor(1.0, 0.9, 0.7, 1.0)
    g_cubeMesh:Draw(0)
    Gfx.Pop()
    -- 8 根彩虹薄棒环绕
    local R = 2.5
    for i = 1, 8 do
        local theta = g_barAngle + (i - 1) * math.pi * 2.0 / 8.0
        local bx = math.cos(theta) * R
        local bz = math.sin(theta) * R
        local c = BAR_COLORS[i]
        Gfx.Push()
        Gfx.Translate(bx, 0.6, bz)
        Gfx.Rotate(math.deg(theta) + 90, 0, 1, 0)
        Gfx.Scale(0.04, 1.2, 0.04)
        Gfx.SetColor(c[1], c[2], c[3], 1.0)
        g_barMesh:Draw(0)
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)
end

-- ============================================================================
-- 8. Draw: 主屏 + PIP 双 HDR-instance 渲染
-- ============================================================================
-- 入口状态 (Window:__call 内自动调过):
--   HDRRenderer::BeginScene() — active=0, 主屏 fbo 已 bind + clear
function Demo:Draw()
    if not g_initOk then return end   -- 初始化失败时跳过 (走默认 Lua.Graphics 绘制)

    -- ===== Main (instance 0, 主屏 fbo 1600x900) =====
    if type(Gfx.SetViewport) == 'function' then Gfx.SetViewport(0, 0, MAIN_W, MAIN_H) end
    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(3.0, 4.0, 6.0, 0.0, 0.6, 0.0)   -- 高远 overview
    end
    drawScene()
    HDR.EndScene()                              -- 手动 EndScene(0): unbind 主屏 fbo + SSAO/AE/LensFx (auto tonemap 关)
    HDR.Tonemap(0, 0, WIN_W, WIN_H)             -- 主屏 fbo → default backbuffer 全屏 (warm LUT 应用)

    -- ===== PIP (instance pipId, PIP fbo 480x270) =====
    HDR.SetActiveInstance(g_pipId)
    HDR.BeginScene()                            -- 手动 BeginScene(pipId): bind PIP fbo + clear
    if type(Gfx.SetViewport) == 'function' then Gfx.SetViewport(0, 0, PIP_W, PIP_H) end
    if type(Gfx.SetCamera) == 'function' then
        -- 低近 ground-level 相机, 绕场旋转 (R 切换)
        local r = 4.5
        Gfx.SetCamera(math.cos(g_pipCamAngle) * r, 1.2, math.sin(g_pipCamAngle) * r,
                      0.0, 0.6, 0.0)
    end
    drawScene()
    HDR.EndScene()                              -- 手动 EndScene(pipId)
    -- PIP fbo (480x270) → default backbuffer 右上角 (320x180), GL 自动 bilinear scale
    HDR.Tonemap(PIP_DISP_X, PIP_DISP_Y, PIP_DISP_W, PIP_DISP_H)

    -- 切回 instance 0; Window:__call 终止自动 EndScene(0) 会再跑一次, SSAO/AE/LensFx 未启用 → no-op
    HDR.SetActiveInstance(0)

    -- ===== HUD (Print 用 2D 屏幕坐标, 写到 default backbuffer) =====
    if type(Gfx.Print) == 'function' then
        -- 提前快照 per-instance state, 避免文本格式化期间频繁切 instance
        local me = HDR.GetExposure(); local mg = HDR.GetGamma()
        local mt = HDR.GetTonemapper(); local mlut = HDR.GetGradingLUTId()
        HDR.SetActiveInstance(g_pipId)
        local pe = HDR.GetExposure(); local pg = HDR.GetGamma()
        local pt = HDR.GetTonemapper(); local plut = HDR.GetGradingLUTId()
        HDR.SetActiveInstance(0)

        Gfx.Push()
        Gfx.SetColor(1, 1, 1, 1)
        Gfx.Print('=== Phase F.0.10.9.2 Multi-HDR-Instance Demo (Main + PIP) ===', 10, 10, 0)
        Gfx.Print(string.format('Window=%dx%d  Main fbo=%dx%d (id=0 warm)  PIP fbo=%dx%d (id=%d cool)',
              WIN_W, WIN_H, MAIN_W, MAIN_H, PIP_W, PIP_H, g_pipId), 10, 30, 0)
        Gfx.SetColor(1.0, 0.85, 0.7, 1.0)
        Gfx.Print(string.format('Main: exp=%.2f gamma=%.2f tonemap=%s LUT=%s',
              me, mg, mt, mlut ~= 0 and tostring(mlut) or 'OFF'), 10, 55, 0)
        Gfx.SetColor(0.7, 0.85, 1.0, 1.0)
        Gfx.Print(string.format('PIP : exp=%.2f gamma=%.2f tonemap=%s LUT=%s',
              pe, pg, pt, plut ~= 0 and tostring(plut) or 'OFF'), 10, 75, 0)
        Gfx.SetColor(1, 1, 1, 1)
        Gfx.Print(string.format('Instances count=%d active=%d',
              HDR.GetInstanceCount(), HDR.GetActiveInstance()), 10, 100, 0)
        Gfx.Print('Keys: L=toggle LUT  E=toggle main exp  R=toggle PIP rotate  ESC=quit', 10, 125, 0)
        Gfx.Pop()
    end
end

-- ============================================================================
-- 9. OnKey: GLFW 键码 (ESC=256, L=76, E=69, R=82)
-- ============================================================================
function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end   -- 仅按下 (action=1)

    if key == 256 then                -- ESC
        print('[demo_multi_hdr_pip] ESC pressed, closing')
        self:Close()
    elseif key == 76 then             -- L: toggle LUT
        g_lutOn = not g_lutOn
        HDR.SetActiveInstance(0)
        HDR.SetGradingLUT(g_lutOn and (g_warmLut or 0) or 0, 0.85)
        HDR.SetActiveInstance(g_pipId)
        HDR.SetGradingLUT(g_lutOn and (g_coolLut or 0) or 0, 0.85)
        HDR.SetActiveInstance(0)
        print('[demo_multi_hdr_pip] LUT toggle: ' .. tostring(g_lutOn))
    elseif key == 69 then             -- E: toggle main exposure dim
        g_mainExpDim = not g_mainExpDim
        HDR.SetActiveInstance(0)
        HDR.SetExposure(g_mainExpDim and 0.4 or 1.2)
        print('[demo_multi_hdr_pip] Main exposure: ' .. tostring(HDR.GetExposure()))
    elseif key == 82 then             -- R: toggle PIP rotate
        g_pipRotate = not g_pipRotate
        print('[demo_multi_hdr_pip] PIP rotate: ' .. tostring(g_pipRotate))
    end
end

-- ============================================================================
-- 10. Cleanup (主循环退出后调) — Lua 框架无 OnClose, 用全局函数手动调
-- ============================================================================
local function cleanup_demo()
    if not g_initOk then return end
    print('[demo_multi_hdr_pip] cleanup: releasing HDR instances, LUTs, meshes')

    -- 1) Delete LUT (F.0.10.9.x.1 RemapLUTIdAcrossInstances 跨 instance 同步清)
    if g_coolLut then HDR.DeleteLUT3D(g_coolLut); g_coolLut = nil end
    if g_warmLut then HDR.DeleteLUT3D(g_warmLut); g_warmLut = nil end

    -- 2) Disable PIP 然后 Destroy
    if g_pipId and g_pipId > 0 then
        HDR.SetActiveInstance(g_pipId); HDR.SetAutoTonemap(true); HDR.Disable()
        HDR.DestroyInstance(g_pipId); g_pipId = 0
    end

    -- 3) Disable main + restore autoTonemap
    HDR.SetActiveInstance(0); HDR.SetAutoTonemap(true); HDR.Disable()

    -- 4) Mesh
    if g_cubeMesh  then g_cubeMesh:Delete();  g_cubeMesh  = nil end
    if g_barMesh   then g_barMesh:Delete();   g_barMesh   = nil end
    if g_planeMesh then g_planeMesh:Delete(); g_planeMesh = nil end

    g_initOk = false
end

-- ============================================================================
-- 11. 主循环
-- ============================================================================
Demo:Open(WIN_W, WIN_H, 'Phase F.0.10.9.2 - Multi-HDR Demo: Main(1600x900 warm) + PIP(480x270 cool)')

while Light.UI.Loop() do
    Light.UI.Resume()
end

cleanup_demo()
print('demo_multi_hdr_pip ok')
