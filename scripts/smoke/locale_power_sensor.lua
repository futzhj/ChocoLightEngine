-- locale_power_sensor.lua
-- Smoke 验证 Light.Locale / Light.Power / Light.Sensor 三个新模块的 API 注册与边界行为。
-- 设计为可在 6 平台 CI runtime 中执行 (Linux/Mac/Win runner 均无真传感器, 关键是不崩溃)。

local function fail(msg)
    print("FAIL: " .. tostring(msg))
    os.exit(1)
end

local function assert_function(v, name)
    if type(v) ~= "function" then
        fail(name .. " is not a function (got " .. type(v) .. ")")
    end
end

local function assert_type(v, expected, name)
    if type(v) ~= expected then
        fail(name .. " expected " .. expected .. ", got " .. type(v))
    end
end

local function require_table(name)
    local ok, mod = pcall(require, name)
    if not ok then
        fail("require(" .. name .. ") failed: " .. tostring(mod))
    end
    assert_type(mod, "table", name)
    return mod
end

-- ==================== Light.Locale ====================

local Locale = require_table("Light.Locale")
assert_function(Locale.GetPreferred, "Light.Locale.GetPreferred")

local list, err = Locale.GetPreferred()
-- 各平台都应返回 table (可能空), nil 仅在严重错误时
if list ~= nil then
    assert_type(list, "table", "Light.Locale.GetPreferred() return")
    -- 抽样检查第一项结构 (若有)
    if list[1] then
        assert_type(list[1], "table", "Locale[1]")
        assert_type(list[1].language, "string", "Locale[1].language")
        -- country 可为 nil
        if list[1].country ~= nil then
            assert_type(list[1].country, "string", "Locale[1].country")
        end
    end
else
    assert_type(err, "string", "Light.Locale.GetPreferred error message")
end

-- ==================== Light.Power ====================

local Power = require_table("Light.Power")
assert_function(Power.Info, "Light.Power.Info")

local state, seconds, percent = Power.Info()
assert_type(state, "string", "Power state")
assert_type(seconds, "number", "Power seconds")
assert_type(percent, "number", "Power percent")

-- state 必须是已知枚举之一
local valid_states = {
    unknown = true, on_battery = true, no_battery = true,
    charging = true, charged = true,
}
if not valid_states[state] then
    fail("Power.Info() returned unexpected state: " .. tostring(state))
end

-- ==================== Light.Sensor ====================

local Sensor = require_table("Light.Sensor")
assert_function(Sensor.OpenAccel, "Light.Sensor.OpenAccel")
assert_function(Sensor.OpenGyro,  "Light.Sensor.OpenGyro")
assert_function(Sensor.GetAccel,  "Light.Sensor.GetAccel")
assert_function(Sensor.GetGyro,   "Light.Sensor.GetGyro")
assert_function(Sensor.Close,     "Light.Sensor.Close")

-- 桌面 CI 通常无传感器, OpenAccel/OpenGyro 应优雅返回 false + err 而非崩溃
local ok_a, err_a = Sensor.OpenAccel()
if ok_a then
    -- 真有传感器 (移动 CI?), 验证读取
    local x, y, z = Sensor.GetAccel()
    assert_type(x, "number", "Accel.x")
    assert_type(y, "number", "Accel.y")
    assert_type(z, "number", "Accel.z")
else
    -- 失败时 err 必须是字符串
    assert_type(err_a, "string", "OpenAccel error message")
    -- 此时 GetAccel 也应安全返回 nil + err
    local gx, gerr = Sensor.GetAccel()
    if gx ~= nil then
        fail("GetAccel should return nil before successful Open")
    end
    assert_type(gerr, "string", "GetAccel error message")
end

local ok_g, err_g = Sensor.OpenGyro()
if not ok_g then
    assert_type(err_g, "string", "OpenGyro error message")
    local gx, gerr = Sensor.GetGyro()
    if gx ~= nil then
        fail("GetGyro should return nil before successful Open")
    end
    assert_type(gerr, "string", "GetGyro error message")
end

-- Close 总是安全的, 即使没 Open 过
Sensor.Close()
Sensor.Close()  -- 重复 Close 不应崩溃

print("locale_power_sensor ok")
