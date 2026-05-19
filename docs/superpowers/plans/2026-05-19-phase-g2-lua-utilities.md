# Phase G.2 Lua Utilities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add common Lua utility libraries to ChocoLight with official `Light.*` APIs and compatibility `require("...")` modules.

**Architecture:** Build small focused C++ Lua modules for compression, codec, path, UUID, and JSON, then add thin compatibility wrappers for `lfs`, `zlib`, `md5`, `sha1`, `json`, and `uuid`. Reuse existing `Light.Crypto` and `Light.Filesystem` where possible; keep OpenSSL out of the default dependency graph.

**Tech Stack:** C++17-style ChocoLight binding code, Lua 5.1, SDL3 filesystem APIs, existing cJSON FetchContent target, existing `tiny_hash`, new miniz FetchContent source integration, GitHub Actions Windows runtime smoke.

---

## Mandatory Project Constraints

- Do **not** run local CMake builds.
- Do **not** run local `light.exe` smoke tests.
- `lightc -p` syntax checks are allowed if a local `lightc.exe` already exists.
- New `Light.XXX` modules must return a table with `lua_newtable` + `luaL_register(L, nullptr, funcs)` or equivalent.
- Do **not** use `luaL_register(L, "Light.XXX", funcs)`.
- Lua binary string smoke tests must use `string.char(...)`, not `"\xNN"` escapes.
- Keep OpenSSL as future TODO only; do not add it as a dependency.

## File Structure

### New C++ files

- `ChocoLight/src/light_utils_core.h`
  - Shared internal helpers for hex, URL codec, CRC32, SHA1, UUID parsing/formatting, and Lua table registration.
- `ChocoLight/src/light_compress.cpp`
  - Official `Light.Compress` module and `zlib` compatibility module.
- `ChocoLight/src/light_codec.cpp`
  - Official `Light.Codec` module.
- `ChocoLight/src/light_path.cpp`
  - Official `Light.Path` module.
- `ChocoLight/src/light_uuid.cpp`
  - Official `Light.UUID` module and `uuid` compatibility module.
- `ChocoLight/src/light_json.cpp`
  - Official `Light.JSON` module and `json` compatibility module.
- `ChocoLight/src/light_lua_compat.cpp`
  - `lfs`, `md5`, and `sha1` compatibility modules.

### Modified C++ / CMake / loader files

- `ChocoLight/CMakeLists.txt`
  - Add miniz FetchContent source and new module sources.
- `ChocoLight/include/light.h`
  - Add `luaopen_Light_Compress`, `luaopen_Light_Codec`, `luaopen_Light_Path`, `luaopen_Light_UUID`, `luaopen_Light_JSON`.
  - Add compatibility exports: `luaopen_lfs`, `luaopen_zlib`, `luaopen_md5`, `luaopen_sha1`, `luaopen_json`, `luaopen_uuid`.
- `lumen-master/src/light/light.cpp`
  - Add Windows preload entries for official and compatibility modules.
- `ChocoLight/src/light_crypto.cpp`
  - Add `SHA1`, `SHA1_Raw`, `HexEncode`, `HexDecode`, and optionally `CRC32` forwarding through shared helper.
- `ChocoLight/src/light_filesystem.cpp`
  - Add `Exists`, `IsFile`, `IsDirectory`, `List`, `Attributes`, `CurrentDir` aliases if needed by `lfs`.

### New smoke scripts

- `scripts/smoke/compress.lua`
- `scripts/smoke/codec.lua`
- `scripts/smoke/path.lua`
- `scripts/smoke/json.lua`
- `scripts/smoke/uuid.lua`
- `scripts/smoke/lua_compat_utils.lua`

### Modified CI / docs files

- `.github/workflows/build-templates.yml`
  - Register the new smoke scripts in Windows runtime smoke.
- `docs/api/Light_Utilities.md`
  - Add user-facing API documentation for the new utility modules.
- `docs/Phase G.2 Lua Utilities/ACCEPTANCE_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/FINAL_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/TODO_PhaseG_2.md`

---

## Task 1: Add shared utility helper header

**Files:**
- Create: `ChocoLight/src/light_utils_core.h`
- Test indirectly through later smoke scripts.

- [ ] **Step 1: Create shared helper header skeleton**

Create `ChocoLight/src/light_utils_core.h` with these responsibilities:

```cpp
#pragma once

#include "light.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace LT::Utils {

std::string HexEncode(const uint8_t* data, size_t len, bool upper);
bool HexDecode(const char* text, size_t len, std::string& out, std::string& err);
std::string URLEncode(const char* data, size_t len);
bool URLDecode(const char* text, size_t len, std::string& out, std::string& err);
uint32_t CRC32(const uint8_t* data, size_t len, uint32_t seed = 0);
void SHA1(const uint8_t* data, size_t len, uint8_t out20[20]);
std::string JoinPathParts(lua_State* L, int firstArg, int lastArg);
std::string NormalizePath(const std::string& path, char sep);
bool ParseUUID(const char* text, size_t len, uint8_t out16[16]);
std::string FormatUUID(const uint8_t raw16[16]);
void RegisterTable(lua_State* L, const luaL_Reg* funcs);

}
```

- [ ] **Step 2: Add inline implementations cautiously**

Keep functions small and deterministic:

- `HexEncode` uses `lua_pushlstring` consumers later, not null-terminated assumptions.
- `HexDecode` rejects odd length and invalid characters.
- `URLDecode` rejects malformed `%` escapes.
- `CRC32` implements standard polynomial `0xEDB88320` and accepts a seed for zlib compatibility.
- `SHA1` is a compact local implementation because current `tiny_hash` only exposes MD5/SHA256 in existing usage.
- `RegisterTable` does:

```cpp
inline void RegisterTable(lua_State* L, const luaL_Reg* funcs) {
    lua_newtable(L);
    luaL_register(L, nullptr, funcs);
}
```

- [ ] **Step 3: Static self-check**

Run only a text check, no build:

```powershell
git diff --check -- ChocoLight/src/light_utils_core.h
```

Expected: no output and exit code `0`.

- [ ] **Step 4: Commit**

```powershell
git add -- ChocoLight/src/light_utils_core.h
git commit -m "feat: add shared Lua utilities helpers"
```

---

## Task 2: Add `Light.Codec`

**Files:**
- Create: `ChocoLight/src/light_codec.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/codec.lua`

- [ ] **Step 1: Write smoke first**

Create `scripts/smoke/codec.lua`:

```lua
local Codec = require("Light.Codec")
assert(type(Codec) == "table")
assert(type(Codec.HexEncode) == "function")
assert(type(Codec.HexDecode) == "function")
assert(type(Codec.URLEncode) == "function")
assert(type(Codec.URLDecode) == "function")
assert(type(Codec.Base64Encode) == "function")
assert(type(Codec.Base64Decode) == "function")

local raw = string.char(0, 1, 2, 15, 16, 255)
assert(Codec.HexEncode(raw) == "0001020f10ff")
assert(Codec.HexEncode(raw, true) == "0001020F10FF")
assert(Codec.HexDecode("0001020f10ff") == raw)

local bad, err = Codec.HexDecode("abc")
assert(bad == nil and type(err) == "string")

local url = Codec.URLEncode("a b+c/?")
assert(url == "a%20b%2Bc%2F%3F")
assert(Codec.URLDecode(url) == "a b+c/?")

local b64 = Codec.Base64Encode("hello")
assert(b64 == "aGVsbG8=")
assert(Codec.Base64Decode(b64) == "hello")

print("[smoke] codec ok")
```

- [ ] **Step 2: Add module implementation**

Create `ChocoLight/src/light_codec.cpp` exposing:

```cpp
extern "C" LIGHT_API int luaopen_Light_Codec(lua_State* L);
```

Functions:

- `Base64Encode(data)` reuses existing base64 logic by moving shared helpers from `light_crypto.cpp` into this file or shared helper.
- `Base64Decode(text)` returns `nil, "Base64Decode: invalid input"` for invalid input.
- `HexEncode(data [, upper])` uses `LT::Utils::HexEncode`.
- `HexDecode(hex)` uses `LT::Utils::HexDecode`.
- `URLEncode(text)` uses `LT::Utils::URLEncode`.
- `URLDecode(text)` uses `LT::Utils::URLDecode`.

Module registration must be:

```cpp
extern "C" LIGHT_API int luaopen_Light_Codec(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"Base64Encode", l_Base64Encode},
        {"Base64Decode", l_Base64Decode},
        {"HexEncode",    l_HexEncode},
        {"HexDecode",    l_HexDecode},
        {"URLEncode",    l_URLEncode},
        {"URLDecode",    l_URLDecode},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterTable(L, funcs);
    return 1;
}
```

- [ ] **Step 3: Register build and preload**

Modify `ChocoLight/CMakeLists.txt` source list near `light_crypto.cpp`:

```cmake
${CHOCO_SRC}/light_codec.cpp
```

Modify `ChocoLight/include/light.h` inside `extern "C"`:

```cpp
LIGHT_API int luaopen_Light_Codec(lua_State* L);
```

Modify `lumen-master/src/light/light.cpp` `g_lightModules` near `Light.Crypto`:

```cpp
{"Light.Codec",               "luaopen_Light_Codec"},
```

- [ ] **Step 4: Syntax/static checks**

```powershell
git diff --check -- ChocoLight/src/light_codec.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/codec.lua
```

If local `lightc.exe` exists, run syntax only:

```powershell
lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\codec.lua
```

Expected: syntax success. Do not run `light.exe`.

- [ ] **Step 5: Commit**

```powershell
git add -- ChocoLight/src/light_codec.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/codec.lua
git commit -m "feat: add Light.Codec utilities"
```

---

## Task 3: Enhance `Light.Crypto` with SHA1 and hex helpers

**Files:**
- Modify: `ChocoLight/src/light_crypto.cpp`
- Create or extend smoke: `scripts/smoke/crypto_utils.lua` if preferred; otherwise include in `scripts/smoke/lua_compat_utils.lua` later.

- [ ] **Step 1: Add expected smoke assertions**

Use known hashes:

```lua
local Crypto = require("Light.Crypto")
assert(Crypto.SHA1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#Crypto.SHA1_Raw("abc") == 20)
assert(Crypto.HexEncode(string.char(0, 255)) == "00ff")
assert(Crypto.HexDecode("00ff") == string.char(0, 255))
```

- [ ] **Step 2: Add C++ functions**

Add these wrappers in `light_crypto.cpp`:

- `l_SHA1`
- `l_SHA1_Raw`
- `l_Crypto_HexEncode`
- `l_Crypto_HexDecode`
- optional `l_Crypto_CRC32`

Use shared helper implementations from `light_utils_core.h`.

- [ ] **Step 3: Extend registration table**

Add to `crypto_funcs`:

```cpp
{"SHA1",      l_SHA1},
{"SHA1_Raw",  l_SHA1_Raw},
{"HexEncode", l_Crypto_HexEncode},
{"HexDecode", l_Crypto_HexDecode},
{"CRC32",     l_Crypto_CRC32},
```

- [ ] **Step 4: Static checks**

```powershell
git diff --check -- ChocoLight/src/light_crypto.cpp scripts/smoke/crypto_utils.lua
```

- [ ] **Step 5: Commit**

```powershell
git add -- ChocoLight/src/light_crypto.cpp scripts/smoke/crypto_utils.lua
git commit -m "feat: extend Light.Crypto utility hashes"
```

---

## Task 4: Add `Light.Path`

**Files:**
- Create: `ChocoLight/src/light_path.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/path.lua`

- [ ] **Step 1: Write smoke first**

Create `scripts/smoke/path.lua`:

```lua
local Path = require("Light.Path")
assert(type(Path.Join) == "function")
assert(type(Path.Normalize) == "function")
assert(type(Path.Basename) == "function")
assert(type(Path.Dirname) == "function")
assert(type(Path.Extname) == "function")
assert(type(Path.Stem) == "function")
assert(type(Path.IsAbsolute) == "function")
assert(type(Path.Split) == "function")

local joined = Path.Join("foo", "bar", "baz.txt")
assert(joined:find("foo") and joined:find("bar") and joined:find("baz.txt"))
assert(Path.Normalize("foo//bar/./baz") == Path.Join("foo", "bar", "baz"))
assert(Path.Basename("foo/bar/baz.txt") == "baz.txt")
assert(Path.Dirname("foo/bar/baz.txt") == "foo/bar" or Path.Dirname("foo/bar/baz.txt") == "foo\\bar")
assert(Path.Extname("foo/bar/baz.txt") == ".txt")
assert(Path.Stem("foo/bar/baz.txt") == "baz")

local parts = Path.Split("foo/bar/baz.txt")
assert(type(parts) == "table" and #parts == 3)

print("[smoke] path ok")
```

- [ ] **Step 2: Implement path string module**

Create `light_path.cpp` with pure string functions. Do not call filesystem APIs.

Functions:

- `Join(...)`
- `Normalize(path)`
- `Basename(path)`
- `Dirname(path)`
- `Extname(path)`
- `Stem(path)`
- `IsAbsolute(path)`
- `Split(path)`
- `Separator()`

- [ ] **Step 3: Register build and preload**

Add source:

```cmake
${CHOCO_SRC}/light_path.cpp
```

Add export:

```cpp
LIGHT_API int luaopen_Light_Path(lua_State* L);
```

Add preload:

```cpp
{"Light.Path",                "luaopen_Light_Path"},
```

- [ ] **Step 4: Static checks and commit**

```powershell
git diff --check -- ChocoLight/src/light_path.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/path.lua
```

```powershell
git add -- ChocoLight/src/light_path.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/path.lua
git commit -m "feat: add Light.Path utilities"
```

---

## Task 5: Add `Light.UUID` and `uuid` compatibility module

**Files:**
- Create: `ChocoLight/src/light_uuid.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/uuid.lua`

- [ ] **Step 1: Write smoke first**

Create `scripts/smoke/uuid.lua`:

```lua
local UUID = require("Light.UUID")
assert(type(UUID.V4) == "function")
assert(type(UUID.IsValid) == "function")
assert(type(UUID.Parse) == "function")
assert(type(UUID.Format) == "function")

local id = UUID.V4()
assert(type(id) == "string" and #id == 36)
assert(UUID.IsValid(id) == true)
local raw = assert(UUID.Parse(id))
assert(type(raw) == "string" and #raw == 16)
assert(UUID.IsValid(UUID.Format(raw)) == true)
assert(UUID.IsValid("not-a-uuid") == false)

local uuid = require("uuid")
assert(type(uuid.v4) == "function")
assert(type(uuid.is_valid) == "function")
local id2 = uuid.v4()
assert(uuid.is_valid(id2) == true)

print("[smoke] uuid ok")
```

- [ ] **Step 2: Implement UUID module**

Create `light_uuid.cpp`:

- `Light.UUID.V4()` generates 16 bytes, sets UUID v4 version and variant bits, returns canonical lowercase string.
- `Light.UUID.IsValid(s)` checks canonical form.
- `Light.UUID.Parse(s)` returns raw 16 bytes or `nil, err`.
- `Light.UUID.Format(raw16)` returns canonical string or `nil, err`.
- `luaopen_uuid` exposes `{ v4 = ..., is_valid = ... }` thin wrappers.

- [ ] **Step 3: Register official and compatibility modules**

Add source:

```cmake
${CHOCO_SRC}/light_uuid.cpp
```

Add exports:

```cpp
LIGHT_API int luaopen_Light_UUID(lua_State* L);
LIGHT_API int luaopen_uuid(lua_State* L);
```

Add preload entries:

```cpp
{"Light.UUID",                "luaopen_Light_UUID"},
{"uuid",                      "luaopen_uuid"},
```

- [ ] **Step 4: Static checks and commit**

```powershell
git diff --check -- ChocoLight/src/light_uuid.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/uuid.lua
```

```powershell
git add -- ChocoLight/src/light_uuid.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/uuid.lua
git commit -m "feat: add Light.UUID and uuid compat module"
```

---

## Task 6: Add `Light.JSON` and `json` compatibility module

**Files:**
- Create: `ChocoLight/src/light_json.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/json.lua`

- [ ] **Step 1: Write smoke first**

Create `scripts/smoke/json.lua`:

```lua
local JSON = require("Light.JSON")
assert(type(JSON.Encode) == "function")
assert(type(JSON.Decode) == "function")
assert(type(JSON.Null) == "function")
assert(type(JSON.IsNull) == "function")

local text = assert(JSON.Encode({ name = "choco", count = 3, ok = true }))
assert(type(text) == "string")
local obj = assert(JSON.Decode(text))
assert(obj.name == "choco")
assert(obj.count == 3)
assert(obj.ok == true)

local arr = assert(JSON.Decode("[1,2,3]"))
assert(arr[1] == 1 and arr[2] == 2 and arr[3] == 3)
local nullValue = assert(JSON.Decode("null"))
assert(JSON.IsNull(nullValue) == true)

local bad, err = JSON.Decode("{")
assert(bad == nil and type(err) == "string")

local json = require("json")
assert(type(json.encode) == "function")
assert(type(json.decode) == "function")
assert(json.decode(json.encode({x = 1})).x == 1)

print("[smoke] json ok")
```

- [ ] **Step 2: Implement cJSON conversion**

Create `light_json.cpp`:

- Include `cJSON.h`.
- `Encode(value [, options])` converts Lua values to cJSON.
- `Decode(text)` parses cJSON to Lua values.
- JSON null maps to a sentinel table returned by `Light.JSON.Null()`.
- `IsNull(value)` checks the sentinel.
- Detect table cycles with a registry stack/table to avoid infinite recursion.
- Keep array/object detection explicit: continuous integer keys `1..n` with no other keys means array.

- [ ] **Step 3: Register modules**

Add source:

```cmake
${CHOCO_SRC}/light_json.cpp
```

Add exports:

```cpp
LIGHT_API int luaopen_Light_JSON(lua_State* L);
LIGHT_API int luaopen_json(lua_State* L);
```

Add preload entries:

```cpp
{"Light.JSON",                "luaopen_Light_JSON"},
{"json",                      "luaopen_json"},
```

- [ ] **Step 4: Static checks and commit**

```powershell
git diff --check -- ChocoLight/src/light_json.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/json.lua
```

```powershell
git add -- ChocoLight/src/light_json.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/json.lua
git commit -m "feat: add Light.JSON and json compat module"
```

---

## Task 7: Add miniz-backed `Light.Compress` and `zlib` compatibility module

**Files:**
- Create: `ChocoLight/src/light_compress.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/compress.lua`

- [ ] **Step 1: Write smoke first**

Create `scripts/smoke/compress.lua`:

```lua
local Compress = require("Light.Compress")
assert(type(Compress.Compress) == "function")
assert(type(Compress.Decompress) == "function")
assert(type(Compress.Deflate) == "function")
assert(type(Compress.Inflate) == "function")
assert(type(Compress.CRC32) == "function")
assert(type(Compress.Adler32) == "function")
assert(type(Compress.Version) == "function")

local raw = "hello hello hello" .. string.char(0, 1, 2, 255)
local packed = assert(Compress.Compress(raw, 6))
assert(type(packed) == "string" and #packed > 0)
assert(Compress.Decompress(packed) == raw)
assert(type(Compress.CRC32(raw)) == "number")
assert(type(Compress.Adler32(raw)) == "number")

local zlib = require("zlib")
assert(type(zlib.compress) == "function")
assert(type(zlib.decompress) == "function")
local packed2 = assert(zlib.compress(raw))
assert(zlib.decompress(packed2) == raw)

print("[smoke] compress ok")
```

- [ ] **Step 2: Add miniz FetchContent integration**

Modify `ChocoLight/CMakeLists.txt` after cJSON FetchContent:

```cmake
FetchContent_Declare(
    miniz
    GIT_REPOSITORY https://github.com/richgel999/miniz.git
    GIT_TAG        3.0.2
    GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(miniz)
if(NOT miniz_POPULATED)
    FetchContent_Populate(miniz)
endif()
message(STATUS "miniz: integrated via FetchContent (3.0.2)")
```

Add to `LIGHT_SOURCES`:

```cmake
${CHOCO_SRC}/light_compress.cpp
${miniz_SOURCE_DIR}/miniz.c
```

Add include dir:

```cmake
${miniz_SOURCE_DIR}
```

- [ ] **Step 3: Implement compression module**

Create `light_compress.cpp` using `miniz.h`:

- `Compress(data [, level])` uses zlib wrapper output.
- `Decompress(data)` grows output buffer until success or a safe maximum.
- `Deflate(data [, level])` is a G.2.0 compatibility alias of `Compress(data [, level])`.
- `Inflate(data)` is a G.2.0 compatibility alias of `Decompress(data)`.
- `CRC32(data)` uses `mz_crc32` or shared helper.
- `Adler32(data)` uses miniz if available, otherwise local standard implementation.
- `Version()` returns miniz version string.
- `luaopen_zlib` exposes lowercase wrappers.

- [ ] **Step 4: Register official and compatibility modules**

Add exports:

```cpp
LIGHT_API int luaopen_Light_Compress(lua_State* L);
LIGHT_API int luaopen_zlib(lua_State* L);
```

Add preload entries:

```cpp
{"Light.Compress",            "luaopen_Light_Compress"},
{"zlib",                      "luaopen_zlib"},
```

- [ ] **Step 5: Static checks and commit**

```powershell
git diff --check -- ChocoLight/src/light_compress.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/compress.lua
```

```powershell
git add -- ChocoLight/src/light_compress.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/compress.lua
git commit -m "feat: add Light.Compress and zlib compat module"
```

---

## Task 8: Enhance `Light.Filesystem` and add `lfs` compatibility module

**Files:**
- Modify: `ChocoLight/src/light_filesystem.cpp`
- Create or extend: `ChocoLight/src/light_lua_compat.cpp`
- Modify: `ChocoLight/CMakeLists.txt`
- Modify: `ChocoLight/include/light.h`
- Modify: `lumen-master/src/light/light.cpp`
- Create: `scripts/smoke/lua_compat_utils.lua`

- [ ] **Step 1: Write compatibility smoke first**

Create `scripts/smoke/lua_compat_utils.lua`:

```lua
local lfs = require("lfs")
assert(type(lfs.attributes) == "function")
assert(type(lfs.dir) == "function")
assert(type(lfs.mkdir) == "function")
assert(type(lfs.rmdir) == "function")
assert(type(lfs.currentdir) == "function")

local md5 = require("md5")
assert(md5.sumhexa("abc") == "900150983cd24fb0d6963f7d28e17f72")
assert(#md5.sum("abc") == 16)

local sha1 = require("sha1")
assert(sha1.sumhexa("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#sha1.sum("abc") == 20)

local cur = assert(lfs.currentdir())
assert(type(cur) == "string" and #cur > 0)
local attr = assert(lfs.attributes(cur))
assert(type(attr) == "table")
assert(attr.mode == "directory" or attr.mode == "file" or attr.mode == "other")

print("[smoke] lua compat utils ok")
```

- [ ] **Step 2: Add filesystem aliases/helpers**

Modify `light_filesystem.cpp` to add:

- `Exists(path)`
- `IsFile(path)`
- `IsDirectory(path)`
- `List(path)`
- `Attributes(path)`
- `CurrentDir()` as alias for `GetCurrentDirectory()`

Use SDL3 `SDL_GetPathInfo` and `SDL_GlobDirectory` already used in the file.

- [ ] **Step 3: Add `lfs`, `md5`, `sha1` compatibility module**

Create or extend `light_lua_compat.cpp`:

- `luaopen_lfs`
  - `attributes(path [, attr])`
  - `dir(path)` iterator backed by a Lua table from `SDL_GlobDirectory`
  - `mkdir(path)` -> `true` or `nil, err`
  - `rmdir(path)` -> `true` or `nil, err`
  - `currentdir()`
  - `chdir(path)` -> `nil, "not supported"`
- `luaopen_md5`
  - `sum(data)` raw 16 bytes
  - `sumhexa(data)` lowercase hex
- `luaopen_sha1`
  - `sum(data)` raw 20 bytes
  - `sumhexa(data)` lowercase hex

- [ ] **Step 4: Register build and preload**

Add source:

```cmake
${CHOCO_SRC}/light_lua_compat.cpp
```

Add exports:

```cpp
LIGHT_API int luaopen_lfs(lua_State* L);
LIGHT_API int luaopen_md5(lua_State* L);
LIGHT_API int luaopen_sha1(lua_State* L);
```

Add preload entries:

```cpp
{"lfs",                       "luaopen_lfs"},
{"md5",                       "luaopen_md5"},
{"sha1",                      "luaopen_sha1"},
```

- [ ] **Step 5: Static checks and commit**

```powershell
git diff --check -- ChocoLight/src/light_filesystem.cpp ChocoLight/src/light_lua_compat.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/lua_compat_utils.lua
```

```powershell
git add -- ChocoLight/src/light_filesystem.cpp ChocoLight/src/light_lua_compat.cpp ChocoLight/CMakeLists.txt ChocoLight/include/light.h lumen-master/src/light/light.cpp scripts/smoke/lua_compat_utils.lua
git commit -m "feat: add lfs md5 sha1 compatibility modules"
```

---

## Task 9: Register CI smoke scripts

**Files:**
- Modify: `.github/workflows/build-templates.yml`

- [ ] **Step 1: Add Resolve-Path variables**

Near existing Phase G smoke variables add:

```powershell
$phaseG2CodecSmoke   = Resolve-Path "scripts\smoke\codec.lua"
$phaseG2PathSmoke    = Resolve-Path "scripts\smoke\path.lua"
$phaseG2UUIDSmoke    = Resolve-Path "scripts\smoke\uuid.lua"
$phaseG2JSONSmoke    = Resolve-Path "scripts\smoke\json.lua"
$phaseG2CompressSmoke = Resolve-Path "scripts\smoke\compress.lua"
$phaseG2CompatSmoke  = Resolve-Path "scripts\smoke\lua_compat_utils.lua"
```

- [ ] **Step 2: Add runtime invocations**

After `$phaseG171PerfSmoke` invocation add:

```powershell
& "$runtimeDir\light.exe" $phaseG2CodecSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" $phaseG2PathSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" $phaseG2UUIDSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" $phaseG2JSONSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" $phaseG2CompressSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& "$runtimeDir\light.exe" $phaseG2CompatSmoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

- [ ] **Step 3: Static checks and commit**

```powershell
git diff --check -- .github/workflows/build-templates.yml
```

```powershell
git add -- .github/workflows/build-templates.yml
git commit -m "ci: add Phase G.2 Lua utilities smokes"
```

---

## Task 10: Add API and acceptance documentation

**Files:**
- Create: `docs/api/Light_Utilities.md`
- Create: `docs/Phase G.2 Lua Utilities/ACCEPTANCE_PhaseG_2.md`
- Create: `docs/Phase G.2 Lua Utilities/FINAL_PhaseG_2.md`
- Create: `docs/Phase G.2 Lua Utilities/TODO_PhaseG_2.md`

- [ ] **Step 1: Create API doc**

Document:

- `Light.Codec`
- `Light.Crypto` new helpers
- `Light.Path`
- `Light.UUID`
- `Light.JSON`
- `Light.Compress`
- compatibility modules `lfs`, `zlib`, `md5`, `sha1`, `json`, `uuid`

Include concise examples using `string.char(...)` for binary data.

- [ ] **Step 2: Create acceptance doc**

Record expected checks:

- Static `git diff --check` passed.
- Lua syntax checks passed if `lightc.exe` exists.
- Local CMake build not run.
- Local `light.exe` smoke not run.
- CI smoke scripts registered.
- OpenSSL not added as dependency.

- [ ] **Step 3: Create final and TODO docs**

`FINAL` should summarize implemented APIs and commits.

`TODO` should explicitly list deferred work:

- OpenSSL optional backend.
- zlib streaming API.
- `cjson` compatibility alias.
- HMAC/PBKDF2 if not implemented in G.2.0.
- Expanded `lfs.chdir` if a safe cross-platform implementation is desired.

- [ ] **Step 4: Commit**

```powershell
git add -- docs/api/Light_Utilities.md "docs/Phase G.2 Lua Utilities/ACCEPTANCE_PhaseG_2.md" "docs/Phase G.2 Lua Utilities/FINAL_PhaseG_2.md" "docs/Phase G.2 Lua Utilities/TODO_PhaseG_2.md"
git commit -m "docs: add Phase G.2 Lua utilities documentation"
```

---

## Task 11: Final verification and push

**Files:**
- Whole repository status.

- [ ] **Step 1: Check status**

```powershell
git status --short
```

Expected: clean working tree before push.

- [ ] **Step 2: Check recent commits**

```powershell
git log --oneline -n 12
```

Expected: Phase G.2 commits are on top of `main`.

- [ ] **Step 3: Push**

```powershell
git push origin main
```

Expected: GitHub Actions starts automatically.

- [ ] **Step 4: CI follow-up**

After CI completes, update:

- `docs/Phase G.2 Lua Utilities/ACCEPTANCE_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/FINAL_PhaseG_2.md`

with CI run URL and pass/fail status.

---

## Self-Review

### Spec coverage

- `Light.*` official APIs: covered by Tasks 2, 4, 5, 6, 7, 8.
- Lua compatibility APIs: covered by Tasks 5, 6, 7, 8.
- OpenSSL excluded: covered by constraints and Task 10 TODO.
- CI smoke: covered by Task 9.
- No local build/runtime smoke: covered by constraints and Task 10 acceptance.

### Placeholder scan

No implementation step depends on an unresolved design decision. Deferred items are explicitly listed as future TODO in Task 10.

### Type consistency

- Official module names use uppercase ChocoLight style: `Light.JSON`, `Light.UUID`.
- Compatibility module names use lowercase Lua ecosystem style: `json`, `uuid`, `zlib`, `lfs`, `md5`, `sha1`.
- Hash raw functions return Lua binary strings via `lua_pushlstring`.
- Runtime failures return `nil, err`; argument type errors use `luaL_check*`.
