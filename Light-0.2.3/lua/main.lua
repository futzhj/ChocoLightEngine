-- =============================================================
-- Light Engine 文字渲染综合演示
-- 文件: Light-0.2.3/lua/main.lua
-- 用法: 双击 light.exe  或  light.exe lua/main.lua
--
-- 演示能力:
--   1. 多字号 (14 / 22 / 36 / 48)
--   2. 多颜色 / 透明度
--   3. 中英混排 (UTF-8 / CJK)
--   4. 旋转 / 缩放 / 矩阵栈
--   5. 动画文字: 打字机 / 彩虹色 / 滚动 / 跳动 / 渐隐渐显
--   6. 字体进阶 (Lua 模拟): 描边 / 阴影 / 发光 / 抗锯齿对比 / 5x7 点阵字体
--   7. 实时 FPS 显示
--   8. 键盘交互: SPACE / 1-5 切换页面
--
-- 备注: 引擎本身不提供 outline / 内置字体 / bitmap font 模块,
--       Page 5 全部用 Lua 多次叠绘 + 手绘字模实现等价视觉效果.
-- =============================================================

local W, H = 1000, 600  -- 窗口尺寸
local PAGES = {
    "基础展示 Basics",
    "颜色与字号 Colors",
    "变换与旋转 Transform",
    "动画效果 Animation",
    "字体进阶 Advanced",
}

-- 创建窗口对象 (使用 Light 框架的 OOP 继承机制)
local TextDemo = Light(Light.UI.Window):New()

-- ----------------------------------------------------------------
-- 工具: UTF-8 安全截断 (避免在多字节字符中间断开导致打字机效果乱码)
-- ----------------------------------------------------------------
local function utf8_chars(s)
    local chars, i, n = {}, 1, #s
    while i <= n do
        local b = string.byte(s, i)
        local len = 1
        if b >= 0xF0 then len = 4
        elseif b >= 0xE0 then len = 3
        elseif b >= 0xC0 then len = 2 end
        chars[#chars + 1] = string.sub(s, i, i + len - 1)
        i = i + len
    end
    return chars
end

local function utf8_sub(s, count)
    local chars = utf8_chars(s)
    local total = math.min(count, #chars)
    return table.concat(chars, "", 1, total), #chars
end

-- ----------------------------------------------------------------
-- 工具: HSV → RGB (彩虹色生成)
-- ----------------------------------------------------------------
local function hsv2rgb(h, s, v)
    local i = math.floor(h * 6)
    local f = h * 6 - i
    local p = v * (1 - s)
    local q = v * (1 - f * s)
    local t = v * (1 - (1 - f) * s)
    i = i % 6
    if     i == 0 then return v, t, p
    elseif i == 1 then return q, v, p
    elseif i == 2 then return p, v, t
    elseif i == 3 then return p, q, v
    elseif i == 4 then return t, p, v
    else                return v, p, q end
end

-- ================================================================
-- 字体进阶工具: 引擎原生无 outline / 内置字体 / bitmap font 等 API,
-- 以下函数全部基于 Print + Rectangle + SetColor 在 Lua 层模拟.
-- ================================================================

-- 描边文字: 第一遍 8 方向偏移叠绘描边色, 第二遍中心绘制填充色
-- thick: 描边粗细 (像素)
local function DrawOutlinedText(text, font, x, y, fillR, fillG, fillB, outR, outG, outB, thick)
    thick = thick or 2
    Light.Graphics.SetColor(outR, outG, outB, 1)
    for dx = -thick, thick do
        for dy = -thick, thick do
            if dx ~= 0 or dy ~= 0 then
                Light.Graphics.Print(text, font, x + dx, y + dy, 0)
            end
        end
    end
    Light.Graphics.SetColor(fillR, fillG, fillB, 1)
    Light.Graphics.Print(text, font, x, y, 0)
end

-- 阴影文字: 偏移半透明黑色 + 原位填充色
local function DrawShadowText(text, font, x, y, fillR, fillG, fillB, offX, offY)
    offX = offX or 3
    offY = offY or 3
    Light.Graphics.SetColor(0, 0, 0, 0.55)
    Light.Graphics.Print(text, font, x + offX, y + offY, 0)
    Light.Graphics.SetColor(fillR, fillG, fillB, 1)
    Light.Graphics.Print(text, font, x, y, 0)
end

-- 发光文字: 4 层渐弱光晕环 (距离越远越透明) + 中心填充色
local function DrawGlowText(text, font, x, y, fillR, fillG, fillB, gR, gG, gB)
    for r = 4, 1, -1 do
        local alpha = 0.18 * (5 - r) / 4
        Light.Graphics.SetColor(gR, gG, gB, alpha)
        for k = 0, 7 do
            local rad = k * math.pi / 4
            Light.Graphics.Print(text, font,
                x + math.cos(rad) * r,
                y + math.sin(rad) * r, 0)
        end
    end
    Light.Graphics.SetColor(fillR, fillG, fillB, 1)
    Light.Graphics.Print(text, font, x, y, 0)
end

-- 5x7 像素字模 (Lua 手绘点阵, '1'=亮像素 '0'=暗像素 '|'=行分隔)
-- 涵盖 A-Z + 0-9 + 空格, 共 37 个字符
local PIXEL_FONT_5X7 = {
    A = "01110|10001|10001|11111|10001|10001|10001",
    B = "11110|10001|10001|11110|10001|10001|11110",
    C = "01111|10000|10000|10000|10000|10000|01111",
    D = "11110|10001|10001|10001|10001|10001|11110",
    E = "11111|10000|10000|11110|10000|10000|11111",
    F = "11111|10000|10000|11110|10000|10000|10000",
    G = "01111|10000|10000|10111|10001|10001|01111",
    H = "10001|10001|10001|11111|10001|10001|10001",
    I = "11111|00100|00100|00100|00100|00100|11111",
    J = "11111|00001|00001|00001|00001|10001|01110",
    K = "10001|10010|10100|11000|10100|10010|10001",
    L = "10000|10000|10000|10000|10000|10000|11111",
    M = "10001|11011|10101|10101|10001|10001|10001",
    N = "10001|11001|10101|10101|10011|10001|10001",
    O = "01110|10001|10001|10001|10001|10001|01110",
    P = "11110|10001|10001|11110|10000|10000|10000",
    Q = "01110|10001|10001|10001|10101|10011|01111",
    R = "11110|10001|10001|11110|10100|10010|10001",
    S = "01111|10000|10000|01110|00001|00001|11110",
    T = "11111|00100|00100|00100|00100|00100|00100",
    U = "10001|10001|10001|10001|10001|10001|01110",
    V = "10001|10001|10001|10001|10001|01010|00100",
    W = "10001|10001|10001|10101|10101|11011|10001",
    X = "10001|10001|01010|00100|01010|10001|10001",
    Y = "10001|10001|01010|00100|00100|00100|00100",
    Z = "11111|00001|00010|00100|01000|10000|11111",
    ["0"] = "01110|10001|10011|10101|11001|10001|01110",
    ["1"] = "00100|01100|00100|00100|00100|00100|01110",
    ["2"] = "01110|10001|00001|00010|00100|01000|11111",
    ["3"] = "01110|10001|00001|00110|00001|10001|01110",
    ["4"] = "00010|00110|01010|10010|11111|00010|00010",
    ["5"] = "11111|10000|11110|00001|00001|10001|01110",
    ["6"] = "01110|10000|10000|11110|10001|10001|01110",
    ["7"] = "11111|00001|00010|00100|01000|01000|01000",
    ["8"] = "01110|10001|10001|01110|10001|10001|01110",
    ["9"] = "01110|10001|10001|01111|00001|00001|01110",
    [" "] = "00000|00000|00000|00000|00000|00000|00000",
}

-- 绘制单个像素字符 (用 Rectangle 拼像素方块), 返回字符占用宽度
local function DrawPixelChar(ch, x, y, scale)
    local glyph = PIXEL_FONT_5X7[ch] or PIXEL_FONT_5X7[" "]
    local row = 0
    for line in string.gmatch(glyph, "[^|]+") do
        for col = 1, 5 do
            if string.sub(line, col, col) == "1" then
                Light.Graphics.Rectangle(Light.Graphics.FillMode,
                    x + (col - 1) * scale, y + row * scale, 0,
                    scale, scale, 0)
            end
        end
        row = row + 1
    end
    return 6 * scale  -- 5 列 + 1 像素列间距
end

-- 绘制点阵字符串 (自动转大写, 仅支持 A-Z 0-9 空格)
local function DrawPixelText(text, x, y, scale)
    text = string.upper(text)
    local cx = x
    for i = 1, #text do
        cx = cx + DrawPixelChar(string.sub(text, i, i), cx, y, scale)
    end
end

-- ----------------------------------------------------------------
-- 生命周期: OnOpen 初始化资源
-- ----------------------------------------------------------------
function TextDemo:OnOpen()
    -- 加载多字号字体 (每个 Font 实例对应一个固定字号)
    self.fontSmall  = Light(Light.Graphics.Font):New("assets/fangzheng.ttf", 14)
    self.fontMedium = Light(Light.Graphics.Font):New("assets/fangzheng.ttf", 22)
    self.fontLarge  = Light(Light.Graphics.Font):New("assets/fangzheng.ttf", 36)
    self.fontTitle  = Light(Light.Graphics.Font):New("assets/fangzheng.ttf", 48)

    -- 状态
    self.time = 0
    self.page = 1
    self.fps = 0
    self.fpsAcc = 0
    self.fpsCount = 0
end

-- ----------------------------------------------------------------
-- 生命周期: Update 每帧更新 (累计时间 + FPS 统计)
-- ----------------------------------------------------------------
function TextDemo:Update(dt)
    self.time = self.time + dt
    self.fpsCount = self.fpsCount + 1
    self.fpsAcc = self.fpsAcc + dt
    if self.fpsAcc >= 1.0 then
        self.fps = self.fpsCount
        self.fpsCount = 0
        self.fpsAcc = 0
    end
end

-- ----------------------------------------------------------------
-- 渲染: 顶部信息栏 (蓝色背景 + 标题 + 状态行)
-- ----------------------------------------------------------------
function TextDemo:DrawHeader()
    Light.Graphics.SetColor(0.20, 0.55, 0.95, 1)
    Light.Graphics.Rectangle(Light.Graphics.FillMode, 0, 0, 0, W, 50, 0)

    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("Light 引擎 · 文字渲染演示", self.fontMedium, 16, 8, 0)

    local info = string.format(
        "FPS: %3d   页面 [%d/%d]: %s   按 SPACE 或 1-4 切换页面",
        self.fps, self.page, #PAGES, PAGES[self.page])
    Light.Graphics.SetColor(0.85, 0.85, 0.85, 1)
    Light.Graphics.Print(info, self.fontSmall, 16, 60, 0)
end

-- ----------------------------------------------------------------
-- Page 1: 基础展示 (多字号 + 中英混排 + 多行)
-- ----------------------------------------------------------------
function TextDemo:DrawPage1()
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("Title 标题字号 48", self.fontTitle, 40, 90, 0)
    Light.Graphics.Print("Large 大字号 36", self.fontLarge, 40, 160, 0)
    Light.Graphics.Print("Medium 中字号 22", self.fontMedium, 40, 215, 0)
    Light.Graphics.Print("Small 小字号 14", self.fontSmall, 40, 255, 0)

    Light.Graphics.SetColor(0.7, 0.85, 1.0, 1)
    Light.Graphics.Print("UTF-8 / CJK 测试: 中文 日本語 한글 €¥£©®™",
        self.fontMedium, 40, 300, 0)
    Light.Graphics.Print("符号: ! @ # $ % ^ & * ( ) _ + - = [ ] { } | ; : , . / ?",
        self.fontMedium, 40, 335, 0)

    -- 多行古诗 (验证多行排版)
    local poem = {
        "床前明月光，疑是地上霜。",
        "举头望明月，低头思故乡。",
        "—— 李白《静夜思》",
    }
    Light.Graphics.SetColor(1.0, 0.9, 0.55, 1)
    for i, line in ipairs(poem) do
        Light.Graphics.Print(line, self.fontMedium, 40, 400 + (i - 1) * 35, 0)
    end
end

-- ----------------------------------------------------------------
-- Page 2: 颜色与字号 (七彩色块 + 透明度梯度)
-- ----------------------------------------------------------------
function TextDemo:DrawPage2()
    local colors = {
        { 1.00, 0.20, 0.20, "红色 RED" },
        { 1.00, 0.55, 0.10, "橙色 ORANGE" },
        { 1.00, 0.95, 0.20, "黄色 YELLOW" },
        { 0.20, 0.95, 0.30, "绿色 GREEN" },
        { 0.20, 0.95, 0.95, "青色 CYAN" },
        { 0.40, 0.55, 1.00, "蓝色 BLUE" },
        { 0.95, 0.30, 0.95, "紫色 PURPLE" },
    }
    for i, c in ipairs(colors) do
        Light.Graphics.SetColor(c[1], c[2], c[3], 1)
        Light.Graphics.Print(c[4], self.fontLarge, 40, 90 + (i - 1) * 55, 0)
    end

    -- 透明度梯度 (右侧栏)
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("透明度 Alpha", self.fontMedium, 480, 90, 0)
    for i = 1, 10 do
        local alpha = i / 10
        Light.Graphics.SetColor(1, 1, 1, alpha)
        Light.Graphics.Print(string.format("alpha = %.1f  Alpha Test", alpha),
            self.fontMedium, 480, 120 + (i - 1) * 32, 0)
    end
end

-- ----------------------------------------------------------------
-- Page 3: 变换与旋转 (Print 内置变换参数 + 矩阵栈)
-- ----------------------------------------------------------------
function TextDemo:DrawPage3()
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("Static 静态文字", self.fontLarge, 40, 100, 0)

    -- 用 Print 自带变换参数: (text, font, x,y,z, rx,ry,rz, sx,sy,sz, ox,oy,oz)
    -- 绕 Z 轴旋转, 角度随时间变化
    local angle = self.time * 60
    Light.Graphics.SetColor(0.4, 1.0, 0.85, 1)
    Light.Graphics.Print("Rotate 旋转", self.fontLarge,
        250, 280, 0,
        0, 0, angle,    -- 绕 Z 轴旋转
        1, 1, 1,
        0, 0, 0)

    -- 缩放呼吸 (sin 振荡)
    local s = 1 + math.sin(self.time * 2) * 0.35
    Light.Graphics.SetColor(1.0, 0.65, 0.45, 1)
    Light.Graphics.Print("Scale 缩放", self.fontLarge,
        600, 280, 0,
        0, 0, 0,
        s, s, 1,
        0, 0, 0)

    -- 矩阵栈: Push → Translate → Rotate → 绘制 → Pop (避免污染全局变换)
    Light.Graphics.Push()
    Light.Graphics.Translate(500, 470, 0)
    Light.Graphics.Rotate(angle * 0.5, 0, 0, 1)
    Light.Graphics.SetColor(1, 1, 0.4, 1)
    Light.Graphics.Print("Push / Translate / Rotate / Pop",
        self.fontMedium, -180, -10, 0)
    Light.Graphics.Pop()

    -- 翻转效果 (X 轴方向缩放为负)
    Light.Graphics.SetColor(0.85, 0.85, 1.0, 1)
    Light.Graphics.Print("Flip 翻转 ABCabc", self.fontLarge,
        80, 480, 0,
        0, 0, 0,
        -1, 1, 1,    -- X 轴翻转
        0, 0, 0)
end

-- ----------------------------------------------------------------
-- Page 4: 动画效果 (打字机 / 彩虹 / 滚动 / 跳动 / 渐隐)
-- ----------------------------------------------------------------
function TextDemo:DrawPage4()
    -- 1. 打字机效果 (按 UTF-8 字符逐个显示, 循环)
    local fullText = "打字机 Typewriter Effect 这是逐字显示的演示文本..."
    local _, totalChars = utf8_sub(fullText, 0)
    local charCount = math.floor(self.time * 8) % (totalChars + 12)
    local typed = utf8_sub(fullText, charCount)
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print(typed .. "_", self.fontMedium, 40, 100, 0)

    -- 2. 彩虹色 (按 UTF-8 字符逐个上色)
    local rainbowText = "Rainbow 彩虹文字效果"
    local rainbowChars = utf8_chars(rainbowText)
    local cx = 40
    for i, ch in ipairs(rainbowChars) do
        local hue = (self.time * 0.3 + i * 0.06) % 1
        local r, g, b = hsv2rgb(hue, 1, 1)
        Light.Graphics.SetColor(r, g, b, 1)
        Light.Graphics.Print(ch, self.fontLarge, cx, 170, 0)
        -- 估算字符宽度: ASCII ~0.55*fontSize, CJK ~1.0*fontSize
        local byte1 = string.byte(ch, 1)
        local advance = (byte1 < 128) and 20 or 38
        cx = cx + advance
    end

    -- 3. 横向滚动文字 (持续从右往左)
    local scrollText = "向左滚动 Scrolling Text · 持续不断的字幕效果 · "
        .. "Light Engine · ChocoLight · "
    local scrollX = W - (self.time * 100) % (W + 800)
    Light.Graphics.SetColor(0.5, 1, 0.6, 1)
    Light.Graphics.Print(scrollText, self.fontMedium, scrollX, 270, 0)

    -- 4. 跳动 (绝对值 sin 模拟弹跳)
    local bounceY = 360 - math.abs(math.sin(self.time * 5)) * 35
    Light.Graphics.SetColor(1, 0.8, 0.2, 1)
    Light.Graphics.Print("Bounce 跳动!", self.fontLarge, 80, bounceY, 0)

    -- 5. 渐隐渐显 (sin 映射到 0..1)
    local alpha = (math.sin(self.time * 1.5) + 1) * 0.5
    Light.Graphics.SetColor(1, 0.35, 0.75, alpha)
    Light.Graphics.Print("Fade 淡入淡出", self.fontLarge, 380, 360, 0)

    -- 6. 旋转 + 缩放组合
    local angle = self.time * 90
    local scale = 1 + math.sin(self.time * 3) * 0.25
    Light.Graphics.SetColor(0.5, 0.85, 1, 1)
    Light.Graphics.Print("Combo 组合效果", self.fontLarge,
        700, 460, 0,
        0, 0, angle,
        scale, scale, 1,
        0, 0, 0)
end

-- ----------------------------------------------------------------
-- Page 5: 字体进阶 (Lua 模拟引擎缺失能力)
--   ① 描边 Outlined  ② 阴影 Shadow  ③ 发光 Glow
--   ④ 抗锯齿对比 AA  ⑤ 5x7 点阵字体 Bitmap
-- ----------------------------------------------------------------
function TextDemo:DrawPage5()
    -- 总标题
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("字体进阶 · Lua 模拟引擎未提供的 4 类视觉效果",
        self.fontMedium, 40, 85, 0)

    -- ① 描边文字
    Light.Graphics.SetColor(0.65, 0.85, 1.0, 1)
    Light.Graphics.Print("① 描边 Outlined  (8 方向偏移叠绘)",
        self.fontSmall, 40, 130, 0)
    DrawOutlinedText("LIGHT 描边", self.fontLarge, 220, 125,
        1.0, 1.0, 0.4,    -- 黄色填充
        0,   0,   0,      -- 黑色描边
        2)
    DrawOutlinedText("DOUBLE", self.fontLarge, 560, 125,
        0.4, 1.0, 1.0,    -- 青色填充
        0.5, 0,   0.5,    -- 紫色粗描边
        3)

    -- ② 阴影文字
    Light.Graphics.SetColor(0.65, 0.85, 1.0, 1)
    Light.Graphics.Print("② 阴影 Shadow  (偏移半透明黑色)",
        self.fontSmall, 40, 195, 0)
    DrawShadowText("SHADOW 阴影", self.fontLarge, 220, 190,
        1, 1, 1, 4, 4)
    DrawShadowText("FAR", self.fontLarge, 580, 190,
        1.0, 0.7, 0.3, 8, 8)

    -- ③ 发光文字
    Light.Graphics.SetColor(0.65, 0.85, 1.0, 1)
    Light.Graphics.Print("③ 发光 Glow  (4 层渐弱光晕环)",
        self.fontSmall, 40, 260, 0)
    DrawGlowText("GLOW 发光", self.fontLarge, 220, 255,
        1, 1, 1,             -- 白色文字
        0.3, 0.8, 1.0)       -- 青色光晕
    DrawGlowText("HOT", self.fontLarge, 580, 255,
        1, 1, 0.3,           -- 黄色文字
        1.0, 0.3, 0.1)       -- 红色光晕

    -- ④ 抗锯齿对比 (引擎默认 stb_truetype 灰度抗锯齿 + GL_LINEAR 双线性)
    Light.Graphics.SetColor(0.65, 0.85, 1.0, 1)
    Light.Graphics.Print("④ 抗锯齿 AA  原生36px(锐利) | 8px×4.5放大(像素感) | 36px×1.6放大(平滑)",
        self.fontSmall, 40, 320, 0)
    Light.Graphics.SetColor(1, 1, 1, 1)
    Light.Graphics.Print("Sharp", self.fontLarge, 40, 345, 0)
    Light.Graphics.Print("Pixel", self.fontSmall, 270, 345, 0,
        0, 0, 0,  4.5, 4.5, 1,  0, 0, 0)
    Light.Graphics.Print("Smooth", self.fontLarge, 600, 345, 0,
        0, 0, 0,  1.6, 1.6, 1,  0, 0, 0)

    -- ⑤ 点阵字体 (Lua 手绘 5x7 字模)
    Light.Graphics.SetColor(0.65, 0.85, 1.0, 1)
    Light.Graphics.Print("⑤ 点阵字体 5x7  (Lua 手绘 37 字模 · Rectangle 拼像素)",
        self.fontSmall, 40, 440, 0)
    Light.Graphics.SetColor(0.4, 1.0, 0.6, 1)
    DrawPixelText("PIXEL FONT 2026", 40, 470, 5)
    Light.Graphics.SetColor(1.0, 0.7, 0.3, 1)
    DrawPixelText("LIGHT 8BIT", 40, 530, 4)
end

-- ----------------------------------------------------------------
-- 生命周期: Draw 每帧绘制
-- ----------------------------------------------------------------
function TextDemo:Draw()
    -- 深色背景 (覆盖整个窗口)
    Light.Graphics.SetColor(0.45, 0.45, 0.45, 1)
    Light.Graphics.Rectangle(Light.Graphics.FillMode, 0, 0, 0, W, H, 0)

    self:DrawHeader()

    if     self.page == 1 then self:DrawPage1()
    elseif self.page == 2 then self:DrawPage2()
    elseif self.page == 3 then self:DrawPage3()
    elseif self.page == 4 then self:DrawPage4()
    elseif self.page == 5 then self:DrawPage5() end

    -- 重置颜色, 避免影响下一帧
    Light.Graphics.SetColor(1, 1, 1, 1)
end

-- ----------------------------------------------------------------
-- 输入: 键盘事件 (action == 1 表示按下)
-- 键码使用 GLFW 兼容映射: SPACE=32, 1-5=49-53
-- ----------------------------------------------------------------
function TextDemo:OnKey(key, scanCode, action, mods)
    if action ~= 1 then return end
    if key == 32 then                                    -- SPACE: 下一页
        self.page = self.page % #PAGES + 1
    elseif key >= 49 and key <= 48 + #PAGES then         -- 1-4: 直接跳页
        self.page = key - 48
    end
end

-- ----------------------------------------------------------------
-- 入口: 打开窗口 + 进入主循环
-- ----------------------------------------------------------------
TextDemo:Open(W, H, "Light · 文字展示 Text Demo")
TextDemo:SetVSync(true)

while Light.UI.Loop() do
    Light.UI.Resume()
end
