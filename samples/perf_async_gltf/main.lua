---@diagnostic disable: undefined-field
-- ============================================================================
-- Phase G.1.5 T6 — Async GLTF Loader Benchmark Sample
-- ============================================================================
-- 用途:
--   量化测量 Phase G.1.5 Mesh.LoadGLTFAsync(withMaterial=true) 路径的主线程
--   帧时分布 (P50/P95/P99/Max), 验证 worker shared_ctx 路径下主线程 P95 < 16.7ms.
--
--   与 perf_async_loader (单 PNG image 上传) 区别:
--   - 走 GLTF 完整路径 (cgltf parse + 1-5 类 material texture + 5 类 worker upload + fence)
--   - 走 with_material=true 双值返回 + Material userdata 生成 (Lua 层栈管理)
--   - 主线程 Tick 路径含 5 texture WriteSlots
--
-- 期望资源:
--   默认: scripts/smoke/assets_g1_5/test_box_textured.glb (G.1.5 fixture, 1×1 红色 PNG)
--         适合测主线程 调度/Tick/fence/上传 路径开销 (非 GPU decode 性能).
--   自定义: arg[1] 指定外部 .glb 路径 (如 Khronos sample model DamagedHelmet.glb)
--           适合测真实 PBR texture (1024×1024 - 4096×4096) GPU decode + upload 全链路.
--   重复次数: arg[2] 指定 (默认 100 次), 总加载量 = 重复次数 × 5 类 texture
--
-- 输出:
--   stdout 报告主线程 P50/P95/P99/Max + 平均加载耗时 + 失败数
--   样例:
--     [perf_async_gltf] frames=180 avg=2.1ms P50=2.0ms P95=4.8ms P99=8.2ms Max=12.5ms
--     [perf_async_gltf] loaded 100/100 (errors=0) avg load=18ms
--     [perf_async_gltf] PASS: P95 < 16.7ms (60fps budget held)
--
-- 用法:
--   .\lumen-master\build\src\light\Release\light.exe samples\perf_async_gltf\main.lua
--   .\lumen-master\build\src\light\Release\light.exe samples\perf_async_gltf\main.lua "path\to\model.glb" 50 1
--
-- CLI 参数:
--   arg[1] : 模型路径 (默认 G.1.5 fixture)
--   arg[2] : 重复加载次数 (默认 100)
--   arg[3] : 每帧最多 dispatch 数 (默认 5; 调小为 1 可降低单帧 dispatch+Tick 压力)
--
-- 解读:
--   * Shared GL Context enabled (probe 成功):
--     - P95 < 16.7ms = 60fps 稳定 → worker 路径成功消除主线程尖峰
--     - P95 > 16.7ms = 仍有阻塞, 需排查 (5 texture WriteSlots + Material 创建可能是瓶颈)
--   * Fallback to main-thread upload (Supports3D=false):
--     - sample 应立即 Error (LoadGLTFAsync fast-path), 报 0 加载成功
-- ============================================================================

local function safe_require(n)
    local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil
end
local UI    = safe_require('Light.UI')
local Gfx   = safe_require('Light.Graphics')
local Mesh  = Gfx and Gfx.Mesh

if not Gfx then print('[perf_async_gltf] Light.Graphics 不可用'); return end
if type(Mesh) ~= 'table' or type(Mesh.LoadGLTFAsync) ~= 'function' then
    print('[skip] Mesh.LoadGLTFAsync unavailable (need Phase G.1.5)')
    return
end
if not (UI and UI.Window) then
    print('[skip] Light.UI.Window unavailable (headless mode)')
    return
end

-- ========== 1. 参数 (CLI: arg[1]=model_path, arg[2]=repeat_count) ==========

local MODEL_PATH       = (arg and arg[1]) or 'scripts/smoke/assets_g1_5/test_box_textured.glb'
local REPEAT_COUNT     = tonumber(arg and arg[2]) or 100
local LOADS_PER_FRAME  = tonumber(arg and arg[3]) or 5    -- 每帧最多 dispatch 数 (CLI 可调)
local MAX_FRAMES       = 600    -- 10s @ 60fps
local FRAME_BUDGET_MS  = 16.7   -- 60fps budget
local WARMUP_FRAMES    = 3      -- 前 3 帧 (window 初始化 / shader 编译) 不计入统计

-- 验证模型文件存在 (lfs / FS 都不一定可用, 用 io.open 兜底)
do
    local f = io.open(MODEL_PATH, 'rb')
    if not f then
        print(string.format('[skip] model file not found: %s', MODEL_PATH))
        print('[hint] 准备模型:')
        print('  - 默认 fixture: python dev/gen_test_glb.py (生成 1×1 PNG fixture)')
        print('  - 自定义: ' .. arg[0] .. ' "path/to/your.glb" [repeat_count]')
        return
    end
    f:close()
end

print(string.format('[perf_async_gltf] model=%s repeat=%d loads_per_frame=%d max_frames=%d',
                    MODEL_PATH, REPEAT_COUNT, LOADS_PER_FRAME, MAX_FRAMES))

-- ========== 2. 状态 ==========

local Demo            = Light(UI.Window):New()
local g_frame         = 0
local g_dispatch_idx  = 0       -- 已 dispatch 数
local g_loaded        = 0       -- 已完成 (Ready / Error)
local g_failed        = 0
local g_futures       = {}      -- pending: { future, dispatch_ms }
local g_frame_times   = {}      -- 每帧时长 (ms)
local g_load_times    = {}      -- 每次加载从 dispatch → ready 总用时 (ms)
local g_report_done   = false

-- 用 ns 精度获取时间戳, 转 ms (Time.GetTicks 只到 ms 精度, 不够细粒度)
local Time = safe_require('Light.Time')
local function now_ms()
    if Time and Time.GetTicksNS then return Time.GetTicksNS() / 1e6 end
    if Time and Time.GetTicks   then return Time.GetTicks() end
    return 0
end

-- ========== 3. 报告输出 (幂等) ==========

local function pct(arr, p)
    local n = #arr
    if n == 0 then return 0 end
    local idx = math.max(1, math.min(n, math.floor(n * p / 100 + 0.5)))
    return arr[idx]
end

local function print_report()
    if g_report_done then return end
    g_report_done = true

    -- 帧时间统计
    table.sort(g_frame_times)
    local fn = #g_frame_times
    local fsum, fmax = 0, 0
    for _, v in ipairs(g_frame_times) do
        fsum = fsum + v
        if v > fmax then fmax = v end
    end
    local favg = fn > 0 and (fsum / fn) or 0

    -- 加载耗时统计
    table.sort(g_load_times)
    local ln = #g_load_times
    local lsum = 0
    for _, v in ipairs(g_load_times) do lsum = lsum + v end
    local lavg = ln > 0 and (lsum / ln) or 0

    print('')
    print(string.format('[perf_async_gltf] frames=%d avg=%.2fms P50=%.2fms P95=%.2fms P99=%.2fms Max=%.2fms',
                        fn, favg, pct(g_frame_times, 50), pct(g_frame_times, 95),
                        pct(g_frame_times, 99), fmax))
    print(string.format('[perf_async_gltf] loaded %d/%d (errors=%d) avg_load=%.1fms P95_load=%.1fms',
                        g_loaded, REPEAT_COUNT, g_failed, lavg, pct(g_load_times, 95)))

    if fn == 0 then
        print('[perf_async_gltf] WARN: no frames captured (Update may not have run)')
    elseif pct(g_frame_times, 95) < FRAME_BUDGET_MS then
        print(string.format('[perf_async_gltf] PASS: P95 < %.1fms (60fps budget held)', FRAME_BUDGET_MS))
    else
        print(string.format('[perf_async_gltf] WARN: P95 >= %.1fms (main thread blocked; check Tick / texture WriteSlots / worker probe)', FRAME_BUDGET_MS))
    end
end

-- ========== 4. Update (主线程逻辑) ==========

function Demo:Update(dt)
    io.stdout:setvbuf('no')
    g_frame = g_frame + 1
    -- WARMUP: 前 N 帧 (window 初始化 / shader 编译) 不计入统计, 避免污染 P95/P99/Max
    if g_frame > WARMUP_FRAMES and dt and dt > 0 then
        table.insert(g_frame_times, dt * 1000.0)
    end

    -- 4.1 dispatch (每帧最多 LOADS_PER_FRAME 个 LoadGLTFAsync)
    local dispatched = 0
    while dispatched < LOADS_PER_FRAME and g_dispatch_idx < REPEAT_COUNT do
        local f = Mesh.LoadGLTFAsync(MODEL_PATH, 0, true)
        if f then
            table.insert(g_futures, { future = f, t0 = now_ms() })
        end
        g_dispatch_idx = g_dispatch_idx + 1
        dispatched = dispatched + 1
    end

    -- 4.2 poll 已 Ready/Error 的 Future
    local pending = {}
    for _, item in ipairs(g_futures) do
        local f = item.future
        if f:IsReady() or f:IsError() then
            if f:IsError() then
                g_failed = g_failed + 1
            else
                table.insert(g_load_times, now_ms() - item.t0)
            end
            g_loaded = g_loaded + 1
        else
            table.insert(pending, item)
        end
    end
    g_futures = pending

    -- 4.3 退出条件: 跑满帧数 OR 全部完成 + grace 5 帧
    local all_done = (g_dispatch_idx >= REPEAT_COUNT) and (#g_futures == 0) and (g_frame > 5)
    if g_frame >= MAX_FRAMES or all_done then
        print_report()
        self:Close()
    end
end

function Demo:Draw() end

function Demo:OnClose()
    print_report()  -- 兜底
end

-- ========== 5. 启动 ==========

local ok, err = pcall(function() Demo:Open(640, 480, 'G.1.5 perf_async_gltf') end)
if not ok then
    print('[skip] window open failed: ' .. tostring(err))
    return
end

while UI.Loop() do UI.Resume() end

print_report()
print('perf_async_gltf sample done')
