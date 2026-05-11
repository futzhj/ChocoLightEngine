-- Phase D.x.7 smoke: Light.Graphics.Image:New(w, h, rgba_bytes)
-- 验证从 RGBA 字节直接构造 Image, 不依赖外部 PNG 资源.

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end
local function eq(a, b, msg) if a ~= b then fail(msg .. " expected="..tostring(b).." got="..tostring(a)) end end

local Gfx = require 'Light.Graphics'
if not Gfx or not Gfx.Image then
    print("Light.Graphics.Image unavailable, skipping")
    print("== image_from_bytes ok (skipped) ==")
    return
end

-- ============================================================
-- T1: 16x16 纯红, 验证 GetWidth/GetHeight/GetTextureId 都返回有效值
-- ============================================================
do
    -- 一个像素 RGBA = 4 字节, 256 像素 × 4 = 1024 字节
    local pixel = string.char(255, 0, 0, 255)
    local bytes = string.rep(pixel, 16 * 16)
    eq(#bytes, 1024, "T1: bytes length should be w*h*4=1024")

    local img = Light(Gfx.Image):New(16, 16, bytes)
    if not img then fail("T1: Image:New(16,16,bytes) returned nil") end
    eq(img:GetWidth(),  16, "T1: GetWidth")
    eq(img:GetHeight(), 16, "T1: GetHeight")
    -- texId 可能 0 (g_render 不可用) 或 > 0 (正常 GL 上下文); 仅断言 type 为 number
    local tid = img:GetTextureId()
    if type(tid) ~= "number" then fail("T1: GetTextureId should return number, got "..type(tid)) end
    pass(string.format("T1: 16x16 red image ok (texId=%d)", tid))
end

-- ============================================================
-- T2: 不规则尺寸 7x3, 验证非整数对齐边界
-- ============================================================
do
    local pixel = string.char(0, 255, 0, 255)  -- 纯绿
    local bytes = string.rep(pixel, 7 * 3)
    local img = Light(Gfx.Image):New(7, 3, bytes)
    if not img then fail("T2: Image:New(7,3,bytes) returned nil") end
    eq(img:GetWidth(),  7, "T2: width=7")
    eq(img:GetHeight(), 3, "T2: height=3")
    pass("T2: irregular 7x3 image ok")
end

-- ============================================================
-- T3: 错误参数 — bytes 长度不匹配
-- ============================================================
do
    -- 期望 4*4*4=64 字节, 实际给 60 字节
    local short_bytes = string.rep(string.char(0,0,0,255), 15)
    eq(#short_bytes, 60, "T3: short_bytes preparation")
    -- 应该不抛错但 log error, img 仍创建但 texId 应为 0
    local img = Light(Gfx.Image):New(4, 4, short_bytes)
    -- ChocoLight 风格: 不抛错, 仅 log; img 还是创建 (可能 width=0)
    if img then
        local w = img:GetWidth()
        if w ~= 0 then
            print("[note] T3: invalid bytes still produced GetWidth="..w.." (engine 容忍策略)")
        end
    end
    pass("T3: invalid byte length handled gracefully (no crash)")
end

-- ============================================================
-- T4: 棋盘格构造 (用例: 调试纹理)
-- ============================================================
do
    local W, H = 8, 8
    local CELL = 2
    -- 构造棋盘格: (cellX + cellY) % 2 == 0 → 黑, 否则 → 白
    local pixels = {}
    for y = 0, H - 1 do
        for x = 0, W - 1 do
            local cx = math.floor(x / CELL)
            local cy = math.floor(y / CELL)
            if (cx + cy) % 2 == 0 then
                pixels[#pixels+1] = string.char(0, 0, 0, 255)
            else
                pixels[#pixels+1] = string.char(255, 255, 255, 255)
            end
        end
    end
    local bytes = table.concat(pixels)
    eq(#bytes, W * H * 4, "T4: checkerboard bytes length")
    local img = Light(Gfx.Image):New(W, H, bytes)
    eq(img:GetWidth(),  W, "T4: checkerboard width")
    eq(img:GetHeight(), H, "T4: checkerboard height")
    pass("T4: 8x8 checkerboard texture constructed ok")
end

-- ============================================================
-- T5: 兼容性 — 现有从文件加载分支不变 (调用 nil path 时不崩)
-- ============================================================
do
    -- 故意传不存在的文件, 应该 log error 但不崩
    local ok = pcall(function()
        local _ = Light(Gfx.Image):New("__nonexistent_file_for_test__.png")
    end)
    if not ok then fail("T5: file-loading branch should not throw on missing file") end
    pass("T5: missing file path handled gracefully")
end

print("")
print("== image_from_bytes smoke ok (5/5 PASS) ==")
