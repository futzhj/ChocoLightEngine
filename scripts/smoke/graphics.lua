-- scripts/smoke/graphics.lua
-- Light.Graphics 模块 smoke: 顶层 API 表 + Phase AW.x 后端内省
--
-- Phase AW.x: GetBackendName 用于运行时识别当前 backend
--   (sample / 调试工具 / 性能基准都需要)
--
-- 不依赖 Window 上下文 (headless 友好)
-- 兼容 Lua 5.1 (lightc -p 严格语法检查 + light.exe runtime)

-- 防止 GH Actions PowerShell stdout 缓冲截断关键日志
if io and io.stdout and io.stdout.setvbuf then
    pcall(function() io.stdout:setvbuf('no') end)
end

-- helper: 安全 require, lightc -p 时跳过实际加载
local function safe_require(name)
    local ok, mod = pcall(require, name)
    if ok and type(mod) == 'table' then return mod end
    return nil
end

-- 防 lightc 阶段未加载 DLL 导致 require 全部失败
local Gfx = safe_require('Light.Graphics')
if not Gfx then
    print('[Light.Graphics] runtime not available (likely lightc -p syntax check), skip with OK')
    return
end

-- ==================== 测试统计 ====================

local PASS = 0
local FAIL = 0
local function CHECK(cond, label)
    if cond then
        PASS = PASS + 1
        print('  PASS: ' .. label)
    else
        FAIL = FAIL + 1
        print('  FAIL: ' .. label)
    end
end

-- ==================== [1] 顶层 API 表 ====================

print('[1] Light.Graphics 顶层 API 表')

CHECK(type(Gfx) == 'table',         'Light.Graphics 是 table')
CHECK(type(Gfx.SetColor) == 'function',  'Gfx.SetColor is function')
CHECK(type(Gfx.GetColor) == 'function',  'Gfx.GetColor is function')
CHECK(type(Gfx.Push) == 'function',      'Gfx.Push is function')
CHECK(type(Gfx.Pop)  == 'function',      'Gfx.Pop is function')

-- ==================== [2] Phase AW.x: GetBackendName ====================

print('[2] Phase AW.x: Light.Graphics.GetBackendName')

CHECK(type(Gfx.GetBackendName) == 'function',
      'Gfx.GetBackendName 存在 (Phase AW.x)')

-- 调用必须 100% 安全 (不 raise / 不返回 nil)
local ok_call, name = pcall(Gfx.GetBackendName)
CHECK(ok_call,             'GetBackendName() 不 raise')
CHECK(type(name) == 'string', 'GetBackendName 返回 string')
CHECK(name and #name > 0,     'GetBackendName 返回非空字符串 (实际="' .. tostring(name) .. '")')

-- 已知 backend 名称白名单 (与 Phase AW.x DESIGN 一致)
local known = {
    GL33Core = true,    -- render_gl33.cpp
    LegacyGL = true,    -- render_legacy.cpp
    None     = true,    -- g_render = nullptr (理论值)
    Unknown  = true,    -- GetName 返回空 (防御值)
}
CHECK(known[name] == true,
      'GetBackendName 返回已知名称 (GL33Core/LegacyGL/None/Unknown), 实际="' .. tostring(name) .. '"')

-- 多次调用稳定性 (无副作用)
local name2 = Gfx.GetBackendName()
local name3 = Gfx.GetBackendName()
CHECK(name == name2 and name == name3,
      'GetBackendName 多次调用结果一致 (无副作用)')

-- ==================== [3] Phase F.0.10.2: SetViewport / GetViewport ====================

print('[3] Phase F.0.10.2: Light.Graphics.SetViewport / GetViewport (split-screen 基础)')

CHECK(type(Gfx.SetViewport) == 'function', 'Gfx.SetViewport 存在 (Phase F.0.10.2)')
CHECK(type(Gfx.GetViewport) == 'function', 'Gfx.GetViewport 存在 (Phase F.0.10.2)')

-- GetViewport 调用必须安全 (headless 下也能调, 返 4 个 integer)
local ok_get, vx, vy, vw, vh = pcall(Gfx.GetViewport)
CHECK(ok_get, 'GetViewport() 不 raise')
CHECK(type(vx) == 'number' and type(vy) == 'number'
   and type(vw) == 'number' and type(vh) == 'number',
      'GetViewport 返回 4 个 number (实际: ' .. tostring(vx) .. ',' .. tostring(vy)
      .. ',' .. tostring(vw) .. ',' .. tostring(vh) .. ')')

-- SetViewport round-trip: 调用后 GetViewport 应返同值
-- 注意: headless 无 GL context 时 SetViewport 仍能调 (backend 接收, 但 GL 调用可能 no-op)
--       GetViewport 在 GL context 不可用时返 [0,0,0,0]
local has_gl = (vw > 0 and vh > 0)
if has_gl then
    -- 选择一个 split-screen 典型值: 左半 (0, 0, 480, 540)
    local ok_set = pcall(Gfx.SetViewport, 0, 0, 480, 540)
    CHECK(ok_set, 'SetViewport(0, 0, 480, 540) 不 raise')
    local _, _, _, gx, gy, gw, gh = pcall(Gfx.GetViewport)
    -- 重新拉一次 GetViewport (上面 pcall 返回顺序: ok, x, y, w, h)
    local rx, ry, rw, rh = Gfx.GetViewport()
    CHECK(rx == 0 and ry == 0 and rw == 480 and rh == 540,
          'SetViewport round-trip OK (实际 GetViewport=' .. tostring(rx) .. ','
          .. tostring(ry) .. ',' .. tostring(rw) .. ',' .. tostring(rh) .. ')')

    -- 恢复初始 viewport (避免影响后续 smoke)
    Gfx.SetViewport(vx, vy, vw, vh)
else
    print('  [skip] GL context 不可用 (headless mode), SetViewport round-trip 跳过')
end

-- 防御性: w <= 0 或 h <= 0 必须 raise (Lua error)
local ok_w0 = pcall(Gfx.SetViewport, 0, 0, 0, 100)
CHECK(not ok_w0, 'SetViewport(_, _, 0, _) raises (w=0 拒绝)')
local ok_h0 = pcall(Gfx.SetViewport, 0, 0, 100, 0)
CHECK(not ok_h0, 'SetViewport(_, _, _, 0) raises (h=0 拒绝)')
local ok_wn = pcall(Gfx.SetViewport, 0, 0, -100, 100)
CHECK(not ok_wn, 'SetViewport(_, _, -100, _) raises (w<0 拒绝)')

-- 防御性: 类型错 (string 而非 number) 应 raise
local ok_type = pcall(Gfx.SetViewport, 'foo', 0, 100, 100)
CHECK(not ok_type, 'SetViewport("foo", ...) raises (类型错拒绝)')

-- ==================== [4] Phase F.0.11: Screenshot / RecordPNGSequence ====================

print('[4] Phase F.0.11: Screenshot / RecordPNGSequence / StopRecord / IsRecording')

-- API surface: 5 函数存在
CHECK(type(Gfx.Screenshot)           == 'function', 'Screenshot 存在')
CHECK(type(Gfx.ScreenshotRegion)     == 'function', 'ScreenshotRegion 存在')
CHECK(type(Gfx.RecordPNGSequence)    == 'function', 'RecordPNGSequence 存在')
CHECK(type(Gfx.StopRecord)           == 'function', 'StopRecord 存在')
CHECK(type(Gfx.IsRecording)          == 'function', 'IsRecording 存在')

-- Screenshot headless: 无 GL context → 应返 nil + err string (不 raise)
local ok_ss, r_ss, e_ss = pcall(Gfx.Screenshot, "headless_test.png")
CHECK(ok_ss, 'Screenshot() 不 raise (headless 下也不抛)')
-- headless 下 viewport w=0/h=0 → 返 nil + err (不是 true)
if r_ss == nil then
    CHECK(type(e_ss) == 'string', 'Screenshot headless 返 nil + err string (actual=' .. tostring(e_ss) .. ')')
else
    CHECK(type(r_ss) == 'boolean', 'Screenshot 返 boolean (有 GL 时)')
end

-- ScreenshotRegion: w<=0 → nil + err (参数校验)
local ok_r, rv, re = pcall(Gfx.ScreenshotRegion, "r.png", 0, 0, -1, 100)
CHECK(ok_r, 'ScreenshotRegion 不 raise')
CHECK(rv == nil and type(re) == 'string', 'ScreenshotRegion(w=-1) 返 nil + err')

-- IsRecording: 初始 active=false
local ok_ir, active0, cnt0 = pcall(Gfx.IsRecording)
CHECK(ok_ir, 'IsRecording 不 raise')
CHECK(active0 == false, 'IsRecording 初始 active=false')
CHECK(cnt0 == 0,        'IsRecording 初始 count=0')

-- RecordPNGSequence: max_frames<0 → nil + err
local ok_neg, rv2, re2 = pcall(Gfx.RecordPNGSequence, "frames/", -1)
CHECK(ok_neg, 'RecordPNGSequence 不 raise')
CHECK(rv2 == nil and type(re2) == 'string', 'RecordPNGSequence(max=-1) 返 nil + err')

-- RecordPNGSequence 启动
local ok_start, r_start = pcall(Gfx.RecordPNGSequence, "frames/", 3)
CHECK(ok_start, 'RecordPNGSequence 不 raise')
CHECK(r_start == true, 'RecordPNGSequence(3) 返 true')

-- IsRecording 激活中
local _, active1, _ = pcall(Gfx.IsRecording)
CHECK(active1 == true, 'IsRecording active=true 录屏中')

-- StopRecord 停止
local ok_stop, n_stop = pcall(Gfx.StopRecord)
CHECK(ok_stop, 'StopRecord 不 raise')
CHECK(type(n_stop) == 'number', 'StopRecord 返 number (写入帧数)')
-- headless 下没有 Draw 帧, frame_count 应为 0 (录屏 hook 未被 l_Window_Call 触发)
CHECK(n_stop == 0, 'StopRecord headless 下 frame_count=0 (无 Draw 帧)')

-- IsRecording 停止后 active=false
local _, active2, _ = pcall(Gfx.IsRecording)
CHECK(active2 == false, 'IsRecording active=false 停止后')

-- ==================== 汇总 ====================

print(string.format('[Light.Graphics smoke] 通过 %d / 失败 %d', PASS, FAIL))
if FAIL > 0 then
    error(string.format('graphics smoke 失败: %d 个断言不通过', FAIL))
end
