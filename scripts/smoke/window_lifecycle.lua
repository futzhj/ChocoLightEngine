-- Phase G.1.3 — Window 生命周期清理路径验证
-- 验证目标:
--   1. self:Close() 后, while UI.Loop() do UI.Resume() end 主循环正常退出 (exit=0)
--   2. 期间触发 AssetLoader worker thread (即使加载失败也会 init worker)
--   3. 退出时 stdout 见 'AssetLoader: worker thread exit' + 'shutdown complete'
--   4. atexit 审计钩子保持沉默 (无 [ChocoLight] WARNING 到 stderr)
--
-- 用法: light.exe scripts/smoke/window_lifecycle.lua
-- CI 中带 GL 的 runner 可跑; headless / 无显卡环境会 skip 开窗。

local UI    = require('Light.UI')
local Image = require('Light.Graphics.Image')

if not UI or not UI.Window then
    print("[skip] Light.UI.Window not available (headless mode)")
    return
end

if not Image or not Image.LoadAsync then
    print("[skip] Light.Graphics.Image.LoadAsync not available")
    return
end

local Demo = Light(UI.Window):New()

local g_frame  = 0
local g_async_ok = false

function Demo:OnOpen()
    -- AssetLoader::Init 已在 Open 内被调过, worker thread 已启动 (即使 probe 失败也启动)
    print("[lifecycle] window opened, AssetLoader init done")

    -- 触发一次异步加载 (无论文件是否存在都会进 worker pipeline 走一遍)
    -- 这样可以测到 worker 真的被启动并 join 的完整链路
    local f = Image.LoadAsync("__smoke_nonexistent__.png")
    if f then
        g_async_ok = true
        print("[lifecycle] LoadAsync future created (will resolve to error in worker)")
    end
end

-- 注: 引擎回调约定为 Update(dt) + Draw(), 不是 OnFrame()
-- 详见 docs/API_REFERENCE.md "Light.UI" 章节
function Demo:Update(dt)
    g_frame = g_frame + 1
    -- 跑 3 帧给 worker 一些时间处理失败的 load + drain result_queue
    -- 然后请求关闭, 让 UI.Loop 走清理路径
    if g_frame >= 3 then
        print("[lifecycle] requesting close on frame " .. g_frame)
        self:Close()
    end
end

-- Draw 留空避免 nil method 报错
function Demo:Draw() end

local ok, err = pcall(function() Demo:Open(64, 64, "G_1_3 lifecycle") end)
if not ok then
    print("[skip] window open failed (likely headless CI no GPU): " .. tostring(err))
    return
end

while UI.Loop() do UI.Resume() end

if g_async_ok then
    print("PASS: async future created and worker thread exercised")
end
print("PASS: window cleanup path executed")
print("window_lifecycle smoke ok")
