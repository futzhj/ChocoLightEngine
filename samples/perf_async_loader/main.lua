-- ============================================================================
-- Phase G.1.x — Async Asset Loader Benchmark Sample
-- ============================================================================
-- 用途:
--   量化测量 G.1.1 Shared GL Context 路径下主线程帧时分布 (P50/P95/P99/Max).
--   通过 OnFrame 内分批调 Image.LoadAsync, 测主线程是否能维持 60fps.
--
-- 期望资源:
--   samples/perf_async_loader/textures/*.png  (用户自备, 推荐 100 张 4K 或 1024x1024 图)
--   - 缺资源时打印 [skip] + 资源准备指引并退出
--
-- 输出:
--   stdout 报告 P50/P95/P99/Max + worker upload 完成数
--   样例:
--     [perf_async_loader] frames=600 P50=2.1ms P95=4.8ms P99=8.2ms Max=12.5ms
--     [perf_async_loader] loaded N/100 textures (worker probe = enabled)
--
-- 退出条件:
--   - 加载完所有图 OR 跑满 maxFrames (默认 600 帧 ≈10s @60fps)
--
-- 用法:
--   .\lumen-master\build\src\light\Release\light.exe samples\perf_async_loader\main.lua
--
-- 解读:
--   * Shared GL Context enabled (probe 成功):
--     - P95 < 16.7ms = 60fps 稳定 → worker 路径成功消除主线程尖峰
--     - P95 > 16.7ms = 仍有阻塞, 需排查 (可能 driver 共享 ctx 实现差或 GL state 切换)
--   * Fallback to main-thread upload:
--     - 此报告反映的是 G.1.0 同步上传基线, 用作对比基准
-- ============================================================================

local UI    = require('Light.UI')
local Time  = require('Light.Time')
local Gfx   = require('Light.Graphics')
local Image = Gfx and Gfx.Image
local FS    = require('Light.Filesystem')

-- ========== 1. 参数 ==========

local MAX_FRAMES        = 600     -- 最多跑 600 帧 (10s @60fps)
local LOADS_PER_FRAME   = 5       -- 每帧最多 dispatch 5 张 LoadAsync (避免单帧 queue overflow)
local FRAME_BUDGET_MS   = 16.7    -- 60fps budget

-- 资源目录基于 arg[0] (sample 自身路径) 推断, 避免依赖 light.exe 当前工作目录
local SAMPLE_DIR        = (arg and arg[0] and arg[0]:match("(.+[/\\])")) or './'
local TEXTURES_SUBDIR   = SAMPLE_DIR .. 'textures'

if not (UI and UI.Window) then
    print("[skip] Light.UI.Window unavailable (headless mode)")
    return
end
if not (Image and Image.LoadAsync) then
    print("[skip] Image.LoadAsync unavailable (engine build missing G.1)")
    return
end
if not (Time and Time.GetTicksNS) then
    print("[skip] Light.Time.GetTicksNS unavailable")
    return
end

-- ========== 2. 扫描资源 ==========

local function scan_pngs()
    if not (FS and FS.GlobDirectory) then return {} end
    -- GlobDirectory(path, pattern) — pattern 仅支持简单 glob, 用 "*" 取所有再 Lua 过滤
    local entries, err = FS.GlobDirectory(TEXTURES_SUBDIR, "*")
    if not entries then
        print(string.format("[debug] GlobDirectory('%s') failed: %s", TEXTURES_SUBDIR, tostring(err)))
        return {}
    end
    local pngs = {}
    for _, name in ipairs(entries) do
        local lower = name:lower()
        if lower:match("%.png$") or lower:match("%.jpg$") or lower:match("%.jpeg$") then
            table.insert(pngs, TEXTURES_SUBDIR .. '/' .. name)
        end
    end
    return pngs
end

local g_pngs = scan_pngs()
if #g_pngs == 0 then
    print("[skip] no textures found under " .. TEXTURES_SUBDIR)
    print("[hint] 准备资源后重跑:")
    print("  1. mkdir " .. TEXTURES_SUBDIR)
    print("  2. 复制 100 张 PNG (推荐 1024x1024 或更大) 到该目录")
    print("  3. 重新执行: light.exe samples/perf_async_loader/main.lua")
    return
end

print(string.format("[perf_async_loader] discovered %d texture files", #g_pngs))

-- ========== 3. 状态 ==========

local Demo            = Light(UI.Window):New()
local g_frame         = 0
local g_dispatch_idx  = 1      -- 下一个要 dispatch 的 PNG index
local g_loaded        = 0      -- 已完成加载的数量 (Future Ready/Error)
local g_failed        = 0
local g_futures       = {}     -- pending futures
local g_frame_times   = {}     -- 每帧时长 (ms)
local g_report_done   = false  -- 报告幂等开关 (Update + OnClose 双重保护)

-- ========== 4. 报告输出 (helper, 在 self:Close() 前调一次, 进程退出路径有崩溃也已输出数据) ==========

local function print_report()
    if g_report_done then return end
    g_report_done = true

    table.sort(g_frame_times)
    local n = #g_frame_times
    local function pct(p)
        if n == 0 then return 0 end
        local idx = math.max(1, math.min(n, math.floor(n * p / 100 + 0.5)))
        return g_frame_times[idx]
    end
    local sum, max_v = 0, 0
    for _, v in ipairs(g_frame_times) do
        sum = sum + v
        if v > max_v then max_v = v end
    end
    local avg = n > 0 and (sum / n) or 0

    print("")
    print(string.format("[perf_async_loader] frames=%d  avg=%.2fms  P50=%.2fms  P95=%.2fms  P99=%.2fms  Max=%.2fms",
                        n, avg, pct(50), pct(95), pct(99), max_v))
    print(string.format("[perf_async_loader] loaded %d/%d textures  (errors=%d)",
                        g_loaded, #g_pngs, g_failed))
    if n == 0 then
        print("[perf_async_loader] WARN: no frames captured (Update may not have run)")
    elseif pct(95) < FRAME_BUDGET_MS then
        print(string.format("[perf_async_loader] PASS: P95 < %.1fms (60fps budget held)", FRAME_BUDGET_MS))
    else
        print(string.format("[perf_async_loader] WARN: P95 >= %.1fms (main thread blocked, check worker probe / driver)", FRAME_BUDGET_MS))
    end
end

-- ========== 5. Update (每帧主线程逻辑, dt 由引擎提供) ==========
-- 注: light_ui.cpp 回调约定为 Update(dt) + Draw(), 不是 OnFrame()

function Demo:Update(dt)
    io.stdout:setvbuf("no")    -- 强制 flush, 让数据先于退出路径崩溃出现在 stdout
    -- dt 单位 = 秒, 转 ms 收集; 首帧 dt 可能为 0, 不计入
    if dt and dt > 0 then
        table.insert(g_frame_times, dt * 1000.0)
    end
    g_frame = g_frame + 1

    -- 5.1 dispatch 新 LoadAsync (每帧最多 LOADS_PER_FRAME 张)
    local dispatched = 0
    while dispatched < LOADS_PER_FRAME and g_dispatch_idx <= #g_pngs do
        local f = Image.LoadAsync(g_pngs[g_dispatch_idx])
        if f then table.insert(g_futures, f) end
        g_dispatch_idx = g_dispatch_idx + 1
        dispatched = dispatched + 1
    end

    -- 5.2 poll 已 Ready/Error 的 Future
    local pending = {}
    for _, f in ipairs(g_futures) do
        if f:IsReady() or f:IsError() then
            if f:IsError() then g_failed = g_failed + 1 end
            g_loaded = g_loaded + 1
        else
            table.insert(pending, f)
        end
    end
    g_futures = pending

    -- 5.3 退出条件: 跑满帧数 OR 全部加载完成 (并 grace 5 帧让最后 fence 翻完)
    if g_frame >= MAX_FRAMES or (g_dispatch_idx > #g_pngs and #g_futures == 0 and g_frame > 5) then
        -- 先输出报告 (即使后续 light.exe 退出路径有问题, 数据也已在 stdout)
        print_report()
        self:Close()
    end
end

-- Draw 留空以避免 nil method 报错; benchmark 不需要任何绘制
function Demo:Draw() end

function Demo:OnClose()
    -- 兜底再调一次 (幂等)
    print_report()
end

-- ========== 6. 启动 ==========

local ok, err = pcall(function() Demo:Open(640, 480, "G.1.x perf_async_loader") end)
if not ok then
    print("[skip] window open failed: " .. tostring(err))
    return
end

while UI.Loop() do UI.Resume() end

-- 主循环退出后兜底 (针对用户手动关窗时的路径)
print_report()
print("perf_async_loader sample done")
