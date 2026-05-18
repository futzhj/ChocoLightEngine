-- ============================================================
-- game_logic.lua — 用户改这个文件演示热重载
--
-- 改下面任意常量, 保存后约 0.5 秒方块自动应用新值:
--   M.COLOR   — RGB 颜色
--   M.SIZE    — 方块边长
--   M.CX/CY   — 方块中心位置
--   M.SPIN    — 倍率 (1.0=匀速, 2.0=两倍)
--
-- 故意写错 (如 M.COLOR = {1,) → 方块按旧逻辑继续转 + 控制台报错
-- 修正错误后自动恢复.
-- ============================================================

local M = {}

-- ======== 用户配置区 (改这些值看实时效果) ========
M.COLOR = {0.2, 0.8, 1.0}        -- 青色
M.SIZE  = 120                     -- 边长 (px)
M.CX    = 400                     -- 中心 X
M.CY    = 300                     -- 中心 Y
M.SPIN  = 1.0                     -- 旋转倍率 (1.0 = main 默认 60°/s)
-- =====================================================

-- 绘制函数 — 每帧 main:Draw() 间接调用
-- state: 由 Light.Reload.Preserve 维护, reload 后保留 angle/frame
function M.draw(window, state)
    local Gfx = Light.Graphics
    Gfx.SetColor(M.COLOR[1], M.COLOR[2], M.COLOR[3])

    -- 应用 SPIN 倍率: 取 state.angle 再乘 SPIN (允许 reload 时改速度)
    local angle = state.angle * M.SPIN
    Gfx.Rectangle(2,
        M.CX - M.SIZE/2, M.CY - M.SIZE/2, M.SIZE, M.SIZE,
        0, 0, angle,
        1, 1, 1,
        M.CX, M.CY, 0)

    -- HUD: 顶部显示帧数 + 角度 (帮你确认 reload 时 state 没归零)
    Gfx.SetColor(1, 1, 1)
    Gfx.Print(string.format("frame=%d  angle=%.1f  spin=%.2f  size=%d",
        state.frame, state.angle, M.SPIN, M.SIZE), 10, 10)
    Gfx.Print("Edit game_logic.lua + save → see live update", 10, 30)
    Gfx.Print("R = manual reload, S = stats, ESC = quit", 10, 50)
end

return M
