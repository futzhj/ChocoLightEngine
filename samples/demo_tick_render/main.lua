-- demo_tick_render — Phase H.0 Tick-Render 解耦演示
--
-- 演示要点:
--   1) 双方块对比 alpha 插值:
--      左方块 — 不用 alpha (每帧渲染 fixed update 后的最新位置)
--               高 fixedHz / 低 render fps 时直观可见跳动
--      右方块 — 用 alpha = Light.Time.GetAlpha() 做 lerp(prev, curr, alpha)
--               即便 60Hz fixed / 144Hz render 仍平滑
--   2) Box2D 弹球 (auto-step) — 引擎自动 Step, Lua 不调 world:Step
--   3) HUD 显示:
--        fixedHz / actualFPS (1/wallDt) / alpha / lastStepCount / accumulator
--   4) 按键:
--        1/2/3/4 — 切 fixedHz (30 / 60 / 120 / 144)
--        A       — 切 alpha 插值开关 (左右方块行为切换)
--        P       — 切 Box2D auto-step (启用 / 禁用)
--        R       — 复位 (默认 60Hz / alpha=ON / auto-step=ON)
--        ESC     — 退出
--
-- 设计教学:
--   - OnFixedUpdate(dt) — dt = fixedDt 永远固定 (60Hz=16.67ms)
--                          物理 + 网络同步 + 决定性逻辑写这里
--   - OnRender(alpha, dt) — alpha ∈ [0, 1) 距离上次 fixed update 进度
--                            状态 lerp 写这里
--   - Update(dt) — 仍可用 wall-clock (兼容老 sample, 不推荐新代码)

local W, H = 1000, 700
local Demo  = {}

-- ============================================================
-- 状态
-- ============================================================

local cube_prev = {x = 100, y = 350, vx = 250}    -- 上次 fixed update 后位置 (用于 lerp)
local cube_curr = {x = 100, y = 350, vx = 250}    -- 当前 fixed update 后位置

local use_alpha = true        -- 切 alpha 插值
local fps_avg   = 0           -- HUD 显示用 (EMA)

-- Box2D 物理 (auto-step 演示; 若 Box2D 不可用则用纯 Lua fallback)
local Physics    = nil
local world      = nil
local ball_body  = nil
local use_box2d  = false

local function init_physics()
    local ok, P = pcall(require, "Light.Physics")
    if not ok or type(P) ~= "table" or type(P.World) ~= "table" then return end
    Physics = P

    -- 创建 World
    world = setmetatable({}, {__index = P.World})
    P.World(world)
    if type(world.__instance) ~= "userdata" then
        world = nil
        return
    end

    -- 设重力 (y 向下, pixels/s^2)
    world:SetGravity(0, 800)
    -- Phase H.0 — 启用 auto-step (引擎自动 Step, Lua 不调)
    world:SetAutoStep(true)

    -- 创建一个动态弹球 (在 (500, 100) 落下, 反弹到地面)
    ball_body = world:CreateBody(500, 100)
    if ball_body and type(ball_body.SetType) == "function" then
        ball_body:SetType("dynamic")
        local shape = P.NewCircleShape(20)
        if shape and type(ball_body.AttachFixture) == "function" then
            ball_body:AttachFixture(shape, {density = 1.0, restitution = 0.7})
        end
    end
    -- 地面 (静态, y=600)
    local ground = world:CreateBody(500, 650)
    if ground and type(ground.SetType) == "function" then
        ground:SetType("static")
        local g = P.NewEdgeShape(-500, 0, 500, 0)
        if g and type(ground.AttachFixture) == "function" then
            ground:AttachFixture(g, {density = 0, friction = 0.5})
        end
    end

    use_box2d = true
end

-- ============================================================
-- 生命周期
-- ============================================================

function Demo:OnOpen()
    Light.Time.SetFixedTimestep(60)
    -- Phase H.0.1 — 启用引擎内置 HUD overlay (默认关; 此处主动打开)
    Light.Time.SetHUDPosition(20, 20)
    Light.Time.SetHUDEnabled(true)
    init_physics()
    print(string.format("[demo_tick_render] init: fixedHz=%d, box2d=%s, HUD=%s",
                         Light.Time.GetFixedTimestep(),
                         tostring(use_box2d),
                         tostring(Light.Time.GetHUDEnabled())))
end

-- ============================================================
-- OnFixedUpdate (60Hz fixed by default)
--   - 推进决定性逻辑
--   - 物理 Box2D auto-step 由引擎在此调度 (不需用户调 world:Step)
-- ============================================================

function Demo:OnFixedUpdate(dt)
    -- 保存上一帧位置 (用于 alpha 插值)
    cube_prev.x = cube_curr.x
    cube_prev.y = cube_curr.y

    -- 推进方块运动 (来回弹)
    cube_curr.x = cube_curr.x + cube_curr.vx * dt
    if cube_curr.x < 80 then
        cube_curr.x = 80
        cube_curr.vx = math.abs(cube_curr.vx)
    elseif cube_curr.x > W - 80 then
        cube_curr.x = W - 80
        cube_curr.vx = -math.abs(cube_curr.vx)
    end
end

-- ============================================================
-- OnRender(alpha, dt)
--   - alpha ∈ [0, 1) 距上次 fixed update 进度
--   - dt = wall-clock frameTime (已 clamp)
-- ============================================================

function Demo:OnRender(alpha, dt)
    -- HUD 平均 FPS (EMA)
    if dt > 0 then
        local instant = 1.0 / dt
        fps_avg = (fps_avg == 0) and instant or (fps_avg * 0.95 + instant * 0.05)
    end
end

-- ============================================================
-- Draw (旧)
-- ============================================================

function Demo:Draw()
    local Gfx = Light.Graphics
    Gfx.Clear(0.08, 0.10, 0.14, 1)

    -- ---------- 左方块: 不用 alpha ----------
    -- 直接渲染最新 cube_curr → 60Hz fixed + 144Hz render 时会看到方块"卡顿"
    Gfx.SetColor(0.9, 0.4, 0.4, 1)
    Gfx.DrawRect(cube_curr.x - 30, 200, 60, 60)
    Gfx.SetColor(1, 1, 1, 1)

    -- ---------- 右方块: 用 alpha 插值 ----------
    -- lerp(prev, curr, alpha) → 在两个 fixed update 之间平滑插值
    local alpha = use_alpha and Light.Time.GetAlpha() or 1.0
    local rx = cube_prev.x + (cube_curr.x - cube_prev.x) * alpha
    Gfx.SetColor(0.4, 0.9, 0.4, 1)
    Gfx.DrawRect(rx - 30, 400, 60, 60)
    Gfx.SetColor(1, 1, 1, 1)

    -- ---------- 标签 ----------
    Gfx.SetColor(0.9, 0.4, 0.4, 1)
    Gfx.DrawText("LEFT: no alpha lerp", 30, 170)
    Gfx.SetColor(0.4, 0.9, 0.4, 1)
    Gfx.DrawText("RIGHT: alpha lerp (smooth)", 30, 370)
    Gfx.SetColor(1, 1, 1, 1)

    -- ---------- Box2D 弹球 ----------
    if use_box2d and ball_body and type(ball_body.GetPosition) == "function" then
        local bx, by = ball_body:GetPosition()
        Gfx.SetColor(1.0, 0.8, 0.2, 1)
        Gfx.DrawCircle(bx, by, 20, true)
        Gfx.SetColor(0.5, 0.5, 0.5, 1)
        Gfx.DrawLine(0, 650, W, 650)
        Gfx.SetColor(1, 1, 1, 1)
        Gfx.DrawText("Box2D auto-step ball", 30, 620)
    end

    -- Phase H.0.1 — 引擎内置 HUD (4 行 fixedHz/FPS/alpha/accumulator)
    Light.Time.DrawHUD()

    -- demo 专属信息: state + 按键提示 (HUD 之下偏移 90px)
    local y = 90
    local function line(txt)
        Gfx.DrawText(txt, 20, y)
        y = y + 16
    end
    line(string.format("alpha lerp: %s   Box2D auto-step: %s   HUD: %s   PAUSED: %s",
                       use_alpha and "ON" or "OFF",
                       (use_box2d and world and world:GetAutoStep()) and "ON" or "OFF",
                       Light.Time.GetHUDEnabled() and "ON" or "OFF",
                       Light.Time.IsPaused() and "YES" or "no"))
    line("Keys: 1=30Hz 2=60Hz 3=120Hz 4=144Hz   A=alpha P=physics H=HUD G=pause R=reset ESC")
end

-- ============================================================
-- 输入
-- ============================================================

function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end       -- press only

    if key == 256 then                   -- ESC
        self:Close()
        return
    end

    if     key == string.byte("1") then Light.Time.SetFixedTimestep(30)
    elseif key == string.byte("2") then Light.Time.SetFixedTimestep(60)
    elseif key == string.byte("3") then Light.Time.SetFixedTimestep(120)
    elseif key == string.byte("4") then Light.Time.SetFixedTimestep(144)
    elseif key == string.byte("A") then
        use_alpha = not use_alpha
        print("[demo_tick_render] alpha lerp = " .. tostring(use_alpha))
    elseif key == string.byte("P") then
        if use_box2d and world then
            local cur = world:GetAutoStep()
            world:SetAutoStep(not cur)
            print("[demo_tick_render] Box2D auto-step = " .. tostring(not cur))
        end
    elseif key == string.byte("H") then
        -- Phase H.0.1 — 切引擎内置 HUD 显示
        local cur = Light.Time.GetHUDEnabled()
        Light.Time.SetHUDEnabled(not cur)
        print("[demo_tick_render] HUD = " .. tostring(not cur))
    elseif key == string.byte("G") then
        -- Phase H.0.3 — 手动切 Pause/Resume (模拟切后台/前台)
        if Light.Time.IsPaused() then
            Light.Time.Resume()
            print("[demo_tick_render] Resume (next dt forced 0)")
        else
            Light.Time.Pause()
            print("[demo_tick_render] Pause (accumulator frozen)")
        end
    elseif key == string.byte("R") then
        Light.Time.SetFixedTimestep(60)
        use_alpha = true
        Light.Time.SetHUDEnabled(true)
        Light.Time.Resume()  -- Phase H.0.3 — reset 确保非 paused
        if use_box2d and world then world:SetAutoStep(true) end
        cube_prev.x, cube_prev.y = 100, 350
        cube_curr.x, cube_curr.y = 100, 350
        cube_curr.vx = 250
        print("[demo_tick_render] reset")
    end
end

-- ============================================================
-- 主入口
-- ============================================================

Light(Demo):Open(W, H, "Phase H.0 - Tick-Render Decouple Demo")

-- Phase H.0.2 — 跨平台一行主循环 (优先用; 老 sample 的 while 写法仍工作).
-- Web: emscripten_set_main_loop_arg, 后台标签页自动暂停 (节能).
-- Native: 阻塞 while 等价 `while UI.Loop() do UI.Resume() end`.
if Light.UI.RunBrowserMainLoop then
    Light.UI.RunBrowserMainLoop()
else
    while Light.UI.Loop() do Light.UI.Resume() end
end
print("demo_tick_render ok")
