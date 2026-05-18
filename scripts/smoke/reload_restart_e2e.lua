-- ============================================================
-- Phase G.0 — RestartScript 端到端验证
--
-- 测试 lumen-master 的 lumen_RequestRestart + pMain 主循环重启逻辑.
-- 验证: 调 RestartScript 后, lumen 在脚本退出后重新 dofile 目标脚本.
--
-- 该脚本利用一个外部文件 (_reload_restart_counter.txt) 跨"重启"传递计数.
-- 第 1 次运行: 写 "1" 到文件, 调 RestartScript(arg[0]), 退出
-- 第 2 次运行 (lumen restart 触发): 读到 "1", 写 "2", 不再 RestartScript
-- 验证完毕 → 删文件 → exit 0
--
-- 失败案例: 如果 lumen restart 没生效, 第 2 次不会执行 → 文件停留在 "1"
-- 该脚本运行后, 外部可检查文件值判断 e2e 通过与否
-- ============================================================

local TMPFILE = '_reload_restart_counter.txt'

local function read_counter()
    local f = io.open(TMPFILE, 'r')
    if not f then return 0 end
    local v = tonumber(f:read('*a') or '0') or 0
    f:close()
    return v
end

local function write_counter(n)
    local f = io.open(TMPFILE, 'w')
    if f then
        f:write(tostring(n))
        f:close()
    end
end

local n = read_counter()
print(string.format('reload_restart_e2e: run #%d (counter=%d)', n + 1, n))

if n == 0 then
    -- 第 1 次: 写 1, 请求 restart
    write_counter(1)
    print('  → calling Reload.RestartScript()')
    local ok, err = Light.Reload.RestartScript()   -- 默认走 arg[0]
    if not ok then
        print('FAIL: RestartScript returned false:', err)
        os.remove(TMPFILE)
        os.exit(1)
    end
    print('  → IsRestartPending =', Light.Reload.IsRestartPending())
    print('  exiting current run, expecting lumen to dofile us again...')
    -- 退出脚本 (隐式 return), lumen 应在 pMain 内捕获 restart pending 并 dofile arg[0]
elseif n == 1 then
    -- 第 2 次: lumen 重启成功
    write_counter(2)
    print('  ✓ lumen restart triggered successfully — e2e PASS')
    os.remove(TMPFILE)
    os.exit(0)
else
    -- 不应到这里
    print('FAIL: unexpected counter value:', n)
    os.remove(TMPFILE)
    os.exit(1)
end
