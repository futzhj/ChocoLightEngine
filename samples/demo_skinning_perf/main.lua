-- ============================================================================
-- ChocoLight Phase AW GPU Skinning 真机性能验证 demo
-- ============================================================================
-- 用途:
--   1. 启动时自动 baseline (60 帧 CPU + 60 帧 GPU + 各 30 帧预热)
--   2. 控制台打印 speedup 数字
--   3. 进入交互模式: 屏上 OSD 实时显示 frame ms / mode / FPS
--
-- 控制:
--   G / C / A : 切换 SkinningMode -> gpu / cpu / auto
--   R         : 重新 baseline
--   ESC       : 退出
--
-- 资产:
--   优先路径: samples/demo_skinning_perf/assets/character.glb
--   运行 setup.ps1 / setup.sh 一键下载, 或手动放置任意 glTF 2.0 skinned mesh
--
-- 兼容: Lua 5.1 + ChocoLight Light.Animation/Time/Graphics/UI.Window
-- ============================================================================

-- ==================== 1. 模块加载 + 资产探测 ====================

local Anim, Time, UI, Gfx
do
    local function safe_require(n)
        local ok, m = pcall(require, n)
        if ok and type(m) == 'table' then return m end
        return nil
    end
    Anim = safe_require('Light.Animation')
    Time = safe_require('Light.Time')
    UI   = safe_require('Light.UI')
    Gfx  = safe_require('Light.Graphics')
end

if not (Anim and Time and Gfx) then
    print('[demo_skinning_perf] 必需模块缺失 (Animation/Time/Graphics)')
    print('  Anim=' .. tostring(Anim) .. ' Time=' .. tostring(Time) .. ' Gfx=' .. tostring(Gfx))
    print('demo_skinning_perf ok (modules unavailable)')
    return
end

-- ==================== 2. 资产路径探测 ====================

local function file_exists(p)
    local f = io.open(p, 'rb')
    if f then f:close(); return true end
    return false
end

local CANDIDATES = {
    'samples/demo_skinning_perf/assets/character.glb',
    'samples/demo_animation/assets/character.glb',
    'samples/demo_animation/character.glb',
    'assets/character.glb',
    'Light-0.2.3/assets/character.glb',
}

local function find_asset()
    for _, p in ipairs(CANDIDATES) do
        if file_exists(p) then return p end
    end
    return nil
end

local function print_setup_hint()
    print('[demo_skinning_perf] 未找到 character.glb 资产')
    print('  尝试过的候选路径:')
    for _, p in ipairs(CANDIDATES) do print('    - ' .. p) end
    print('')
    print('  快速 setup:')
    print('    Windows : .\\samples\\demo_skinning_perf\\setup.ps1')
    print('    Linux/Mac: chmod +x samples/demo_skinning_perf/setup.sh && ./samples/demo_skinning_perf/setup.sh')
    print('')
    print('  或手动下载 glTF 2.0 skinned mesh 到 samples/demo_skinning_perf/assets/character.glb')
end

-- ==================== 3. FrameStat: ring buffer 帧时间统计 ====================

local FrameStat = {}
FrameStat.__index = FrameStat

function FrameStat.new(window_size)
    return setmetatable({
        size = window_size or 60,
        buf  = {},
        idx  = 1,
        cnt  = 0,
    }, FrameStat)
end

function FrameStat:Push(ms)
    self.buf[self.idx] = ms
    self.idx = self.idx + 1
    if self.idx > self.size then self.idx = 1 end
    if self.cnt < self.size then self.cnt = self.cnt + 1 end
end

function FrameStat:Avg()
    if self.cnt == 0 then return 0 end
    local sum = 0
    for i = 1, self.cnt do sum = sum + self.buf[i] end
    return sum / self.cnt
end

function FrameStat:Min()
    if self.cnt == 0 then return 0 end
    local m = self.buf[1]
    for i = 2, self.cnt do if self.buf[i] < m then m = self.buf[i] end end
    return m
end

function FrameStat:Max()
    if self.cnt == 0 then return 0 end
    local m = self.buf[1]
    for i = 2, self.cnt do if self.buf[i] > m then m = self.buf[i] end end
    return m
end

function FrameStat:Count() return self.cnt end
function FrameStat:Reset()
    self.idx, self.cnt = 1, 0
    self.buf = {}
end

-- ==================== 4. 时间测量 helper (ns -> ms) ====================

local function now_ms()
    return Time.GetTicksNS() / 1e6
end

-- ==================== 5. OOP Window 主体 ====================

local SCREEN_W, SCREEN_H = 1024, 768
if not UI or not UI.Window then
    print('[demo_skinning_perf] Light.UI.Window 不可用 (headless?)')
    print('demo_skinning_perf ok (headless)')
    return
end

local Game = Light(UI.Window):New()

-- 状态
local g_state = {
    pack            = nil,    -- LoadSkinnedGLTF 返回 pack
    animator        = nil,    -- Animator
    mesh            = nil,    -- SkinnedMesh
    transform       = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1},  -- identity
    cpu_baseline    = nil,    -- {avg, min, max}
    gpu_baseline    = nil,    -- {avg, min, max}
    runtime_stat    = FrameStat.new(60),
    backend_name    = 'Unknown',
    gpu_supported   = false,
    asset_path      = nil,
    -- 交互状态
    last_render_ms  = 0,
    last_t0         = 0,
    error_msg       = nil,    -- DrawSkinnedMesh 异常时显示
    error_count     = 0,
}

-- ==================== 6. 渲染单帧 (核心 Draw + 计时) ====================

local function render_one_frame_timed(dt)
    local m = g_state.mesh
    local a = g_state.animator
    if not (m and a) then return 0 end

    -- 推进动画 (CPU 路径相同, GPU 路径关节矩阵也来自这里)
    a:Update(dt)

    -- 计时核心: 仅 DrawSkinnedMesh (这是 CPU/GPU 路径的差异点)
    local t0 = now_ms()
    local ok, err = pcall(Anim.DrawSkinnedMesh, m, a, g_state.transform, nil)
    local t1 = now_ms()

    if not ok then
        if g_state.error_count == 0 then
            print('[demo_skinning_perf] DrawSkinnedMesh raise: ' .. tostring(err))
        end
        g_state.error_msg = tostring(err)
        g_state.error_count = g_state.error_count + 1
    elseif ok and err == false then
        -- 后端返回 false + err (非 raise)
        if g_state.error_count == 0 then
            print('[demo_skinning_perf] DrawSkinnedMesh failed: false (silent fallback)')
        end
        g_state.error_count = g_state.error_count + 1
    end

    return t1 - t0
end

-- ==================== 7. Baseline 测量 ====================

local function run_baseline(mode_name, frames, warmup)
    Anim.SetSkinningMode(mode_name)
    local actual = Anim.GetSkinningMode()
    if actual ~= mode_name then
        print(string.format('  [WARN] mode "%s" 不被支持, 实际生效="%s" (将测得 %s 路径)',
                            mode_name, actual, actual))
    end

    -- 预热 (首次 GPU 上传 + shader JIT)
    for i = 1, warmup do render_one_frame_timed(1/60) end

    -- 测量
    local stat = FrameStat.new(frames)
    for i = 1, frames do
        local ms = render_one_frame_timed(1/60)
        stat:Push(ms)
    end
    return {
        avg   = stat:Avg(),
        min   = stat:Min(),
        max   = stat:Max(),
        count = stat:Count(),
        actual= actual,
    }
end

local function print_baseline_table()
    local cb = g_state.cpu_baseline
    local gb = g_state.gpu_baseline
    print('')
    print('==== Phase AW Skinning Performance Baseline ====')
    print('Backend       : ' .. g_state.backend_name)
    print('Asset         : ' .. tostring(g_state.asset_path))
    if g_state.mesh then
        print(string.format('Mesh          : %d vertices, %d indices',
                            g_state.mesh:GetVertexCount(),
                            g_state.mesh:GetIndexCount()))
    end
    print(string.format('GPU support   : %s', tostring(g_state.gpu_supported)))
    print('')
    if cb then
        print(string.format('  CPU: avg=%.3fms  min=%.3fms  max=%.3fms  (n=%d, actual=%s)',
                            cb.avg, cb.min, cb.max, cb.count, cb.actual))
    end
    if gb then
        print(string.format('  GPU: avg=%.3fms  min=%.3fms  max=%.3fms  (n=%d, actual=%s)',
                            gb.avg, gb.min, gb.max, gb.count, gb.actual))
    end
    if cb and gb and gb.avg > 1e-6 then
        print(string.format('Speedup       : %.1fx (CPU avg / GPU avg)', cb.avg / gb.avg))
    end
    print('================================================')
    print('')
    print('Entering interactive mode. Keys: G=GPU C=CPU A=AUTO R=re-baseline ESC=quit')
    print('')
end

-- ==================== 8. OSD 渲染 ====================

local function fmt_ms(ms)  return string.format('%.3fms', ms) end
local function fmt_fps(ms) if ms < 1e-6 then return 'inf' end; return string.format('%.0f', 1000.0 / ms) end

local function draw_osd()
    local stat = g_state.runtime_stat
    local mode = Anim.GetSkinningMode()

    -- 颜色按模式
    local r, gC, b
    if mode == 'gpu' then r, gC, b = 0.4, 1.0, 0.4   -- 绿
    else                   r, gC, b = 1.0, 0.8, 0.3   -- 黄 (cpu)
    end

    -- 左上 panel
    Gfx.SetColor(0, 0, 0, 0.6)
    Gfx.Rectangle(2, 8, 8, 0, 360, 180, 0)

    Gfx.SetColor(r, gC, b, 1)
    Gfx.Print('Phase AW Skinning Perf', nil, 16, 16, 0)
    Gfx.SetColor(1, 1, 1, 1)
    Gfx.Print('Backend  : ' .. g_state.backend_name,                    nil, 16, 40,  0)
    Gfx.Print('Mode     : ' .. mode,                                     nil, 16, 60,  0)
    Gfx.Print('Frame avg: ' .. fmt_ms(stat:Avg()),                       nil, 16, 80,  0)
    Gfx.Print('Frame min: ' .. fmt_ms(stat:Min()),                       nil, 16, 100, 0)
    Gfx.Print('Frame max: ' .. fmt_ms(stat:Max()),                       nil, 16, 120, 0)
    Gfx.Print('FPS (cap): ' .. fmt_fps(stat:Avg()),                      nil, 16, 140, 0)
    Gfx.SetColor(0.7, 0.7, 0.7, 1)
    Gfx.Print('G/C/A=mode  R=re-baseline  ESC=quit',                     nil, 16, 165, 0)

    -- 右下 baseline panel
    if g_state.cpu_baseline and g_state.gpu_baseline then
        local x, y = SCREEN_W - 280, SCREEN_H - 110
        Gfx.SetColor(0, 0, 0, 0.6)
        Gfx.Rectangle(2, x - 8, y - 8, 0, 280, 100, 0)
        Gfx.SetColor(1, 1, 1, 1)
        Gfx.Print('Baseline (60 frames):',                                       nil, x, y,      0)
        Gfx.Print(string.format('  CPU: %.3fms', g_state.cpu_baseline.avg),      nil, x, y + 22, 0)
        Gfx.Print(string.format('  GPU: %.3fms', g_state.gpu_baseline.avg),      nil, x, y + 42, 0)
        if g_state.gpu_baseline.avg > 1e-6 then
            Gfx.SetColor(0.5, 1.0, 0.5, 1)
            Gfx.Print(string.format('  Speedup: %.1fx',
                      g_state.cpu_baseline.avg / g_state.gpu_baseline.avg),      nil, x, y + 64, 0)
        end
    end

    -- 错误条 (如有)
    if g_state.error_count > 0 then
        Gfx.SetColor(1, 0.3, 0.3, 1)
        Gfx.Print(string.format('ERROR x%d: %s', g_state.error_count,
                  tostring(g_state.error_msg or '?')),                    nil, 16, SCREEN_H - 30, 0)
    end
end

-- ==================== 9. 加载 + baseline (在 OnOpen 同步执行) ====================

local function load_and_baseline()
    -- 9.1 资产探测
    g_state.asset_path = find_asset()
    if not g_state.asset_path then
        print_setup_hint()
        return false
    end
    print('[demo_skinning_perf] 加载 ' .. g_state.asset_path .. ' ...')

    -- 9.2 LoadSkinnedGLTF
    local pack, err = Anim.LoadSkinnedGLTF(g_state.asset_path)
    if not pack then
        print('[demo_skinning_perf] LoadSkinnedGLTF 失败: ' .. tostring(err))
        return false
    end
    g_state.pack = pack

    -- 9.3 Skeleton + Animator + Clip
    if not pack.skeleton or not pack.mesh then
        print('[demo_skinning_perf] glTF 缺少 skeleton 或 mesh primitive')
        return false
    end
    g_state.mesh     = pack.mesh
    g_state.animator = Anim.NewAnimator(pack.skeleton)

    local clipNames = pack.clipNames or {}
    for _, n in ipairs(clipNames) do
        local c = pack.clips and pack.clips[n]
        if c then g_state.animator:AddState(n, c) end
    end
    if clipNames[1] then
        g_state.animator:Play(clipNames[1])
    end

    -- 9.4 Backend / GPU 检测
    g_state.backend_name = Gfx.GetBackendName()
    Anim.SetSkinningMode('gpu')
    g_state.gpu_supported = (Anim.GetSkinningMode() == 'gpu')

    -- 9.5 Baseline (CPU + GPU)
    print('[demo_skinning_perf] 校准 CPU baseline (60 帧 + 30 预热) ...')
    g_state.cpu_baseline = run_baseline('cpu', 60, 30)

    print('[demo_skinning_perf] 校准 GPU baseline (60 帧 + 30 预热) ...')
    g_state.gpu_baseline = run_baseline('gpu', 60, 30)

    print_baseline_table()

    -- 9.6 默认进入 GPU 模式
    Anim.SetSkinningMode('gpu')
    g_state.runtime_stat:Reset()
    return true
end

-- ==================== 10. Window 回调 ====================

function Game:OnOpen()
    if not load_and_baseline() then
        print('[demo_skinning_perf] 初始化失败, 自动退出')
        self:Close()
    end
end

function Game:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end  -- 仅处理按下
    if     key == 256 then           -- ESC
        self:Close()
    elseif key == 71  then           -- G
        Anim.SetSkinningMode('gpu')
        g_state.runtime_stat:Reset()
        print('[demo_skinning_perf] mode -> gpu (actual=' .. Anim.GetSkinningMode() .. ')')
    elseif key == 67  then           -- C
        Anim.SetSkinningMode('cpu')
        g_state.runtime_stat:Reset()
        print('[demo_skinning_perf] mode -> cpu (actual=' .. Anim.GetSkinningMode() .. ')')
    elseif key == 65  then           -- A
        Anim.SetSkinningMode('auto')
        g_state.runtime_stat:Reset()
        print('[demo_skinning_perf] mode -> auto (actual=' .. Anim.GetSkinningMode() .. ')')
    elseif key == 82  then           -- R
        print('[demo_skinning_perf] re-baseline ...')
        g_state.cpu_baseline = run_baseline('cpu', 60, 30)
        g_state.gpu_baseline = run_baseline('gpu', 60, 30)
        Anim.SetSkinningMode('gpu')  -- 默认回到 GPU
        g_state.runtime_stat:Reset()
        print_baseline_table()
    end
end

function Game:Update(dt)
    -- Update 阶段不直接渲染 mesh; 实际渲染在 Draw 中并计时
    -- 这里仅推进 time-based 状态 (FrameStat 累计在 Draw 中完成)
end

function Game:Draw()
    -- 清屏色
    Gfx.SetColor(0.1, 0.1, 0.15, 1)

    -- 实际 skinning 渲染 + 计时
    local ms = render_one_frame_timed(1/60)
    g_state.runtime_stat:Push(ms)

    -- OSD
    draw_osd()
end

-- ==================== 11. 启动 ====================

Game:Open(SCREEN_W, SCREEN_H, 'ChocoLight Phase AW Skinning Perf')
while UI.Loop() do
    UI.Resume()
end

print('demo_skinning_perf ok')
