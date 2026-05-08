-- ChocoLight Sample: Light.Locale + Light.Power + Light.Sensor
--
-- 平台/系统状态查询. 移动 + 桌面均可,Web 可能部分降级.

local Locale = require 'Light.Locale'
local Power  = require 'Light.Power'
local Sensor = require 'Light.Sensor'

print("==== Light.Locale ====")
local langs, err = Locale.GetPreferred()
if langs then
    print(string.format("system languages (%d):", #langs))
    for i, l in ipairs(langs) do
        print(string.format("  [%d] %s_%s", i, l.language, l.country or ""))
    end
else
    print("Locale.GetPreferred err:", err)
end

print("\n==== Light.Power ====")
local p, perr = Power.GetState()
if p then
    print(string.format("state=%s percent=%d seconds=%d",
                        tostring(p.state), p.percent or -1, p.seconds or -1))
else
    print("Power.GetState err:", perr)
end

print("\n==== Light.Sensor ====")
local sensors = Sensor.GetSensors() or {}
print(string.format("found %d sensor(s)", #sensors))
for i, s in ipairs(sensors) do
    print(string.format("  [%d] id=%d name=%s type=%s",
                        i, s.id, s.name, s.type))
end

if #sensors > 0 then
    -- 演示: 打开第一个传感器, 抓取一次数据
    local dev = Sensor.Open(sensors[1].id)
    if dev then
        local data = Sensor.GetData(dev) or {}
        print("first reading: " ..
              table.concat({ string.format("%.3f", data[1] or 0),
                             string.format("%.3f", data[2] or 0),
                             string.format("%.3f", data[3] or 0) }, ", "))
        Sensor.Close(dev)
    end
else
    print("(no sensors on this platform - typical for desktop CI)")
end

print("\ndemo_locale_power_sensor ok")
