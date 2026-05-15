-- ============================================================================
-- ChocoLight Phase F.0.10.1 — TAA Multi-Instance Split Demo
-- ============================================================================
-- 演示 4 个独立 TAA instance 各自持有 history RT + 14 sub-phase 参数,
-- 切换 active 时 history 互不污染 (vs demo_taa_compare 是单 instance 切参数会 reset history).
--
-- 4 个 Instance Profile (启动自动创建/配置):
--   Instance 0 (default): F.0 baseline (ycocg + sharp=0.5)
--   Instance 1: 高画质 Lanczos (sharp=0, halfRes=true, upscale=lanczos)  ← F.0.14 重点
--   Instance 2: 强锐化 RCAS  (sharp=1.5, sharpenMode=rcas)               ← F.0.12 重点
--   Instance 3: motion adaptive 全开 (variance + motionAdaptive + motionAdaptiveSharpness) ← F.0.8/0.13 重点
--
-- 场景: 中央旋转金色 cube + 8 根彩虹薄棒 + 黑色地面 (与 demo_taa_compare 一致, 方便对比).
--
-- 控制:
--   1-4     : 切到 active instance 0/1/2/3
--   R       : Disable 当前 instance + 重新 apply profile (彻底 reset history)
--   C       : 销毁所有 user instance (1/2/3), 回 default + 重新自动创建
--   ESC     : 退出
--
-- 与 demo_taa_compare 的核心区别:
--   * compare: 单 instance, 切 preset 会重置 history → 需要 30 帧 stabilize
--   * split:   4 instance 各自持续累积 history → 切 active 时立即看到稳定画面 (无 ghosting reset)
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
    print('[demo_taa_split] Light.Graphics not available')
    print('demo_taa_split ok (no graphics)')
    return
end

local HDR = Gfx.HDR
local TAA = Gfx.TAA
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_taa_split] need HDR + TAA subtables')
    print('demo_taa_split ok (subtable missing)')
    return
end

-- 检查 Phase F.0.10 multi-instance API 存在 (老 build fallback 跳过)
if type(TAA.CreateInstance) ~= 'function'
   or type(TAA.SetActiveInstance) ~= 'function'
   or type(TAA.GetActiveInstance) ~= 'function'
   or type(TAA.DestroyInstance) ~= 'function'
   or type(TAA.GetInstanceCount) ~= 'function' then
    print('[demo_taa_split] Phase F.0.10 multi-instance API missing (need: CreateInstance/SetActiveInstance/GetActiveInstance/DestroyInstance/GetInstanceCount)')
    print('demo_taa_split ok (legacy build, multi-instance not available)')
    return
end

print('==== ChocoLight Phase F.0.10.1 TAA Multi-Instance Split Demo ====')
print('[demo_taa_split] Backend          = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_taa_split] HDR.IsSupported  = ' .. tostring(HDR.IsSupported()))
print('[demo_taa_split] TAA.IsSupported  = ' .. tostring(TAA.IsSupported()))
print('[demo_taa_split] Instance count   = ' .. tostring(TAA.GetInstanceCount()))

-- Headless 模式: 验证 instance API 后退出 (CI smoke)
if not UI or not UI.Window then
    print('[demo_taa_split] UI.Window not available, API probe only')
    -- 验证 instance API 在 headless 下也能创建槽位
    local id1 = TAA.CreateInstance()
    local id2 = TAA.CreateInstance()
    local id3 = TAA.CreateInstance()
    print(string.format('  CreateInstance x3 -> %s, %s, %s', tostring(id1), tostring(id2), tostring(id3)))
    print('  GetInstanceCount = ' .. tostring(TAA.GetInstanceCount()))
    if id1 then TAA.DestroyInstance(id1) end
    if id2 then TAA.DestroyInstance(id2) end
    if id3 then TAA.DestroyInstance(id3) end
    print('  After cleanup count = ' .. tostring(TAA.GetInstanceCount()))
    print('demo_taa_split ok (headless API check)')
    return
end

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local pok, win, err = pcall(function() return Window.Open(WIN_W, WIN_H, 'Phase F.0.10.1 - TAA Multi-Instance Split Demo') end)
if not pok then
    print('[demo_taa_split] Window.Open raised: ' .. tostring(win))
    print('demo_taa_split ok (no window)')
    return
end
if not win then
    print('[demo_taa_split] Window.Open returned nil: ' .. tostring(err))
    print('demo_taa_split ok (no window)')
    return
end

-- ============================================================================
-- 1. 几何 (与 demo_taa_compare 完全一致, 方便侧对比)
-- ============================================================================

-- vertex format: pos3 + normal3 + uv2 + color4 = 12 floats
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
        print('[demo_taa_split] Gfx.Mesh.New not available')
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
        print('[demo_taa_split] mesh build failed')
        win:Close()
        return
    end
end

-- ============================================================================
-- 2. HDR + camera + 灯光
-- ============================================================================

local hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
print('[demo_taa_split] HDR.Enable = ' .. tostring(hdrEnabled))

if type(Gfx.SetPerspective) == 'function' then
    Gfx.SetPerspective(60, WIN_W / WIN_H, 0.1, 100.0)
end
if type(Gfx.SetDepthTest) == 'function' then
    Gfx.SetDepthTest(true)
end
if type(Gfx.SetDirectionalLight) == 'function' then
    Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
end

-- ============================================================================
-- 3. 4 个 Instance Profile (default + 3 user)
-- ============================================================================

-- 每个 profile: 描述 + TAA 参数 (apply_instance 内 SetActiveInstance + SetXxx)
local PROFILES = {
    [0] = {
        name = 'Default (F.0 baseline)',
        desc = 'ycocg AABB clip + sharpness=0.5 (推荐默认)',
        clipMode = 'ycocg', sharpness = 0.5, antiFlicker = true,
        sharpenMode = 'unsharp', halfResHistory = false, upscaleMode = 'bilinear',
        motionAdaptive = false, motionAdaptiveSharpness = false,
    },
    [1] = {
        name = 'Instance 1 - Lanczos Upscale (F.0.14)',
        desc = 'sharp=0 + halfRes=true + lanczos 25-tap upscaler (-55% blur)',
        clipMode = 'variance', sharpness = 0.0, antiFlicker = true,
        sharpenMode = 'unsharp', halfResHistory = true, upscaleMode = 'lanczos',
        motionAdaptive = false, motionAdaptiveSharpness = false,
    },
    [2] = {
        name = 'Instance 2 - RCAS Strong Sharpen (F.0.12)',
        desc = 'sharp=1.5 + RCAS (FSR2 robust) -- 强锐化 + noise/edge 保护',
        clipMode = 'ycocg', sharpness = 1.5, antiFlicker = true,
        sharpenMode = 'rcas', halfResHistory = false, upscaleMode = 'bilinear',
        motionAdaptive = false, motionAdaptiveSharpness = false,
    },
    [3] = {
        name = 'Instance 3 - Motion Adaptive All (F.0.8/0.13)',
        desc = 'variance + motionAdaptive γ + motionAdaptiveSharpness (高速防 trail)',
        clipMode = 'variance', sharpness = 0.8, antiFlicker = true,
        sharpenMode = 'rcas', halfResHistory = false, upscaleMode = 'bilinear',
        motionAdaptive = true, motionAdaptiveSharpness = true,
    },
}

-- ============================================================================
-- 4. 启动: 创建 3 个 user instance + 各自 Enable + apply profile
-- ============================================================================

local function apply_instance_params(idx)
    local p = PROFILES[idx]
    if not p then return false end
    if not TAA.IsEnabled() then
        local ok = TAA.Enable(WIN_W, WIN_H)
        if not ok then return false end
    end
    TAA.SetClipMode(p.clipMode)
    TAA.SetSharpness(p.sharpness)
    if TAA.SetAntiFlicker     then TAA.SetAntiFlicker(p.antiFlicker)     end
    if TAA.SetSharpenMode     then TAA.SetSharpenMode(p.sharpenMode)     end
    if TAA.SetHalfResHistory  then TAA.SetHalfResHistory(p.halfResHistory) end
    if TAA.SetUpscaleMode     then TAA.SetUpscaleMode(p.upscaleMode)     end
    if TAA.SetMotionAdaptive  then TAA.SetMotionAdaptive(p.motionAdaptive) end
    if TAA.SetMotionAdaptiveSharpness then
        TAA.SetMotionAdaptiveSharpness(p.motionAdaptiveSharpness)
    end
    return true
end

-- 创建 3 个 user instance (id 应为 1, 2, 3)
local user_ids = {}
local function create_user_instances()
    user_ids = {}
    for i = 1, 3 do
        local id = TAA.CreateInstance()
        if id and id > 0 then
            user_ids[i] = id
            TAA.SetActiveInstance(id)
            apply_instance_params(i)
        else
            print(string.format('[demo_taa_split] CreateInstance #%d 失败 (count=%d)', i, TAA.GetInstanceCount()))
        end
    end
    -- 默认 instance (id=0) 也配 profile, 切回 default
    TAA.SetActiveInstance(0)
    apply_instance_params(0)
    print(string.format('[demo_taa_split] Created %d user instances, count=%d, active=0',
        #user_ids, TAA.GetInstanceCount()))
end

local function destroy_user_instances()
    for i = #user_ids, 1, -1 do
        if user_ids[i] then TAA.DestroyInstance(user_ids[i]) end
    end
    user_ids = {}
    TAA.SetActiveInstance(0)
end

create_user_instances()

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

local cubeAngle, barAngle = 0.0, 0.0

local BAR_COLORS = {
    {1.0, 0.2, 0.2}, {1.0, 0.6, 0.2}, {1.0, 1.0, 0.2}, {0.2, 1.0, 0.2},
    {0.2, 1.0, 1.0}, {0.2, 0.5, 1.0}, {0.7, 0.2, 1.0}, {1.0, 0.2, 0.7},
}

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

    -- 1-4: 切 active instance (注意: 数字键 1 → instance 0, 2 → instance 1, ... 更符合直觉)
    -- 用按键 0/1/2/3 直接对应 instance ID
    for i = 0, 3 do
        if keyTap(tostring(i)) then
            local target = i  -- 0=default, 1/2/3=user instance
            if target > 0 and not user_ids[target] then
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
        end
    end

    -- R: 重置当前 instance history (Disable + Enable + re-apply)
    if keyTap('r') then
        local active = TAA.GetActiveInstance()
        if TAA.IsEnabled() then TAA.Disable() end
        apply_instance_params(active)
        print(string.format('[demo_taa_split] Reset instance %d history (re-stabilize)', active))
    end

    -- C: 销毁所有 user instance + 重建
    if keyTap('c') then
        destroy_user_instances()
        print(string.format('[demo_taa_split] Destroyed all user instances, count=%d', TAA.GetInstanceCount()))
        create_user_instances()
    end

    -- ========== 渲染 ==========
    win:BeginFrame(0.02, 0.02, 0.03, 1.0)

    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(0.0, 2.2, 5.0, 0.0, 0.6, 0.0)
    end

    -- 地面
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

    -- ========== HUD ==========
    if win.DrawText then
        local active = TAA.GetActiveInstance()
        local p = PROFILES[active]
        local y = 8
        local line = function(s) win:DrawText(8, y, s, 1, 1, 1, 1); y = y + 16 end

        -- 1) 当前 active instance + profile 名
        line(string.format('[Instance %d/%d] %s',
            active, TAA.GetInstanceCount() - 1, p and p.name or '?'))
        if p then line('Profile: ' .. p.desc) end

        -- 2) 实际 TAA 参数 (反映 GetXxx 真实值, 而非 profile 表)
        if TAA.IsEnabled() then
            local cmode  = TAA.GetClipMode and TAA.GetClipMode() or 'rgb'
            local sharp  = TAA.GetSharpness and TAA.GetSharpness() or 0
            local sm     = TAA.GetSharpenMode and TAA.GetSharpenMode() or 'unsharp'
            local hr     = TAA.GetHalfResHistory and TAA.GetHalfResHistory() or false
            local um     = TAA.GetUpscaleMode and TAA.GetUpscaleMode() or 'bilinear'
            local ma     = TAA.GetMotionAdaptive and TAA.GetMotionAdaptive() or false
            local mas    = TAA.GetMotionAdaptiveSharpness and TAA.GetMotionAdaptiveSharpness() or false
            line(string.format('TAA: clip=%s | sharp=%.2f(%s) | halfRes=%s | upscale=%s',
                cmode, sharp, sm, hr and 'ON' or 'OFF', um))
            line(string.format('Motion-Adaptive: gamma=%s | sharpness=%s | jitter=%s',
                ma and 'ON' or 'OFF', mas and 'ON' or 'OFF',
                TAA.GetJitterEnabled() and 'ON' or 'OFF'))
        else
            line('TAA: OFF')
        end

        -- 3) Instance lifecycle 状态
        line(string.format('Instances: count=%d | user_ids=[%s, %s, %s]',
            TAA.GetInstanceCount(),
            tostring(user_ids[1] or 'X'),
            tostring(user_ids[2] or 'X'),
            tostring(user_ids[3] or 'X')))

        -- 4) Keys
        line('Keys: 0=default | 1/2/3=user instances | R=reset history | C=destroy+recreate | ESC')
        line('Tip: 切 active 不会重置 history (与 demo_taa_compare 切 preset 重置不同) -- F.0.10 核心价值!')
    end

    win:EndFrame()
end

-- ============================================================================
-- 6. 反向清理
-- ============================================================================

destroy_user_instances()
if TAA.IsEnabled() then TAA.Disable() end
if hdrEnabled    then HDR.Disable()  end
if cubeMesh      then cubeMesh:Delete()  end
if barMesh       then barMesh:Delete()   end
if planeMesh     then planeMesh:Delete() end
print('demo_taa_split ok')
