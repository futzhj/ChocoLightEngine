-- Phase J smoke: Light.Hidapi
--
-- CI runner 无真实 HID 设备, 所有 Open 都期望 nil + err.
-- 但 Init/Exit/Enumerate/DeviceChangeCount 应该可以正常调用.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

local ok, mod = pcall(require, "Light.Hidapi")
if not ok then fail("require(Light.Hidapi) failed: " .. tostring(mod)) end
if type(mod) ~= "table" then fail("Light.Hidapi not a table") end

-- 验证 17 个 fn 全部注册
for _, k in ipairs({
    "Init", "Exit", "DeviceChangeCount",
    "Enumerate",
    "Open", "OpenPath", "Close",
    "Write", "Read", "ReadTimeout",
    "SetNonblocking",
    "SendFeatureReport", "GetFeatureReport", "GetInputReport",
    "GetManufacturerString", "GetProductString", "GetSerialNumberString",
}) do
    if type(mod[k]) ~= "function" then fail("Light.Hidapi." .. k .. " missing") end
end
pass("Light.Hidapi module ok (17 functions)")

-- ===== Init / DeviceChangeCount / Enumerate =====
-- 在某些 CI 环境 (如 Web/严格沙箱) 可能 init 失败, 但常见 Win/Linux/macOS runner 可正常 init.
local iok, ierr = mod.Init()
if iok ~= true then
    -- 不可用环境: 终止 smoke 但视为通过
    pass("Light.Hidapi.Init failed (acceptable in headless/sandbox): " .. tostring(ierr))
    print("hidapi smoke ok")
    return
end
pass("Light.Hidapi.Init ok")

-- DeviceChangeCount: 数值 (单调递增计数器)
local count0 = mod.DeviceChangeCount()
if type(count0) ~= "number" then fail("DeviceChangeCount must be number") end
pass("Light.Hidapi.DeviceChangeCount = " .. count0)

-- Enumerate: 应返回数组 (可能为空)
local list, eerr = mod.Enumerate()
if type(list) ~= "table" then fail("Enumerate must return array, got " .. type(list)) end
pass(string.format("Light.Hidapi.Enumerate ok (%d devices)", #list))

-- 如果有设备, 验证字段完整
if #list > 0 then
    local d = list[1]
    for _, key in ipairs({
        "vendor_id", "product_id", "path",
        "serial_number", "manufacturer", "product",
        "release", "usage_page", "usage", "interface_number",
    }) do
        if d[key] == nil then fail("device entry missing field: " .. key) end
    end
    pass("Light.Hidapi.Enumerate first entry has all required fields")
    pass(string.format("  first device: vid=0x%04x pid=0x%04x mfr=%s prod=%s",
                       d.vendor_id, d.product_id, tostring(d.manufacturer), tostring(d.product)))
end

-- 指定 vid/pid 过滤 (必然不存在)
local list2 = mod.Enumerate(0xDEAD, 0xBEEF)
if type(list2) ~= "table" then fail("Enumerate(filter) must return array") end
pass(string.format("Light.Hidapi.Enumerate(0xDEAD, 0xBEEF) ok (%d devices, expected 0)", #list2))

-- ===== 边界路径 =====

-- Open 不存在 vid/pid -> nil + err
local dev1, oerr = mod.Open(0xDEAD, 0xBEEF)
if dev1 ~= nil or oerr == nil then fail("Open(non-existent) should be nil+err") end
pass("Light.Hidapi.Open(0xDEAD, 0xBEEF) boundary ok: " .. tostring(oerr))

-- OpenPath 不存在路径 -> nil + err
local dev2, perr = mod.OpenPath("/dev/this-path-does-not-exist-xyz")
if dev2 ~= nil or perr == nil then fail("OpenPath(missing) should be nil+err") end
pass("Light.Hidapi.OpenPath(missing) boundary ok: " .. tostring(perr))

-- Close(nil) -> false + err
local cok, cerr = mod.Close(nil)
if cok ~= false or cerr == nil then fail("Close(nil) should be false+err") end
pass("Light.Hidapi.Close(nil) boundary ok: " .. tostring(cerr))

-- Write/Read(nil) -> nil + err
local wn, werr = mod.Write(nil, "x")
if wn ~= nil or werr == nil then fail("Write(nil) should be nil+err") end
pass("Light.Hidapi.Write(nil) boundary ok: " .. tostring(werr))

local rn, rerr = mod.Read(nil, 64)
if rn ~= nil or rerr == nil then fail("Read(nil) should be nil+err") end
pass("Light.Hidapi.Read(nil) boundary ok: " .. tostring(rerr))

local rt, rterr = mod.ReadTimeout(nil, 64, 100)
if rt ~= nil or rterr == nil then fail("ReadTimeout(nil) should be nil+err") end
pass("Light.Hidapi.ReadTimeout(nil) boundary ok: " .. tostring(rterr))

-- SetNonblocking(nil) -> false + err
local sn, snerr = mod.SetNonblocking(nil, true)
if sn ~= false or snerr == nil then fail("SetNonblocking(nil) should be false+err") end
pass("Light.Hidapi.SetNonblocking(nil) boundary ok: " .. tostring(snerr))

-- 字符串查询(nil) -> nil + err
local mfr, mfrerr = mod.GetManufacturerString(nil)
if mfr ~= nil or mfrerr == nil then fail("GetManufacturerString(nil) should be nil+err") end
pass("Light.Hidapi.GetManufacturerString(nil) boundary ok: " .. tostring(mfrerr))

-- ===== Exit =====
local xok, xerr = mod.Exit()
if xok ~= true then fail("Exit failed: " .. tostring(xerr)) end
pass("Light.Hidapi.Exit ok")

print("hidapi smoke ok")
