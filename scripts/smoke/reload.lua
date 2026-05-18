-- ================================================================
-- Phase G.0 — Light.Reload (Lua 脚本热重载) smoke
--
-- 覆盖:
--   - Module / File / Preserve / ResetState / Clear
--   - WatchModule / UnwatchModule (找不到模块时优雅失败)
--   - SetErrorHandler / GetLastError / Stats
--   - RestartScript / IsRestartPending
-- ================================================================

local pass, fail = 0, 0
local function p(cond, msg)
    if cond then pass = pass + 1; print('PASS '..msg)
    else fail = fail + 1; print('FAIL '..msg) end
end

local Reload = Light.Reload

-- ============================================================
-- 1) 模块存在性 + API 完整性
-- ============================================================
p(type(Reload) == 'table',                  'Light.Reload module exists')
p(type(Reload.Module) == 'function',        'Reload.Module is function')
p(type(Reload.File) == 'function',          'Reload.File is function')
p(type(Reload.Preserve) == 'function',      'Reload.Preserve is function')
p(type(Reload.ResetState) == 'function',    'Reload.ResetState is function')
p(type(Reload.WatchModule) == 'function',   'Reload.WatchModule is function')
p(type(Reload.UnwatchModule) == 'function', 'Reload.UnwatchModule is function')
p(type(Reload.SetErrorHandler) == 'function','Reload.SetErrorHandler is function')
p(type(Reload.GetLastError) == 'function',  'Reload.GetLastError is function')
p(type(Reload.Stats) == 'function',         'Reload.Stats is function')
p(type(Reload.Clear) == 'function',         'Reload.Clear is function')
p(type(Reload.RestartScript) == 'function', 'Reload.RestartScript is function')
p(type(Reload.IsRestartPending) == 'function','Reload.IsRestartPending is function')

-- 干净起点
Reload.Clear()

-- ============================================================
-- 2) Reload.File — 失败路径
-- ============================================================
local r, err = Reload.File('this_file_does_not_exist_xyz.lua')
p(r == nil and type(err) == 'string',       'File(nonexistent) → nil + err string')

-- ============================================================
-- 3) Reload.Preserve — 第一次调 factory, 之后返同一 state
-- ============================================================
local s1 = Reload.Preserve('test_state_alpha', function() return {count = 42, msg = 'init'} end)
p(type(s1) == 'table' and s1.count == 42,   'Preserve first call → factory invoked')

s1.count = 100   -- 修改后保留
local s2 = Reload.Preserve('test_state_alpha', function() return {count = 999} end)
p(s2 == s1 and s2.count == 100,             'Preserve second call returns same table (factory NOT called)')

-- ============================================================
-- 4) Reload.ResetState — 强制重新 factory
-- ============================================================
local ok = Reload.ResetState('test_state_alpha')
p(ok == true,                               'ResetState returns true for existing key')

local s3 = Reload.Preserve('test_state_alpha', function() return {count = 7} end)
p(s3.count == 7 and s3 ~= s1,               'After ResetState, Preserve invokes factory anew')

p(Reload.ResetState('nonexistent_key') == false, 'ResetState returns false for unknown key')

-- ============================================================
-- 5) Reload.Stats — 数值类型 + 字段齐全
-- ============================================================
local st = Reload.Stats()
p(type(st) == 'table',                      'Stats returns table')
p(type(st.modules_reloaded) == 'number',    'Stats.modules_reloaded is number')
p(type(st.files_reloaded) == 'number',      'Stats.files_reloaded is number')
p(type(st.errors) == 'number',              'Stats.errors is number')
p(type(st.preserved_count) == 'number',     'Stats.preserved_count is number')
p(type(st.watched_count) == 'number',       'Stats.watched_count is number')
p(st.preserved_count >= 1,                  'Stats.preserved_count >= 1 (test_state_alpha still preserved)')

-- ============================================================
-- 6) Reload.GetLastError — File 失败后有记录
-- ============================================================
Reload.File('nonexistent_for_lasterror.lua')   -- 触发错误
local le = Reload.GetLastError()
p(type(le) == 'table',                      'GetLastError returns table after failure')
p(type(le.path) == 'string' and le.path:find('nonexistent_for_lasterror', 1, true) ~= nil,
                                            'GetLastError.path matches failed file')
p(type(le.msg) == 'string',                 'GetLastError.msg is string')
p(type(le.time) == 'number',                'GetLastError.time is number')

-- ============================================================
-- 7) Reload.SetErrorHandler — 注册 hook 被调用
-- ============================================================
local called_count = 0
local handler_path, handler_msg
Reload.SetErrorHandler(function(p_, m_)
    called_count = called_count + 1
    handler_path = p_
    handler_msg = m_
end)
Reload.File('nonexistent_for_hook.lua')
p(called_count == 1,                        'SetErrorHandler hook invoked on File failure')
p(handler_path:find('nonexistent_for_hook', 1, true) ~= nil,
                                            'Error handler receives path argument')

-- 取消注册
Reload.SetErrorHandler(nil)
Reload.File('nonexistent_for_hook_2.lua')
p(called_count == 1,                        'SetErrorHandler(nil) unregisters hook')

-- ============================================================
-- 8) Reload.Clear — 清所有 preserved + watched
-- ============================================================
Reload.Clear()
local s4 = Reload.Preserve('test_state_alpha', function() return {count = 100} end)
p(s4.count == 100,                          'After Clear, Preserve invokes factory (no stale state)')

local stC = Reload.Stats()
p(stC.preserved_count == 1,                 'Stats.preserved_count = 1 after Clear + 1 Preserve')

-- ============================================================
-- 9) Reload.WatchModule — 未知模块优雅失败
-- ============================================================
local wOk = Reload.WatchModule('this_module_does_not_exist_abc')
p(wOk == false,                             'WatchModule(unknown) returns false (graceful)')

p(Reload.UnwatchModule('this_module_does_not_exist_abc') == false,
                                            'UnwatchModule(unknown) returns false')

-- ============================================================
-- 10) Reload.RestartScript / IsRestartPending
-- ============================================================
p(Reload.IsRestartPending() == false,       'IsRestartPending = false initially')

-- 注意: 这个 smoke 在 headless 跑, RequestRestart 后我们也不真的 restart (脚本退出会触发 lumen 内部 dofile),
--       所以 smoke 内不要主动调 RestartScript() 否则会循环执行整个 smoke. 仅测 IsRestartPending 静默.
local rOk, rErr = Reload.RestartScript('')
p(rOk == false and type(rErr) == 'string',  'RestartScript("") → false + err (empty path rejected)')

-- ============================================================
-- 11) Module() — 用一个内置 Lua module 测试 (string)
-- ============================================================
-- 测试: Reload.Module 应能 require 标准库 (虽然不常见, 但语义应正确)
-- 跳过此项以避免污染 package.loaded["string"]
-- 改测一个不存在的模块, 应返 nil + err
local mr, mre = Reload.Module('nonexistent_module_def')
p(mr == nil and type(mre) == 'string',      'Module(nonexistent) → nil + err')

-- ============================================================
-- 12) Reload.File 成功路径 (写临时文件 + 加载)
-- ============================================================
-- 用 string.dump 不适合 (chunk 字节码相关), 我们用 io.open 写一个 tmp 文件然后 File 它
do
    local tmppath = 'reload_smoke_tmp.lua'
    local f = io.open(tmppath, 'w')
    if f then
        f:write('return "reload_smoke_value_42"\n')
        f:close()

        local rv = Reload.File(tmppath)
        p(rv == 'reload_smoke_value_42',    'File() loads + executes + returns chunk return value')

        -- 清理
        os.remove(tmppath)
    else
        p(false,                            'File() success path skipped: cannot create tmp file')
    end
end

-- ============================================================
-- 13) 终清理
-- ============================================================
Reload.Clear()

print(string.format("reload smoke: %d pass / %d fail", pass, fail))
if fail > 0 then error("reload smoke FAIL") end
