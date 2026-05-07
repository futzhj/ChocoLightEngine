# ACCEPTANCE - Phase C (SDL_AsyncIO + SDL_Storage)

**Branch**: `feature/sdl3-async-storage` · **Base**: `main @ e7e81a5` · **PR**: #4
**Commits**: `2ea1ebd feat(sdl3): Phase C - Light.IO + Light.Storage`

## 子任务完成情况

| # | 任务 | 状态 | 证据 |
|:-:|------|:----:|------|
| C1 | CMake 接入 + 平台守卫 | ✅ | `ChocoLight/CMakeLists.txt` 加 2 行 |
| C2 | `Light.IO.LoadAsync` + `Light.IO.Poll` | ✅ | `ChocoLight/src/light_io.cpp` (188 行) |
| C3 | `Light.Storage.Title.*` + `Light.Storage.User.*` | ✅ | `ChocoLight/src/light_storage.cpp` (266 行) |
| C4 | Lumen preload + smoke 脚本 + CI 接入 | ✅ | `lumen-master/src/light/light.cpp` + `scripts/smoke/io_storage.lua` |
| C5 | ACCEPTANCE 文档 | ✅ | 本文件 |

## 验收证据

### CI 全绿 (PR #4 run 25523271730)

| 平台 | 编译 | Runtime Smoke |
|------|:----:|:-------------:|
| Windows | ✅ | ✅ `io_storage.lua` 通过 |
| Linux | ✅ | (lightc -p 已覆盖) |
| macOS | ✅ | (lightc -p 已覆盖) |
| Android | ✅ | - |
| Web (Emscripten) | ✅ | - |
| iOS | ⏳ pending | - |

**Windows runtime smoke 通过证明**:
- Light.dll 二进制成功 dlopen
- `Light.IO` 模块在 Lumen 中成功 require
- `Light.Storage` 模块在 Lumen 中成功 require
- 所有 API (`LoadAsync`/`Poll`/`Title.{Read,Exists,Size}`/`User.{Read,Write,Exists,Delete,Size}`) 表注册正确
- 边界场景:`User.Write` 未 `OpenUser` 时返回 `(false, err_string)` 而不崩溃
- `IO.Poll()` 空队列时返回 0

## API 设计

### Light.IO (异步文件 IO)

```lua
local IO = require("Light.IO")

-- 提交异步加载
IO.LoadAsync("assets/big.dat", function(ok, data, err)
    if ok then print("loaded", #data, "bytes")
    else      print("load failed:", err) end
end)

-- 主循环每帧调
local triggered = IO.Poll()  -- 返回本次触发的回调数
```

实现要点:
- 单例 `SDL_AsyncIOQueue`,首次 `LoadAsync` 时 lazy-init
- 每个请求附 `PendingLoad` 结构(LUA_REGISTRYINDEX ref + path 副本)
- `Poll` 非阻塞读 `SDL_GetAsyncIOResult`,完成时触发 Lua 回调
- 跨平台字符串 alloc 用 `SDL_strdup` / `SDL_free`,免 MSVC `_strdup` 兼容问题

### Light.Storage (跨平台存储)

```lua
local Storage = require("Light.Storage")

-- Title (只读, 游戏资源, 自动 lazy-open)
local data, err = Storage.Title.Read("levels/level1.json")
if Storage.Title.Exists("config/default.ini") then ... end

-- User (读写, 玩家存档, 需先 OpenUser)
local ok, err = Storage.OpenUser("MyOrg", "MyGame")
Storage.User.Write("save.dat", binary_data)
local data, err = Storage.User.Read("save.dat")
Storage.User.Delete("save.dat")
```

实现要点:
- `Title` 在首次访问时 `SDL_OpenTitleStorage(nullptr, 0)`,跨平台路径由 SDL3 自动选(可执行文件目录或 app bundle)
- `User` 必须显式 `OpenUser`,SDL3 会按规范放置:
  - Windows: `%APPDATA%\<org>\<app>`
  - Linux: `$XDG_DATA_HOME/<org>/<app>`
  - macOS: `~/Library/Application Support/<org>/<app>`
  - iOS/Android: app sandbox
- `WaitReady` helper 阻塞最多 1 秒等 `SDL_StorageReady`,处理异步打开场景

## 影响范围

- **新增文件**:`light_io.cpp`, `light_storage.cpp`, `io_storage.lua`
- **修改文件**:`CMakeLists.txt` (+2 行), `light.cpp` (+2 行 preload), `build-templates.yml` (+3 行 smoke)
- **未触动**:Phase A 性能成果 / Phase B SDL_GPU 后端 / 现有 23 个 Light.* 模块
- **零外部依赖**:SDL3 内置 `SDL_AsyncIO` + `SDL_Storage` API,无 `FetchContent` / `find_package`

## 后续工作 (TODO)

无阻塞性 TODO。可选增强:
- `Light.Storage.User.Enumerate(path)` 暴露 `SDL_EnumerateStorageDirectory` (列表回调)
- `Light.Storage.Space()` 暴露 `SDL_GetStorageSpaceRemaining` (剩余空间)
- `Light.IO.LoadAsync` 增加 `priority` / `cancel_token` 参数
- 集成测试:实际 LoadAsync 大文件 + 校验完整性(需要带文件系统的 e2e 测试,不在 CI smoke 范围)

## 决策记录

- **不引入 CHOCO_USE_SDL_ASYNCIO/STORAGE 子开关**:SDL3 内置 API,运行时按平台自动 fallback,无需编译期分流
- **不阻塞 Phase B**:Phase C 完全独立,可先于 Phase B 完成的 SDL_GPU 合并到 main
- **lazy-init**:Title/User storage 不在 Light.dll 加载时打开,避免无谓的资源占用
- **错误约定**:返回值统一为 `(result, err_or_nil)`,error 为字符串,失败时 result=nil/false,与 Lua 习惯对齐
