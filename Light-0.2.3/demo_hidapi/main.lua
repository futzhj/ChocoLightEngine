-- ChocoLight Sample: Light.Hidapi (Phase J)
--
-- 演示原始 HID 设备发现 + 字符串属性查询.
-- 真正的读写 (Write / Read / FeatureReport) 需要具体设备协议, 这里只列出.

local HID = require 'Light.Hidapi'

print("==== Light.Hidapi ====")

local ok, err = HID.Init()
if not ok then
    print("HID.Init failed (typical on Web/headless sandbox): " .. tostring(err))
    print("\ndemo_hidapi ok (no HID subsystem)")
    return
end
print("HID.Init ok")

print("DeviceChangeCount = " .. HID.DeviceChangeCount())

-- 列举所有 HID 设备
local list = HID.Enumerate() or {}
print(string.format("\nfound %d HID device(s):\n", #list))

for i, d in ipairs(list) do
    print(string.format("  [%d] vid=0x%04X pid=0x%04X usage_page=0x%04X usage=0x%04X",
                        i, d.vendor_id, d.product_id, d.usage_page, d.usage))
    print(string.format("       mfr=%s", tostring(d.manufacturer)))
    print(string.format("       prod=%s", tostring(d.product)))
    print(string.format("       serial=%s", tostring(d.serial_number)))
    print(string.format("       path=%s", tostring(d.path):sub(1, 80)))
    print()
end

-- 演示 Open + 字符串查询 (尝试打开第一个设备, 多数情况下需要管理员权限或会失败)
if #list > 0 then
    local d = list[1]
    local dev, oerr = HID.OpenPath(d.path)
    if dev then
        print(string.format("\nopened first device. mfr=%s prod=%s serial=%s",
                            tostring(HID.GetManufacturerString(dev)),
                            tostring(HID.GetProductString(dev)),
                            tostring(HID.GetSerialNumberString(dev))))

        -- 设置非阻塞 (一般用法)
        HID.SetNonblocking(dev, true)

        -- 不实际 Read/Write, 因为协议设备依赖
        HID.Close(dev)
    else
        print("\nOpen first device failed (typical without privilege): " .. tostring(oerr))
    end
end

-- 也可按 vid/pid 过滤 (举例: 苹果鼠标 vid=0x05AC; 不会匹配, 仅示范 API)
local apple_devs = HID.Enumerate(0x05AC, 0x0000) or {}
print(string.format("\nApple-vendor devices (vid=0x05AC): %d", #apple_devs))

HID.Exit()
print("\ndemo_hidapi ok")
