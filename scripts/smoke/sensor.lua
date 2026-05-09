-- Phase AL smoke: Light.Sensor (Phase G + Phase AL 完整覆盖)
--
-- 在桌面 CI runner 上 Sensor 通常没有真传感器, 关键是验证:
--   * 17 fns 全部注册
--   * 9 const 全部存在且类型正确
--   * 所有边界路径 (nil handle / 非法 id / 越界 num_values) 不崩溃
--   * Phase G 5 fns 行为不破坏 (向后兼容)
--   * Phase AL 12 fns 在无传感器情况下优雅失败

local function fail(msg)
    print("FAIL: " .. tostring(msg))
    os.exit(1)
end

local function pass(msg) print(msg) end

local function assert_function(t, k)
    if type(t[k]) ~= "function" then fail(k .. " not a function") end
end

local function assert_number(v, name)
    if type(v) ~= "number" then fail(name .. " not number, got " .. type(v)) end
end

-- ==================== 1) module loads ====================

local ok, S = pcall(require, "Light.Sensor")
if not ok then fail("require(Light.Sensor) failed: " .. tostring(S)) end
if type(S) ~= "table" then fail("Light.Sensor not a table") end

-- ==================== 2) Phase G: 5 fns ====================

for _, k in ipairs({ "OpenAccel", "OpenGyro", "GetAccel", "GetGyro", "Close" }) do
    assert_function(S, k)
end
pass("Light.Sensor module ok (5 Phase G functions)")

-- ==================== 3) Phase AL: 12 fns ====================

local phase_al_fns = {
    "GetSensors", "GetSensorNameForID", "GetSensorTypeForID",
    "GetSensorNonPortableTypeForID",
    "OpenSensorByID", "GetSensorFromID", "CloseSensor",
    "GetSensorName", "GetSensorType", "GetSensorID",
    "GetSensorData", "UpdateSensors",
}
for _, k in ipairs(phase_al_fns) do
    assert_function(S, k)
end
pass("Light.Sensor Phase AL fns ok (12 functions)")

-- ==================== 4) 9 constants ====================

local consts = {
    SENSOR_INVALID = -1,
    SENSOR_UNKNOWN = 0,
    SENSOR_ACCEL = 1,
    SENSOR_GYRO = 2,
    SENSOR_ACCEL_L = 3,
    SENSOR_GYRO_L = 4,
    SENSOR_ACCEL_R = 5,
    SENSOR_GYRO_R = 6,
}
for k, expected in pairs(consts) do
    assert_number(S[k], k)
    if S[k] ~= expected then fail(k .. " expected " .. expected .. " got " .. S[k]) end
end
assert_number(S.STANDARD_GRAVITY, "STANDARD_GRAVITY")
if math.abs(S.STANDARD_GRAVITY - 9.80665) > 0.0001 then
    fail("STANDARD_GRAVITY expected 9.80665, got " .. S.STANDARD_GRAVITY)
end
pass(string.format("Light.Sensor constants ok (9 consts, GRAVITY=%.5f)",
                   S.STANDARD_GRAVITY))

-- ==================== 5) GetSensors() returns table ====================

local list = S.GetSensors()
if type(list) ~= "table" then fail("GetSensors() should return table") end
pass(string.format("Light.Sensor.GetSensors ok (%d sensors)", #list))

-- ==================== 6) UpdateSensors() never crashes ====================

local upd = S.UpdateSensors()
if type(upd) ~= "boolean" then fail("UpdateSensors() should return boolean") end
pass("Light.Sensor.UpdateSensors() ok = " .. tostring(upd))

-- ==================== 7) ID-based fns boundary: 非法 id ====================

local INVALID_ID = 0xDEADBEEF

local n1, e1 = S.GetSensorNameForID(INVALID_ID)
if n1 ~= nil or e1 == nil then fail("GetSensorNameForID(garbage) should be nil+err") end
pass("Light.Sensor.GetSensorNameForID(0xDEADBEEF) boundary ok: " .. tostring(e1))

local t1 = S.GetSensorTypeForID(INVALID_ID)
if type(t1) ~= "string" then fail("GetSensorTypeForID should return string") end
pass("Light.Sensor.GetSensorTypeForID(0xDEADBEEF) = '" .. t1 .. "' ok")

local nt, ent = S.GetSensorNonPortableTypeForID(INVALID_ID)
if nt ~= nil and type(nt) ~= "number" then fail("GetSensorNonPortableTypeForID bad return") end
-- 桌面 CI 多半返回 nil+err, 实机可能返回真整数; 不强求
pass("Light.Sensor.GetSensorNonPortableTypeForID(garbage) ok ("
     .. (nt and tostring(nt) or "nil/" .. tostring(ent)) .. ")")

local h1, eh1 = S.OpenSensorByID(INVALID_ID)
if h1 ~= nil or eh1 == nil then fail("OpenSensorByID(garbage) should be nil+err") end
pass("Light.Sensor.OpenSensorByID(0xDEADBEEF) boundary ok: " .. tostring(eh1))

local h2, eh2 = S.GetSensorFromID(INVALID_ID)
if h2 ~= nil or eh2 == nil then fail("GetSensorFromID(garbage) should be nil+err") end
pass("Light.Sensor.GetSensorFromID(0xDEADBEEF) boundary ok: " .. tostring(eh2))

-- ==================== 8) ID-based fns: 非数字 id ====================

for _, fname in ipairs({ "GetSensorNameForID", "GetSensorNonPortableTypeForID",
                        "OpenSensorByID", "GetSensorFromID" }) do
    local rv, re = S[fname]("not_a_number")
    if rv ~= nil or re == nil then fail(fname .. "(string) should be nil+err") end
end
local tnan = S.GetSensorTypeForID("not_a_number")
if tnan ~= "invalid" then fail("GetSensorTypeForID(string) should return 'invalid'") end
pass("Light.Sensor 5 ID-based fns reject non-number arg ok")

-- ==================== 9) handle-based fns boundary: nil handle ====================

local handle_fns = {
    "CloseSensor", "GetSensorName", "GetSensorType", "GetSensorID", "GetSensorData",
}
for _, fname in ipairs(handle_fns) do
    local rv, re = S[fname](nil)
    -- CloseSensor 返回 (false, err); 其他返回 (nil, err)
    if fname == "CloseSensor" then
        if rv ~= false or re == nil then fail(fname .. "(nil) should be false+err") end
    else
        if rv ~= nil or re == nil then fail(fname .. "(nil) should be nil+err") end
    end
end
pass("Light.Sensor 5 handle-based fns reject nil handle ok")

-- ==================== 10) GetSensorData(nil, n) edge cases ====================

-- num_values 越界 / 非法 类型, 句柄 nil 应短路
local d1, ed1 = S.GetSensorData(nil, 100)  -- > 16
if d1 ~= nil or ed1 == nil then fail("GetSensorData(nil, 100) should short-circuit") end
local d2, ed2 = S.GetSensorData(nil, -5)
if d2 ~= nil or ed2 == nil then fail("GetSensorData(nil, -5) should short-circuit") end
local d3, ed3 = S.GetSensorData(nil, "garbage")
if d3 ~= nil or ed3 == nil then fail("GetSensorData(nil, str) should short-circuit") end
pass("Light.Sensor.GetSensorData boundary (nil/oob/string) ok")

-- ==================== 11) Phase G 高级 API 行为不破坏 ====================

-- 桌面 CI 无 sensor 时 OpenAccel 返回 false+err
local ok_a, err_a = S.OpenAccel()
if type(ok_a) ~= "boolean" then fail("OpenAccel should return boolean") end
if not ok_a and type(err_a) ~= "string" then fail("OpenAccel error should be string") end
pass("Light.Sensor.OpenAccel ok = " .. tostring(ok_a) ..
     (err_a and (" (" .. tostring(err_a) .. ")") or ""))

local ok_g, err_g = S.OpenGyro()
if type(ok_g) ~= "boolean" then fail("OpenGyro should return boolean") end
pass("Light.Sensor.OpenGyro ok = " .. tostring(ok_g))

-- GetAccel 在未打开时返回 nil+err
if not ok_a then
    local ax, eax = S.GetAccel()
    if ax ~= nil or eax == nil then fail("GetAccel without open should be nil+err") end
    pass("Light.Sensor.GetAccel(unopened) boundary ok")
end
if not ok_g then
    local gx, egx = S.GetGyro()
    if gx ~= nil or egx == nil then fail("GetGyro without open should be nil+err") end
    pass("Light.Sensor.GetGyro(unopened) boundary ok")
end

-- Close 总是安全
local c1 = S.Close()
if c1 ~= true then fail("Close() should return true") end
local c2 = S.Close()  -- 重复 Close
if c2 ~= true then fail("Close() second call should return true") end
pass("Light.Sensor.Close (idempotent) ok")

-- ==================== 12) 真有传感器时打开/读取 (Optional) ====================

if #list > 0 then
    -- 如果运行环境真有 sensor, 跑端到端验证
    local first_id = list[1]
    local h, eh = S.OpenSensorByID(first_id)
    if h then
        local hname, _ = S.GetSensorName(h)
        local htype, _ = S.GetSensorType(h)
        local hid, _ = S.GetSensorID(h)
        pass(string.format("Light.Sensor live device: id=%s name=%s type=%s rid=%s",
            tostring(first_id), tostring(hname), tostring(htype), tostring(hid)))
        S.CloseSensor(h)
    else
        pass("Light.Sensor.OpenSensorByID(real id) failed: " .. tostring(eh))
    end
else
    pass("Light.Sensor no live sensors on this CI host (expected)")
end

print("sensor smoke ok (Phase G + Phase AL)")
