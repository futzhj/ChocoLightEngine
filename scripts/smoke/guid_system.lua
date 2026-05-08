-- Phase H smoke: Light.Guid + Light.System
-- 全部为只读/纯函数路径, 任何 CI 环境均可跑.

local function pass(msg) print(msg) end
local function fail(msg) print("FAIL: " .. msg); os.exit(1) end

-- ==================== Light.Guid ====================
do
    local ok, mod = pcall(require, "Light.Guid")
    if not ok then fail("require(Light.Guid) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.Guid not a table") end
    for _, k in ipairs({ "ToString", "FromString" }) do
        if type(mod[k]) ~= "function" then fail("Light.Guid." .. k .. " missing") end
    end
    pass("Light.Guid module ok")

    -- 长度边界: 输入非 16 字节
    local s, err = mod.ToString("short")
    if s ~= nil or err == nil then fail("ToString(<16) should be nil + err") end
    pass("Light.Guid.ToString length boundary ok: " .. tostring(err))

    -- Lua 5.1 不支持 \xNN 转义, 必须用 string.char(0) 或 \0 生成真 NUL 字节
    local raw32 = string.rep(string.char(0), 32)
    local s2, err2 = mod.ToString(raw32)
    if s2 ~= nil or err2 == nil then fail("ToString(>16) should be nil + err") end
    pass("Light.Guid.ToString >16 boundary ok: " .. tostring(err2))

    -- 全零 GUID 互转
    local raw_zero = string.rep(string.char(0), 16)
    local hex_zero = mod.ToString(raw_zero)
    if type(hex_zero) ~= "string" or #hex_zero ~= 32 then
        fail("ToString(zero) should return 32-char string, got " .. tostring(hex_zero))
    end
    if hex_zero ~= "00000000000000000000000000000000" then
        fail("ToString(zero) should be 32 zeros, got: " .. hex_zero)
    end
    pass("Light.Guid.ToString zero -> " .. hex_zero)

    -- 往返: hex -> raw -> hex (大小写无关)
    local hex_in = "0123456789abcdef0123456789ABCDEF"
    local raw    = mod.FromString(hex_in)
    if type(raw) ~= "string" or #raw ~= 16 then
        fail("FromString output must be 16 bytes, got " .. tostring(#raw))
    end
    local hex_out = mod.ToString(raw)
    -- SDL3 输出小写, 比较时需统一
    if string.lower(hex_out) ~= string.lower(hex_in) then
        fail("roundtrip mismatch: in=" .. hex_in .. " out=" .. hex_out)
    end
    pass("Light.Guid.{To,From}String roundtrip ok: " .. hex_out)
end

-- ==================== Light.System ====================
do
    local ok, mod = pcall(require, "Light.System")
    if not ok then fail("require(Light.System) failed: " .. tostring(mod)) end
    if type(mod) ~= "table" then fail("Light.System not a table") end
    for _, k in ipairs({
        "GetPlatform", "GetSDLVersion", "GetSDLRevision",
        "GetSystemRAM", "GetLogicalCPUCores", "GetCPUCacheLineSize",
        "GetSIMDAlignment", "GetCPUFeatures",
    }) do
        if type(mod[k]) ~= "function" then fail("Light.System." .. k .. " missing") end
    end
    pass("Light.System module ok (8 functions)")

    -- Platform: 必须是非空字符串
    local p = mod.GetPlatform()
    if type(p) ~= "string" or #p == 0 then fail("GetPlatform must be non-empty string") end
    pass("Light.System.GetPlatform = " .. p)

    -- SDL 版本: {major, minor, patch} 都是 >=0 整数, major 必须 >= 3
    local v = mod.GetSDLVersion()
    if type(v) ~= "table" then fail("GetSDLVersion must return table") end
    if type(v.major) ~= "number" or v.major < 3 then
        fail("major must be >=3, got " .. tostring(v.major))
    end
    if type(v.minor) ~= "number" or v.minor < 0 then fail("minor invalid") end
    if type(v.patch) ~= "number" or v.patch < 0 then fail("patch invalid") end
    pass(string.format("Light.System.GetSDLVersion = %d.%d.%d", v.major, v.minor, v.patch))

    -- Revision: 字符串
    local r = mod.GetSDLRevision()
    if type(r) ~= "string" then fail("GetSDLRevision must be string") end
    pass("Light.System.GetSDLRevision len=" .. #r)

    -- RAM / Cores: 数值
    local ram = mod.GetSystemRAM()
    if type(ram) ~= "number" then fail("GetSystemRAM must be number") end
    pass("Light.System.GetSystemRAM = " .. ram .. " MB")

    local cores = mod.GetLogicalCPUCores()
    if type(cores) ~= "number" or cores < 1 then
        fail("GetLogicalCPUCores must be >=1, got " .. tostring(cores))
    end
    pass("Light.System.GetLogicalCPUCores = " .. cores)

    -- Cache line / SIMD alignment: 通常是 32 / 64 / 128 等 2 次幂
    local cls = mod.GetCPUCacheLineSize()
    if type(cls) ~= "number" or cls < 1 then fail("CacheLineSize invalid") end
    pass("Light.System.GetCPUCacheLineSize = " .. cls)

    local simd = mod.GetSIMDAlignment()
    if type(simd) ~= "number" or simd < 1 then fail("SIMDAlignment invalid") end
    pass("Light.System.GetSIMDAlignment = " .. simd)

    -- CPU features: 必须包含全部 14 个键且都是 boolean
    local f = mod.GetCPUFeatures()
    if type(f) ~= "table" then fail("GetCPUFeatures must return table") end
    for _, key in ipairs({
        "mmx", "sse", "sse2", "sse3", "sse41", "sse42",
        "avx", "avx2", "avx512f",
        "neon", "altivec", "arm_simd",
        "lsx", "lasx",
    }) do
        if type(f[key]) ~= "boolean" then
            fail("CPUFeatures." .. key .. " must be boolean, got " .. type(f[key]))
        end
    end
    -- 简短报告几个常见特性
    pass(string.format("Light.System.GetCPUFeatures sse2=%s avx=%s avx2=%s neon=%s",
                       tostring(f.sse2), tostring(f.avx), tostring(f.avx2), tostring(f.neon)))
end

print("guid_system smoke ok")
