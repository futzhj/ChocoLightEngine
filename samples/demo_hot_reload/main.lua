-- ============================================================
-- Phase G.0 — demo_hot_reload
--
-- 演示 Light.Reload 三大能力:
--   1. WatchModule('game_logic') — 改 game_logic.lua 自动重载
--   2. Preserve('demo_state', factory) — reload 后角度/帧数继续累加 (不归零)
--   3. SetErrorHandler — syntax error 时方块继续按旧逻辑转 + 日志报错
--
-- 操作:
--   - 启动后看到一个彩色旋转方块
--   - 用编辑器改 samples/demo_hot_reload/game_logic.lua (颜色/速度/大小)
--   - 保存后约 0.5 秒自动 reload, 方块立即应用新值
--   - 故意写 syntax error → 方块按旧逻辑继续转 + 控制台报错
--   - 修正 error 后自动恢复
--   - 按 ESC / 关窗 退出
--   - 按 R 主动调 Reload.Module('game_logic')
--   - 按 S 打印 Reload.Stats()
-- ============================================================

local UI     = Light.UI
local Gfx    = Light.Graphics
local Reload = Light.Reload

if not Reload then
    print('Light.Reload not available — please rebuild with Phase G.0')
    return
end

print('demo_hot_reload — 修改 game_logic.lua, 观察实时效果')
print('  按 R: 手动 reload  按 S: 打印 Stats  ESC: 退出')

-- 错误处理: reload 失败时打印, 不挂 demo
Reload.SetErrorHandler(function(path, err)
    print(string.format('[RELOAD ERR] %s -- %s', path, err))
end)

-- 监视 game_logic 模块, mtime 变化自动 reload
if Reload.WatchModule('game_logic') then
    print('  watching game_logic.lua for changes...')
else
    print('  WARN: WatchModule(game_logic) failed — manual reload only')
end

-- 状态保留: reload 后 angle/frame 不归零
local state = Reload.Preserve('demo_state', function()
    return { angle = 0.0, frame = 0, last_color = {0, 0, 0} }
end)

local Demo = Light(UI.Window):New()

function Demo:OnOpen()
    print('Demo opened')
end

function Demo:Draw()
    -- 间接调用: 每帧从 package.loaded 取最新 game_logic
    -- 即使 reload 时 game_logic 暂时不可用, pcall 包住不挂 demo
    local ok, gl = pcall(require, 'game_logic')
    if ok and gl and type(gl.draw) == 'function' then
        local ok2, err = pcall(gl.draw, self, state)
        if not ok2 then
            print('[draw err]', err)
        end
    else
        -- game_logic 不可用 → 显示提示
        Gfx.SetColor(1, 0, 0)
        Gfx.Rectangle(2, 350, 280, 100, 40, 0, 0, 0, 1, 1, 1, 0, 0, 0)
    end
end

function Demo:Update(dt)
    state.frame = state.frame + 1
    -- 用户回调里直接累加角度 (因为我们不用模块的 update, 简化)
    -- game_logic.lua 用 state.angle 在 draw 里推进 + 渲染
    if dt > 0.1 then dt = 0.1 end
    state.angle = (state.angle + dt * 60.0) % 360.0   -- 60°/s 基础速度
end

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end
    if key == 256 then
        self:Close()
    elseif key == string.byte('R') or key == string.byte('r') then
        -- 手动 reload
        print('--- manual reload game_logic ---')
        local m, err = Reload.Module('game_logic')
        if m then print('OK: reloaded') else print('FAIL:', err) end
    elseif key == string.byte('S') or key == string.byte('s') then
        local st = Reload.Stats()
        print(string.format(
            'Stats: modules=%d files=%d errors=%d preserved=%d watched=%d',
            st.modules_reloaded, st.files_reloaded, st.errors,
            st.preserved_count, st.watched_count))
    end
end

Demo:Open(800, 600, 'demo_hot_reload (Phase G.0)')

-- 主循环 + HotReload 轮询 (默认 0.5s 间隔)
while UI.Loop() do
    UI.Resume()
    -- 每帧调 Check, 内部累加时间到 interval 才真正扫描 (开销小)
    Light.HotReload.Check(1.0 / 60.0)   -- 假设 ~60fps; 不精确不影响 (interval 是积分)
end

print('demo_hot_reload exited')
print(string.format('  final frames = %d, angle = %.1f deg', state.frame, state.angle))
