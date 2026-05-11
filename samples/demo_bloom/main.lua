-- ============================================================================
-- ChocoLight Phase E.4 — Bloom post-processing demo
-- ============================================================================
-- 演示 Bloom (亮度提取 + 多尺度高斯模糊金字塔 + 加性合成) 后处理.
--
-- 画面布局:
--   亮点阵列: 多个高亮 (顶点色 > 1.0) 小方块, 散布在黑色背景上,
--             Bloom OFF 时为硬边缘色块, Bloom ON 时四周浮现柔和辉光.
--   底部 OSD: Bloom / HDR 状态 + 参数 + 操作提示.
--
-- 预期效果:
--   Bloom OFF -> 仅看见亮度 > 1.0 的硬边色块, 周围背景全黑.
--   Bloom ON  -> 亮色块四周扩散出彩色光晕, 强度随 Intensity, 范围随 Radius.
--
-- 控制:
--   B       : 切换 Bloom ON / OFF
--   1 / 2   : Threshold -/+ (步长 0.1; clamp [0, +inf))
--   3 / 4   : Intensity -/+ (步长 0.1; clamp [0, +inf))
--   5 / 6   : Radius    -/+ (步长 0.05; clamp [0, 1])
--   7 / 8   : Levels    -/+ (clamp [2, 8]; 下次 Enable/Resize 生效)
--   R       : 重置默认参数 (Thr=1.0 Int=0.8 Rad=0.7 Lv=5)
--   ESC     : 退出
--
-- 依赖: Light.UI.Window + Light.Graphics.HDR + Light.Graphics.Bloom + Light.Time
-- 后端: 仅 GL33 支持 (Bloom.IsSupported = false 时 fallback API surface 演示).
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
    print('[demo_bloom] Light.Graphics 不可用')
    print('demo_bloom ok (no graphics)')
    return
end

local HDR   = Gfx.HDR
local Bloom = Gfx.Bloom
if type(HDR) ~= 'table' or type(Bloom) ~= 'table' then
    print('[demo_bloom] 需要 Light.Graphics.HDR + Light.Graphics.Bloom 子表')
    print('demo_bloom ok (subtable missing)')
    return
end

print('==== ChocoLight Phase E.4 Bloom demo ====')
print('[demo_bloom] Backend           = ' ..
    tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_bloom] HDR.IsSupported   = ' .. tostring(HDR.IsSupported()))
print('[demo_bloom] Bloom.IsSupported = ' .. tostring(Bloom.IsSupported()))

-- ==================== 2. Headless 探测 ====================

if not UI or not UI.Window then
    print('[demo_bloom] UI.Window 不可用, 仅执行 API 探测')
    print('  Bloom.IsEnabled    = ' .. tostring(Bloom.IsEnabled()))
    print('  Bloom.GetThreshold = ' .. tostring(Bloom.GetThreshold()))
    print('  Bloom.GetIntensity = ' .. tostring(Bloom.GetIntensity()))
    print('  Bloom.GetRadius    = ' .. tostring(Bloom.GetRadius()))
    print('  Bloom.GetLevels    = ' .. tostring(Bloom.GetLevels()))
    print('  Bloom.GetAutoEnable= ' .. tostring(Bloom.GetAutoEnable()))
    print('demo_bloom ok (headless API check)')
    return
end

-- ==================== 3. 打开窗口 ====================

local Window = UI.Window
local WIN_W, WIN_H = 960, 540
local win, openErr = Window.Open(WIN_W, WIN_H, 'Phase E.4 — Bloom Demo')
if not win then
    print('[demo_bloom] Window.Open 失败: ' .. tostring(openErr))
    print('demo_bloom ok (no window)')
    return
end

-- ==================== 4. 启用 HDR + Bloom ====================
-- Bloom 依赖 HDR RT, 默认 autoEnable=true 时 HDR.Enable 会拉起 Bloom.

local hdrEnabled = false
if HDR.IsSupported() then
    hdrEnabled = HDR.Enable(WIN_W, WIN_H)
    print('[demo_bloom] HDR.Enable = ' .. tostring(hdrEnabled))
end
if hdrEnabled then
    print('[demo_bloom] Bloom.IsEnabled (auto) = ' .. tostring(Bloom.IsEnabled()))
end

-- ==================== 5. 主循环 ====================

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

-- 亮点阵列 (顶点色 R/G/B 可 > 1.0; 仅 HDR ON 时才能体现 Bloom 辉光)
local SPOTS = {
    { x = 120, y = 120, w = 40, h = 40, r = 3.5, g = 0.6, b = 0.4 },   -- 红
    { x = 280, y = 200, w = 60, h = 60, r = 0.5, g = 3.0, b = 0.8 },   -- 绿
    { x = 480, y = 100, w = 30, h = 30, r = 0.4, g = 0.6, b = 3.8 },   -- 蓝
    { x = 620, y = 280, w = 80, h = 80, r = 2.5, g = 2.2, b = 0.8 },   -- 暖白
    { x = 800, y = 160, w = 40, h = 40, r = 3.0, g = 2.0, b = 3.0 },   -- 紫
    { x = 180, y = 360, w = 50, h = 50, r = 3.5, g = 2.5, b = 0.4 },   -- 橙
    { x = 380, y = 400, w = 36, h = 36, r = 0.6, g = 3.6, b = 3.0 },   -- 青
    { x = 720, y = 420, w = 64, h = 64, r = 3.6, g = 0.5, b = 1.6 },   -- 品红
}

while win:IsOpen() do
    local now = (Time and Time.GetSeconds and Time.GetSeconds()) or (lastTime + 0.016)
    local dt = now - lastTime
    lastTime = now
    if dt > 0.1 then dt = 0.1 end

    for k, v in pairs(keyCooldown) do
        keyCooldown[k] = math.max(0, v - dt)
    end

    win:PollEvents()

    if win:IsKeyPressed('escape') then
        win:Close()
        break
    end

    -- B: 切换 Bloom
    if keyTap('b') then
        if Bloom.IsEnabled() then
            Bloom.Disable()
            print('[demo_bloom] Bloom OFF')
        else
            if not hdrEnabled then
                hdrEnabled = HDR.IsSupported() and HDR.Enable(WIN_W, WIN_H) or false
            end
            local ok = hdrEnabled and Bloom.Enable(WIN_W, WIN_H) or false
            print('[demo_bloom] Bloom ' .. (ok and 'ON' or 'OFF (enable failed)'))
        end
    end

    -- 1/2: Threshold -/+
    if keyTap('1') then
        Bloom.SetThreshold(clampNum(Bloom.GetThreshold() - 0.1, 0.0))
    end
    if keyTap('2') then
        Bloom.SetThreshold(clampNum(Bloom.GetThreshold() + 0.1, 0.0))
    end

    -- 3/4: Intensity -/+
    if keyTap('3') then
        Bloom.SetIntensity(clampNum(Bloom.GetIntensity() - 0.1, 0.0))
    end
    if keyTap('4') then
        Bloom.SetIntensity(clampNum(Bloom.GetIntensity() + 0.1, 0.0))
    end

    -- 5/6: Radius -/+
    if keyTap('5') then
        Bloom.SetRadius(clampNum(Bloom.GetRadius() - 0.05, 0.0, 1.0))
    end
    if keyTap('6') then
        Bloom.SetRadius(clampNum(Bloom.GetRadius() + 0.05, 0.0, 1.0))
    end

    -- 7/8: Levels -/+ (下次 Enable/Resize 生效)
    if keyTap('7') then
        Bloom.SetLevels(Bloom.GetLevels() - 1)
        print('[demo_bloom] Levels -> ' .. Bloom.GetLevels() .. ' (重启 Bloom 生效)')
    end
    if keyTap('8') then
        Bloom.SetLevels(Bloom.GetLevels() + 1)
        print('[demo_bloom] Levels -> ' .. Bloom.GetLevels() .. ' (重启 Bloom 生效)')
    end

    -- R: 重置默认参数
    if keyTap('r') then
        Bloom.SetThreshold(1.0)
        Bloom.SetIntensity(0.8)
        Bloom.SetRadius(0.7)
        Bloom.SetLevels(5)
        if Bloom.IsEnabled() then
            Bloom.Resize(WIN_W, WIN_H)   -- 重建 pyramid 让 Levels 生效
        end
        print('[demo_bloom] reset defaults')
    end

    -- 渲染
    win:BeginFrame(0.0, 0.0, 0.0, 1.0)   -- 纯黑背景突出辉光

    -- 暗背景网格 (低亮度参考)
    Gfx.SetColor(0.04, 0.04, 0.06, 1.0)
    for gx = 0, WIN_W, 40 do
        Gfx.Rectangle(Gfx.FillMode, gx, 0, 0, 1, WIN_H, 0)
    end
    for gy = 0, WIN_H, 40 do
        Gfx.Rectangle(Gfx.FillMode, 0, gy, 0, WIN_W, 1, 0)
    end

    -- 亮点 (顶点色 > 1.0)
    for _, s in ipairs(SPOTS) do
        Gfx.SetColor(s.r, s.g, s.b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, s.x, s.y, 0, s.w, s.h, 0)
    end
    Gfx.SetColor(1, 1, 1, 1)

    -- OSD
    if win.DrawText then
        local y = 8
        local line = function(s)
            win:DrawText(8, y, s, 1, 1, 1, 1)
            y = y + 16
        end
        line(string.format('HDR: %s   Bloom: %s   Supported: HDR=%s Bloom=%s   Backend: %s',
            hdrEnabled and 'ON' or 'OFF',
            Bloom.IsEnabled() and 'ON' or 'OFF',
            tostring(HDR.IsSupported()),
            tostring(Bloom.IsSupported()),
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?')))
        line(string.format('Threshold: %.2f   Intensity: %.2f   Radius: %.2f   Levels: %d (auto=%s)',
            Bloom.GetThreshold(), Bloom.GetIntensity(),
            Bloom.GetRadius(), Bloom.GetLevels(),
            tostring(Bloom.GetAutoEnable())))
        line('Keys: B=Bloom  1/2=Threshold  3/4=Intensity  5/6=Radius  7/8=Levels  R=reset  ESC=quit')
    end

    win:EndFrame()
end

-- ==================== 6. 清理 ====================

if Bloom.IsEnabled() then
    Bloom.Disable()
end
if hdrEnabled then
    HDR.Disable()
end

if win.Close and win:IsOpen() then win:Close() end
print('demo_bloom ok')
