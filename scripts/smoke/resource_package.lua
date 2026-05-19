local Package = require("Light.Plugins.Package")

local function fail(msg)
    error("[resource_package] " .. tostring(msg), 2)
end

local function assert_eq(actual, expected, label)
    if actual ~= expected then
        fail(label .. ": expected " .. tostring(expected) .. ", got " .. tostring(actual))
    end
end

local function assert_true(value, label)
    if not value then
        fail(label .. ": expected truthy value")
    end
end

local function write_file(path, data)
    local f, err = io.open(path, "wb")
    if not f then
        fail("open write failed: " .. tostring(err))
    end
    f:write(data)
    f:close()
end

local function le16(n)
    return string.char(n % 256, math.floor(n / 256) % 256)
end

local function le32(n)
    local b1 = n % 256
    local b2 = math.floor(n / 256) % 256
    local b3 = math.floor(n / 65536) % 256
    local b4 = math.floor(n / 16777216) % 256
    return string.char(b1, b2, b3, b4)
end

local function bxor(a, b)
    local bitlib = bit32 or bit
    if bitlib and bitlib.bxor then
        return bitlib.bxor(a, b)
    end

    local res, p = 0, 1
    while a > 0 or b > 0 do
        local aa = a % 2
        local bb = b % 2
        if aa ~= bb then
            res = res + p
        end
        a = math.floor(a / 2)
        b = math.floor(b / 2)
        p = p * 2
    end
    return res
end

local sep = package.config:sub(1, 1)
local tmp = os.getenv("TEMP") or os.getenv("TMP") or "."
local base = tmp .. sep .. "chocolight_resource_package_smoke"
if sep == "\\" then
    os.execute('mkdir "' .. base .. '" >NUL 2>NUL')
else
    os.execute('mkdir -p "' .. base .. '" >/dev/null 2>/dev/null')
end

-- Synthetic PFDW: header(12) + payload + index(16)
local wdf_path = base .. sep .. "synthetic.wdf"
local wdf_payload = "hello-wdf"
local wdf_hash = 0x12345678
local wdf_offset = 12
local wdf_index_offset = wdf_offset + #wdf_payload
local wdf_data = "PFDW" .. le32(1) .. le32(wdf_index_offset)
    .. wdf_payload
    .. le32(wdf_hash) .. le32(wdf_offset) .. le32(#wdf_payload) .. le32(#wdf_payload)
write_file(wdf_path, wdf_data)

local probe = assert(Package.Probe(wdf_path))
assert_eq(probe.kind, "WDF", "PFDW probe kind")
assert_eq(probe.subtype, "PFDW", "PFDW probe subtype")

local pkg = assert(Package.Open(wdf_path))
local info = pkg:GetInfo()
assert_eq(info.kind, "WDF", "WDF info kind")
assert_eq(info.count, 1, "WDF info count")
local list = assert(pkg:List())
assert_eq(#list, 1, "WDF list count")
assert_eq(list[1].key, wdf_hash, "WDF list key")
assert_true(pkg:Has(wdf_hash), "WDF Has hash")
assert_eq(assert(pkg:GetData(wdf_hash)), wdf_payload, "WDF data")
assert_eq(assert(pkg:Close()), true, "WDF close")

-- Synthetic WPK: IDX + archive file
local idx_path = base .. sep .. "addon.idx"
local wpk_path = base .. sep .. "addon0.wpk"
local wpk_payload = "hello-wpk"
local md5 = string.rep(string.char(1), 16)
local idx_data = "SKPW" .. string.rep(string.char(0), 8) .. le32(1) .. string.rep(string.char(0), 16)
    .. md5 .. le32(#wpk_payload) .. le32(0) .. le16(0) .. le16(0)
write_file(idx_path, idx_data)
write_file(wpk_path, wpk_payload)

local wpk = assert(Package.Open(idx_path))
local wpk_info = wpk:GetInfo()
assert_eq(wpk_info.kind, "WPK", "WPK info kind")
assert_eq(wpk_info.count, 1, "WPK info count")
local wpk_list = assert(wpk:List())
assert_eq(#wpk_list, 1, "WPK list count")
assert_eq(assert(wpk:GetData(wpk_list[1].key)), wpk_payload, "WPK data")
wpk:Close()

-- Synthetic FLS: encrypted index + raw payload. The encrypted index reverses the parser formula.
local fls_path = base .. sep .. "synthetic.fls"
local fls_payload = "PNAM-fls"
local fls_hash = 0x01020304
local fls_flag_plain = 0
local fls_size_plain = #fls_payload
local fls_offset_plain = 12 + 16
local enc_hash = fls_hash
local enc_flag = bxor(fls_flag_plain, fls_hash)
local enc_size = bxor(fls_size_plain, ((fls_hash * enc_flag + 914014) % 4294967296))
local enc_offset_key = (16193790 * (0 * enc_size) + 223990124) % 4294967296
local enc_offset = bxor(fls_offset_plain, enc_offset_key)
local fls_data = "0SLF" .. le32(1) .. le32(12)
    .. le32(enc_hash) .. le32(enc_flag) .. le32(enc_size) .. le32(enc_offset)
    .. fls_payload
write_file(fls_path, fls_data)

local fls = assert(Package.Open(fls_path))
local fls_info = fls:GetInfo()
assert_eq(fls_info.kind, "FLS", "FLS info kind")
assert_eq(fls_info.count, 1, "FLS info count")
local fls_list = assert(fls:List())
assert_eq(#fls_list, 1, "FLS list count")
assert_eq(assert(fls:GetData(fls_hash, { raw = true })), fls_payload, "FLS raw data")
fls:Close()

local assets = os.getenv("LIGHT_TEST_ASSETS") or "assets"
local f = io.open(assets .. sep .. "wzife.wdf", "rb")
if f then
    f:close()
    local real_wdf = assert(Package.Open(assets .. sep .. "wzife.wdf"))
    assert_true(real_wdf:GetInfo().count > 0, "real WDF count")
    real_wdf:Close()
else
    print("[resource_package] skip real assets: " .. assets)
end

print("resource_package smoke ok")
