# Phase G.2 — Lua 常用工具库双轨兼容设计

## 1. 背景与目标

ChocoLight 当前已经提供大量 `Light.*` 引擎模块，并已有 `Light.Crypto` 与 `Light.Filesystem`：

- `Light.Crypto` 已覆盖 `MD5`、`SHA256`、`AES256`、`Base64`、随机字节与简化密码派生。
- `Light.Filesystem` 已覆盖路径发现、创建/删除/重命名/复制、路径信息、目录 glob。
- 当前尚未提供 Lua 生态常见包名入口，例如 `require("lfs")`、`require("zlib")`、`require("md5")`、`require("json")`、`require("uuid")`。
- 当前第三方目录没有 `zlib/miniz`，也不默认接入 OpenSSL。

本阶段目标是补齐 Lua 常用工具库能力，并采用“双轨兼容”策略：

1. **正式 API** 放在 `Light.Plugins.*` 命名空间下，保持 ChocoLight 插件风格。
2. **兼容 API** 支持常见 Lua 包名，方便迁移既有 Lua 生态代码。
3. **本期默认不接 OpenSSL**，避免跨平台 CI、移动端、Web 端构建复杂度上升。
4. 继续遵守 ChocoLight 现有工作流：不本地 CMake build，不本地运行 `light.exe` smoke；提交后由 GitHub Actions 验证。

## 2. 范围拆分

Phase G.2 按三段实施，避免一次性引入过大改动。

### 2.1 Phase G.2.0 Core Utils

补齐正式 `Light.Plugins.*` API：

- `Light.Plugins.Compress`
- `Light.Plugins.Codec`
- `Light.Plugins.Path`
- `Light.Plugins.UUID`
- `Light.Plugins.JSON`
- 增强 `Light.Crypto`
- 增强 `Light.Filesystem`

目标是先建立稳定的底层能力和 smoke 覆盖。

### 2.2 Phase G.2.1 Lua Compat Layer

在 `Light.Plugins.*` 能力稳定后，增加 Lua 生态兼容入口：

- `require("lfs")`
- `require("zlib")`
- `require("md5")`
- `require("sha1")`
- `require("json")`
- `require("uuid")`

兼容层只做薄封装，优先转发到 `Light.Plugins.*`，避免两套实现分叉。

### 2.3 Phase G.2.2 Extended Utils

补充扩展工具：

- 表序列化与反序列化辅助
- deep copy
- table merge
- table equals
- pretty print
- OpenSSL 可选增强后端设计预留

OpenSSL 不进入默认依赖，仅作为后续可选模块候选，例如 `Light.Crypto.OpenSSL`。

## 3. API 设计

### 3.1 `Light.Plugins.Compress`

职责：压缩、解压与 CRC 校验。

建议 API：

- `Compress(data [, level]) -> string | nil, err`
- `Decompress(data) -> string | nil, err`
- `Deflate(data [, level]) -> string | nil, err`
- `Inflate(data) -> string | nil, err`
- `CRC32(data) -> integer`
- `Adler32(data) -> integer`
- `Version() -> string`

实现建议：

- 引入 `miniz` 单文件库，优先保证 Windows/Linux/macOS/Android/iOS/Web 都能构建。
- 压缩等级 clamp 到库支持范围。
- 二进制输入输出全部使用 Lua string，测试中用 `string.char()` 构造字节数据。

### 3.2 `Light.Plugins.Codec`

职责：文本/二进制编码工具。

建议 API：

- `Base64Encode(data) -> string`
- `Base64Decode(text) -> string | nil, err`
- `HexEncode(data [, upper]) -> string`
- `HexDecode(hex) -> string | nil, err`
- `URLEncode(text) -> string`
- `URLDecode(text) -> string | nil, err`

实现建议：

- `Base64` 可复用现有 `Light.Crypto` 的实现，也可以迁移公共 helper，避免重复代码。
- `HexDecode` 需校验奇数长度和非法字符。
- `URLDecode` 遇到非法 `%XX` 返回 `nil, err`，不静默吞错。

### 3.3 `Light.Crypto` 增强

职责：轻量哈希与常用安全工具。

现有能力保留：

- `MD5`
- `MD5_Raw`
- `SHA256`
- `SHA256_Raw`
- `AES256_Encrypt`
- `AES256_Decrypt`
- `Base64Encode`
- `Base64Decode`
- `RandomBytes`
- `RandomHex`
- `KeyFromPassword`

建议新增：

- `SHA1(data) -> string`
- `SHA1_Raw(data) -> string`
- `CRC32(data) -> integer`，可转发 `Light.Plugins.Compress.CRC32`
- `HexEncode(data [, upper]) -> string`，可转发 `Light.Plugins.Codec.HexEncode`
- `HexDecode(hex) -> string | nil, err`

可选延后：

- `HMAC_SHA256(key, data)`
- `PBKDF2_HMAC_SHA256(password, salt, iterations, keyLen)`

OpenSSL 不作为默认实现来源。

### 3.4 `Light.Plugins.Path`

职责：纯字符串路径处理，不执行文件系统 IO。

建议 API：

- `Join(...) -> string`
- `Normalize(path) -> string`
- `Basename(path) -> string`
- `Dirname(path) -> string`
- `Extname(path) -> string`
- `Stem(path) -> string`
- `IsAbsolute(path) -> boolean`
- `Split(path) -> table`
- `Separator() -> string`

实现建议：

- 接受 `/` 与 `\`，输出默认使用当前平台分隔符，兼容层可保留 Lua 常见行为。
- `Normalize` 处理 `.`、`..`、重复分隔符。
- 不访问磁盘，避免与 `Light.Filesystem` 职责重叠。

### 3.5 `Light.Plugins.UUID`

职责：UUID 生成与校验。

建议 API：

- `V4() -> string`
- `IsValid(s) -> boolean`
- `Parse(s) -> string | nil, err`，返回 16 字节 raw string
- `Format(raw16) -> string | nil, err`

实现建议：

- 使用 `Light.Crypto.RandomBytes(16)` 同等随机来源。
- 设置 UUID v4 的 version/variant 位。
- `Parse` 支持标准 36 字符格式。

### 3.6 `Light.Plugins.JSON`

职责：Lua table 与 JSON 文本互转。

建议 API：

- `Encode(value [, options]) -> string | nil, err`
- `Decode(text) -> value | nil, err`
- `Null() -> light-json-null-sentinel`
- `IsNull(value) -> boolean`

映射规则：

- Lua string/number/boolean 映射到 JSON 基本类型。
- Lua nil 不可作为数组元素直接编码；object 字段为 nil 时跳过。
- array 判定：连续正整数键 `1..n` 且无其他键。
- object 判定：其他 table 作为 JSON object，key 必须可转字符串。
- JSON null 解码为 `Light.Plugins.JSON.Null()` sentinel，避免与 Lua nil 混淆。

实现建议：

- 优先复用现有网络 RPC 中已使用的 cJSON 能力，或引入轻量单文件 JSON。
- 编码时需要循环引用检测，发现循环返回 `nil, err`。

### 3.7 `Light.Filesystem` 增强

职责：为 `lfs` 兼容层补足缺口。

建议新增或确认：

- `Exists(path) -> boolean`
- `IsFile(path) -> boolean`
- `IsDirectory(path) -> boolean`
- `List(path) -> table | nil, err`
- `Attributes(path) -> table | nil, err`
- `CurrentDir() -> string | nil, err`，可作为 `GetCurrentDirectory` 别名

若现有 API 已能表达，则优先在兼容层转发，不强行重复新增。

## 4. Lua 生态兼容层设计

兼容层以薄封装为原则，不复制核心逻辑。

### 4.1 `lfs`

建议函数：

- `lfs.attributes(path [, attr])`
- `lfs.dir(path)`
- `lfs.mkdir(path)`
- `lfs.rmdir(path)`
- `lfs.currentdir()`
- `lfs.chdir(path)` 若底层无安全实现，本期可返回 `nil, "not supported"`

注意：

- `lfs.dir(path)` 需要返回 iterator 风格，尽量兼容 LuaFileSystem。
- `attributes` 字段名尽量匹配 `mode`、`size`、`modification` 等常见字段。

### 4.2 `zlib`

建议函数：

- `zlib.compress(data [, level])`
- `zlib.decompress(data)`
- `zlib.deflate(data [, level])`
- `zlib.inflate(data)`
- `zlib.crc32(data [, crc])`

注意：

- 本期优先实现一次性字符串 API。
- 流式 compressor/decompressor 可放到后续阶段。

### 4.3 `md5` / `sha1`

建议函数：

- `md5.sum(data) -> raw16`
- `md5.sumhexa(data) -> hex32`
- `sha1.sum(data) -> raw20`
- `sha1.sumhexa(data) -> hex40`

注意：

- `md5` 可直接转发现有 `Light.Crypto.MD5_Raw/MD5`。
- `sha1` 需要新增底层 SHA1。

### 4.4 `json`

建议函数：

- `json.encode(value)`
- `json.decode(text)`
- `json.null`

注意：

- 若需要兼容不同生态，可后续加 `cjson` 别名。
- 第一阶段只注册 `json`，避免一次性兼容过多方言。

### 4.5 `uuid`

建议函数：

- `uuid.v4()`
- `uuid.is_valid(s)`

薄封装 `Light.Plugins.UUID`。

## 5. 模块注册与加载

### 5.1 正式模块

新增 `Light.Plugins.*` 子模块应加入：

- `ChocoLight/src/*.cpp`
- `ChocoLight/CMakeLists.txt`
- `ChocoLight/include/light.h` 声明，若项目决定继续维护权威声明表
- `lumen-master/src/light/light.cpp` 的 `g_lightModules`

注册方式必须避开 ChocoLight OOP 根对象陷阱，并沿用现有 `Light.Plugins.WDFData` 模式：

- 先 `LT::EnsureLightTable(L)`
- 确保 `Light.Plugins` 父表存在
- 创建或复用 `Light.Plugins.Xxx` 子表
- 在子表上使用 `luaL_setfuncs` 或 `luaL_register(L, NULL, funcs)` 注册函数
- 返回该子表，保证 `require("Light.Plugins.Xxx")` 与 `Light.Plugins.Xxx` 指向一致
- 不使用 `luaL_register(L, "Light.Plugins.XXX", funcs)`

### 5.2 兼容模块

兼容模块建议独立命名：

- `luaopen_lfs`
- `luaopen_zlib`
- `luaopen_md5`
- `luaopen_sha1`
- `luaopen_json`
- `luaopen_uuid`

在 preload 中注册包名：

- `lfs`
- `zlib`
- `md5`
- `sha1`
- `json`
- `uuid`

兼容模块内部可以直接调用共享 helper，或通过 C++ helper 层复用 `Light.Plugins.*` 底层实现，不依赖 Lua 层 `require("Light.Plugins.X")`，避免加载顺序问题。

## 6. 错误处理策略

统一原则：

- 参数类型错误：使用 `luaL_check*` 抛 Lua error，符合现有简单参数 API 风格。
- 运行时失败：返回 `nil, err`。
- 布尔操作：成功返回 `true`，失败返回 `false, err`。
- 二进制输出：使用 `lua_pushlstring`，不能使用 `lua_pushstring`。

## 7. 测试策略

新增 smoke 建议：

- `scripts/smoke/compress.lua`
- `scripts/smoke/codec.lua`
- `scripts/smoke/path.lua`
- `scripts/smoke/json.lua`
- `scripts/smoke/uuid.lua`
- `scripts/smoke/lua_compat_utils.lua`

覆盖点：

- API surface 存在性。
- round-trip：compress/decompress、hex encode/decode、base64 encode/decode、json encode/decode。
- 二进制字符串边界：必须用 `string.char()`。
- 错误输入：非法 hex、非法 URL escape、损坏压缩数据、JSON 语法错误。
- `require("lfs")` 等兼容模块可加载。
- Lua 5.1 兼容，不使用 `\xNN` 字符串转义。

CI 注册：

- 将 smoke 加入 `.github/workflows/build-templates.yml` 的 Windows runtime smoke 段。
- 不本地运行 `light.exe`。

## 8. 文档策略

建议新增文档目录：

- `docs/Phase G.2 Lua Utilities/ALIGNMENT_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/DESIGN_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/TASK_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/ACCEPTANCE_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/FINAL_PhaseG_2.md`
- `docs/Phase G.2 Lua Utilities/TODO_PhaseG_2.md`

API 文档应同步到现有 API reference 或 `docs/api` 下相应文件。

## 9. 非目标

本阶段不做：

- OpenSSL 默认依赖。
- TLS/HTTPS 栈重构。
- RSA/ECDSA/X509/证书验证。
- zlib 流式 compressor/decompressor。
- 完整 LuaRocks 生态移植。
- 本地 CMake 编译或本地 runtime smoke。

## 10. 风险与缓解

| 风险 | 缓解 |
|------|------|
| 跨平台库引入失败 | 优先单文件、无系统依赖库，如 `miniz` |
| 兼容 API 与正式 API 分叉 | 兼容层只做薄封装，共享 C++ helper |
| JSON table 映射歧义 | 明确 array/object/null 规则，并写 smoke |
| Lua 5.1 二进制字符串测试错误 | 统一使用 `string.char()` |
| 模块注册触发 `object is a static module` | 禁止 `luaL_register(L, "Light.XXX", ...)` |
| 一次性范围过大 | 拆成 G.2.0/G.2.1/G.2.2 |

## 11. 成功标准

Phase G.2 完成后应满足：

1. `Light.Plugins.Compress/Codec/Path/UUID/JSON` 可正常 require 并使用。
2. `Light.Crypto` 与 `Light.Filesystem` 得到必要增强。
3. `require("lfs")/require("zlib")/require("md5")/require("sha1")/require("json")/require("uuid")` 可用。
4. 所有新增 smoke 在 GitHub Actions Windows runtime smoke 通过。
5. 全平台 CI 构建保持通过。
6. OpenSSL 未成为默认依赖。
