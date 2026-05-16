-- ============================================================================
-- ChocoLight Phase F.0.10.10.1 — Quad-Split Demo with CloneInstance setup
-- ============================================================================
-- F.0.10.10 真物理 4-screen split-screen + F.0.10.9.x.3 CloneInstance 简化 setup,
-- 4 个 HDR instance × 4 个 TAA instance, 每 quad 独立 LUT/exposure/tonemap/sharpen profile.
--
-- vs F.0.10.10 老版本:
--   1) HDR/TAA setup 改用 CloneInstance(0) + override, 节省 ~30 行 boilerplate
--   2) OnOpen 末加 GetState() 4 instance 快照打印, 直观确认 per-quad profile 隔离
--   3) Bloom.SetRadius clamp [0,1] 小 bug 修正
--
-- Quad 布局 (1280×720, 各 quad 640×360):
--   ┌──────────────┬──────────────┐
--   │  TL (quad 0) │  TR (quad 1) │
--   │  黄昏暖调      │  冷夜冷调      │
--   ├──────────────┼──────────────┤
--   │  BL (quad 2) │  BR (quad 3) │
--   │  复古中性      │  赛博青蓝      │
--   └──────────────┴──────────────┘
--
-- 4 Profile (LUT × exposure × tonemap × sharpen):
--   TL: warm LUT,  exp=1.5, ACES,       RCAS sharp=1.2,  strong Bloom + strong MB
--   TR: cool LUT,  exp=0.6, Uncharted2, Lanczos halfRes, light Bloom + temporal SSR
--   BL: 无 LUT,    exp=1.0, Reinhard,   unsharp=0.5,    mid Bloom + mid SSR
--   BR: cool LUT,  exp=1.2, ACES,       RCAS sharp=1.5, AntiFlicker + variance clip
--
-- 帧流程 (per quad i):
--   apply_postfx_profile(i)
--   HDR.SetActiveInstance(hdr_ids[i])     # 切到 instance i 的 fbo
--   HDR.BeginScene()                       # bind instance i 的 fbo + clear
--   Gfx.SetViewport(0, 0, HALF_W, HALF_H)  # 在 instance i 的 sceneTex 上渲染
--   SetCamera(camera_i); drawScene()
--   Bloom.Process / SSR.Process / MB.Process(0, 0, HALF_W, HALF_H)  # 全屏 region
--   TAA.SetActiveInstance(taa_ids[i]); TAA.ApplyJitter(); TAA.Process(0, 0, HALF_W, HALF_H)
--   HDR.EndScene()                          # unbind instance i 的 fbo
-- 完成 4 instance 渲染后 tonemap pass:
--   for i in 0..3:
--     HDR.SetActiveInstance(hdr_ids[i])
--     HDR.Tonemap(quad_x[i], quad_y[i], HALF_W, HALF_H, params_i)  # 4 instance → 4 quad
--
-- 控制:
--   1/2/3/4 : 重置对应 instance history
--   L       : 全局 LUT toggle
--   ESC     : 退出
-- ============================================================================

local function safe_require(n) local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_quad_split] Light.Graphics 不可用'); print('demo_quad_split ok (no graphics)'); return end
local HDR, TAA = Gfx.HDR, Gfx.TAA
local Bloom, SSR, MB = Gfx.Bloom, Gfx.SSR, Gfx.MotionBlur
if type(HDR) ~= 'table' or type(TAA) ~= 'table' then
    print('[demo_quad_split] HDR/TAA 子表缺失'); print('demo_quad_split ok (subtable missing)'); return
end

-- 必需 multi-instance API + region API
local function api_missing(t, name) return type(t[name]) ~= 'function' end
if api_missing(HDR, 'CreateInstance') or api_missing(HDR, 'SetActiveInstance')
    or api_missing(HDR, 'BeginScene') or api_missing(HDR, 'EndScene')
    or api_missing(HDR, 'Tonemap') or api_missing(HDR, 'SetAutoTonemap')
    or api_missing(TAA, 'CreateInstance') or api_missing(TAA, 'Process')
    or api_missing(Gfx, 'SetViewport') then
    print('[demo_quad_split] Phase F.0.10.x API missing (need HDR/TAA multi-instance + region API)')
    print('demo_quad_split ok (legacy build)'); return
end

print('==== ChocoLight Phase F.0.10.10.1 Quad-Split Demo (CloneInstance setup) ====')
print('[demo_quad_split] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_quad_split] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
print('[demo_quad_split] TAA.IsSupported = ' .. tostring(TAA.IsSupported()))

-- ============================================================================
-- Headless API probe (CI 兼容)
-- ============================================================================
local function run_headless_api_probe()
    print('[demo_quad_split] running headless API probe (no UI window)')

    -- 1) HDR multi-instance Create/Destroy round-trip
    local cnt0 = HDR.GetInstanceCount()
    local ids = {}
    for i = 1, 3 do ids[i] = HDR.CreateInstance() end
    if HDR.GetInstanceCount() == cnt0 + 3 then
        print('  PASS: HDR.CreateInstance x3 (count=' .. HDR.GetInstanceCount() .. ')')
    else print('  FAIL: HDR multi-instance Create') end
    for i = 3, 1, -1 do if ids[i] and ids[i] > 0 then HDR.DestroyInstance(ids[i]) end end
    if HDR.GetInstanceCount() == cnt0 then print('  PASS: HDR.DestroyInstance x3 cleanup ok')
    else print('  FAIL: HDR.DestroyInstance cleanup') end

    -- 2) TAA multi-instance Create/Destroy round-trip
    local tcnt0 = TAA.GetInstanceCount()
    local tids = {}
    for i = 1, 3 do tids[i] = TAA.CreateInstance() end
    if TAA.GetInstanceCount() == tcnt0 + 3 then
        print('  PASS: TAA.CreateInstance x3 (count=' .. TAA.GetInstanceCount() .. ')')
    else print('  FAIL: TAA multi-instance Create') end
    for i = 3, 1, -1 do if tids[i] and tids[i] > 0 then TAA.DestroyInstance(tids[i]) end end

    -- 3) HDR.BeginScene/EndScene headless silent no-op (HDR 未 Enable)
    local ok1, _ = pcall(HDR.BeginScene)
    local ok2, _ = pcall(HDR.EndScene)
    if ok1 and ok2 then print('  PASS: HDR.BeginScene/EndScene headless silent no-op')
    else print('  FAIL: HDR.BeginScene/EndScene 抛异常') end

    -- 4) Tonemap headless 退化 (HDR 未启用 → nil + err)
    local r, e = HDR.Tonemap(0, 0, 100, 100)
    if r == nil and type(e) == 'string' then print('  PASS: HDR.Tonemap(rgn) headless: ' .. e)
    else print('  FAIL: HDR.Tonemap expected nil + err') end

    -- 5) Bloom/SSR/MB.Process headless 退化
    if type(Bloom) == 'table' and not api_missing(Bloom, 'Process') then
        local r, e = Bloom.Process(0, 0, 100, 100)
        if r == nil and type(e) == 'string' then print('  PASS: Bloom.Process(rgn) headless: ' .. e) end
    end
    if type(SSR) == 'table' and not api_missing(SSR, 'Process') then
        local r, e = SSR.Process(0, 0, 100, 100)
        if r == nil and type(e) == 'string' then print('  PASS: SSR.Process(rgn) headless: ' .. e) end
    end
    if type(MB) == 'table' and not api_missing(MB, 'Process') then
        local r, e = MB.Process(0, 0, 100, 100)
        if r == nil and type(e) == 'string' then print('  PASS: MB.Process(rgn) headless: ' .. e) end
    end

    -- 6) MAX_INSTANCES = 4 (default + 3 user) 验证: 第 4 个 user instance 应失败
    local extra = {}
    for i = 1, 4 do extra[i] = HDR.CreateInstance() end
    if extra[4] == 0 or extra[4] == nil then
        print('  PASS: HDR MAX_INSTANCES=4 enforced (4th create returns 0/nil)')
    else
        print('  FAIL: HDR MAX_INSTANCES not enforced (got id=' .. tostring(extra[4]) .. ')')
    end
    for i = 1, 4 do if extra[i] and extra[i] > 0 then HDR.DestroyInstance(extra[i]) end end
end

if not UI or not UI.Window then
    run_headless_api_probe()
    print('demo_quad_split ok (headless API check, no UI.Window)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_quad_split] Light global 不可用')
    run_headless_api_probe()
    print('demo_quad_split ok (no Light global)')
    return
end

-- ============================================================================
-- 几何 + 全局常量
-- ============================================================================
local WIN_W, WIN_H   = 1280, 720
local HALF_W, HALF_H = WIN_W / 2, WIN_H / 2
-- 4 quad 屏幕坐标 (Tonemap 目标位置, default fb 像素坐标)
local QUAD_RECTS = {
    [0] = { x = 0,      y = HALF_H, w = HALF_W, h = HALF_H, label = 'TL  Warm Sunset (ACES + warm LUT + RCAS)' },
    [1] = { x = HALF_W, y = HALF_H, w = HALF_W, h = HALF_H, label = 'TR  Cool Night (Uncharted2 + cool LUT + Lanczos)' },
    [2] = { x = 0,      y = 0,      w = HALF_W, h = HALF_H, label = 'BL  Neutral Vintage (Reinhard, no LUT)' },
    [3] = { x = HALF_W, y = 0,      w = HALF_W, h = HALF_H, label = 'BR  Cyber Cyan (ACES + cool LUT + AntiFlicker)' },
}
-- 注意: Y 轴向上 (OpenGL viewport), TL 视觉左上 = y=HALF_H (上半屏), BL 视觉左下 = y=0

-- 4 quad 不同相机角度 (展示同场景不同视角)
local CAMERAS = {
    [0] = { eye = {  3.0, 4.0,  6.0 }, at = { 0.0, 0.6, 0.0 } },   -- TL: 高远 overview
    [1] = { eye = { -3.5, 1.0,  3.0 }, at = { 0.0, 0.8, 0.0 } },   -- TR: 低近 ground-level
    [2] = { eye = {  0.0, 6.0,  0.1 }, at = { 0.0, 0.0, 0.0 } },   -- BL: 顶视
    [3] = { eye = {  5.5, 0.8,  1.2 }, at = { 0.0, 0.6, 0.0 } },   -- BR: 侧近 cinematic
}

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

-- ============================================================================
-- Demo 类
-- ============================================================================
local Demo = Light(Light.UI.Window):New()

local g_cubeMesh, g_barMesh, g_planeMesh = nil, nil, nil
local g_hdr_ids   = { [0] = 0 }   -- quad i → HDR instance id (i=0 用 default 0)
local g_taa_ids   = {}            -- quad i → TAA instance id
-- Phase F.0.10.10.2: Bloom/SSR/MB multi-instance (与 HDR/TAA 同模板)
local g_bloom_ids = { [0] = 0 }
local g_ssr_ids   = { [0] = 0 }
local g_mb_ids    = { [0] = 0 }
local g_warmLut, g_coolLut = nil, nil
local g_cubeAngle, g_barAngle = 0.0, 0.0
local g_lutOn  = true
local g_initOk = false
local g_recording = false  -- Phase F.0.11 录屏状态
local g_auto_shot_frames = 3  -- Draw() 调用 N 次后自动截图 (0=已触发)

-- 4 quad 的 tonemap 显式参数 (传给 HDR.Tonemap rgn 重载)
-- tonemap: 'aces' / 'uncharted2' / 'reinhard'  exposure: 0.6~1.5
local function build_tm_params(i)
    if     i == 0 then return { exposure = 1.5, gamma = 2.2, tonemap = 'aces',
                                 lut = g_lutOn and g_warmLut or 0, lutStrength = 0.85 }
    elseif i == 1 then return { exposure = 0.6, gamma = 2.4, tonemap = 'uncharted2',
                                 lut = g_lutOn and g_coolLut or 0, lutStrength = 0.85 }
    elseif i == 2 then return { exposure = 1.0, gamma = 2.2, tonemap = 'reinhard',
                                 lut = 0, lutStrength = 0.0 }
    elseif i == 3 then return { exposure = 1.2, gamma = 2.2, tonemap = 'aces',
                                 lut = g_lutOn and g_coolLut or 0, lutStrength = 0.50 }
    end
    return nil
end

-- ============================================================================
-- 4 quad 的 TAA per-instance setup (Clone 模式, F.0.10.9.x.3)
-- ============================================================================
-- 旧模式: 4 个 instance 全量 Set* (每 instance 5~7 行)
-- 新模式: instance 0 (default) 设 TL base profile, instance 1/2/3 用 Clone(0) 复制
--         然后仅 override 差异字段.
-- 差异统计 (vs TL base):
--   TR (1): 6 字段 (clipMode/varianceGamma/halfRes/upscaleMode/sharpness/sharpenMode)
--   BL (2): 4 字段 (clipMode/sharpness/sharpenMode/antiFlicker)
--   BR (3): 3 字段 (clipMode/varianceGamma/sharpness)  -- sharpenMode='rcas' 与 base 同
--
-- TL base 设 RCAS sharp=1.2 + ycocg + antiFlicker=true; Clone 自动继承.
local function setup_taa_base_profile()  -- instance 0 (TL) 完整 setup
    TAA.SetActiveInstance(0)
    TAA.Enable(HALF_W, HALF_H)
    TAA.SetClipMode('ycocg')
    TAA.SetSharpness(1.2)
    if TAA.SetSharpenMode then TAA.SetSharpenMode('rcas') end
    if TAA.SetAntiFlicker then TAA.SetAntiFlicker(true) end
end

-- override 差异字段 (用于 instance 1/2/3, 假定 Clone(0) 已复制 TL base)
local function override_taa_diff(i)
    if     i == 1 then  -- TR: Lanczos halfRes variance
        TAA.SetClipMode('variance')
        if TAA.SetVarianceGamma then TAA.SetVarianceGamma(1.0) end
        TAA.SetSharpness(0.0)
        if TAA.SetSharpenMode then TAA.SetSharpenMode('unsharp') end
        if TAA.SetHalfResHistory then TAA.SetHalfResHistory(true) end
        if TAA.SetUpscaleMode then TAA.SetUpscaleMode('lanczos') end
    elseif i == 2 then  -- BL: 中性 unsharp, 关 antiFlicker
        TAA.SetClipMode('rgb'); TAA.SetSharpness(0.5)
        if TAA.SetSharpenMode then TAA.SetSharpenMode('unsharp') end
        if TAA.SetAntiFlicker then TAA.SetAntiFlicker(false) end
    elseif i == 3 then  -- BR: variance + RCAS 更强 sharpen (sharpenMode='rcas' Clone 已带)
        TAA.SetClipMode('variance')
        if TAA.SetVarianceGamma then TAA.SetVarianceGamma(0.8) end
        TAA.SetSharpness(1.5)
    end
end

-- 1/2/3/4 键 reset 用 (退化为完整 setup; 因 Disable 已释放 RT, 需 Enable 重建)
local function setup_taa_instance(i)
    TAA.SetActiveInstance(g_taa_ids[i])
    TAA.Enable(HALF_W, HALF_H)
    if i == 0 then
        -- 退化重设 TL base (键 1 reset)
        TAA.SetClipMode('ycocg'); TAA.SetSharpness(1.2)
        if TAA.SetSharpenMode then TAA.SetSharpenMode('rcas') end
        if TAA.SetAntiFlicker then TAA.SetAntiFlicker(true) end
    else
        -- 键 2/3/4 reset: instance 已 Disable 释 RT, Enable 后字段保留, 不用再 override
        -- (Disable 不会清 user-settable 字段, 只清 RT handle + history)
    end
end

-- ============================================================================
-- Phase F.0.10.10.2 — Bloom/SSR/MB multi-instance (与 HDR/TAA 同 Clone 模式)
--
-- 旧 (F.0.10.10.1): 每帧 16 次 SetXxx 切换全局 Bloom/SSR/MB 单例 → 状态污染风险高
-- 新 (本 phase):    OnOpen 阶段 Clone + SetState 一次性写 profile,
--                    render_quad 里仅 3 次 SetActiveInstance(id) 切换 instance
--                    优势: 状态隔离 + 切换更快 + 与 HDR/TAA 同范式 (全 5 renderer 统一)
-- ============================================================================

-- 4 quad 的 Bloom/SSR/MB profile (一次性 SetState 写入, 不再每帧切换)
-- 注: Bloom.SetRadius clamp [0, 1]; 原 1.5/1.2 已 silent clamp 到 1.0
local BLOOM_PROFILES = {
    [0] = { intensity = 1.5, threshold = 0.8, radius = 1.0 },
    [1] = { intensity = 0.4, threshold = 1.5, radius = 0.8 },
    [2] = { intensity = 0.8, threshold = 1.0, radius = 0.9 },
    [3] = { intensity = 1.2, threshold = 0.9, radius = 1.0 },
}
local SSR_PROFILES = {
    [0] = { intensity = 0.4, temporal_enabled = false },
    [1] = { intensity = 1.0, temporal_enabled = true  },
    [2] = { intensity = 0.7, temporal_enabled = true  },
    [3] = { intensity = 0.6, temporal_enabled = true  },
}
local MB_PROFILES = {
    [0] = { strength = 0.8, sample_count = 12 },
    [1] = { strength = 0.0 },
    [2] = { strength = 0.3, sample_count = 8  },
    [3] = { strength = 0.5, sample_count = 10 },
}

-- 切换 active instance (1 instance = 1 quad 的 Bloom/SSR/MB 状态快照)
-- 与 HDR.SetActiveInstance / TAA.SetActiveInstance 模式一致, 每帧仅 3 次切换
local function apply_postfx_profile(i)
    if Bloom and Bloom.IsEnabled and Bloom.IsEnabled() and g_bloom_ids[i] then
        Bloom.SetActiveInstance(g_bloom_ids[i])
    end
    if SSR and SSR.IsEnabled and SSR.IsEnabled() and g_ssr_ids[i] then
        SSR.SetActiveInstance(g_ssr_ids[i])
    end
    if MB and MB.IsEnabled and MB.IsEnabled() and g_mb_ids[i] then
        MB.SetActiveInstance(g_mb_ids[i])
    end
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
    print('[demo_quad_split] OnOpen: setup 4 HDR + 4 TAA instance, LUTs, postfx')

    -- Mesh
    local Mesh = Gfx.Mesh
    if not Mesh or type(Mesh.New) ~= 'function' then
        print('[demo_quad_split] Mesh.New 不可用'); self:Close(); return
    end
    local cv, ci = buildCube(0.5); local bv, bi = buildCube(1.0); local pv, pi = buildPlane()
    g_cubeMesh = Mesh.New(cv, ci); g_barMesh = Mesh.New(bv, bi); g_planeMesh = Mesh.New(pv, pi)
    if not g_cubeMesh or not g_barMesh or not g_planeMesh then
        print('[demo_quad_split] mesh build failed'); self:Close(); return
    end

    -- ============ HDR multi-instance setup (Clone 模式, F.0.10.9.x.3) ============
    -- 旧 (F.0.10.10) 模式: 4 × {CreateInstance + SetActiveInstance + Enable + SetAutoTonemap(false)} 16 行
    -- 新 (本 phase) 模式: 1 × full setup default + 3 × {CloneInstance(0) + Enable} ~6 行
    --
    -- CloneInstance(0) 复制 default 全部 user-settable 字段 (含 autoTonemap=false), 仅 RT/sceneTex
    -- 重置为 0; 因此子 instance 必须再调 Enable() 自建 RT.
    if not HDR.IsSupported() then
        print('[demo_quad_split] HDR not supported, abort'); self:Close(); return
    end
    -- 1) Default instance (quad 0) 设 base profile
    if not HDR.Enable(HALF_W, HALF_H) then
        print('[demo_quad_split] HDR.Enable default failed'); self:Close(); return
    end
    HDR.SetAutoTonemap(false)   -- ← 每帧手动 HDR.Tonemap(rgn,params), 这一行同时定义后续 clone 的默认行为
    -- 2) Clone default 到 quad 1/2/3 (1 行 = 1 instance, 含 autoTonemap=false)
    for i = 1, 3 do
        local id = HDR.CloneInstance(0)
        if not id or id <= 0 then
            print(string.format('[demo_quad_split] HDR.CloneInstance(0) #%d failed (slot full?)', i))
            self:Close(); return
        end
        g_hdr_ids[i] = id
        HDR.SetActiveInstance(id)
        if not HDR.Enable(HALF_W, HALF_H) then
            print(string.format('[demo_quad_split] HDR.Enable cloned instance %d failed', id))
            self:Close(); return
        end
        -- 注: autoTonemap=false 已被 Clone 复制, 此处零额外 Set*
    end
    HDR.SetActiveInstance(0)
    print(string.format('[demo_quad_split] 4 HDR instance via Clone: ids=[%d, %d, %d, %d], each %dx%d',
          g_hdr_ids[0], g_hdr_ids[1], g_hdr_ids[2], g_hdr_ids[3], HALF_W, HALF_H))

    -- LUT 加载 (lut id 是 backend 全局, 任 instance 可引用)
    if HDR.LoadCubeLUT then
        g_warmLut = HDR.LoadCubeLUT('samples/demo_quad_split/luts/warm_red.cube')
        g_coolLut = HDR.LoadCubeLUT('samples/demo_quad_split/luts/cool_blue.cube')
        print(string.format('[demo_quad_split] LUT loaded: warm=%s, cool=%s',
              tostring(g_warmLut), tostring(g_coolLut)))
    end

    -- ============ Bloom / SSR / MotionBlur multi-instance setup (F.0.10.10.2) ============
    -- 模式: default instance (id=0) Enable + 写 base profile;
    --       Clone(0) × 3 user instance + 写各 quad 差异 profile (SetState 一次性 N 字段)
    --
    -- 与 HDR/TAA 同范式: 5 renderer 全部走 multi-instance, 完整统一.
    --
    -- helper: 给单 renderer 创建 4 instance, 写入 profiles 表 (per-i field map)
    local function setup_4_instance(RND, name, w, h, profiles, id_table)
        if not (RND and RND.IsSupported and RND.IsSupported() and RND.Enable) then
            print('[demo_quad_split] ' .. name .. ' not supported, skip multi-instance setup')
            return false
        end
        -- 1) default instance (id=0)
        RND.SetActiveInstance(0)
        if not RND.Enable(w, h) then
            print('[demo_quad_split] ' .. name .. '.Enable(default) failed'); return false
        end
        if RND.SetState then RND.SetState(profiles[0]) end
        id_table[0] = 0
        -- 2) Clone default 到 quad 1/2/3, 各自 SetState 差异 profile
        for i = 1, 3 do
            local id = RND.CloneInstance(0)
            if not id or id <= 0 then
                print(string.format('[demo_quad_split] %s.CloneInstance(0) #%d failed', name, i))
                return false
            end
            id_table[i] = id
            RND.SetActiveInstance(id)
            if not RND.Enable(w, h) then
                print(string.format('[demo_quad_split] %s.Enable cloned instance %d failed', name, id))
                return false
            end
            if RND.SetState then RND.SetState(profiles[i]) end
        end
        RND.SetActiveInstance(0)
        print(string.format('[demo_quad_split] 4 %s instance via Clone: ids=[%d,%d,%d,%d]',
              name, id_table[0], id_table[1], id_table[2], id_table[3]))
        return true
    end

    if setup_4_instance(Bloom, 'Bloom', HALF_W, HALF_H, BLOOM_PROFILES, g_bloom_ids) then
        HDR.SetAutoBloom(false)
    end
    if setup_4_instance(SSR,   'SSR',   HALF_W, HALF_H, SSR_PROFILES,   g_ssr_ids) then
        HDR.SetAutoSSR(false)
    end
    if setup_4_instance(MB,    'MB',    HALF_W, HALF_H, MB_PROFILES,    g_mb_ids) then
        HDR.SetAutoMotionBlur(false)
    end

    -- ============ TAA multi-instance setup (Clone 模式, F.0.10.9.x.3) ============
    -- 旧 (F.0.10.10): 3 × CreateInstance + 4 × {SetActive + Enable + 5~7 × Set*} ≈ 35 行
    -- 新 (本 phase): 1 × full setup + 3 × {Clone(0) + Enable + 3~6 × override} ≈ 25 行
    --
    -- TL base profile (instance 0) 设 ycocg + rcas + sharp=1.2 + antiFlicker=true.
    -- Clone(0) 自动继承 base + RT 字段重置; override_taa_diff(i) 只调差异字段.
    g_taa_ids[0] = 0
    HDR.SetAutoTAA(false)   -- 关 auto TAA (我们手动 TAA.Process)

    -- 1) Default instance (TL base profile) 完整 setup
    setup_taa_base_profile()

    -- 2) Clone default 到 quad 1/2/3, override 差异字段
    for i = 1, 3 do
        local id = TAA.CloneInstance(0)
        if not id or id <= 0 then
            print(string.format('[demo_quad_split] TAA.CloneInstance(0) #%d failed', i))
            self:Close(); return
        end
        g_taa_ids[i] = id
        TAA.SetActiveInstance(id)
        if not TAA.Enable(HALF_W, HALF_H) then
            print(string.format('[demo_quad_split] TAA.Enable cloned instance %d failed', id))
            self:Close(); return
        end
        override_taa_diff(i)   -- 仅 3~6 个字段差异
    end
    TAA.SetActiveInstance(0)
    print(string.format('[demo_quad_split] 4 TAA instance via Clone: ids=[%d, %d, %d, %d]',
          g_taa_ids[0], g_taa_ids[1], g_taa_ids[2], g_taa_ids[3]))

    -- ============ GetState() 验证 4 instance 配置确实独立 (F.0.10.9.x.3) ============
    -- print 每个 instance 的关键差异字段, 直观确认 setup 正确
    if HDR.GetState and TAA.GetState then
        print('[demo_quad_split] === per-instance setup snapshot (GetState) ===')
        for i = 0, 3 do
            HDR.SetActiveInstance(g_hdr_ids[i])
            TAA.SetActiveInstance(g_taa_ids[i])
            local hs, ts = HDR.GetState(), TAA.GetState()
            print(string.format('  Q%d HDR.enabled=%s auto_tonemap=%s | TAA clip=%s sharpen=%s sharp=%.2f antiflicker=%s halfRes=%s upscale=%s',
                i, tostring(hs.enabled), tostring(hs.auto_tonemap),
                tostring(ts.clip_mode), tostring(ts.sharpen_mode), ts.sharpness or 0,
                tostring(ts.anti_flicker), tostring(ts.half_res_history), tostring(ts.upscale_mode)))
        end
        HDR.SetActiveInstance(0); TAA.SetActiveInstance(0)
    end

    -- 相机 + 光照 (project ratio 用 quad 的, 因为每 quad 是 640x360)
    if type(Gfx.SetPerspective) == 'function' then
        Gfx.SetPerspective(60, HALF_W / HALF_H, 0.1, 100.0)
    end
    if type(Gfx.SetDepthTest) == 'function' then Gfx.SetDepthTest(true) end
    if type(Gfx.SetDirectionalLight) == 'function' then
        Gfx.SetDirectionalLight(0.5, -1.0, -0.3, 1.0, 0.95, 0.85, 5.0)
    end

    g_initOk = true
    print('[demo_quad_split] OnOpen: setup ok, entering render loop')
end

function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    g_cubeAngle = g_cubeAngle + dt * math.rad(30)
    g_barAngle  = g_barAngle  + dt * math.rad(60)

end

-- 给 quad i 渲染 + 后处理 (在 instance i 的 fbo 内)
local function render_quad(i)
    apply_postfx_profile(i)

    HDR.SetActiveInstance(g_hdr_ids[i])
    HDR.BeginScene()                                  -- bind instance i 的 fbo + clear

    Gfx.SetViewport(0, 0, HALF_W, HALF_H)             -- 在 instance i 的 sceneTex 上
    local cam = CAMERAS[i]
    if type(Gfx.SetCamera) == 'function' then
        Gfx.SetCamera(cam.eye[1], cam.eye[2], cam.eye[3], cam.at[1], cam.at[2], cam.at[3])
    end
    drawScene()

    -- 后处理链: Bloom → SSR → MB → TAA (顺序与 EndScene 自动路径一致)
    if Bloom and Bloom.IsEnabled and Bloom.IsEnabled() then
        Bloom.Process(0, 0, HALF_W, HALF_H)
    end
    if SSR and SSR.IsEnabled and SSR.IsEnabled() then
        SSR.Process(0, 0, HALF_W, HALF_H)
    end
    if MB and MB.IsEnabled and MB.IsEnabled() then
        MB.Process(0, 0, HALF_W, HALF_H)
    end

    TAA.SetActiveInstance(g_taa_ids[i])
    if TAA.ApplyJitter then TAA.ApplyJitter() end
    TAA.Process(0, 0, HALF_W, HALF_H)

    HDR.EndScene()                                    -- unbind instance i 的 fbo
end

function Demo:Draw()
    if not g_initOk then return end

    -- 4 quad scene 渲染 + 后处理 (各自独立 instance, history 不互扰)
    for i = 0, 3 do render_quad(i) end

    -- Tonemap pass: 4 instance 各自整张 sceneTex (640×360) → default fb 的对应 quad
    Gfx.SetViewport(0, 0, WIN_W, WIN_H)
    for i = 0, 3 do
        HDR.SetActiveInstance(g_hdr_ids[i])
        local q = QUAD_RECTS[i]
        local p = build_tm_params(i)
        if p then HDR.Tonemap(q.x, q.y, q.w, q.h, p) end
    end

    -- 切回 default 防止 EndFrame 重复处理
    HDR.SetActiveInstance(0)
    TAA.SetActiveInstance(0)

    -- HUD (default fb 上)
    if Gfx.Print then
        Gfx.Push()
        Gfx.SetColor(1, 1, 1, 1)
        Gfx.Print('=== Phase F.0.10.10.1 Quad-Split (4 HDR x 4 TAA via CloneInstance) ===', 8, 8, 0)
        Gfx.Print(string.format('Window=%dx%d  Each quad=%dx%d  HDR ids=[%d,%d,%d,%d]  TAA ids=[%d,%d,%d,%d]',
              WIN_W, WIN_H, HALF_W, HALF_H,
              g_hdr_ids[0], g_hdr_ids[1], g_hdr_ids[2], g_hdr_ids[3],
              g_taa_ids[0], g_taa_ids[1], g_taa_ids[2], g_taa_ids[3]),
              8, 28, 0)
        for i = 0, 3 do
            local q = QUAD_RECTS[i]
            -- quad label 显示在每 quad 左上角 (default fb 像素坐标; Y 反向: top-edge=q.y+q.h-20)
            Gfx.Print(string.format('[Q%d] %s', i, q.label), q.x + 8, q.y + q.h - 20, 0)
        end
        -- F.0.11 录屏状态显示
        local rec_status = g_recording and ' | [REC]' or ''
        Gfx.Print('Keys: 1/2/3/4=reset history | L=LUT | F8=screenshot | R=record(PNG) | ESC=quit' .. rec_status,
              8, WIN_H - 24, 0)
        Gfx.Pop()
    end

    -- Phase F.0.11: 第 3 帧 Draw 后触发自动截图 (EndFrame hook 里执行, HDR tonemap 已完成)
    -- demo 启动验证模式 (CHOCO_AUTO_EXIT 环境变量): 自动截图 + 退出, CI / smoke 验证用
    -- F.0.11.2: CHOCO_RECORD_ASYNC=1 启用 PBO 异步 readback (验证零回归 + 性能)
    if g_auto_shot_frames > 0 then
        g_auto_shot_frames = g_auto_shot_frames - 1
        if g_auto_shot_frames == 0 then
            if os.getenv('CHOCO_RECORD_ASYNC') == '1' and Gfx.SetRecordAsync then
                Gfx.SetRecordAsync(true)
                print('[demo_quad_split] PBO async readback enabled (F.0.11.2)')
            end
            Gfx.RecordPNGSequence('docs/screenshots/', 1)
            print('[demo_quad_split] auto screenshot → docs/screenshots/frame_0000.png'); io.flush()
        end
    elseif os.getenv('CHOCO_AUTO_EXIT') == '1' then
        -- 倒计时归零后再多跑 3 帧让 hook 写完 PNG, 然后自动退出
        -- (async 模式需要至少 2 帧: 第 1 启动 PBO, 第 2 取数据)
        g_auto_shot_frames = g_auto_shot_frames - 1   -- 继续 -1, -2, -3 ...
        if g_auto_shot_frames <= -4 then
            print('[demo_quad_split] CHOCO_AUTO_EXIT=1 → self:Close()'); io.flush()
            self:Close()
        end
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then self:Close()
    elseif key >= string.byte('1') and key <= string.byte('4') then
        local i = key - string.byte('1')   -- 1→0, 2→1, 3→2, 4→3
        TAA.SetActiveInstance(g_taa_ids[i])
        if TAA.IsEnabled() then TAA.Disable() end
        setup_taa_instance(i)
        TAA.SetActiveInstance(0)
        print(string.format('[demo_quad_split] Reset quad %d history (TAA instance %d)',
              i, g_taa_ids[i]))
    elseif key == string.byte('L') then
        g_lutOn = not g_lutOn
        print('[demo_quad_split] LUT toggle = ' .. tostring(g_lutOn))
    elseif key == 297 then  -- F8 = 截图 (GLFW key code 297)
        -- Phase F.0.11: 全屏截图
        local path = 'docs/screenshots/quad_split.png'
        local ok, err = Gfx.Screenshot(path)
        if ok then
            print('[demo_quad_split] Screenshot saved: ' .. path)
        else
            print('[demo_quad_split] Screenshot failed: ' .. tostring(err))
        end
    elseif key == string.byte('R') then
        -- Phase F.0.11: R 键切换录屏 (PNG 序列 → frames/ 目录)
        if not g_recording then
            local dir = 'docs/recordings/quad/'
            local ok, err = Gfx.RecordPNGSequence(dir, 120)  -- 录 120 帧 (~2s @ 60fps)
            if ok then
                g_recording = true
                print('[demo_quad_split] RecordPNGSequence started: ' .. dir .. ' (max 120 frames)')
            else
                print('[demo_quad_split] RecordPNGSequence failed: ' .. tostring(err))
            end
        else
            local n = Gfx.StopRecord()
            g_recording = false
            print(string.format('[demo_quad_split] RecordPNGSequence stopped (%d frames written)', n))
        end
    end
end

local function cleanup_demo()
    if not g_initOk then return end
    print('[demo_quad_split] cleanup: releasing 4× (HDR/TAA/Bloom/SSR/MB) instances, LUTs, meshes')

    -- LUTs (RemapLUTIdAcrossInstances 跨 instance 同步清)
    if g_warmLut then HDR.DeleteLUT3D(g_warmLut); g_warmLut = nil end
    if g_coolLut then HDR.DeleteLUT3D(g_coolLut); g_coolLut = nil end

    -- TAA: Disable + Destroy user instance
    for i = 1, 3 do
        if g_taa_ids[i] and g_taa_ids[i] > 0 then
            TAA.SetActiveInstance(g_taa_ids[i])
            if TAA.IsEnabled() then TAA.Disable() end
            TAA.DestroyInstance(g_taa_ids[i])
        end
    end
    -- TAA instance 0 (default)
    TAA.SetActiveInstance(0)
    if TAA.IsEnabled() then TAA.Disable() end

    -- ============ Bloom/SSR/MB multi-instance cleanup (F.0.10.10.2) ============
    -- 与 HDR/TAA 对称: Disable user instance + DestroyInstance, 最后 Disable default
    local function teardown_4_instance(RND, name, id_table)
        if not RND or not RND.IsEnabled then return end
        for i = 3, 1, -1 do
            if id_table[i] and id_table[i] > 0 then
                RND.SetActiveInstance(id_table[i])
                if RND.IsEnabled() then RND.Disable() end
                if RND.DestroyInstance then RND.DestroyInstance(id_table[i]) end
            end
        end
        RND.SetActiveInstance(0)
        if RND.IsEnabled() then RND.Disable() end
    end
    teardown_4_instance(MB,    'MB',    g_mb_ids)
    teardown_4_instance(SSR,   'SSR',   g_ssr_ids)
    teardown_4_instance(Bloom, 'Bloom', g_bloom_ids)
    HDR.SetAutoBloom(true); HDR.SetAutoSSR(true); HDR.SetAutoMotionBlur(true); HDR.SetAutoTAA(true)

    -- HDR: Disable + Destroy user instance + 复位 autoTonemap
    for i = 3, 1, -1 do
        if g_hdr_ids[i] and g_hdr_ids[i] > 0 then
            HDR.SetActiveInstance(g_hdr_ids[i])
            HDR.SetAutoTonemap(true)
            if HDR.IsEnabled() then HDR.Disable() end
            HDR.DestroyInstance(g_hdr_ids[i])
        end
    end
    HDR.SetActiveInstance(0)
    HDR.SetAutoTonemap(true)
    if HDR.IsEnabled() then HDR.Disable() end

    -- Mesh
    if g_cubeMesh  then g_cubeMesh:Delete();  g_cubeMesh  = nil end
    if g_barMesh   then g_barMesh:Delete();   g_barMesh   = nil end
    if g_planeMesh then g_planeMesh:Delete(); g_planeMesh = nil end

    g_initOk = false
end

Demo:Open(WIN_W, WIN_H, 'Phase F.0.10.10.1 - Quad-Split (4 HDR x 4 TAA via Clone)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_quad_split ok')
