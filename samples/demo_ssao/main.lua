-- ============================================================================
-- ChocoLight Phase E.8 — SSAO (Screen-Space Ambient Occlusion) demo
-- ============================================================================
-- 3D PBR 场景 (代码生成 cube + plane mesh) + 旋转相机 + SSAO toggle.
--   * 显式 SetPerspective + SetCamera + SetDepthTest(true)
--   * 双 RT 旁路: HDR depth RB -> SSAO depth tex 自动 blit
--   * SSAO 在角落 / 物体之间生成自然阴影
--
-- 控制:
--   F        : 切换 SSAO on/off
--   1 / 2    : Radius        -/+ (步长 0.1, 范围 [0.05, 5.0])
--   3 / 4    : Bias          -/+ (步长 0.005, 范围 [0, 0.2])
--   5 / 6    : Intensity     -/+ (步长 0.1, 范围 [0, 4.0])
--   7 / 8    : Power         -/+ (步长 0.25, 范围 [0.5, 8.0])
--   B        : 切换 BlurEnabled
--   K        : 切换 KernelSize (8 / 16)
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
    print('[demo_ssao] Light.Graphics not available')
    print('demo_ssao ok (no graphics)')
    return
end

local HDR  = Gfx.HDR
local SSAO = Gfx.SSAO
if type(HDR) ~= 'table' or type(SSAO) ~= 'table' then
    print('[demo_ssao] need HDR + SSAO subtables')
    print('demo_ssao ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.8 SSAO demo ====')
print('[demo_ssao] Backend       = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_ssao] HDR.IsSupported  = ' .. tostring(HDR.IsSupported()))
print('[demo_ssao] SSAO.IsSupported = ' .. tostring(SSAO.IsSupported()))

if not UI or not UI.Window then
    print('[demo_ssao] UI.Window not available, API probe only')
    print('  SSAO.GetRadius        = ' .. tostring(SSAO.GetRadius()))
    print('  SSAO.GetBias          = ' .. tostring(SSAO.GetBias()))
    print('  SSAO.GetIntensity     = ' .. tostring(SSAO.GetIntensity()))
    print('  SSAO.GetKernelSize    = ' .. tostring(SSAO.GetKernelSize()))
    print('  SSAO.GetPower         = ' .. tostring(SSAO.GetPower()))
    print('  SSAO.GetBlurEnabled   = ' .. tostring(SSAO.GetBlurEnabled()))
    print('demo_ssao ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, err = Window.Open(WIN_W, WIN_H, 'Phase E.8 - SSAO Demo')
if not win then
    print('[demo_ssao] Window.Open failed: ' .. tostring(err))
    print('demo_ssao ok (no window)')
    return
end

-- ============================================================================
-- 1. 生成几何体: plane (地面) + 多个 cube (产生自然 AO 阴影)
-- ============================================================================

-- vertex format: pos3 + normal3 + uv2 + color4 = 12 floats
-- helper: build cube mesh (size = 1.0)
local function buildCube()
    local s = 0.5  -- half size
    local v = {}
    local idx = {}
    -- 6 面 × 4 顶点 = 24 verts; 每面 2 三角形 = 12 indices/face
    -- 法线指向各自轴向; UV (0,0)-(1,1); 白色
    local function addQuad(p1, p2, p3, p4, n)
        local base = #v / 12   -- 当前顶点起始索引
        -- 4 顶点
        for _, p in ipairs({p1, p2, p3, p4}) do
            for i = 1, 3 do v[#v + 1] = p[i] end       -- pos
            for i = 1, 3 do v[#v + 1] = n[i] end       -- normal
            v[#v + 1] = 0; v[#v + 1] = 0               -- uv (不重要)
            v[#v + 1] = 1; v[#v + 1] = 1
            v[#v + 1] = 1; v[#v + 1] = 1               -- color RGBA
        end
        -- 2 三角形 (1-indexed for Lua, 0-based stored internally)
        idx[#idx + 1] = base + 1; idx[#idx + 1] = base + 2; idx[#idx + 1] = base + 3
        idx[#idx + 1] = base + 1; idx[#idx + 1] = base + 3; idx[#idx + 1] = base + 4
    end
    -- +X
    addQuad({ s,-s,-s}, { s, s,-s}, { s, s, s}, { s,-s, s}, { 1, 0, 0})
    -- -X
    addQuad({-s,-s, s}, {-s, s, s}, {-s, s,-s}, {-s,-s,-s}, {-1, 0, 0})
    -- +Y
    addQuad({-s, s,-s}, {-s, s, s}, { s, s, s}, { s, s,-s}, { 0, 1, 0})
    -- -Y
    addQuad({-s,-s, s}, {-s,-s,-s}, { s,-s,-s}, { s,-s, s}, { 0,-1, 0})
    -- +Z
    addQuad({-s,-s, s}, { s,-s, s}, { s, s, s}, {-s, s, s}, { 0, 0, 1})
    -- -Z
    addQuad({ s,-s,-s}, {-s,-s,-s}, {-s, s,-s}, { s, s,-s}, { 0, 0,-1})
    return v, idx
end

-- plane (XZ 面 朝上, 大小 10x10, 中心在原点)
local function buildPlane()
    local s = 5.0
    local v = {
        -- 4 顶点: pos + normal(0,1,0) + uv + color
        -s, 0, -s,  0, 1, 0,  0, 0,  0.7, 0.7, 0.7, 1,
         s, 0, -s,  0, 1, 0,  1, 0,  0.7, 0.7, 0.7, 1,
         s, 0,  s,  0, 1, 0,  1, 1,  0.7, 0.7, 0.7, 1,
        -s, 0,  s,  0, 1, 0,  0, 1,  0.7, 0.7, 0.7, 1,
    }
    local idx = {1, 2, 3, 1, 3, 4}
    return v, idx
end

local cubeMesh = nil
local planeMesh = nil
do
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_ssao] Gfx.Mesh.New not available, skipping 3D scene')
        win:Close()
        print('demo_ssao ok (no mesh api)')
        return
    end
    local cv, ci = buildCube()
    local pv, pi = buildPlane()
    local m1, e1 = Mesh.New(cv, ci)
    local m2, e2 = Mesh.New(pv, pi)
    if not m1 then print('[demo_ssao] cube mesh failed: ' .. tostring(e1)); win:Close(); return end
    if not m2 then print('[demo_ssao] plane mesh failed: ' .. tostring(e2)); win:Close(); return end
    cubeMesh = m1
    planeMesh = m2
    print(string.format('[demo_ssao] cube: %d verts / %d idx; plane: %d verts / %d idx',
        m1:GetVertexCount(), m1:GetIndexCount(), m2:GetVertexCount(), m2:GetIndexCount()))
end

-- ============================================================================
-- 2. HDR + SSAO Enable + camera setup
-- ============================================================================

local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_ssao] HDR.Enable  = ' .. tostring(hdrEnabled))

local ssaoEnabled = hdrEnabled and SSAO.Enable(WIN_W, WIN_H) or false
print('[demo_ssao] SSAO.Enable = ' .. tostring(ssaoEnabled))

-- 3D camera (透视投影 + LookAt)
if type(Gfx.SetPerspective) == 'function' then
    Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0)
end
if type(Gfx.SetDepthTest) == 'function' then
    Gfx.SetDepthTest(true)
end

-- 主灯光 (PBR direction light)
if type(Gfx.SetDirectionalLight) == 'function' then
    Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 1.2)
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

-- 3D 场景: 8 个 cube + 1 个 plane
local SCENE = {
    -- 中心 + 周围立方体 (产生丰富 AO)
    {x =  0.0, y = 0.5, z =  0.0, scale = 1.0, color = {0.8, 0.3, 0.3}},
    {x =  2.0, y = 0.5, z =  1.5, scale = 1.0, color = {0.3, 0.8, 0.3}},
    {x = -2.0, y = 0.5, z = -1.5, scale = 1.0, color = {0.3, 0.3, 0.8}},
    {x =  1.5, y = 0.5, z = -2.0, scale = 0.7, color = {0.8, 0.8, 0.3}},
    {x = -1.5, y = 0.5, z =  2.0, scale = 0.7, color = {0.3, 0.8, 0.8}},
    -- 叠堆 (相邻几何, AO 效果最强)
    {x =  0.0, y = 1.5, z =  0.0, scale = 0.6, color = {0.9, 0.5, 0.3}},
    {x =  3.0, y = 0.5, z = -2.5, scale = 1.2, color = {0.5, 0.5, 0.9}},
    {x = -3.0, y = 0.5, z =  2.5, scale = 0.8, color = {0.9, 0.3, 0.9}},
}

local camAngle = 0.0   -- 围绕 Y 轴旋转

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt = now - lastTime
    lastTime = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    camAngle = camAngle + dt * 0.3   -- 慢转

    win:PollEvents()
    if win:IsKeyPressed('escape') then win:Close(); break end

    if keyTap('f') then
        if SSAO.IsEnabled() then SSAO.Disable(); print('[demo] SSAO OFF')
        else local ok = SSAO.Enable(WIN_W, WIN_H); print('[demo] SSAO ' .. (ok and 'ON' or 'OFF (fail)')) end
    end

    -- 1/2: Radius
    if keyTap('1') then SSAO.SetRadius(clampNum(SSAO.GetRadius() - 0.1, 0.05, 5.0)) end
    if keyTap('2') then SSAO.SetRadius(clampNum(SSAO.GetRadius() + 0.1, 0.05, 5.0)) end

    -- 3/4: Bias
    if keyTap('3') then SSAO.SetBias(clampNum(SSAO.GetBias() - 0.005, 0.0, 0.2)) end
    if keyTap('4') then SSAO.SetBias(clampNum(SSAO.GetBias() + 0.005, 0.0, 0.2)) end

    -- 5/6: Intensity
    if keyTap('5') then SSAO.SetIntensity(clampNum(SSAO.GetIntensity() - 0.1, 0.0, 4.0)) end
    if keyTap('6') then SSAO.SetIntensity(clampNum(SSAO.GetIntensity() + 0.1, 0.0, 4.0)) end

    -- 7/8: Power
    if keyTap('7') then SSAO.SetPower(clampNum(SSAO.GetPower() - 0.25, 0.5, 8.0)) end
    if keyTap('8') then SSAO.SetPower(clampNum(SSAO.GetPower() + 0.25, 0.5, 8.0)) end

    -- B: BlurEnabled
    if keyTap('b') then
        SSAO.SetBlurEnabled(not SSAO.GetBlurEnabled())
        print('[demo] SSAO blur = ' .. tostring(SSAO.GetBlurEnabled()))
    end

    -- K: KernelSize toggle 8 / 16
    if keyTap('k') then
        local k = SSAO.GetKernelSize()
        SSAO.SetKernelSize(k == 16 and 8 or 16)
        print('[demo] SSAO kernel = ' .. tostring(SSAO.GetKernelSize()))
    end

    -- R: reset
    if keyTap('r') then
        SSAO.SetRadius(0.5); SSAO.SetBias(0.025); SSAO.SetIntensity(1.0)
        SSAO.SetKernelSize(16); SSAO.SetPower(2.0); SSAO.SetBlurEnabled(true)
        print('[demo] reset defaults')
    end

    -- ========== 渲染 ==========
    win:BeginFrame(0.1, 0.12, 0.15, 1.0)

    -- 相机围绕场景旋转
    local cx = math.cos(camAngle) * 7.0
    local cz = math.sin(camAngle) * 7.0
    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(cx, 4.0, cz, 0.0, 0.5, 0.0)
    end

    -- 地面 (灰色 PBR)
    if planeMesh then
        Gfx.Push()
        Gfx.SetColor(0.7, 0.7, 0.7, 1.0)
        planeMesh:Draw(0)
        Gfx.Pop()
    end

    -- 8 个立方体 (各种颜色)
    for _, c in ipairs(SCENE) do
        Gfx.Push()
        Gfx.Translate(c.x, c.y, c.z)
        Gfx.Scale(c.scale, c.scale, c.scale)
        Gfx.SetColor(c.color[1], c.color[2], c.color[3], 1.0)
        cubeMesh:Draw(0)
        Gfx.Pop()
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD (UI / 2D 文字 - 切回 2D 模式可能需要 ResetCamera; 简化省略)
    if win.DrawText then
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end
        line(string.format('HDR: %s   SSAO: %s',
            hdrEnabled and 'ON' or 'OFF',
            SSAO.IsEnabled() and 'ON' or 'OFF'))
        line(string.format('SSAO: radius=%.2f bias=%.3f intensity=%.2f',
            SSAO.GetRadius(), SSAO.GetBias(), SSAO.GetIntensity()))
        line(string.format('SSAO: power=%.2f kernel=%d blur=%s',
            SSAO.GetPower(), SSAO.GetKernelSize(),
            SSAO.GetBlurEnabled() and 'ON' or 'OFF'))
        line('Keys: F=SSAO 1/2=radius 3/4=bias 5/6=int 7/8=power B=blur K=kernel R=reset ESC')
    end

    win:EndFrame()
end

-- ============================================================================
-- 4. 反向清理
-- ============================================================================

if SSAO.IsEnabled() then SSAO.Disable() end
if hdrEnabled then HDR.Disable() end
if cubeMesh then cubeMesh:Delete() end
if planeMesh then planeMesh:Delete() end
print('demo_ssao ok')
