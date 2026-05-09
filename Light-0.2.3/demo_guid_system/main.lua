-- ChocoLight Sample: Light.Guid + Light.System
--
-- 跨平台 GUID 生成 + 系统/CPU 信息查询.

local Guid = require 'Light.Guid'
local Sys  = require 'Light.System'

print("==== Light.Guid ====")

-- 生成 5 个 GUID
local guids = {}
for i = 1, 5 do guids[i] = Guid.New() end

for i, g in ipairs(guids) do
    print(string.format("  [%d] %s  valid=%s", i, g, tostring(Guid.IsValid(g))))
end

-- 二进制 / 字符串互转
local g = Guid.New()
local bin = Guid.ToBinary(g)
print(string.format("\n字符串 GUID: %s (%d 字节)", g, #g))
print(string.format("二进制 GUID: 16 字节 -> hex %s",
                    bin:gsub(".", function(c) return string.format("%02X", c:byte()) end)))

local g2 = Guid.FromBinary(bin)
print("roundtrip equal: " .. tostring(Guid.Equal(g, g2)))

-- 边界: 非法字符串
print("Guid.IsValid('not a guid') = " .. tostring(Guid.IsValid("not a guid")))

print("\n==== Light.System ====")

print("Platform:           " .. (Sys.GetPlatform() or "unknown"))
print("CPU count:          " .. (Sys.GetCPUCount() or 0))
print("RAM (MB):           " .. (Sys.GetRAMMB() or 0))
print("Cache line size:    " .. (Sys.GetCacheLineSize() or 0) .. " bytes")
print("Has AVX:            " .. tostring(Sys.HasAVX()))
print("Has SSE2:           " .. tostring(Sys.HasSSE2()))
print("Has NEON:           " .. tostring(Sys.HasNEON()))
print("Has AltiVec:        " .. tostring(Sys.HasAltiVec()))

-- 高精度计时演示
print(string.format("\nTickMS:             %d", Sys.GetTickMS() or 0))
local pf = Sys.GetPerformanceFrequency() or 1
local pc1 = Sys.GetPerformanceCounter() or 0
-- busy loop ~ 1ms
local target = pc1 + pf / 1000
while (Sys.GetPerformanceCounter() or 0) < target do end
local pc2 = Sys.GetPerformanceCounter() or 0
print(string.format("PerfFreq:           %d ticks/sec", pf))
print(string.format("Elapsed (busy 1ms): %.3f ms", (pc2 - pc1) * 1000.0 / pf))

print("\ndemo_guid_system ok")
