-- ============================================================================
-- ChocoLight Phase E.3 — HDR + ACES Tonemapping demo
-- ============================================================================
-- 演示 HDR 离屏渲染 + ACES filmic tonemap 对高亮场景的压缩能力.
--
-- 画面布局:
--   左半:  10 个矩形, 亮度线性递增 (0.2, 0.6, 1.0, 1.4, 1.8, 2.2, 2.6, 3.0, 3.4, 3.8)
--   右半:  相同布局, 带红/绿/蓝色调
--   底部:  OSD 显示 HDR 状态 / Exposure / Gamma / 操作提示
--
-- 预期效果:
--   HDR OFF  -> 顶点颜色 > 1.0 被硬件 clamp 为白色, 亮度 > 1.0 的 6 个矩形
--              视觉上无差异 (全部白块)
--   HDR ON   -> ACES 曲线将 0 ~ +∞ 压缩到 0 ~ 1, 亮度 > 1.0 的矩形仍可区分
--              (随亮度增加, 颜色从饱和 -> 压缩 -> 趋近白色, 但保留细节)
--
-- 控制:
--   H       : 切换 HDR 启用 / 禁用
--   Z / X   : 减小 / 增大 Exposure (步长 0.1, [0.1, 5.0])
--   C / V   : 减小 / 增大 Gamma    (步长 0.1, [1.0, 3.0])
--   R       : 重置 Exposure=1.0, Gamma=2.2
--   ESC     : 退出
--
-- 兼容: Lua 5.1 + ChocoLight Light.Graphics.HDR / UI.Window / Time
-- 后端: 仅 GL33 支持 HDR (RGBA16F RT), Legacy 后端 HDR.IsSupported 返回 false.
-- ============================================================================

-- ==================== 1. 模块加载 ====================

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
    print('[demo_hdr] Light.Graphics 不可用')
    print('demo_hdr ok (no graphics)')
    return
end

local HDR = Gfx.HDR
if type(HDR) ~= 'table' then
    print('[demo_hdr] Light.Graphics.HDR 子表缺失')
    print('demo_hdr ok (no HDR subtable)')
    return
end

print('==== ChocoLight Phase E.3 HDR demo ====')
print('[demo_hdr] Backend: ' ..
    tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_hdr] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))

-- ==================== 2. Headless 探测 ====================

if not UI or not UI.Window then
    print('[demo_hdr] UI.Window 不可用, 仅执行 API 探测')
    print('  HDR.IsEnabled       = ' .. tostring(HDR.IsEnabled()))
    print('  HDR.GetExposure     = ' .. tostring(HDR.GetExposure()))
    print('  HDR.GetGamma        = ' .. tostring(HDR.GetGamma()))
    print('  HDR.GetSceneTexture = ' .. tostring(HDR.GetSceneTexture()))
    print('demo_hdr ok (headless API check)')
    return
end

-- ==================== 3. 打开窗口 ====================

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, openErr = Window.Open(WIN_W, WIN_H, 'Phase E.3 — HDR + ACES Tonemap Demo')
if not win then
    print('[demo_hdr] Window.Open 失败: ' .. tostring(openErr))
    print('demo_hdr ok (no window)')
    return
end

-- ==================== 4. 启用 HDR ====================
-- 注意: 必须在 Window.Open 之后再 Enable, 否则无 GL context.

local hdrEnabled = false
local function tryEnableHDR()
    if not HDR.IsSupported() then
        print('[demo_hdr] 当前后端不支持 HDR (Legacy backend?), 留在 LDR 模式')
        return false
    end
    local ok = HDR.Enable(WIN_W, WIN_H)
    if ok then
        print('[demo_hdr] HDR.Enable(' .. WIN_W .. ', ' .. WIN_H .. ') = true')
        return true
    end
    print('[demo_hdr] HDR.Enable 失败')
    return false
end

hdrEnabled = tryEnableHDR()

-- ==================== 5. 主循环 ====================

local exposure = 1.0
local gamma    = 2.2
local lastTime = (Time and Time.GetSeconds and Time.GetSeconds()) or 0

-- 输入消抖 (防按键长按连触发)
local keyCooldown = {}
local function keyTap(name)
    if win:IsKeyPressed(name) then
        if (keyCooldown[name] or 0) <= 0 then
            keyCooldown[name] = 0.15   -- 150ms 去抖
            return true
        end
    end
    return false
end

local function clamp(v, lo, hi)
    if v < lo then return lo end
    if v > hi then return hi end
    return v
end

-- 10 个亮度档位 (可超过 1.0 以测试 HDR 压缩)
local BRIGHTS = { 0.2, 0.6, 1.0, 1.4, 1.8, 2.2, 2.6, 3.0, 3.4, 3.8 }
local BAR_W = 80
local BAR_H = 180
local BAR_GAP = 8

while win:IsOpen() do
    -- 时间步进
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt = now - lastTime
    lastTime = now
    if dt > 0.1 then dt = 0.1 end

    -- 按键冷却衰减
    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    -- 事件处理
    win:PollEvents()

    if win:IsKeyPressed('escape') then
        win:Close()
        break
    end

    -- H: 切换 HDR
    if keyTap('h') then
        if hdrEnabled then
            HDR.Disable()
            hdrEnabled = false
            print('[demo_hdr] HDR OFF')
        else
            hdrEnabled = tryEnableHDR()
            print('[demo_hdr] HDR ' .. (hdrEnabled and 'ON' or 'OFF (enable failed)'))
        end
    end

    -- Z/X: Exposure -/+
    if keyTap('z') then
        exposure = clamp(exposure - 0.1, 0.1, 5.0)
        HDR.SetExposure(exposure)
    end
    if keyTap('x') then
        exposure = clamp(exposure + 0.1, 0.1, 5.0)
        HDR.SetExposure(exposure)
    end

    -- C/V: Gamma -/+
    if keyTap('c') then
        gamma = clamp(gamma - 0.1, 1.0, 3.0)
        HDR.SetGamma(gamma)
    end
    if keyTap('v') then
        gamma = clamp(gamma + 0.1, 1.0, 3.0)
        HDR.SetGamma(gamma)
    end

    -- R: reset
    if keyTap('r') then
        exposure = 1.0
        gamma    = 2.2
        HDR.SetExposure(exposure)
        HDR.SetGamma(gamma)
    end

    -- 渲染
    win:BeginFrame(0.05, 0.05, 0.08, 1.0)

    -- 左半: 白色梯度
    local startX = 40
    local y0     = 80
    for i, b in ipairs(BRIGHTS) do
        local x = startX + (i - 1) * (BAR_W + BAR_GAP)
        Gfx.SetColor(b, b, b, 1.0)                          -- 灰度梯度
        Gfx.Rectangle(Gfx.FillMode, x, y0, 0, BAR_W, BAR_H, 0)
        Gfx.SetColor(1, 1, 1, 1)
        if win.DrawText then
            win:DrawText(x + 4, y0 + BAR_H + 4, string.format('%.1f', b), 1, 1, 1, 1)
        end
    end

    -- 右半: 彩色梯度 (突出 HDR 对彩色压缩的效果)
    local colOffY = 80 + BAR_H + 40
    local palette = {
        {1.0, 0.3, 0.2},   -- 暖红
        {0.2, 1.0, 0.3},   -- 荧光绿
        {0.2, 0.4, 1.0},   -- 深蓝
    }
    for ci, rgb in ipairs(palette) do
        local yy = colOffY + (ci - 1) * (BAR_H * 0.4 + 8)
        for i, b in ipairs(BRIGHTS) do
            local x = startX + (i - 1) * (BAR_W + BAR_GAP)
            Gfx.SetColor(rgb[1] * b, rgb[2] * b, rgb[3] * b, 1.0)
            Gfx.Rectangle(Gfx.FillMode, x, yy, 0, BAR_W, BAR_H * 0.4, 0)
        end
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if win.DrawText then
        local y = 8
        local line = function(s)
            win:DrawText(8, y, s, 1, 1, 1, 1)
            y = y + 16
        end
        line(string.format('HDR: %s | Supported: %s | Backend: %s',
            hdrEnabled and 'ON' or 'OFF',
            tostring(HDR.IsSupported()),
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?')))
        line(string.format('Exposure: %.2f   Gamma: %.2f   SceneTex: %d',
            HDR.GetExposure(), HDR.GetGamma(), HDR.GetSceneTexture()))
        line('Brightness scale: 0.2 ... 3.8 (values > 1.0 need HDR to resolve)')
        line('Keys: H=toggle HDR  Z/X=exposure -/+  C/V=gamma -/+  R=reset  ESC=quit')
    end

    win:EndFrame()
end

-- ==================== 6. 清理 ====================

if hdrEnabled then
    HDR.Disable()
    print('[demo_hdr] HDR.Disable (cleanup)')
end

if win.Close and win:IsOpen() then win:Close() end
print('demo_hdr ok')
