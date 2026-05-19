# Phase G.2 Lua Utilities 最终总结

## 目标

为 ChocoLight 补齐常用 Lua 工具库绑定，并保持两条访问路径：

- **正式 API**：`Light.Plugins.*`
- **Lua 生态兼容入口**：`require("xxx")`

本阶段不把 OpenSSL 加入默认依赖，避免扩大构建矩阵风险。

## 交付内容

### Light.Plugins.Codec

新增文件：`ChocoLight/src/light_plugins_codec.cpp`

- `Base64Encode` / `Base64Decode`
- `HexEncode` / `HexDecode`
- `URLEncode` / `URLDecode`

### Light.Crypto 增强

修改文件：`ChocoLight/src/light_crypto.cpp`

- `SHA1`
- `SHA1_Raw`
- `HexEncode`
- `HexDecode`
- `CRC32`

### Light.Plugins.Path

新增文件：`ChocoLight/src/light_plugins_path.cpp`

- `Join`
- `Normalize`
- `Basename`
- `Dirname`
- `Extname`
- `Stem`
- `IsAbsolute`
- `Split`
- `Separator`

### Light.Plugins.UUID / uuid

新增文件：`ChocoLight/src/light_plugins_uuid.cpp`

- `Light.Plugins.UUID.V4`
- `Light.Plugins.UUID.IsValid`
- `Light.Plugins.UUID.Parse`
- `Light.Plugins.UUID.Format`
- `uuid.v4`
- `uuid.is_valid`

### Light.Plugins.JSON / json

新增文件：`ChocoLight/src/light_plugins_json.cpp`

- `Light.Plugins.JSON.Encode`
- `Light.Plugins.JSON.Decode`
- `Light.Plugins.JSON.Null`
- `Light.Plugins.JSON.IsNull`
- `json.encode`
- `json.decode`

### Light.Plugins.Compress / zlib

新增文件：`ChocoLight/src/light_plugins_compress.cpp`

- `Compress` / `Decompress`
- `Deflate` / `Inflate`
- `CRC32`
- `Adler32`
- `Version`
- `zlib` lowercase compatibility wrappers

### Light.Filesystem / lfs / md5 / sha1

修改文件：`ChocoLight/src/light_filesystem.cpp`
新增文件：`ChocoLight/src/light_lua_compat.cpp`

- `Light.Filesystem.Exists`
- `Light.Filesystem.IsFile`
- `Light.Filesystem.IsDirectory`
- `Light.Filesystem.List`
- `Light.Filesystem.Attributes`
- `Light.Filesystem.CurrentDir`
- `lfs.attributes`
- `lfs.dir`
- `lfs.mkdir`
- `lfs.rmdir`
- `lfs.currentdir`
- `lfs.chdir` 明确返回不支持
- `md5.sum` / `md5.sumhexa`
- `sha1.sum` / `sha1.sumhexa`

## 构建与加载集成

修改文件：

- `ChocoLight/CMakeLists.txt`
- `ChocoLight/include/light.h`
- `lumen-master/src/light/light.cpp`
- `.github/workflows/build-templates.yml`

集成点：

- 新增模块源文件加入 `LIGHT_SOURCES`。
- miniz 通过 FetchContent 引入，参与 `Light` target 编译。
- 新增 `luaopen_*` 声明。
- Windows loader preload 表注册正式模块和兼容模块。
- GitHub Actions Windows runtime smoke 注册 Phase G.2 脚本。

## Smoke 脚本

新增：

- `scripts/smoke/codec.lua`
- `scripts/smoke/crypto_utils.lua`
- `scripts/smoke/path.lua`
- `scripts/smoke/uuid.lua`
- `scripts/smoke/json.lua`
- `scripts/smoke/compress.lua`
- `scripts/smoke/lua_compat_utils.lua`

## 提交记录

本阶段主要提交：

- `87edbb1 feat: add shared Lua utility helpers`
- `a3b16af feat: add Light.Plugins.Codec utilities`
- `244d02e feat: extend Light.Crypto utility hashes`
- `08a7db0 feat: add Light.Plugins.Path utilities`
- `6987983 feat: add Light.Plugins.UUID and uuid compat module`
- `4a37629 feat: add Light.Plugins.JSON and json compat module`
- `de38616 feat: add Light.Plugins.Compress and zlib compat module`
- `5ecdd11 feat: add lfs md5 sha1 compatibility modules`
- `6b880a7 ci: add Phase G.2 Lua utilities smokes`

## 约束执行情况

- **本地 CMake build**：未执行。
- **本地 `light.exe` smoke**：未执行。
- **OpenSSL 默认依赖**：未引入。
- **正式命名空间**：全部使用 `Light.Plugins.*`。
- **兼容层**：按 Lua 生态小写模块名暴露。
- **注册模式**：避免 `luaL_register(L, "Light.Xxx", ...)`，使用局部表注册或 `RegisterPluginsSubmodule`。
