# Phase G.3 Resource Package MVP Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first Phase G.3 slice: `Light.Plugins.Package` for WDF/PFDW, WPK/IDX, and FLS container probing, listing, and byte extraction.

**Architecture:** Add a small C++ resource package core with three focused parsers (`WdfPackage`, `WpkPackage`, `FlsPackage`) behind one Lua handle module. Keep TCP/IGS/MAP out of this slice, but make returned bytes suitable for later decoders.

**Tech Stack:** ChocoLight C++17-style Lua binding, Lua 5.1, existing `light_lua_helpers.h`, standard `FILE*` I/O, existing module table in `lumen-master/src/light/light.cpp`, smoke Lua scripts.

---

## Mandatory Project Constraints

- 用中文注释关键复杂逻辑，尤其是二进制结构、offset/size 校验、FLS 解密公式。
- 不提交 `assets/` 大资源目录。
- 新模块禁止使用 `luaL_register(L, "Light.Plugins.Package", funcs)`。
- 运行时失败统一返回 `nil, err`。
- 参数类型错误使用 `luaL_check*` / `LT::CheckStringStrict` 抛 Lua 参数错误。
- 本计划只实现 Package MVP：WDF/PFDW、WPK/IDX、FLS；不实现 TCP/IGS/MAP 解码。
- NXPK/MHWD 只返回 unsupported，不实现读取。
- `SFDW/WDFS/WDFX/WDFH` 在本 slice 中只 Probe 出 subtype 并返回 `supported=false` 或 unsupported error，不声称已完整支持。
- 完整本地 CMake build / runtime smoke 需要用户批准；可先运行 `git diff --check` 和 Lua 语法检查。

---

## File Structure

### New files

- `ChocoLight/src/resource_package.h`
  - 公共数据结构、parser 接口、文件读取 helper、little-endian 读取、模块注册 helper。
- `ChocoLight/src/resource_package.cpp`
  - `ProbePackageFile`、`OpenPackageFile`、共享边界检查、文件长度获取。
- `ChocoLight/src/resource_wdf.cpp`
  - PFDW/WDFP parser；NXPK/MHWD unsupported；WDFX/WDFH/SFDW/WDFS probe-only。
- `ChocoLight/src/resource_wpk.cpp`
  - `SKPW` IDX parser；WPK 分卷定位；raw bytes 读取。
- `ChocoLight/src/resource_fls.cpp`
  - `0SLF` parser；索引解密；raw/decrypted 自动选择。
- `ChocoLight/src/resource_crypto.cpp`
  - FLS data XOR；AC/XC magic probe stub；本 slice 不做完整 AC/XC 解密。
- `ChocoLight/src/light_plugins_package.cpp`
  - `Light.Plugins.Package` Lua binding。
- `scripts/smoke/resource_package.lua`
  - Synthetic 文件测试 + 可选 assets 测试。

### Modified files

- `ChocoLight/include/light.h`
  - 声明 `luaopen_Light_Plugins_Package`。
- `lumen-master/src/light/light.cpp`
  - 注册 `Light.Plugins.Package`。
- `ChocoLight/CMakeLists.txt`
  - 加入新增 `.cpp`。
- `.github/workflows/build-templates.yml`
  - 注册 smoke 脚本。
- `docs/api/Light_Plugins.md`
  - 增加 `Light.Plugins.Package` 文档。
- `docs/Phase G.3 Resource Format Plugins/ACCEPTANCE_PhaseG_3.md`
  - 回填本 slice 实现和验证结果。

---

## Public Lua API for This Slice

```lua
local Package = require("Light.Plugins.Package")

local info, err = Package.Probe("assets/wzife.wdf")
local pkg, err = Package.Open("assets/wzife.wdf")
local info = pkg:GetInfo()
local entries = pkg:List()
local ok = pkg:Has(entries[1].key)
local data, err = pkg:GetData(entries[1].key)
local raw, err = pkg:GetData(entries[1].key, { raw = true })
pkg:Close()
```

Handle methods:

- `GetInfo() -> table`
- `List() -> table | nil, err`
- `Has(key) -> boolean`
- `GetData(key [, options]) -> string | nil, err`
- `Close() -> true`
- `__tostring() -> string`
- `__gc` frees the C++ package instance

---

## Task 1: Write Package smoke script first

**Files:**
- Create: `scripts/smoke/resource_package.lua`

- [ ] **Step 1: Create smoke script with surface and synthetic tests**

Create `scripts/smoke/resource_package.lua`:

```lua
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

local function hex32(n)
    return string.format("%08x", n)
end

local sep = package.config:sub(1, 1)
local tmp = os.getenv("TEMP") or os.getenv("TMP") or "."
local base = tmp .. sep .. "chocolight_resource_package_smoke"
os.execute('mkdir "' .. base .. '" >NUL 2>NUL')

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

-- Synthetic FLS: encrypted index + raw payload. The encrypted index is generated by reversing the parser formula.
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

-- Optional real assets. Missing assets should skip, not fail.
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
```

- [ ] **Step 2: Run syntax check**

Run:

```powershell
.\tools\lightc.exe -p scripts\smoke\resource_package.lua
```

Expected if `lightc.exe` exists: syntax check passes. If the tool is absent, record that syntax check was skipped because the binary was not present.

- [ ] **Step 3: Record expected initial failure**

Do not run runtime smoke yet unless a local `light.exe` runtime is available and the user approves. The expected runtime failure before C++ implementation is:

```text
module 'Light.Plugins.Package' not found
```

- [ ] **Step 4: Commit smoke script**

```powershell
git add -- scripts/smoke/resource_package.lua
git commit -m "test: add resource package smoke coverage"
```

---

## Task 2: Add shared package core types

**Files:**
- Create: `ChocoLight/src/resource_package.h`
- Create: `ChocoLight/src/resource_package.cpp`

- [ ] **Step 1: Create public internal header**

Create `ChocoLight/src/resource_package.h`:

```cpp
#pragma once

#include "light.h"
#include "light_lua_helpers.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace LT::Resource {

constexpr uint32_t LT_MAGIC_PACKAGE = LT::Magic4('P','K','G','C');

struct PackageInfo {
    std::string kind;
    std::string subtype;
    std::string path;
    uint32_t count = 0;
    uint64_t indexOffset = 0;
    bool supported = false;
};

struct ResourceEntry {
    std::string keyText;
    uint32_t id = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t packedSize = 0;
    uint32_t archive = 0;
    std::string md5;
};

struct ReadOptions {
    bool raw = false;
    bool decode = true;
    uint64_t maxBytes = 0;
};

class IResourcePackage {
public:
    virtual ~IResourcePackage() = default;
    virtual const PackageInfo& Info() const = 0;
    virtual const std::vector<ResourceEntry>& Entries() const = 0;
    virtual bool Has(const std::string& key) const = 0;
    virtual bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) = 0;
};

struct PackageHandle {
    uint32_t magic = LT_MAGIC_PACKAGE;
    std::unique_ptr<IResourcePackage> package;
};

bool ReadFileHead(const std::string& path, size_t bytes, std::string& out, std::string& err);
bool GetFileSize(FILE* fp, uint64_t& outSize);
bool ReadAt(FILE* fp, uint64_t offset, uint64_t size, std::string& out, std::string& err);
bool RangeInFile(uint64_t fileSize, uint64_t offset, uint64_t size);
uint16_t ReadLE16(const uint8_t* p);
uint32_t ReadLE32(const uint8_t* p);
std::string HexLower(const uint8_t* data, size_t len);

bool ProbePackageFile(const std::string& path, PackageInfo& info, std::string& err);
std::unique_ptr<IResourcePackage> OpenPackageFile(const std::string& path, std::string& err);

std::unique_ptr<IResourcePackage> OpenWdfPackage(const std::string& path, std::string& err);
std::unique_ptr<IResourcePackage> OpenWpkPackage(const std::string& path, std::string& err);
std::unique_ptr<IResourcePackage> OpenFlsPackage(const std::string& path, std::string& err);

void RegisterPluginsSubmodule(lua_State* L, const char* name, const luaL_Reg* funcs);

} // namespace LT::Resource
```

- [ ] **Step 2: Implement shared helpers**

Create `ChocoLight/src/resource_package.cpp`:

```cpp
#include "resource_package.h"

#include <cerrno>
#include <cstring>

namespace LT::Resource {

bool ReadFileHead(const std::string& path, size_t bytes, std::string& out, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        err = std::string("open failed: ") + path;
        return false;
    }
    out.assign(bytes, '\0');
    size_t n = std::fread(out.data(), 1, bytes, fp);
    std::fclose(fp);
    out.resize(n);
    if (n == 0) {
        err = "empty file";
        return false;
    }
    return true;
}

bool GetFileSize(FILE* fp, uint64_t& outSize) {
    long cur = std::ftell(fp);
    if (cur < 0) return false;
    if (std::fseek(fp, 0, SEEK_END) != 0) return false;
    long end = std::ftell(fp);
    if (end < 0) return false;
    if (std::fseek(fp, cur, SEEK_SET) != 0) return false;
    outSize = static_cast<uint64_t>(end);
    return true;
}

bool RangeInFile(uint64_t fileSize, uint64_t offset, uint64_t size) {
    if (offset > fileSize) return false;
    if (size > fileSize - offset) return false;
    return true;
}

bool ReadAt(FILE* fp, uint64_t offset, uint64_t size, std::string& out, std::string& err) {
    uint64_t fileSize = 0;
    if (!GetFileSize(fp, fileSize)) {
        err = "failed to determine file size";
        return false;
    }
    if (!RangeInFile(fileSize, offset, size)) {
        err = "read range out of bounds";
        return false;
    }
    if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
        err = "seek failed";
        return false;
    }
    out.assign(static_cast<size_t>(size), '\0');
    size_t n = std::fread(out.data(), 1, static_cast<size_t>(size), fp);
    if (n != static_cast<size_t>(size)) {
        err = "read failed";
        return false;
    }
    return true;
}

uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

std::string HexLower(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    return out;
}

bool ProbePackageFile(const std::string& path, PackageInfo& info, std::string& err) {
    std::string head;
    if (!ReadFileHead(path, 32, head, err)) return false;
    if (head.size() < 4) {
        err = "file too small";
        return false;
    }

    const std::string magic = head.substr(0, 4);
    info.path = path;
    info.subtype = magic;

    if (magic == "PFDW" || magic == "WDFP") {
        info.kind = "WDF";
        info.supported = true;
        return true;
    }
    if (magic == "WDFX" || magic == "WDFH" || magic == "SFDW" || magic == "WDFS") {
        info.kind = "WDF";
        info.supported = false;
        return true;
    }
    if (magic == "NXPK" || magic == "MHWD") {
        info.kind = "WDF";
        info.supported = false;
        err = std::string("unsupported WDF subtype: ") + magic;
        return false;
    }
    if (magic == "SKPW") {
        info.kind = "WPK";
        info.supported = true;
        return true;
    }
    if (magic == "0SLF") {
        info.kind = "FLS";
        info.supported = true;
        return true;
    }

    err = "unknown package format";
    return false;
}

std::unique_ptr<IResourcePackage> OpenPackageFile(const std::string& path, std::string& err) {
    PackageInfo info;
    if (!ProbePackageFile(path, info, err)) return nullptr;
    if (info.kind == "WDF") return OpenWdfPackage(path, err);
    if (info.kind == "WPK") return OpenWpkPackage(path, err);
    if (info.kind == "FLS") return OpenFlsPackage(path, err);
    err = "unknown package format";
    return nullptr;
}

void RegisterPluginsSubmodule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Plugins");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Plugins");
        lua_newtable(L);
        lua_rawset(L, -3);
        lua_pushstring(L, "Plugins");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, name);
        lua_newtable(L);
        luaL_setfuncs(L, funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
    } else {
        luaL_setfuncs(L, funcs, 0);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
}

} // namespace LT::Resource
```

- [ ] **Step 3: Run static check**

```powershell
git diff --check -- ChocoLight\src\resource_package.h ChocoLight\src\resource_package.cpp
```

Expected: no output and exit code `0`.

- [ ] **Step 4: Commit**

```powershell
git add -- ChocoLight\src\resource_package.h ChocoLight\src\resource_package.cpp
git commit -m "feat: add resource package core helpers"
```

---

## Task 3: Implement PFDW/WDFP package parser

**Files:**
- Create: `ChocoLight/src/resource_wdf.cpp`
- Modify: `scripts/smoke/resource_package.lua`

- [ ] **Step 1: Implement parser**

Create `ChocoLight/src/resource_wdf.cpp` with these behaviours:

```cpp
#include "resource_package.h"

#include <algorithm>
#include <unordered_map>

namespace LT::Resource {
namespace {

struct WdfEntryRecord {
    uint32_t hash = 0;
    uint32_t offset = 0;
    uint32_t size = 0;
    uint32_t packedSize = 0;
};

class WdfPackage final : public IResourcePackage {
public:
    WdfPackage(FILE* fp, std::string path) : fp_(fp), path_(std::move(path)) {}
    ~WdfPackage() override { if (fp_) std::fclose(fp_); }

    bool Init(std::string& err) {
        uint64_t fileSize = 0;
        if (!GetFileSize(fp_, fileSize)) { err = "failed to determine file size"; return false; }

        std::string header;
        if (!ReadAt(fp_, 0, 12, header, err)) return false;
        const std::string magic = header.substr(0, 4);
        if (magic == "NXPK" || magic == "MHWD") {
            err = std::string("unsupported WDF subtype: ") + magic;
            return false;
        }
        if (!(magic == "PFDW" || magic == "WDFP")) {
            err = std::string("unsupported WDF subtype: ") + magic;
            return false;
        }

        const uint8_t* h = reinterpret_cast<const uint8_t*>(header.data());
        uint32_t count = ReadLE32(h + 4);
        uint32_t indexOffset = ReadLE32(h + 8);
        uint64_t indexBytes = static_cast<uint64_t>(count) * 16u;
        if (!RangeInFile(fileSize, indexOffset, indexBytes)) {
            err = "invalid WDF index range";
            return false;
        }

        std::string index;
        if (!ReadAt(fp_, indexOffset, indexBytes, index, err)) return false;

        info_.kind = "WDF";
        info_.subtype = magic;
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = indexOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(index.data() + i * 16);
            ResourceEntry e;
            e.id = ReadLE32(p + 0);
            e.offset = ReadLE32(p + 4);
            e.size = ReadLE32(p + 8);
            e.packedSize = ReadLE32(p + 12);
            e.keyText = std::to_string(e.id);
            if (!RangeInFile(fileSize, e.offset, e.size)) {
                err = "invalid WDF entry range";
                return false;
            }
            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }

    bool Has(const std::string& key) const override {
        return keyToIndex_.find(key) != keyToIndex_.end();
    }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(key);
        if (it == keyToIndex_.end()) { err = "entry not found"; return false; }
        const ResourceEntry& e = entries_[it->second];
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) { err = "entry exceeds maxBytes"; return false; }
        return ReadAt(fp_, e.offset, e.size, out, err);
    }

private:
    FILE* fp_ = nullptr;
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenWdfPackage(const std::string& path, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = std::string("open failed: ") + path; return nullptr; }
    auto pkg = std::make_unique<WdfPackage>(fp, path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
```

- [ ] **Step 2: Run static checks**

```powershell
git diff --check -- ChocoLight\src\resource_wdf.cpp scripts\smoke\resource_package.lua
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Commit**

```powershell
git add -- ChocoLight\src\resource_wdf.cpp scripts\smoke\resource_package.lua
git commit -m "feat: add PFDW package parser"
```

---

## Task 4: Implement WPK/IDX package parser

**Files:**
- Create: `ChocoLight/src/resource_wpk.cpp`

- [ ] **Step 1: Implement parser**

Create `ChocoLight/src/resource_wpk.cpp` with these behaviours:

```cpp
#include "resource_package.h"

#include <unordered_map>

namespace LT::Resource {
namespace {

std::string Dirname(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string Stem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

class WpkPackage final : public IResourcePackage {
public:
    explicit WpkPackage(std::string path) : path_(std::move(path)) {}

    bool Init(std::string& err) {
        FILE* fp = std::fopen(path_.c_str(), "rb");
        if (!fp) { err = std::string("open failed: ") + path_; return false; }
        struct Close { FILE* f; ~Close(){ if (f) std::fclose(f); } } close{fp};

        uint64_t fileSize = 0;
        if (!GetFileSize(fp, fileSize)) { err = "failed to determine IDX size"; return false; }
        if (fileSize < 32) { err = "IDX too small"; return false; }

        std::string header;
        if (!ReadAt(fp, 0, 32, header, err)) return false;
        if (header.substr(0, 4) != "SKPW") { err = "invalid IDX magic"; return false; }
        const uint8_t* h = reinterpret_cast<const uint8_t*>(header.data());
        uint32_t count = ReadLE32(h + 12);
        uint64_t recordsOffset = 32;
        uint64_t recordsBytes = static_cast<uint64_t>(count) * 28u;
        if (!RangeInFile(fileSize, recordsOffset, recordsBytes)) { err = "invalid IDX record range"; return false; }

        std::string records;
        if (!ReadAt(fp, recordsOffset, recordsBytes, records, err)) return false;

        info_.kind = "WPK";
        info_.subtype = "SKPW";
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = recordsOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(records.data() + i * 28);
            ResourceEntry e;
            e.md5 = HexLower(p, 16);
            e.size = ReadLE32(p + 16);
            e.packedSize = e.size;
            e.offset = ReadLE32(p + 20);
            e.archive = ReadLE16(p + 24);
            e.keyText = e.md5;
            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }
    bool Has(const std::string& key) const override { return keyToIndex_.find(key) != keyToIndex_.end(); }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(key);
        if (it == keyToIndex_.end()) { err = "entry not found"; return false; }
        const ResourceEntry& e = entries_[it->second];
        if (e.archive == 0xFF) { err = "external WPK entry is not stored in archive"; return false; }
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) { err = "entry exceeds maxBytes"; return false; }

        std::string archivePath = Dirname(path_) + "/" + Stem(path_) + std::to_string(e.archive) + ".wpk";
        FILE* fp = std::fopen(archivePath.c_str(), "rb");
        if (!fp) { err = std::string("open WPK archive failed: ") + archivePath; return false; }
        struct Close { FILE* f; ~Close(){ if (f) std::fclose(f); } } close{fp};
        return ReadAt(fp, e.offset, e.size, out, err);
    }

private:
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenWpkPackage(const std::string& path, std::string& err) {
    auto pkg = std::make_unique<WpkPackage>(path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
```

- [ ] **Step 2: Run static check**

```powershell
git diff --check -- ChocoLight\src\resource_wpk.cpp
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Commit**

```powershell
git add -- ChocoLight\src\resource_wpk.cpp
git commit -m "feat: add WPK IDX package parser"
```

---

## Task 5: Implement FLS package parser and crypto helper

**Files:**
- Create: `ChocoLight/src/resource_crypto.cpp`
- Create: `ChocoLight/src/resource_fls.cpp`
- Modify: `ChocoLight/src/resource_package.h`

- [ ] **Step 1: Add crypto function declarations**

Append to `namespace LT::Resource` in `ChocoLight/src/resource_package.h`:

```cpp
void DecryptFlsIndex(uint8_t* data, size_t bytes, uint32_t count);
void DecryptFlsData(const uint8_t* input, size_t bytes, uint32_t hash, std::string& out);
bool LooksLikeKnownPayload(const std::string& data);
```

- [ ] **Step 2: Implement crypto helper**

Create `ChocoLight/src/resource_crypto.cpp`:

```cpp
#include "resource_package.h"

#include <cstring>

namespace LT::Resource {

void DecryptFlsIndex(uint8_t* data, size_t bytes, uint32_t count) {
    if (!data || bytes < static_cast<size_t>(count) * 16u) return;
    uint32_t accumulator = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* p = data + static_cast<size_t>(i) * 16u;
        uint32_t hash = ReadLE32(p + 0);
        uint32_t flag = ReadLE32(p + 4);
        uint32_t size = ReadLE32(p + 8);
        uint32_t offset = ReadLE32(p + 12);

        uint32_t decOffset = offset ^ static_cast<uint32_t>(16193790u * (i * size) + 223990124u);
        uint32_t decSize = size ^ static_cast<uint32_t>(hash * flag + 914014u);
        uint32_t decFlag = flag ^ static_cast<uint32_t>(hash + accumulator);
        accumulator = static_cast<uint32_t>(accumulator + 243041234u);

        p[4] = static_cast<uint8_t>(decFlag & 0xFF);
        p[5] = static_cast<uint8_t>((decFlag >> 8) & 0xFF);
        p[6] = static_cast<uint8_t>((decFlag >> 16) & 0xFF);
        p[7] = static_cast<uint8_t>((decFlag >> 24) & 0xFF);
        p[8] = static_cast<uint8_t>(decSize & 0xFF);
        p[9] = static_cast<uint8_t>((decSize >> 8) & 0xFF);
        p[10] = static_cast<uint8_t>((decSize >> 16) & 0xFF);
        p[11] = static_cast<uint8_t>((decSize >> 24) & 0xFF);
        p[12] = static_cast<uint8_t>(decOffset & 0xFF);
        p[13] = static_cast<uint8_t>((decOffset >> 8) & 0xFF);
        p[14] = static_cast<uint8_t>((decOffset >> 16) & 0xFF);
        p[15] = static_cast<uint8_t>((decOffset >> 24) & 0xFF);
    }
}

void DecryptFlsData(const uint8_t* input, size_t bytes, uint32_t hash, std::string& out) {
    out.assign(bytes, '\0');
    uint32_t key = static_cast<uint32_t>((hash + 9581739u) ^ 0x937F4912u);
    uint8_t k[4] = {
        static_cast<uint8_t>(key & 0xFF),
        static_cast<uint8_t>((key >> 8) & 0xFF),
        static_cast<uint8_t>((key >> 16) & 0xFF),
        static_cast<uint8_t>((key >> 24) & 0xFF),
    };
    for (size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<char>(input[i] ^ k[i & 3]);
    }
}

bool LooksLikeKnownPayload(const std::string& data) {
    if (data.empty()) return false;
    const auto starts = [&](const char* s, size_t n) {
        return data.size() >= n && std::memcmp(data.data(), s, n) == 0;
    };
    if (starts("@#$V", 4) || starts("[inf", 4) || starts("[ima", 4) || starts("[fra", 4)) return true;
    if (starts("PNAM", 4) || starts("0.1M", 4) || starts("SKPW", 4)) return true;
    if (starts("\x89PNG", 4) || starts("BM", 2) || starts("\xFF\xD8\xFF", 3)) return true;
    if (data.size() >= 2) {
        uint16_t magic = ReadLE16(reinterpret_cast<const uint8_t*>(data.data()));
        switch (magic) {
            case 0x3049: case 0x3054: case 0x3149: case 0x3154:
            case 0x3449: case 0x3454: case 0x3549: case 0x3554:
            case 0x5053: case 0x5052: case 0x5054:
                return true;
            default:
                break;
        }
    }
    return false;
}

} // namespace LT::Resource
```

- [ ] **Step 3: Implement FLS parser**

Create `ChocoLight/src/resource_fls.cpp` with these behaviours:

```cpp
#include "resource_package.h"

#include <unordered_map>

namespace LT::Resource {
namespace {

class FlsPackage final : public IResourcePackage {
public:
    FlsPackage(FILE* fp, std::string path) : fp_(fp), path_(std::move(path)) {}
    ~FlsPackage() override { if (fp_) std::fclose(fp_); }

    bool Init(std::string& err) {
        uint64_t fileSize = 0;
        if (!GetFileSize(fp_, fileSize)) { err = "failed to determine FLS size"; return false; }
        std::string header;
        if (!ReadAt(fp_, 0, 12, header, err)) return false;
        if (header.substr(0, 4) != "0SLF") { err = "invalid FLS magic"; return false; }
        const uint8_t* h = reinterpret_cast<const uint8_t*>(header.data());
        uint32_t count = ReadLE32(h + 4);
        uint32_t indexOffset = ReadLE32(h + 8);
        uint64_t indexBytes = static_cast<uint64_t>(count) * 16u;
        if (!RangeInFile(fileSize, indexOffset, indexBytes)) { err = "invalid FLS index range"; return false; }

        std::string index;
        if (!ReadAt(fp_, indexOffset, indexBytes, index, err)) return false;
        DecryptFlsIndex(reinterpret_cast<uint8_t*>(index.data()), index.size(), count);

        info_.kind = "FLS";
        info_.subtype = "0SLF";
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = indexOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(index.data() + i * 16);
            ResourceEntry e;
            e.id = ReadLE32(p + 0);
            e.offset = ReadLE32(p + 12);
            e.size = ReadLE32(p + 8);
            e.packedSize = e.size;
            e.keyText = std::to_string(e.id);
            if (!RangeInFile(fileSize, e.offset, e.size)) { err = "invalid FLS entry range"; return false; }
            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }
    bool Has(const std::string& key) const override { return keyToIndex_.find(key) != keyToIndex_.end(); }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(key);
        if (it == keyToIndex_.end()) { err = "entry not found"; return false; }
        const ResourceEntry& e = entries_[it->second];
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) { err = "entry exceeds maxBytes"; return false; }

        std::string raw;
        if (!ReadAt(fp_, e.offset, e.size, raw, err)) return false;
        if (opts.raw || !opts.decode) { out.swap(raw); return true; }

        std::string decoded;
        DecryptFlsData(reinterpret_cast<const uint8_t*>(raw.data()), raw.size(), e.id, decoded);
        bool rawOk = LooksLikeKnownPayload(raw);
        bool decodedOk = LooksLikeKnownPayload(decoded);
        if (rawOk && !decodedOk) { out.swap(raw); return true; }
        out.swap(decoded);
        return true;
    }

private:
    FILE* fp_ = nullptr;
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenFlsPackage(const std::string& path, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) { err = std::string("open failed: ") + path; return nullptr; }
    auto pkg = std::make_unique<FlsPackage>(fp, path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
```

- [ ] **Step 4: Run static check**

```powershell
git diff --check -- ChocoLight\src\resource_package.h ChocoLight\src\resource_crypto.cpp ChocoLight\src\resource_fls.cpp
```

Expected: no output and exit code `0`.

- [ ] **Step 5: Commit**

```powershell
git add -- ChocoLight\src\resource_package.h ChocoLight\src\resource_crypto.cpp ChocoLight\src\resource_fls.cpp
git commit -m "feat: add FLS package parser"
```

---

## Task 6: Add Lua binding for `Light.Plugins.Package`

**Files:**
- Create: `ChocoLight/src/light_plugins_package.cpp`

- [ ] **Step 1: Create binding implementation**

Create `ChocoLight/src/light_plugins_package.cpp`:

```cpp
#include "resource_package.h"

namespace {

using LT::Resource::PackageHandle;

PackageHandle* CheckPackage(lua_State* L, int idx) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, idx, "Light.Plugins.Package.Handle"));
    if (!h || h->magic != LT::Resource::LT_MAGIC_PACKAGE || !h->package) {
        luaL_error(L, "Light.Plugins.Package: invalid or closed handle");
        return nullptr;
    }
    return h;
}

void PushInfo(lua_State* L, const LT::Resource::PackageInfo& info) {
    lua_createtable(L, 0, 6);
    lua_pushstring(L, info.kind.c_str()); lua_setfield(L, -2, "kind");
    lua_pushstring(L, info.subtype.c_str()); lua_setfield(L, -2, "subtype");
    lua_pushstring(L, info.path.c_str()); lua_setfield(L, -2, "path");
    lua_pushinteger(L, static_cast<lua_Integer>(info.count)); lua_setfield(L, -2, "count");
    lua_pushinteger(L, static_cast<lua_Integer>(info.indexOffset)); lua_setfield(L, -2, "indexOffset");
    lua_pushboolean(L, info.supported ? 1 : 0); lua_setfield(L, -2, "supported");
}

LT::Resource::ReadOptions ReadOptionsFromLua(lua_State* L, int idx) {
    LT::Resource::ReadOptions opts;
    if (lua_type(L, idx) != LUA_TTABLE) return opts;
    lua_getfield(L, idx, "raw"); opts.raw = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "decode"); if (!lua_isnil(L, -1)) opts.decode = lua_toboolean(L, -1) != 0; lua_pop(L, 1);
    lua_getfield(L, idx, "maxBytes"); if (lua_isnumber(L, -1)) opts.maxBytes = static_cast<uint64_t>(lua_tointeger(L, -1)); lua_pop(L, 1);
    return opts;
}

std::string KeyFromLua(lua_State* L, int idx) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        return std::to_string(static_cast<uint32_t>(luaL_checkinteger(L, idx)));
    }
    size_t len = 0;
    const char* s = luaL_checklstring(L, idx, &len);
    return std::string(s, len);
}

int l_Package_Probe(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    LT::Resource::PackageInfo info;
    std::string err;
    if (!LT::Resource::ProbePackageFile(path, info, err)) return LT::PushNilError(L, "%s", err.c_str());
    PushInfo(L, info);
    return 1;
}

int l_Package_Open(lua_State* L) {
    const char* path = LT::CheckStringStrict(L, 1);
    std::string err;
    auto pkg = LT::Resource::OpenPackageFile(path, err);
    if (!pkg) return LT::PushNilError(L, "%s", err.c_str());
    auto* h = static_cast<PackageHandle*>(lua_newuserdata(L, sizeof(PackageHandle)));
    new (h) PackageHandle();
    h->package = std::move(pkg);
    luaL_getmetatable(L, "Light.Plugins.Package.Handle");
    lua_setmetatable(L, -2);
    return 1;
}

int l_Handle_GetInfo(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    PushInfo(L, h->package->Info());
    return 1;
}

int l_Handle_List(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    const auto& entries = h->package->Entries();
    lua_createtable(L, static_cast<int>(entries.size()), 0);
    int arr = lua_gettop(L);
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        lua_createtable(L, 0, 8);
        if (!e.keyText.empty() && e.md5.empty()) lua_pushinteger(L, static_cast<lua_Integer>(e.id));
        else lua_pushstring(L, e.keyText.c_str());
        lua_setfield(L, -2, "key");
        lua_pushinteger(L, static_cast<lua_Integer>(e.id)); lua_setfield(L, -2, "id");
        lua_pushinteger(L, static_cast<lua_Integer>(e.offset)); lua_setfield(L, -2, "offset");
        lua_pushinteger(L, static_cast<lua_Integer>(e.size)); lua_setfield(L, -2, "size");
        lua_pushinteger(L, static_cast<lua_Integer>(e.packedSize)); lua_setfield(L, -2, "packedSize");
        lua_pushinteger(L, static_cast<lua_Integer>(e.archive)); lua_setfield(L, -2, "archive");
        if (!e.md5.empty()) { lua_pushstring(L, e.md5.c_str()); lua_setfield(L, -2, "md5"); }
        lua_rawseti(L, arr, static_cast<int>(i + 1));
    }
    return 1;
}

int l_Handle_Has(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    std::string key = KeyFromLua(L, 2);
    lua_pushboolean(L, h->package->Has(key) ? 1 : 0);
    return 1;
}

int l_Handle_GetData(lua_State* L) {
    auto* h = CheckPackage(L, 1);
    std::string key = KeyFromLua(L, 2);
    LT::Resource::ReadOptions opts = ReadOptionsFromLua(L, 3);
    std::string data;
    std::string err;
    if (!h->package->Read(key, opts, data, err)) return LT::PushNilError(L, "%s", err.c_str());
    lua_pushlstring(L, data.data(), data.size());
    return 1;
}

int l_Handle_Close(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    if (h && h->magic == LT::Resource::LT_MAGIC_PACKAGE) {
        h->package.reset();
    }
    lua_pushboolean(L, 1);
    return 1;
}

int l_Handle_GC(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    if (h && h->magic != LT::LT_MAGIC_DEAD) {
        h->package.reset();
        h->magic = LT::LT_MAGIC_DEAD;
        h->~PackageHandle();
    }
    return 0;
}

int l_Handle_Tostring(lua_State* L) {
    auto* h = static_cast<PackageHandle*>(luaL_checkudata(L, 1, "Light.Plugins.Package.Handle"));
    lua_pushfstring(L, "Light.Plugins.Package.Handle: %p", h);
    return 1;
}

const luaL_Reg kPackageFns[] = {
    {"Probe", l_Package_Probe},
    {"Open",  l_Package_Open},
    {nullptr, nullptr}
};

const luaL_Reg kHandleFns[] = {
    {"GetInfo", l_Handle_GetInfo},
    {"List", l_Handle_List},
    {"Has", l_Handle_Has},
    {"GetData", l_Handle_GetData},
    {"Close", l_Handle_Close},
    {"__gc", l_Handle_GC},
    {"__tostring", l_Handle_Tostring},
    {nullptr, nullptr}
};

} // namespace

extern "C" LIGHT_API int luaopen_Light_Plugins_Package(lua_State* L) {
    if (luaL_newmetatable(L, "Light.Plugins.Package.Handle")) {
        luaL_setfuncs(L, kHandleFns, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);
    LT::Resource::RegisterPluginsSubmodule(L, "Package", kPackageFns);
    return 1;
}
```

- [ ] **Step 2: Run static check**

```powershell
git diff --check -- ChocoLight\src\light_plugins_package.cpp
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Commit**

```powershell
git add -- ChocoLight\src\light_plugins_package.cpp
git commit -m "feat: expose Light.Plugins.Package"
```

---

## Task 7: Wire build, exports, module table, and CI smoke

**Files:**
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `.github/workflows/build-templates.yml`

- [ ] **Step 1: Add export declaration**

In `ChocoLight/include/light.h`, add near other plugin declarations:

```cpp
LIGHT_API int luaopen_Light_Plugins_Package(lua_State* L);
```

- [ ] **Step 2: Add preload entry**

In `lumen-master/src/light/light.cpp`, add after `Light.Plugins` or other plugin entries:

```cpp
{"Light.Plugins.Package",    "luaopen_Light_Plugins_Package"},
```

- [ ] **Step 3: Add CMake sources**

In `ChocoLight/CMakeLists.txt`, add to `LIGHT_SOURCES`:

```cmake
    ${CHOCO_SRC}/resource_package.cpp
    ${CHOCO_SRC}/resource_crypto.cpp
    ${CHOCO_SRC}/resource_wdf.cpp
    ${CHOCO_SRC}/resource_wpk.cpp
    ${CHOCO_SRC}/resource_fls.cpp
    ${CHOCO_SRC}/light_plugins_package.cpp
```

- [ ] **Step 4: Register smoke in CI**

In `.github/workflows/build-templates.yml`, follow the existing smoke registration pattern and add a Windows runtime step for:

```text
scripts/smoke/resource_package.lua
```

Use a step name containing `resource_package` so CI logs are searchable.

- [ ] **Step 5: Run static check**

```powershell
git diff --check -- ChocoLight\include\light.h lumen-master\src\light\light.cpp ChocoLight\CMakeLists.txt .github\workflows\build-templates.yml
```

Expected: no output and exit code `0`.

- [ ] **Step 6: Commit**

```powershell
git add -- ChocoLight\include\light.h lumen-master\src\light\light.cpp ChocoLight\CMakeLists.txt .github\workflows\build-templates.yml
git commit -m "build: wire resource package plugin"
```

---

## Task 8: Document `Light.Plugins.Package`

**Files:**
- Modify or create: `docs/api/Light_Plugins.md`
- Modify: `docs/Phase G.3 Resource Format Plugins/ACCEPTANCE_PhaseG_3.md`
- Modify: `docs/Phase G.3 Resource Format Plugins/TODO_PhaseG_3.md`

- [ ] **Step 1: Add API documentation**

Add this section to `docs/api/Light_Plugins.md`:

```markdown
## Light.Plugins.Package

```lua
local Package = require("Light.Plugins.Package")
```

资源包读取插件。运行时失败返回 `nil, err`，参数类型错误使用 Lua 原生错误。

- `Package.Probe(path) -> table | nil, err`
- `Package.Open(path) -> PackageHandle | nil, err`
- `handle:GetInfo() -> table`
- `handle:List() -> table | nil, err`
- `handle:Has(key) -> boolean`
- `handle:GetData(key [, options]) -> string | nil, err`
- `handle:Close() -> true`

`options`:

```lua
{
    raw = false,
    decode = true,
    maxBytes = nil,
}
```

当前 MVP 支持：

- `PFDW/WDFP` WDF 基础条目读取。
- `SKPW` IDX + WPK 分卷读取。
- `0SLF` FLS 索引解密和条目读取。

当前 MVP 明确不支持：

- `NXPK`
- `MHWD`
- `WDFX/WDFH/SFDW/WDFS` 完整读取

示例：

```lua
local pkg = assert(Package.Open("assets/wzife.wdf"))
local info = pkg:GetInfo()
local entries = assert(pkg:List())
local data = assert(pkg:GetData(entries[1].key))
pkg:Close()
```
```

- [ ] **Step 2: Update acceptance record**

In `docs/Phase G.3 Resource Format Plugins/ACCEPTANCE_PhaseG_3.md`, add an execution record row:

```markdown
| 2026-05-19 | G.3.1 Package MVP plan | ✅ | `docs/superpowers/plans/2026-05-19-phase-g3-resource-package-mvp.md` |
```

During actual implementation, replace or append rows with build/smoke evidence.

- [ ] **Step 3: Update TODO status**

In `docs/Phase G.3 Resource Format Plugins/TODO_PhaseG_3.md`, under Package MVP, mark the plan as ready and keep implementation status pending until code is merged.

- [ ] **Step 4: Run static check**

```powershell
git diff --check -- docs\api\Light_Plugins.md "docs\Phase G.3 Resource Format Plugins\ACCEPTANCE_PhaseG_3.md" "docs\Phase G.3 Resource Format Plugins\TODO_PhaseG_3.md"
```

Expected: no output and exit code `0`.

- [ ] **Step 5: Commit**

```powershell
git add -- docs\api\Light_Plugins.md "docs\Phase G.3 Resource Format Plugins\ACCEPTANCE_PhaseG_3.md" "docs\Phase G.3 Resource Format Plugins\TODO_PhaseG_3.md"
git commit -m "docs: document resource package plugin"
```

---

## Task 9: Verification pass

**Files:**
- No new files.

- [ ] **Step 1: Check tracked changes only**

Run:

```powershell
git status --short
```

Expected:

```text
?? assets/
```

plus no unintended source/doc files. If `assets/` appears, leave it untracked.

- [ ] **Step 2: Run diff whitespace check**

```powershell
git diff --check HEAD
```

Expected: no output and exit code `0`.

- [ ] **Step 3: Optional runtime smoke with assets**

Only if the user approves local runtime execution and `light.exe` exists:

```powershell
$env:LIGHT_TEST_ASSETS="E:\jinyiNew\Light\assets"
.\build\light.exe scripts\smoke\resource_package.lua
```

Expected tail:

```text
resource_package smoke ok
```

- [ ] **Step 4: CI verification**

Push branch or open PR and wait for GitHub Actions. Expected:

```text
build-windows runtime smoke resource_package: pass
all required jobs: success
```

- [ ] **Step 5: Final docs update**

After verification, update `ACCEPTANCE_PhaseG_3.md` with the exact command/CI run and update `FINAL_PhaseG_3.md` to state G.3.1 Package MVP status.

Commit:

```powershell
git add -- "docs\Phase G.3 Resource Format Plugins\ACCEPTANCE_PhaseG_3.md" "docs\Phase G.3 Resource Format Plugins\FINAL_PhaseG_3.md"
git commit -m "docs: record resource package verification"
```

---

## Plan Self-Review

### Spec coverage

- Package API from `DESIGN_PhaseG_3.md`: covered by Tasks 2, 6, 8.
- WDF/PFDW MVP: covered by Task 3.
- WPK/IDX MVP: covered by Task 4.
- FLS MVP: covered by Task 5.
- NXPK/MHWD unsupported: covered by Task 2 and Task 3.
- Assets skip rule: covered by Task 1 and Task 9.
- Module registration constraint: covered by Tasks 2, 6, 7.

### Deliberately out of this plan

- TCP/IGS/MAP decoding: separate plans after Package MVP.
- Full AC/XC decryption: the WPK parser reads raw bytes in this MVP; AC/XC is a follow-up because it needs focused validation against encrypted samples.
- WDFX/WDFH/SFDW/WDFS full support: probe-only in this MVP because current assets only verify PFDW.

### Type consistency

- Lua module name is `Light.Plugins.Package` in all tasks.
- Handle metatable is `Light.Plugins.Package.Handle` in all binding steps.
- C++ namespace is `LT::Resource` in all core files.
- Public handle methods use `GetInfo/List/Has/GetData/Close` consistently.
