# Phase G.2 Lua Utilities 验收记录

## 范围

Phase G.2 交付 Lua 常用工具库绑定，采用双轨兼容策略：

- **正式 API**：`Light.Plugins.Codec`、`Light.Plugins.Path`、`Light.Plugins.UUID`、`Light.Plugins.JSON`、`Light.Plugins.Compress`
- **增强 API**：`Light.Crypto` 增加 `SHA1`、`SHA1_Raw`、`HexEncode`、`HexDecode`、`CRC32`
- **兼容入口**：`require("lfs")`、`require("zlib")`、`require("md5")`、`require("sha1")`、`require("json")`、`require("uuid")`

## 已完成项

- **共享工具层**：新增 `light_utils_core.h`，提供 hex、URL、CRC32、SHA1、UUID、路径和 Lua 注册辅助。
- **Codec**：新增 base64、hex、URL 编解码。
- **Crypto**：补充 SHA1、raw SHA1、hex、CRC32。
- **Path**：新增纯字符串路径工具。
- **UUID**：新增 UUID v4、校验、解析、格式化以及 `uuid` 兼容模块。
- **JSON**：新增 cJSON-backed 编解码、null sentinel、循环检测以及 `json` 兼容模块。
- **Compress**：新增 miniz-backed 压缩/解压、CRC32、Adler32、版本查询以及 `zlib` 兼容模块。
- **Filesystem/LFS**：增强 `Light.Filesystem` 并新增 `lfs`、`md5`、`sha1` 兼容模块。
- **CI smoke**：Windows runtime smoke 已注册 Phase G.2 新增脚本。

## 静态检查

已在每个实现任务后执行对应 `git diff --check`：

- `light_plugins_codec.cpp` / `codec.lua`
- `light_crypto.cpp` / `crypto_utils.lua`
- `light_plugins_path.cpp` / `path.lua`
- `light_plugins_uuid.cpp` / `uuid.lua`
- `light_plugins_json.cpp` / `json.lua`
- `light_plugins_compress.cpp` / `compress.lua`
- `light_filesystem.cpp` / `light_lua_compat.cpp` / `lua_compat_utils.lua`
- `.github/workflows/build-templates.yml`

结果：均通过。

## 本地验证约束

- **未运行本地 CMake build**：遵守项目约束。
- **未运行本地 `light.exe` smoke**：遵守项目约束。
- **Lua 语法检查**：当前 worktree 未发现 `lumen-master/build/src/lightc/Release/lightc.exe`，因此本地 `lightc -p` 检查跳过。
- **运行时 smoke**：已交由 GitHub Actions Windows runtime smoke 执行。

## 关键验收点

- **命名空间**：所有正式新增模块均位于 `Light.Plugins.*`。
- **require 一致性**：smoke 覆盖 `Light.Plugins.Xxx == require("Light.Plugins.Xxx")`。
- **注册模式**：未使用 `luaL_register(L, "Light.Xxx", ...)` 形式，避免 `object is a static module` 问题。
- **二进制安全**：二进制输入输出使用 Lua string 与 `lua_pushlstring`。
- **OpenSSL**：未引入 OpenSSL 默认依赖。
- **miniz**：仅为压缩模块引入 miniz FetchContent。

## Smoke 脚本

新增或扩展的 smoke：

- `scripts/smoke/codec.lua`
- `scripts/smoke/crypto_utils.lua`
- `scripts/smoke/path.lua`
- `scripts/smoke/uuid.lua`
- `scripts/smoke/json.lua`
- `scripts/smoke/compress.lua`
- `scripts/smoke/lua_compat_utils.lua`

其中 Windows runtime smoke 已注册除 `crypto_utils.lua` 外的 Phase G.2 模块集合；`crypto_utils.lua` 可在后续 CI 列表中按需要补入。
