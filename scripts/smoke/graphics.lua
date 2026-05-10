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

-- ==================== 汇总 ====================

print(string.format('[Light.Graphics smoke] 通过 %d / 失败 %d', PASS, FAIL))
if FAIL > 0 then
    error(string.format('graphics smoke 失败: %d 个断言不通过', FAIL))
end
