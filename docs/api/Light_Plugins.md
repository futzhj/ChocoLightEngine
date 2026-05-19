# Light.Plugins Lua Utilities API

本文档记录 Phase G.2 新增的 Lua 工具库 API。正式 API 统一放在 `Light.Plugins.*` 命名空间下；Lua 生态兼容入口通过 `require("xxx")` 提供。

## 通用约定

- **二进制数据**：所有二进制输入/输出均使用 Lua string，例如 `string.char(0, 255)`。
- **错误返回**：运行时失败返回 `nil, err`；参数类型错误使用 Lua 原生参数检查报错。
- **命名空间一致性**：`require("Light.Plugins.Xxx")` 返回的表与 `Light.Plugins.Xxx` 是同一个表。
- **兼容层**：`require("json")`、`require("uuid")`、`require("zlib")`、`require("lfs")`、`require("md5")`、`require("sha1")` 返回普通 Lua table。

## Light.Plugins.Codec

```lua
local Codec = require("Light.Plugins.Codec")
```

- `Codec.Base64Encode(data) -> string`
- `Codec.Base64Decode(text) -> string | nil, err`
- `Codec.HexEncode(data [, upper]) -> string`
- `Codec.HexDecode(text) -> string | nil, err`
- `Codec.URLEncode(text) -> string`
- `Codec.URLDecode(text) -> string | nil, err`

示例：

```lua
local raw = string.char(0, 1, 2, 255)
assert(Codec.HexDecode(Codec.HexEncode(raw)) == raw)
assert(Codec.Base64Decode(Codec.Base64Encode("hello")) == "hello")
```

## Light.Crypto 新增辅助函数

```lua
local Crypto = require("Light.Crypto")
```

- `Crypto.SHA1(data) -> string`
- `Crypto.SHA1_Raw(data) -> string`
- `Crypto.HexEncode(data [, upper]) -> string`
- `Crypto.HexDecode(text) -> string | nil, err`
- `Crypto.CRC32(data) -> number`

示例：

```lua
assert(Crypto.SHA1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#Crypto.SHA1_Raw("abc") == 20)
assert(Crypto.HexEncode(string.char(0, 255)) == "00ff")
```

## Light.Plugins.Path

```lua
local Path = require("Light.Plugins.Path")
```

纯字符串路径工具，不调用文件系统。

- `Path.Join(...) -> string`
- `Path.Normalize(path) -> string`
- `Path.Basename(path) -> string`
- `Path.Dirname(path) -> string`
- `Path.Extname(path) -> string`
- `Path.Stem(path) -> string`
- `Path.IsAbsolute(path) -> boolean`
- `Path.Split(path) -> table`
- `Path.Separator() -> string`

示例：

```lua
local p = Path.Join("foo", "bar", "baz.txt")
assert(Path.Basename(p) == "baz.txt")
assert(Path.Extname(p) == ".txt")
```

## Light.Plugins.UUID

```lua
local UUID = require("Light.Plugins.UUID")
```

- `UUID.V4() -> string`
- `UUID.IsValid(text) -> boolean`
- `UUID.Parse(text) -> string | nil, err`
- `UUID.Format(raw16) -> string | nil, err`

示例：

```lua
local id = UUID.V4()
assert(UUID.IsValid(id))
local raw = assert(UUID.Parse(id))
assert(#raw == 16)
assert(UUID.IsValid(UUID.Format(raw)))
```

## Light.Plugins.JSON

```lua
local JSON = require("Light.Plugins.JSON")
```

- `JSON.Encode(value [, options]) -> string | nil, err`
- `JSON.Decode(text) -> value | nil, err`
- `JSON.Null() -> sentinel`
- `JSON.IsNull(value) -> boolean`

JSON `null` 会映射到 `JSON.Null()` 返回的 sentinel table。

示例：

```lua
local text = assert(JSON.Encode({ name = "choco", ok = true }))
local obj = assert(JSON.Decode(text))
assert(obj.name == "choco")
assert(JSON.IsNull(JSON.Decode("null")))
```

## Light.Plugins.Compress

```lua
local Compress = require("Light.Plugins.Compress")
```

基于 miniz 的 zlib wrapper 压缩。`Decompress` 有 64MB 安全上限。

- `Compress.Compress(data [, level]) -> string | nil, err`
- `Compress.Decompress(data) -> string | nil, err`
- `Compress.Deflate(data [, level]) -> string | nil, err`
- `Compress.Inflate(data) -> string | nil, err`
- `Compress.CRC32(data) -> number`
- `Compress.Adler32(data) -> number`
- `Compress.Version() -> string`

示例：

```lua
local raw = "hello" .. string.char(0, 1, 2, 255)
local packed = assert(Compress.Compress(raw, 6))
assert(Compress.Decompress(packed) == raw)
```

## Light.Filesystem 新增便捷函数

```lua
local FS = require("Light.Filesystem")
```

- `FS.CurrentDir() -> string | nil, err`
- `FS.Exists(path) -> boolean`
- `FS.IsFile(path) -> boolean`
- `FS.IsDirectory(path) -> boolean`
- `FS.List(path) -> table | nil, err`
- `FS.Attributes(path [, key]) -> table|value | nil, err`

## Light.Plugins.Package

```lua
local Package = require("Light.Plugins.Package")
```

资源包读取插件。运行时失败返回 `nil, err`，参数类型错误使用 Lua 原生参数检查报错。二进制条目内容以 Lua string 返回。

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

## 兼容模块

### json

```lua
local json = require("json")
assert(json.decode(json.encode({ x = 1 })).x == 1)
```

- `json.encode(value) -> string | nil, err`
- `json.decode(text) -> value | nil, err`

### uuid

```lua
local uuid = require("uuid")
local id = uuid.v4()
assert(uuid.is_valid(id))
```

- `uuid.v4() -> string`
- `uuid.is_valid(text) -> boolean`

### zlib

```lua
local zlib = require("zlib")
local packed = assert(zlib.compress("abc"))
assert(zlib.decompress(packed) == "abc")
```

- `zlib.compress(data [, level]) -> string | nil, err`
- `zlib.decompress(data) -> string | nil, err`
- `zlib.deflate(data [, level]) -> string | nil, err`
- `zlib.inflate(data) -> string | nil, err`
- `zlib.crc32(data) -> number`
- `zlib.adler32(data) -> number`
- `zlib.version() -> string`

### md5

```lua
local md5 = require("md5")
assert(md5.sumhexa("abc") == "900150983cd24fb0d6963f7d28e17f72")
assert(#md5.sum("abc") == 16)
```

- `md5.sum(data) -> string`
- `md5.sumhexa(data) -> string`

### sha1

```lua
local sha1 = require("sha1")
assert(sha1.sumhexa("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d")
assert(#sha1.sum("abc") == 20)
```

- `sha1.sum(data) -> string`
- `sha1.sumhexa(data) -> string`

### lfs

```lua
local lfs = require("lfs")
local cur = assert(lfs.currentdir())
local attr = assert(lfs.attributes(cur))
assert(attr.mode == "directory" or attr.mode == "file" or attr.mode == "other")
```

- `lfs.attributes(path [, attr]) -> table|value | nil, err`
- `lfs.dir(path) -> iterator | nil, err`
- `lfs.mkdir(path) -> true | nil, err`
- `lfs.rmdir(path) -> true | nil, err`
- `lfs.currentdir() -> string | nil, err`
- `lfs.chdir(path) -> nil, err`

`lfs.chdir` 当前明确返回不支持，避免跨平台工作目录状态带来的隐式副作用。
