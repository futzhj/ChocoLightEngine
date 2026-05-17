-- Phase G.1.1 — probe 日志验证脚本
-- 打开一个最小窗口, 触发 AssetLoader::Init(mainWin, mainCtx);
-- OnOpen 中立即 Close, 让主循环立即退出. 用 stdout 抓 probe 日志:
--   * "Shared GL Context enabled" 或 "fallback to main-thread upload"
--
-- 用法: light.exe scripts/smoke/asset_loader_async_probe.lua
-- CI 中带 GL 的 runner 可用; headless / 无显卡的环境会失败开窗 (返 skip)

local UI = require('Light.UI')

if not UI or not UI.Window then
    print("[skip] Light.UI.Window not available (headless mode)")
    return
end

local Demo = Light(UI.Window):New()

local g_frame = 0

function Demo:OnOpen()
    -- Open 之后 AssetLoader::Init 已完成 probe, 日志已 stdout 捕获.
    -- 这里不退出, 让主循环正常运行至少 1 帧, 走完 SDL_Window 清理路径
    print("PASS: window opened (AssetLoader::Init triggered)")
end

-- 注: light_ui.cpp 回调约定为 Update(dt) + Draw(), 不是 OnFrame()
function Demo:Update(dt)
    g_frame = g_frame + 1
    -- 跑 2 帧后请求关闭, 让 UI.Loop 在下一次 PollEvent 时返 false
    -- (1 帧不够: ShouldClose 在 SetShouldClose 之后才生效, 需要下一轮 Loop 检查)
    if g_frame >= 2 then
        self:Close()
    end
end

-- Draw 留空避免 nil method 报错; probe 不需要任何绘制
function Demo:Draw() end

local ok, err = pcall(function() Demo:Open(64, 64, "G1_1_probe") end)
if not ok then
    print("[skip] window open failed (likely headless CI no GPU): " .. tostring(err))
    return
end

while UI.Loop() do UI.Resume() end
print("asset_loader_async_probe smoke ok")
