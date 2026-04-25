---
name: chocolight-engine
description: ChocoLight 引擎架构规范与开发指南
---

# ChocoLight 引擎技能包

## 引擎定位

ChocoLight 是基于 **Lumen VM**（Lua 5.1 兼容虚拟机）的轻量级跨平台 2D/3D 游戏引擎，采用 **Lua 脚本驱动 + C++ 原生模块** 的双层架构。

## 核心架构

```
┌────────────────────────────────────────────┐
│              Lua Game Scripts              │  ← 业务层
├────────────────────────────────────────────┤
│         Light OOP Framework (Lua)          │  ← 类系统 / metatable
├────────────────────────────────────────────┤
│           ChocoLight DLL / .so             │  ← 原生模块
│  Graphics │ Network │ AV │ UI │ DB │ ...   │
├────────────────────────────────────────────┤
│              Lumen VM (C++17)              │  ← 虚拟机
│   GC │ Parser │ VM │ Compiler │ LNI API    │
├────────────────────────────────────────────┤
│  GLFW │ OpenGL │ FFmpeg │ SQLite │ WinSock │  ← 第三方
└────────────────────────────────────────────┘
```

## 模块清单

| 模块 | 文件 | 功能 | 关键依赖 |
|------|------|------|----------|
| Graphics | `light_graphics.cpp` | 2D/3D 渲染、Canvas FBO、裁剪 | OpenGL 1.x/2.x |
| Image | `light_graphics_image.cpp` | 纹理加载、ImageData、Font | stb_image, stb_truetype |
| Canvas | `light_graphics_canvas.cpp` | 帧缓冲对象 | OpenGL FBO |
| UI | `light_ui.cpp` | GLFW 窗口管理、事件循环 | GLFW 3.4 |
| Network | `light_network.cpp` | HTTP/WebSocket 客户端+服务器 | WinSock2 |
| AV | `light_av.cpp` | 音视频播放 | FFmpeg (动态加载) |
| DB | `light_db.cpp` | SQLite 数据库 | sqlite3 |
| Record | `light_record.cpp` | Lua ORM 框架 | 纯 Lua |
| Data | `light_data.cpp` | 二进制缓冲区 | 无 |
| Plugins | `light_plugins.cpp` | WDF/NEM 游戏格式解析 | 无 |
| Module | `light_module.cpp` | OOP 类系统 + 引擎入口 | 纯 Lua |
| Math | `light_math.cpp` | 数学扩展 | 无 |
| AntiDebug | `light_antidebug.cpp` | 5 层反调试 | Win32 API |

## 目标平台

| 平台 | 构建方式 | 输出 |
|------|----------|------|
| Windows x64 | MSVC / CMake | light.exe + lua51.dll |
| Linux x64 | GCC / CMake | light + liblua51.so |
| macOS Universal | Clang / CMake | light + liblua51.dylib |
| Android | NDK + Gradle | APK (JNI bridge) |
| iOS | Xcode / CMake | .app (ObjC bridge) |
| Web | Emscripten | WASM + .js |

## 关键 API 模式

### OOP (metatable 继承)
```lua
local MyWindow = Light(Light.UI.Window):New()
function MyWindow:Draw() ... end
function MyWindow:Update(dt) ... end
MyWindow:Open(800, 600, "My Game")
```

### 图形绘制
```lua
Light.Graphics.Push()
Light.Graphics.SetColor(r, g, b, a)
Light.Graphics.Rectangle(Light.Graphics.FillMode, x, y, z, w, h, d)
Light.Graphics.Draw(image, x, y, z)
Light.Graphics.Print("Text", font, x, y, z)
Light.Graphics.Pop()
```

### HTTP / WebSocket
```lua
local http = Light(Light.Network.Http):New("example.com", 80)
http:Open()
http:SendRequest("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
function http:OnHttp(status, headers, body) ... end
```

### 数据库 ORM
```lua
local db = Light(Light.DB.SQLite):New("game.db")
local Users = Light(Light.Record):New(db, "users")
Users:Table()  -- CREATE TABLE IF NOT EXISTS
local user = Users:Create()
user.name = "Player1"
user:Insert()
```

## 打包工具

| 工具 | 用途 |
|------|------|
| `lightpack.exe` | 将 Lua 脚本打包进 Windows exe 模板 |
| `lightpack_mobile.exe` | 注入 Lua 到 APK/IPA (支持 `--encrypt` XOR 加密) |
| `lua2header.exe` | Lua 脚本转 C header (用于嵌入编译) |

## 加密保护

- 移动端: CLPK 格式 (XOR + obfuscated master key)
- PC 端: lightpack 内置 XOR 加密 + opcode 重映射
- 运行时: AntiDebug 5 层检测 + 静默异常策略
