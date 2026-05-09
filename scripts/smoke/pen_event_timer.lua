-- Phase AR smoke: Pen events + Light.Event + Light.Time AddTimer
-- (no actual Pen device required; verifies API registration + headless safety)

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- ==================== 1) Pen 常量 ====================

local ok, UI = pcall(require, "Light.UI")
if not ok then fail("require(Light.UI) failed: " .. tostring(UI)) end

local pen_input = {
    PEN_INPUT_DOWN       = 1,         -- 1 << 0
    PEN_INPUT_BUTTON_1   = 2,         -- 1 << 1
    PEN_INPUT_BUTTON_2   = 4,
    PEN_INPUT_BUTTON_3   = 8,
    PEN_INPUT_BUTTON_4   = 16,
    PEN_INPUT_BUTTON_5   = 32,
    PEN_INPUT_ERASER_TIP = 1073741824, -- 1 << 30
}
for k, expected in pairs(pen_input) do
    if type(UI[k]) ~= "number" then fail("Light.UI." .. k .. " not number") end
    if UI[k] ~= expected then
        fail("Light.UI." .. k .. " expected " .. expected .. ", got " .. tostring(UI[k]))
    end
end
pass("Light.UI Pen input flags ok (7 consts)")

local pen_axis = {
    PEN_AXIS_PRESSURE = 0, PEN_AXIS_XTILT = 1, PEN_AXIS_YTILT = 2,
    PEN_AXIS_DISTANCE = 3, PEN_AXIS_ROTATION = 4, PEN_AXIS_SLIDER = 5,
    PEN_AXIS_TANGENTIAL_PRESSURE = 6, PEN_AXIS_COUNT = 7,
}
for k, expected in pairs(pen_axis) do
    if type(UI[k]) ~= "number" then fail("Light.UI." .. k .. " not number") end
    if UI[k] ~= expected then
        fail("Light.UI." .. k .. " expected " .. expected)
    end
end
pass("Light.UI Pen axis IDs ok (8 consts)")

-- ==================== 2) Light.Event 模块 ====================

local ok2, Event = pcall(require, "Light.Event")
if not ok2 then fail("require(Light.Event) failed: " .. tostring(Event)) end
if type(Event) ~= "table" then fail("Light.Event not a table") end

local ev_fns = {
    "HasEvent", "HasEvents", "FlushEvent", "FlushEvents",
    "Push", "Pump", "SetEnabled", "IsEnabled", "Register",
}
for _, k in ipairs(ev_fns) do
    if type(Event[k]) ~= "function" then
        fail("Light.Event." .. k .. " not a function (got " .. type(Event[k]) .. ")")
    end
end
pass("Light.Event module ok (" .. #ev_fns .. " fns)")

-- 几个常用事件类型常量
for _, k in ipairs({"QUIT", "KEY_DOWN", "KEY_UP", "MOUSE_MOTION",
                   "MOUSE_BUTTON_DOWN", "PEN_DOWN", "PEN_UP", "USER"}) do
    if type(Event[k]) ~= "number" then
        fail("Light.Event." .. k .. " not a number constant")
    end
end
pass("Light.Event event type constants ok (8 sampled)")

-- ==================== 3) Light.Event 行为 ====================

-- Pump 不崩
Event.Pump()
pass("Event.Pump() ok")

-- HasEvent (任何类型, 即使队列空也应返回 boolean)
local has = Event.HasEvent(Event.QUIT)
if type(has) ~= "boolean" then fail("HasEvent should return boolean") end
pass("HasEvent(QUIT) = " .. tostring(has))

-- HasEvents 范围查询
local has_range = Event.HasEvents(Event.KEY_DOWN, Event.MOUSE_WHEEL)
if type(has_range) ~= "boolean" then fail("HasEvents should return boolean") end
pass("HasEvents(KEY_DOWN..MOUSE_WHEEL) = " .. tostring(has_range))

-- Flush 不崩
Event.FlushEvent(Event.QUIT)
Event.FlushEvents(Event.KEY_DOWN, Event.MOUSE_WHEEL)
pass("FlushEvent / FlushEvents ok")

-- SetEnabled / IsEnabled round-trip (用一个无害的事件类型)
local sample_type = Event.MOUSE_MOTION
local was_enabled = Event.IsEnabled(sample_type)
Event.SetEnabled(sample_type, false)
if Event.IsEnabled(sample_type) ~= false then
    fail("SetEnabled(false) did not stick")
end
Event.SetEnabled(sample_type, true)
if Event.IsEnabled(sample_type) ~= true then
    fail("SetEnabled(true) did not stick")
end
-- 还原
Event.SetEnabled(sample_type, was_enabled)
pass("SetEnabled / IsEnabled round-trip ok (was " .. tostring(was_enabled) .. ")")

-- Register 用户事件 ID
local id1 = Event.Register(1)
if type(id1) ~= "number" then fail("Register(1) should return number") end
if id1 == 0 then fail("Register(1) returned 0 (allocation failed?)") end
pass("Event.Register(1) = " .. id1)

local id2 = Event.Register(3)
if type(id2) ~= "number" or id2 == 0 then fail("Register(3) failed") end
if id2 <= id1 then
    fail("Register(3) should return id > " .. id1 .. ", got " .. id2)
end
pass("Event.Register(3) = " .. id2 .. " (range " .. id2 .. ".." .. (id2 + 2) .. ")")

-- Register(0) 边界
local id0 = Event.Register(0)
if type(id0) ~= "number" then fail("Register(0) should return number") end
pass("Event.Register(0) = " .. id0 .. " (edge case)")

-- Push 用户事件
local pushed = Event.Push(id1, 42, 100, 200)
if type(pushed) ~= "boolean" then fail("Push should return boolean") end
pass("Event.Push(id1, 42, 100, 200) = " .. tostring(pushed))

-- Push 仅 type 参数 (其他默认 0)
local pushed2 = Event.Push(id1)
if type(pushed2) ~= "boolean" then fail("Push(type) should return boolean") end
pass("Event.Push(id1) = " .. tostring(pushed2))

-- 验证刚 push 的事件确实在队列中
Event.Pump()
local has_user = Event.HasEvent(id1)
if type(has_user) ~= "boolean" then fail("HasEvent(id1) should return boolean") end
pass("HasEvent(id1) after Push = " .. tostring(has_user))

-- 清掉以免干扰主循环
Event.FlushEvent(id1)
pass("FlushEvent(id1) cleanup ok")

-- ==================== 4) Light.Time AddTimer / RemoveTimer ====================

local ok3, Time = pcall(require, "Light.Time")
if not ok3 then fail("require(Light.Time) failed: " .. tostring(Time)) end

if type(Time.AddTimer) ~= "function" then fail("Time.AddTimer not function") end
if type(Time.RemoveTimer) ~= "function" then fail("Time.RemoveTimer not function") end
pass("Light.Time.AddTimer / RemoveTimer ok")

-- AddTimer(很大的 ms, fn) → 返回 >0 timer_id, 不会被实际触发
local timer_id = Time.AddTimer(60000, function() end)  -- 60s 后触发, 远超 smoke 时长
if type(timer_id) ~= "number" then fail("AddTimer should return number") end
if timer_id == 0 then fail("AddTimer(60000, fn) returned 0 - SDL not init?") end
pass("AddTimer(60000, fn) = " .. timer_id)

-- RemoveTimer 应返回 true
local removed = Time.RemoveTimer(timer_id)
if type(removed) ~= "boolean" then fail("RemoveTimer should return boolean") end
if not removed then fail("RemoveTimer(" .. timer_id .. ") returned false") end
pass("RemoveTimer(" .. timer_id .. ") = true")

-- RemoveTimer(无效 id) → false, 不崩
local removed_bad = Time.RemoveTimer(999999)
if type(removed_bad) ~= "boolean" then fail("RemoveTimer(invalid) should return boolean") end
if removed_bad then fail("RemoveTimer(999999) should be false") end
pass("RemoveTimer(999999) = false (no crash)")

-- ==================== 5) 兼容性: Light.Time 现有 fns 仍可用 ====================

if type(Time.GetTicks) ~= "function" then fail("Time.GetTicks regression!") end
if type(Time.GetPerformanceCounter) ~= "function" then fail("Time.GetPerformanceCounter regression!") end
local t = Time.GetTicks()
if type(t) ~= "number" or t < 0 then fail("Time.GetTicks() bad value") end
pass("Light.Time backward compat ok (GetTicks=" .. t .. "ms)")

print("pen_event_timer smoke ok")
