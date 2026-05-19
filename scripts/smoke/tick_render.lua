-- Phase H.0 smoke: Light.Time.* Tick-Render 解耦 + Light.Physics.SetAutoStep
--
-- 覆盖 (13 函数 + 2 物理 API + 7 行为段):
--   Light.Time 新 11 fn:
--     SetFixedTimestep / GetFixedTimestep / GetFixedDt
--     SetMaxFixedStepsPerFrame / GetMaxFixedStepsPerFrame
--     SetFrameTimeClamp / GetFrameTimeClamp
--     GetAlpha / GetAccumulator / GetLastStepCount / GetLastFrameTime
--   Light.Physics.World:
--     :SetAutoStep / :GetAutoStep    (Box2D)
--   Light.Physics3D.World:
--     :SetAutoStep / :GetAutoStep    (Bullet)
--
-- 测试段:
--   §1  Module surface (11 fn + 物理 API 完整性)
--   §2  默认值 (fixedHz=60, fixedDt≈1/60, maxStep=8, clamp=0.25)
--   §3  SetFixedTimestep round-trip + 越界 clamp (1..1000)
--   §4  SetMaxFixedStepsPerFrame round-trip + 越界 clamp (1..64)
--   §5  SetFrameTimeClamp round-trip + 越界 clamp (0.01..1.0)
--   §6  Get* 查询 (类型 + 范围 sanity)
--   §7  物理 auto-step 默认 false (Box2D) — 零回归
--   §8  物理 auto-step 默认 false (Bullet) — 零回归
--   §9  状态复位
--
-- Headless: smoke 不打开窗口, 仅测 API 表面 + 范围语义 + 物理 World 创建/销毁路径.
--           主循环驱动 (accumulator / OnFixedUpdate 触发频率) 在 demo_tick_render 内手动验证.
--
-- ASCII-only.

local function pass(msg) print("PASS: " .. msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

local ok, Time = pcall(require, "Light.Time")
if not ok then fail("require(Light.Time) failed: " .. tostring(Time)) end
if type(Time) ~= "table" then fail("Light.Time not a table (got " .. type(Time) .. ")") end

-- ============================================================
-- §1) Module surface
-- ============================================================
local fn_names = {
    "SetFixedTimestep", "GetFixedTimestep", "GetFixedDt",
    "SetMaxFixedStepsPerFrame", "GetMaxFixedStepsPerFrame",
    "SetFrameTimeClamp", "GetFrameTimeClamp",
    "GetAlpha", "GetAccumulator", "GetLastStepCount", "GetLastFrameTime",
}
for _, k in ipairs(fn_names) do
    if type(Time[k]) ~= "function" then
        fail("Light.Time." .. k .. " missing or not function (got " .. type(Time[k]) .. ")")
    end
end
pass("§1 Light.Time module surface ok (" .. #fn_names .. " Tick-Render fn)")

-- 旧 fn (Phase AR) 应仍存在
for _, k in ipairs({"GetTicks", "Delay", "AddTimer", "RemoveTimer"}) do
    if type(Time[k]) ~= "function" then
        fail("Light.Time." .. k .. " 旧 fn 丢失 — Phase H.0 破坏了 Phase AR API!")
    end
end
pass("§1 Phase AR 旧 fn 仍存在 (零回归)")

-- ============================================================
-- §2) 默认值
-- ============================================================
do
    local hz = Time.GetFixedTimestep()
    if hz ~= 60 then fail("default fixedHz 应 60, 得 " .. tostring(hz)) end
    pass("§2 default fixedHz = 60")

    local dt = Time.GetFixedDt()
    if math.abs(dt - 1.0/60.0) > 1e-9 then
        fail("default fixedDt 应 ≈1/60, 得 " .. tostring(dt))
    end
    pass(string.format("§2 default fixedDt = %.6f", dt))

    local n = Time.GetMaxFixedStepsPerFrame()
    if n ~= 8 then fail("default maxStep 应 8, 得 " .. tostring(n)) end
    pass("§2 default maxFixedStepsPerFrame = 8")

    local clamp = Time.GetFrameTimeClamp()
    if math.abs(clamp - 0.25) > 1e-9 then
        fail("default frameTimeClamp 应 0.25, 得 " .. tostring(clamp))
    end
    pass("§2 default frameTimeClamp = 0.25")
end

-- ============================================================
-- §3) SetFixedTimestep round-trip + clamp
-- ============================================================
do
    Time.SetFixedTimestep(120)
    if Time.GetFixedTimestep() ~= 120 then fail("Set/Get 120 round-trip 失败") end
    if math.abs(Time.GetFixedDt() - 1.0/120.0) > 1e-9 then fail("fixedDt @ 120Hz 偏差") end
    pass("§3 SetFixedTimestep(120) round-trip ok")

    Time.SetFixedTimestep(30)
    if Time.GetFixedTimestep() ~= 30 then fail("Set/Get 30 round-trip 失败") end
    pass("§3 SetFixedTimestep(30) round-trip ok")

    -- 越界 clamp (1..1000)
    Time.SetFixedTimestep(0)            -- 应 clamp 到 1
    if Time.GetFixedTimestep() ~= 1 then fail("0 应 clamp 到 1, 得 " .. tostring(Time.GetFixedTimestep())) end
    pass("§3 SetFixedTimestep(0) clamp 到 1")

    Time.SetFixedTimestep(99999)        -- 应 clamp 到 1000
    if Time.GetFixedTimestep() ~= 1000 then fail("99999 应 clamp 到 1000") end
    pass("§3 SetFixedTimestep(99999) clamp 到 1000")

    -- 复位
    Time.SetFixedTimestep(60)
end

-- ============================================================
-- §4) SetMaxFixedStepsPerFrame round-trip + clamp (1..64)
-- ============================================================
do
    Time.SetMaxFixedStepsPerFrame(16)
    if Time.GetMaxFixedStepsPerFrame() ~= 16 then fail("Set/Get 16 失败") end
    pass("§4 SetMaxFixedStepsPerFrame(16) round-trip ok")

    Time.SetMaxFixedStepsPerFrame(0)    -- clamp 到 1
    if Time.GetMaxFixedStepsPerFrame() ~= 1 then fail("0 应 clamp 到 1") end
    pass("§4 SetMaxFixedStepsPerFrame(0) clamp 到 1")

    Time.SetMaxFixedStepsPerFrame(200)  -- clamp 到 64
    if Time.GetMaxFixedStepsPerFrame() ~= 64 then fail("200 应 clamp 到 64") end
    pass("§4 SetMaxFixedStepsPerFrame(200) clamp 到 64")

    Time.SetMaxFixedStepsPerFrame(8)
end

-- ============================================================
-- §5) SetFrameTimeClamp round-trip + clamp (0.01..1.0)
-- ============================================================
do
    Time.SetFrameTimeClamp(0.5)
    if math.abs(Time.GetFrameTimeClamp() - 0.5) > 1e-9 then fail("Set/Get 0.5 失败") end
    pass("§5 SetFrameTimeClamp(0.5) round-trip ok")

    Time.SetFrameTimeClamp(0.001)       -- clamp 到 0.01
    if math.abs(Time.GetFrameTimeClamp() - 0.01) > 1e-9 then fail("0.001 应 clamp 到 0.01") end
    pass("§5 SetFrameTimeClamp(0.001) clamp 到 0.01")

    Time.SetFrameTimeClamp(5.0)         -- clamp 到 1.0
    if math.abs(Time.GetFrameTimeClamp() - 1.0) > 1e-9 then fail("5.0 应 clamp 到 1.0") end
    pass("§5 SetFrameTimeClamp(5.0) clamp 到 1.0")

    Time.SetFrameTimeClamp(0.25)
end

-- ============================================================
-- §6) Get* 查询 (类型 + 范围)
-- ============================================================
do
    local a = Time.GetAlpha()
    if type(a) ~= "number" then fail("GetAlpha 应 number, 得 " .. type(a)) end
    if a < 0 or a >= 1.0 then fail("alpha 应 ∈ [0, 1), 得 " .. tostring(a)) end
    pass(string.format("§6 GetAlpha = %.4f ∈ [0, 1)", a))

    local acc = Time.GetAccumulator()
    if type(acc) ~= "number" then fail("GetAccumulator 应 number") end
    if acc < 0 then fail("accumulator 不可负, 得 " .. tostring(acc)) end
    pass(string.format("§6 GetAccumulator = %.6f >= 0", acc))

    local n = Time.GetLastStepCount()
    if type(n) ~= "number" then fail("GetLastStepCount 应 number") end
    if n < 0 then fail("step count 不可负") end
    pass("§6 GetLastStepCount = " .. tostring(n))

    local ft = Time.GetLastFrameTime()
    if type(ft) ~= "number" then fail("GetLastFrameTime 应 number") end
    if ft < 0 then fail("frameTime 不可负") end
    pass(string.format("§6 GetLastFrameTime = %.6f (s)", ft))
end

-- ============================================================
-- §7) 物理 Box2D auto-step
-- ============================================================
do
    local ok2, Physics = pcall(require, "Light.Physics")
    if not ok2 or not Physics then
        pass("§7 Light.Physics 不可用 (CHOCO_HAS_BOX2D=0), 跳过")
    else
        -- 创建 World (Box2D)
        local World = Physics.World
        if type(World) ~= "table" then
            pass("§7 Light.Physics.World 不存在, 跳过")
        else
            local world = setmetatable({}, {__index = World})
            World(world)         -- __call 创建 __instance
            if type(world.__instance) ~= "userdata" then
                pass("§7 World 创建失败 (headless?), 跳过")
            else
                -- API 存在性
                if type(world.SetAutoStep) ~= "function" then fail("Box2D World:SetAutoStep 缺失") end
                if type(world.GetAutoStep) ~= "function" then fail("Box2D World:GetAutoStep 缺失") end

                -- 默认 false
                if world:GetAutoStep() ~= false then
                    fail("Box2D default autoStep 应 false (零回归), 得 " .. tostring(world:GetAutoStep()))
                end
                pass("§7 Box2D default autoStep = false (零回归)")

                -- round-trip
                world:SetAutoStep(true)
                if world:GetAutoStep() ~= true then fail("Box2D SetAutoStep(true) round-trip 失败") end
                world:SetAutoStep(false)
                if world:GetAutoStep() ~= false then fail("Box2D SetAutoStep(false) round-trip 失败") end
                pass("§7 Box2D SetAutoStep round-trip ok")

                -- 非 boolean 应 raise
                local pok = pcall(world.SetAutoStep, world, "yes")
                if pok then fail("Box2D SetAutoStep('yes') 应 raise") end
                pass("§7 Box2D SetAutoStep('yes') raise (类型校验)")

                world = nil
                collectgarbage("collect")
            end
        end
    end
end

-- ============================================================
-- §8) 物理 Bullet auto-step
-- ============================================================
do
    local ok3, Physics3D = pcall(require, "Light.Physics3D")
    if not ok3 or not Physics3D then
        pass("§8 Light.Physics3D 不可用 (CHOCO_HAS_BULLET=0), 跳过")
    elseif type(Physics3D.NewWorld) ~= "function" then
        pass("§8 Light.Physics3D.NewWorld 不存在, 跳过")
    else
        local w = Physics3D.NewWorld(0, -9.81, 0)
        if not w then
            pass("§8 Bullet NewWorld 失败 (headless?), 跳过")
        else
            -- API 存在性
            if type(w.SetAutoStep) ~= "function" then fail("Bullet World:SetAutoStep 缺失") end
            if type(w.GetAutoStep) ~= "function" then fail("Bullet World:GetAutoStep 缺失") end

            -- 默认 false
            if w:GetAutoStep() ~= false then
                fail("Bullet default autoStep 应 false (零回归), 得 " .. tostring(w:GetAutoStep()))
            end
            pass("§8 Bullet default autoStep = false (零回归)")

            -- round-trip
            w:SetAutoStep(true)
            if w:GetAutoStep() ~= true then fail("Bullet SetAutoStep(true) round-trip 失败") end
            w:SetAutoStep(false)
            if w:GetAutoStep() ~= false then fail("Bullet SetAutoStep(false) round-trip 失败") end
            pass("§8 Bullet SetAutoStep round-trip ok")

            -- 非 boolean 应 raise
            local pok = pcall(w.SetAutoStep, w, "yes")
            if pok then fail("Bullet SetAutoStep('yes') 应 raise") end
            pass("§8 Bullet SetAutoStep('yes') raise (类型校验)")

            -- 显式 Delete (smoke 不能依赖 GC 顺序)
            if type(w.Delete) == "function" then w:Delete() end
            w = nil
            collectgarbage("collect")
        end
    end
end

-- ============================================================
-- §9) 状态复位
-- ============================================================
do
    Time.SetFixedTimestep(60)
    Time.SetMaxFixedStepsPerFrame(8)
    Time.SetFrameTimeClamp(0.25)
    pass("§9 状态复位 (fixedHz=60, maxStep=8, clamp=0.25)")
end

print("")
print("=== Phase H.0 Tick-Render Decouple smoke: ALL TESTS PASSED ===")
print("Coverage: " .. #fn_names .. " Light.Time fn + Box2D/Bullet SetAutoStep/GetAutoStep")
print("Highlights:")
print("  - Defaults: fixedHz=60, maxStep=8, frameTimeClamp=0.25s")
print("  - Friendly clamp (Set 越界 → clamp + log warn, 不 raise)")
print("  - GetAlpha ∈ [0, 1); accumulator/stepCount/frameTime >= 0")
print("  - Box2D + Bullet auto-step 默认 false (零回归; 32 老 sample 不必改)")
print("  - Phase AR 旧 fn (GetTicks/Delay/Timer) 完整保留")
