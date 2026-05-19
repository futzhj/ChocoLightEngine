# Phase G.2 Lua Utilities TODO

## 必须跟进

- **CI 结果回填**：推送后等待 GitHub Actions 完成，若失败按日志定位并补丁修复。
- **运行时 smoke 确认**：Phase G.2 Windows runtime smoke 已注册，需要 CI 验证真实执行结果。

## 可选增强

- **OpenSSL 可选后端**：作为独立可选 backend 引入，不进入默认依赖图。
- **zlib streaming API**：补 `deflate/inflate` 流式对象接口，兼容更完整的 Lua zlib 用法。
- **`cjson` 兼容别名**：可追加 `require("cjson")`，映射到现有 JSON 编解码。
- **HMAC/PBKDF2**：若后续脚本生态需要，可在 `Light.Crypto` 或 `Light.Plugins.Crypto` 中新增。
- **Expanded `lfs.chdir`**：如能找到安全跨平台策略，再实现真实 chdir；当前明确返回 `nil, "chdir: not supported"`。
- **JSON options**：`JSON.Encode(value [, options])` 当前预留 options 参数，后续可加 pretty-print、array/object 策略等。
- **压缩安全上限配置**：当前 `Decompress` 使用 64MB 安全上限，后续可暴露可配置参数。

## 文档跟进

- **主 README**：如需要，可在主 README 的 Lua API 区域链接 `docs/api/Light_Plugins.md`。
- **兼容性表**：后续可补完整 LuaFileSystem / zlib / md5 / sha1 / json / uuid API 对照表。

## 已知限制

- **未本地编译**：遵守项目约束，编译结果以 CI 为准。
- **未本地运行 smoke**：遵守项目约束，运行时结果以 CI 为准。
- **miniz FetchContent**：首次 CI 构建会拉取 miniz 3.0.2。
- **JSON 字符串二进制限制**：cJSON 字符串按 C 字符串处理，不保证 NUL 字节内嵌 JSON 字符串往返。
- **UUID 随机源**：当前使用 C++ `std::random_device` + `mt19937_64`，适合工具级 UUID v4，不声明为密码学安全随机。
