-- ============================================================================
-- ChocoLight Phase E.3 — HDR + ACES Tonemapping Demo (callback-model)
-- ============================================================================
-- 演示 HDR 离屏渲染 + ACES filmic tonemap 对高亮场景的压缩能力.
--
-- 画面布局:
--   上行:  10 个矩形, 亮度线性递增 (0.2, 0.6, 1.0, 1.4, 1.8, 2.2, 2.6, 3.0, 3.4, 3.8)
--   下行:  3 条彩色梯度 (红/绿/蓝)
--   底部:  OSD 显示 HDR 状态 / Exposure / Gamma / 操作提示
--
-- 预期效果:
--   HDR OFF -> 顶点颜色 > 1.0 被硬件 clamp 为白色, 亮度 > 1.0 的 6 个矩形视觉上无差异
--   HDR ON  -> ACES 曲线将 0~+∞ 压缩到 0~1, 亮度 > 1.0 的矩形仍可区分
--
-- 控制:
--   H : 切换 HDR 启用 / 禁用
--   Z / X : 减小 / 增大 Exposure (步长 0.1, 范围 [0.1, 5.0])
--   C / V : 减小 / 增大 Gamma   (步长 0.1, 范围 [1.0, 3.0])
--   T : 循环切换 Tonemap (aces → reinhard → uncharted2 → linear)
--   B : 切换 Bloom (需 HDR ON)
--   R : 重置全部参数
--   ESC : 退出
-- ============================================================================

local function safe_require(n)
    local ok, m = pcall(require, n); if ok and type(m) == 'table' then return m end; return nil
end
local UI  = safe_require('Light.UI')
local Gfx = safe_require('Light.Graphics')

if not Gfx then print('[demo_hdr] Light.Graphics 不可用'); print('demo_hdr ok (no graphics)'); return end
local HDR   = Gfx.HDR
local Bloom = Gfx.Bloom
if type(HDR) ~= 'table' then print('[demo_hdr] HDR 子表缺失'); print('demo_hdr ok (no HDR subtable)'); return end

print('==== ChocoLight Phase E.3 HDR demo (callback-model) ====')
print('[demo_hdr] Backend         = ' .. tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?'))
print('[demo_hdr] HDR.IsSupported = ' .. tostring(HDR.IsSupported()))
if Bloom then print('[demo_hdr] Bloom.IsSupported = ' .. tostring(Bloom.IsSupported())) end

-- Headless 探测 (CI 兼容)
if not UI or not UI.Window then
    print('[demo_hdr] UI.Window 不可用, 仅执行 API 探测')
    print('  HDR.IsEnabled       = ' .. tostring(HDR.IsEnabled()))
    print('  HDR.GetExposure     = ' .. tostring(HDR.GetExposure()))
    print('  HDR.GetGamma        = ' .. tostring(HDR.GetGamma()))
    print('  HDR.GetSceneTexture = ' .. tostring(HDR.GetSceneTexture()))
    print('demo_hdr ok (headless API check)')
    return
end
if type(Light) ~= 'function' and type(Light) ~= 'table' then
    print('[demo_hdr] Light global 不可用')
    print('demo_hdr ok (no Light global)')
    return
end

-- ============================================================================
-- 常量 + 工具
-- ============================================================================
local WIN_W, WIN_H = 960, 540
local TONEMAPS  = { 'aces', 'reinhard', 'uncharted2', 'linear' }
local BRIGHTS   = { 0.2, 0.6, 1.0, 1.4, 1.8, 2.2, 2.6, 3.0, 3.4, 3.8 }
local BAR_W, BAR_H, BAR_GAP = 80, 180, 8

local function clamp(v, lo, hi) return v < lo and lo or (v > hi and hi or v) end

-- ============================================================================
-- Demo 类
-- ============================================================================
local Demo = Light(Light.UI.Window):New()

local g_hdrEnabled = false
local g_exposure   = 1.0
local g_gamma      = 2.2
local g_tmIndex    = 1   -- 1-based, TONEMAPS 索引

local function tryEnableHDR()
    if not HDR.IsSupported() then
        print('[demo_hdr] 当前后端不支持 HDR, 留 LDR 模式'); return false
    end
    local ok = HDR.Enable(WIN_W, WIN_H)
    if ok then print('[demo_hdr] HDR.Enable(' .. WIN_W .. 'x' .. WIN_H .. ') ok'); return true end
    print('[demo_hdr] HDR.Enable 失败'); return false
end

function Demo:OnOpen()
    g_hdrEnabled = tryEnableHDR()
end

function Demo:Update(dt)
    -- 静态画面, 无时间动画
end

function Demo:Draw()
    -- 上排: 灰度梯度
    local startX = 40
    local y0     = 80
    for i, b in ipairs(BRIGHTS) do
        local x = startX + (i - 1) * (BAR_W + BAR_GAP)
        Gfx.SetColor(b, b, b, 1.0)
        Gfx.Rectangle(Gfx.FillMode, x, y0, 0, BAR_W, BAR_H, 0)
        Gfx.SetColor(1, 1, 1, 1)
        if Gfx.Print then
            Gfx.Print(string.format('%.1f', b), x + 4, y0 + BAR_H + 4, 0)
        end
    end

    -- 下排: 彩色梯度 (红/绿/蓝)
    local colOffY = 80 + BAR_H + 40
    local palette = { {1.0, 0.3, 0.2}, {0.2, 1.0, 0.3}, {0.2, 0.4, 1.0} }
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
    if Gfx.Print then
        local y = 8; local function line(s) Gfx.Print(s, 8, y, 0); y = y + 16 end
        line(string.format('HDR: %s | Supported: %s | Backend: %s',
            g_hdrEnabled and 'ON' or 'OFF',
            tostring(HDR.IsSupported()),
            tostring(Gfx.GetBackendName and Gfx.GetBackendName() or '?')))
        line(string.format('Exposure: %.2f   Gamma: %.2f   Tonemap: %-10s  SceneTex: %d',
            HDR.GetExposure(), HDR.GetGamma(), HDR.GetTonemapper(), HDR.GetSceneTexture()))
        if Bloom then
            line(string.format('Bloom: %s | Supported: %s | Thr: %.2f Int: %.2f Rad: %.2f Lv: %d',
                Bloom.IsEnabled() and 'ON' or 'OFF',
                tostring(Bloom.IsSupported()),
                Bloom.GetThreshold(), Bloom.GetIntensity(),
                Bloom.GetRadius(), Bloom.GetLevels()))
        end
        line('Brightness scale: 0.2 ... 3.8 (values > 1.0 need HDR to resolve)')
        line('Keys: H=HDR  Z/X=exp  C/V=gamma  T=tonemap  B=Bloom  R=reset  ESC=quit')
    end
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end   -- 仅按下

    if key == 256 then                 -- ESC
        self:Close()
    elseif key == string.byte('H') then  -- 切 HDR
        if g_hdrEnabled then HDR.Disable(); g_hdrEnabled = false; print('[demo_hdr] HDR OFF')
        else g_hdrEnabled = tryEnableHDR(); print('[demo_hdr] HDR ' .. (g_hdrEnabled and 'ON' or 'OFF')) end
    elseif key == string.byte('Z') then  -- exposure -
        g_exposure = clamp(g_exposure - 0.1, 0.1, 5.0); HDR.SetExposure(g_exposure)
    elseif key == string.byte('X') then  -- exposure +
        g_exposure = clamp(g_exposure + 0.1, 0.1, 5.0); HDR.SetExposure(g_exposure)
    elseif key == string.byte('C') then  -- gamma -
        g_gamma = clamp(g_gamma - 0.1, 1.0, 3.0); HDR.SetGamma(g_gamma)
    elseif key == string.byte('V') then  -- gamma +
        g_gamma = clamp(g_gamma + 0.1, 1.0, 3.0); HDR.SetGamma(g_gamma)
    elseif key == string.byte('T') then  -- tonemap 循环
        g_tmIndex = (g_tmIndex % #TONEMAPS) + 1
        HDR.SetTonemapper(TONEMAPS[g_tmIndex])
        print('[demo_hdr] Tonemapper -> ' .. TONEMAPS[g_tmIndex])
    elseif key == string.byte('B') then  -- Bloom 切换 (需 HDR)
        if Bloom then
            if Bloom.IsEnabled() then Bloom.Disable(); print('[demo_hdr] Bloom OFF')
            elseif g_hdrEnabled then
                local ok = Bloom.Enable(WIN_W, WIN_H)
                print('[demo_hdr] Bloom ' .. (ok and 'ON' or 'OFF (enable failed)'))
            else print('[demo_hdr] 需先启 HDR 才能开 Bloom (按 H)') end
        end
    elseif key == string.byte('R') then  -- reset
        g_exposure = 1.0; g_gamma = 2.2; g_tmIndex = 1
        HDR.SetExposure(g_exposure); HDR.SetGamma(g_gamma)
        HDR.SetTonemapper(TONEMAPS[g_tmIndex])
        if Bloom and g_hdrEnabled and not Bloom.IsEnabled() then
            Bloom.Enable(WIN_W, WIN_H)
        end
    end
end

local function cleanup_demo()
    if Bloom and Bloom.IsEnabled() then Bloom.Disable(); print('[demo_hdr] Bloom.Disable (cleanup)') end
    if g_hdrEnabled then HDR.Disable(); print('[demo_hdr] HDR.Disable (cleanup)') end
end

Demo:Open(WIN_W, WIN_H, 'Phase E.3 - HDR + ACES Tonemap Demo (callback)')
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_hdr ok')
