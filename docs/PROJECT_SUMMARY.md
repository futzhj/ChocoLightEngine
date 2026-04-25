# ChocoLight Engine — 项目总结

> 轻量级跨平台 Lua 游戏引擎 · C++17 · 逆向还原重建  
> 仓库: `https://github.com/futzhj/ChocoLightEngine`

---

## 1. 项目概述

ChocoLight 是一款以 **Lua 脚本** 为核心的轻量级跨平台游戏引擎，底层使用 C++17 实现，核心虚拟机为 **Lumen**（Lua 5.1 的现代化 C++17 重实现）。引擎通过 DLL 形式提供功能模块，由 Lumen 运行时加载并执行 Lua 脚本驱动游戏逻辑。

**核心特点:**
- **逆向还原**: 整个引擎从原始 Light.dll 通过 IDA 反编译逐字节还原
- **模块化架构**: 功能按 `Light.*` 命名空间组织为独立 Lua 模块
- **嵌入式 OOP**: 内置 7276 字节 Lua OOP 框架，支持原型链继承、运算符重载
- **全平台**: Windows / Linux / macOS / Android / iOS / Web(WASM)，CI/CD 全覆盖

---

## 2. 技术栈

| 层级 | 技术 | 说明 |
|------|------|------|
| **语言** | C++17 + Lua 5.1 | 引擎核心 C++，游戏逻辑 Lua |
| **虚拟机** | Lumen | Lua 5.1 C++17 重实现，含 5.2/5.3 扩展 API |
| **原生接口** | LNI (Lumen Native Interface) | Handle-based C++17 API，灵感源自 JNI |
| **窗口** | GLFW 3.x | 跨平台窗口/输入管理 |
| **渲染** | OpenGL (3.3 Core / 2.1 回退) | 运行时检测 GL 版本自动选择 |
| **音频** | miniaudio + FFmpeg | miniaudio 优先，FFmpeg 回退 |
| **视频** | FFmpeg 5.x (动态加载) | avformat-59 / avcodec-59 / avutil-57 / swscale / swresample |
| **网络** | libuv | HTTP 1.1 + WebSocket (RFC 6455) |
| **数据库** | SQLite3 (WAL 模式) | 嵌入式关系数据库 |
| **图像** | stb_image / stb_truetype | 纹理加载 / 字体渲染 |
| **构建** | CMake | 跨平台构建系统 |
| **CI/CD** | GitHub Actions | 6 平台自动构建 + Release |

---

## 3. 架构设计

### 3.1 整体架构

```
┌─────────────────────────────────────────────────────┐
│                    Lua 脚本层                         │
│    main.lua → Light(Light.UI.Window):New()           │
│    Light.Graphics.Draw / Light.AV.Play / ...         │
├─────────────────────────────────────────────────────┤
│              嵌入式 OOP 框架 (7276 bytes)              │
│    New / Extends / Is / Mixin / Clone / Cast         │
│    原型链继承 · 运算符重载 · 方法缓存                    │
├─────────────────────────────────────────────────────┤
│                Light.dll 模块层 (C++17)                │
│  ┌──────┐ ┌──────┐ ┌─────┐ ┌──────┐ ┌───────┐      │
│  │  UI  │ │ GFX  │ │ AV  │ │  DB  │ │Network│      │
│  └──────┘ └──────┘ └─────┘ └──────┘ └───────┘      │
│  ┌──────┐ ┌──────┐ ┌───────┐ ┌────────┐            │
│  │ Data │ │Record│ │Plugins│ │AntiDbg │            │
│  └──────┘ └──────┘ └───────┘ └────────┘            │
├─────────────────────────────────────────────────────┤
│           渲染/音频/网络 后端抽象层                      │
│  RenderBackend · AudioBackend · PlatformNet          │
├─────────────────────────────────────────────────────┤
│              Lumen VM (lua51.dll)                     │
│    C++17 Lua 5.1 · LNI · 高性能哈希                   │
├─────────────────────────────────────────────────────┤
│                操作系统 / 硬件                          │
│  Windows · Linux · macOS · Android · iOS · Web       │
└─────────────────────────────────────────────────────┘
```

### 3.2 模块注册机制

所有模块通过 `LT::RegisterModule()` 统一注册到全局 `Light` 表:

```cpp
// 通用注册模式
void LT::RegisterModule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    EnsureLightTable(L);      // 确保 Light 表存在 (首次调用加载 OOP 框架)
    // 在 Light 表上创建子表并注册函数
}
```

### 3.3 模块清单

| 模块 | 入口函数 | 功能 | 实现方式 |
|------|---------|------|---------|
| `Light` | `luaopen_Light` | 核心入口 + OOP 框架 | C++ + 嵌入 Lua |
| `Light.Debug` | `luaopen_Light_Debug` | 调试工具 | C++ |
| `Light.Data` | `luaopen_Light_Data` | 二进制缓冲区 | C++ |
| `Light.Math` | `luaopen_Light_Math` | 数学运算 | C++ |
| `Light.UI` | `luaopen_Light_UI` | 事件循环 (Loop/Resume) | C++ |
| `Light.UI.Window` | `luaopen_Light_UI_Window` | GLFW 窗口管理 (12 函数) | C++ |
| `Light.Graphics` | `luaopen_Light_Graphics` | 2D/3D 图形渲染 | C++ |
| `Light.AV` | `luaopen_Light_AV` | 音视频播放基类 | C++ |
| `Light.AV.Audio` | `luaopen_Light_AV_Audio` | 音频加载/播放 | C++ |
| `Light.AV.AudioData` | `luaopen_Light_AV_AudioData` | PCM 数据访问 | C++ |
| `Light.AV.Video` | `luaopen_Light_AV_Video` | 视频解码/渲染 | C++ |
| `Light.DB.SQLite` | `luaopen_Light_DB_SQLite` | SQLite3 接口 | C++ |
| `Light.Network` | `luaopen_Light_Network` | HTTP/WebSocket | C++ + 嵌入 Lua |
| `Light.Record` | `luaopen_Light_Record` | ORM 框架 | 纯嵌入 Lua |
| `Light.Plugins` | `luaopen_Light_Plugins` | 游戏格式 (WDF/NEM/WAS) | C++ |

---

## 4. 核心模块详解

### 4.1 OOP 框架 (`light_module.cpp`)

嵌入 7276 字节的纯 Lua 实现，提供完整的面向对象编程能力:

- **原型链继承**: `Light(Proto):Extends(Base)` → 通过 `getmetatable` 链式查找
- **实例化**: `Light(Proto):New(...)` → 创建新实例 + 调用 `__call` 构造器
- **类型检查**: `Light(obj):Is(Proto)` → 沿原型链匹配
- **运算符重载**: `__add / __sub / __mul / __div / __concat`
- **Mixin**: `Light(obj):Mixin(ext)` → 混入非双下划线方法
- **方法缓存**: `SetMethodCache(true)` → 首次查找后缓存到实例

```lua
-- 使用示例
local MyWindow = Light(Light.UI.Window):New()
function MyWindow:OnOpen() ... end
function MyWindow:Draw() ... end
MyWindow:Open(800, 600, "My Game")
```

### 4.2 图形系统 (`light_graphics.cpp` + `render_backend.cpp`)

**渲染后端抽象层**: 运行时检测 GL 版本 (3.3 Core → 2.1 回退)，通过 `RenderBackend` 接口统一调度。

**绘图 API** (24 个 Lua 函数):

| 类别 | 函数 |
|------|------|
| **基元** | Draw, DrawQuad, DrawSprite, Print, Line, Triangle, Rectangle, RoundedRectangle, Quad, Polygon, Arc, Circle |
| **变换** | Push, Pop, Translate, Rotate, Scale |
| **状态** | GetColor, SetColor, GetCanvas, SetCanvas, GetScissor, SetScissor |
| **常量** | `LineMode=1`, `FillMode=2` |

**文本渲染**:
- UTF-8 解码 (支持 1-4 字节序列)
- 动态字形缓存 (stb_truetype → OpenGL 纹理)
- 自定义字体加载 (`Light.Graphics.Font`)

**精灵渲染**:
- WAS 帧数据 → 懒创建 GL 纹理 → 缓存到 Lua 表 `__texId`
- 支持 hotspot 偏移、变换参数

### 4.3 窗口管理 (`light_ui.cpp`)

**Window 模块** (12 函数精确匹配 IDA 注册表):

```
Open(w, h, title)  — 创建 GLFW 窗口 (支持 1-3 参数重载)
Close()            — 关闭窗口
ID()               — 获取原生窗口句柄
Get/SetWidth       — 宽度存取
Get/SetHeight      — 高度存取
Get/SetDimensions  — 尺寸存取 (返回 w, h)
SetVSync(bool)     — 垂直同步开关
__call()           — 事件循环步进 (清屏 → Draw → Update(dt) → SwapBuffers)
```

**事件回调**: 键盘 (`OnKey`) / 鼠标按钮 (`OnMouseButton`) / 鼠标移动 (`OnMousePosition`) 通过 GLFW 回调转发到 Lua 方法。

**主循环模式**:
```lua
while Light.UI.Loop() do
  Light.UI.Resume()   -- Poll事件 → 反调试检查 → Window:__call → 渲染
end
```

### 4.4 音视频系统 (`light_av.cpp`)

**FFmpeg 动态加载**: 运行时通过 `LoadLibraryA` + `GetProcAddress` 加载 5 个 DLL，避免编译期依赖。

| DLL | 版本 | 用途 |
|-----|------|------|
| avformat-59.dll | 5.x | 容器格式解析 |
| avcodec-59.dll | 5.x | 编解码器 |
| avutil-57.dll | 5.x | 工具函数 |
| swresample-4.dll | 4.x | 音频格式转换 |
| swscale-9.dll | 9.x | 视频色彩空间转换 |

**音频双后端**:
1. **miniaudio** (优先) — 原生音频播放
2. **FFmpeg + PCM** (回退) — 解码后通过 AudioBackend 播放

**视频播放**: FFmpeg 解码 → swscale YUV→RGBA → OpenGL 纹理 → 帧同步渲染
- 支持 waveOut 音频同步
- 环形缓冲区 (64 槽 × 4096 样本)
- 安全探针 (`ProbeCodecPar`): 在 AVStream 内存中扫描 codecpar，避免依赖 FFmpeg 结构体布局

### 4.5 网络系统 (`light_network.cpp`)

**HTTP 1.1**:
- 客户端: `Http.Open(host, port)` → `Http.SendRequest(method, path, headers, body)`
- 服务器: `HttpServer.Open(host, port)` → Lua 回调处理请求

**WebSocket (RFC 6455)**:
- `Http.Upgrade()` — HTTP → WebSocket 协议升级
- `Http.SendMessage()` — 帧发送
- 支持掩码、分片、控制帧

**嵌入式 Web 微框架** (纯 Lua):
- 路由注册 (`app:get/post/put/delete`)
- 会话管理
- WebSocket 聊天示例

### 4.6 数据库 (`light_db.cpp` + `light_record.cpp`)

**SQLite3 接口**:
- `Execute(sql)` — SQL 执行 + 结果回调
- `Escape(str)` — SQL 字符串转义
- `Blob(data)` — BLOB 字面量
- `TypeName(id)` — 类型 ID → SQL 类型名映射 (ORM 用)
- WAL 模式 (`PRAGMA journal_mode=WAL`)

**纯 Lua ORM (`Light.Record`)**:
- 14 种字段类型: Serial(0) ~ TimeStamp(13)
- `Where` 条件构建器 (Same/Greater/Less/Or 变体，链式调用)
- `Record` 单行操作 (Insert/Update/Delete)
- `Records` 表级管理 (Table/Fetch/FetchOne/FetchPage/Find/Count/PageCount)
- 事务支持 (Begin/Commit/RollBack)

### 4.7 数据缓冲区 (`light_data.cpp`)

提供 Lua 层的二进制数据操作能力:

- **栈操作**: Push / Pop
- **随机访问**: Insert / Delete / At / `__index` / `__newindex`
- **头部操作**: Shift / Unshift
- **类型化推入**: si8/si16/si32/si64/ui8/ui16/ui32/ui64/fl32/dbl/byte/char/void
- **指针交互**: GetPointer → lightuserdata (供 C 层使用)

### 4.8 游戏格式插件 (`light_plugins.cpp`)

**WDF (NetEase PFDW 资源包)**:
- Magic: `"PFDW"` (4 字节)
- FNV-1a 哈希表索引
- 资源解码: 字节翻转 + XOR 0x5A
- 提取 API: GetRaw / GetTGA / GetImage / GetAudio / GetSpriteSheet

**WAS 精灵帧**:
- Magic: `0x5053` ('SP')
- RLE 压缩解码
- RGBA 像素还原
- 帧数组 + hotspot 偏移

**NEM 2D 地图**:
- 网格地图数据
- 障碍物检测
- 基础寻路算法

---

## 5. 安全系统

### 5.1 脚本保护

**CLPK 加密格式** (76 字节头):
```
[4B] magic: "CLPK"
[4B] version: 1
[4B] payload_size
[1B] xor_key_len
[63B] xor_key (用 master key 加密, 零填充)
[NB] XOR 加密的 Lua 脚本数据
```

**双密钥体系**:
- **DLL 主密钥**: `"LightDLL!Secure7"` (16 字节, XOR 混淆存储)
- **移动端主密钥**: `"ChocoLight2026!@#"` (17 字节, XOR 混淆存储)

### 5.2 反调试 (`light_antidebug.cpp`)

5 层检测机制:

| 层级 | 方法 | Windows API |
|------|------|------------|
| 1 | API 检测 | `IsDebuggerPresent()` |
| 2 | 远程调试 | `CheckRemoteDebuggerPresent()` |
| 3 | 内核查询 | `NtQueryInformationProcess(ProcessDebugPort)` |
| 4 | 时序异常 | `QueryPerformanceCounter` 间隔检测 |
| 5 | 硬件断点 | `GetThreadContext` 检查 DR0-DR3 |

**静默异常策略**: 
- 检测到调试器不会崩溃，而是渐进增大 `anomalyFactor`
- 在渲染循环中随机跳过帧 (`af * 30%` 概率)
- 使调试体验逐渐恶化但不暴露检测逻辑

### 5.3 其他安全措施
- 二进制名验证 (`CheckBinaryName`)
- Opcode 重映射 (Lumen VM 层)
- Nuitka C 编译保护

---

## 6. 跨平台支持

### 6.1 平台矩阵

| 平台 | 架构 | 构建方式 | CI/CD |
|------|------|---------|-------|
| Windows | x64 | CMake + MSVC | ✅ windows-latest |
| Linux | x64 | CMake + GCC | ✅ ubuntu-latest |
| macOS | Universal (x86_64+arm64) | CMake + Clang | ✅ macos-latest |
| Web | WASM | Emscripten + CMake | ✅ emsdk 3.1.51 |
| Android | arm64-v8a | Gradle + NDK | ✅ JDK 17 |
| iOS | arm64 | CMake + Xcode | ✅ deployment target 16.4 |

### 6.2 CI/CD 流水线

`build-templates.yml` 自动化构建全部 6 个平台:
1. **构建**: 各平台编译 Lumen (light/lightw/lightc + lua51)
2. **打包**: 生成平台模板 (含工具链)
3. **发布**: tag 推送时自动创建 GitHub Release (tar.gz 归档)

### 6.3 后端抽象层

引擎通过三个抽象后端实现跨平台:
- **RenderBackend**: GL 3.3 Core / GL 2.1 Legacy 自动选择
- **AudioBackend**: miniaudio 跨平台音频
- **PlatformNet**: libuv 跨平台网络

---

## 7. 项目文件结构

```
Light/
├── ChocoLight/                    # 引擎核心 (C++17 DLL)
│   ├── include/
│   │   ├── light.h                # 公共接口 + 模块声明
│   │   ├── light_crypto.h         # XOR 加密工具
│   │   └── light_antidebug.h      # 反调试接口
│   ├── src/
│   │   ├── cc_core.cpp            # 断言/日志/DllMain
│   │   ├── light_module.cpp       # 核心入口 + OOP 框架 + Demo
│   │   ├── light_ui.cpp           # GLFW 窗口管理
│   │   ├── light_graphics.cpp     # 2D/3D 图形渲染
│   │   ├── light_av.cpp           # 音视频 (FFmpeg 动态加载)
│   │   ├── light_network.cpp      # HTTP/WebSocket + Web 框架
│   │   ├── light_db.cpp           # SQLite3 接口
│   │   ├── light_record.cpp       # 纯 Lua ORM
│   │   ├── light_data.cpp         # 二进制缓冲区
│   │   ├── light_plugins.cpp      # WDF/NEM/WAS 游戏格式
│   │   ├── light_antidebug.cpp    # 5 层反调试实现
│   │   └── render_backend.cpp     # Mat4 矩阵 + 渲染后端工厂
│   ├── third_party/               # 第三方库
│   │   ├── glfw/                  # GLFW 3.x 源码
│   │   ├── sqlite3/               # SQLite3 源码
│   │   ├── miniaudio.h            # miniaudio 单头文件
│   │   ├── stb_image.h            # stb 图像加载
│   │   └── stb_truetype.h         # stb 字体渲染
│   └── CMakeLists.txt             # 构建配置
├── lumen-master/                  # Lumen VM (Lua 5.1 C++17 重实现)
│   ├── include/                   # Lua 公共头文件
│   ├── CMakeLists.txt             # VM 构建配置
│   └── README.md                  # Lumen 文档
├── Light-0.2.3/                   # 运行时示例
│   ├── assets/                    # 资源文件 (图片/字体/音频/视频)
│   ├── lib/                       # FFmpeg DLL
│   └── lua/main.lua               # 示例 Lua 脚本
├── templates/                     # 平台模板
│   ├── android/                   # Android Gradle 项目
│   ├── ios/                       # iOS CMake 项目
│   └── shared/choco_crypt.h       # 跨平台加密头文件
├── cpp_godot/                     # Godot 引擎集成 (WDF/WAS/NEM 读取)
└── .github/workflows/
    └── build-templates.yml        # 6 平台 CI/CD
```

---

## 8. 开发范式

### 8.1 Lua 脚本开发模式

```lua
-- 1. 创建窗口实例 (继承 Light.UI.Window)
local Game = Light(Light.UI.Window):New()

-- 2. 初始化回调 (加载资源)
function Game:OnOpen()
  self.font = Light(Light.Graphics.Font):New("font.ttf", 18)
  self.img  = Light(Light.Graphics.Image):New("bg.jpg")
  self.bgm  = Light(Light.AV.Audio):New("bgm.mp3")
end

-- 3. 每帧更新
function Game:Update(dt) ... end

-- 4. 每帧绘制
function Game:Draw()
  Light.Graphics.Draw(self.img, 0, 0, 0)
  Light.Graphics.Print("Hello", self.font, 100, 100, 0)
end

-- 5. 输入事件
function Game:OnKey(key, scanCode, action, mods) ... end
function Game:OnMouseButton(x, y, button, action, mods) ... end

-- 6. 打开窗口 + 事件循环
Game:Open(800, 600, "My Game")
while Light.UI.Loop() do Light.UI.Resume() end
```

### 8.2 数据库 ORM 使用

```lua
local db = Light(Light.DB.SQLite):New("game.db")

-- 定义模型
local Player = Light(Light.Record):Extends(Light.Record)
Player.__table = "players"
Player.__primary = "id"
Player.__database = db
Player.__fields = {
  id   = Light.Record.Serial,
  name = Light.Record.Text,
  score = Light.Record.Int
}
Player:Table()  -- CREATE TABLE IF NOT EXISTS

-- CRUD
local p = Player:Create()
p.name = "Alice"; p.score = 100
p:Insert()

local ret, players = Player:Fetch(10)
local ret, found = Player:Find(Player:Where("name", "Alice"))
```

---

## 9. 代码统计

| 文件 | 行数 | 主要内容 |
|------|------|---------|
| light_av.cpp | 1414 | FFmpeg 动态加载 + 音视频播放 |
| light_graphics.cpp | 1029 | 2D/3D 渲染 + 文本 + 精灵 |
| light_network.cpp | 891 | HTTP/WebSocket + Web 框架 |
| light_plugins.cpp | 833 | WDF/NEM/WAS 游戏格式 |
| light_module.cpp | 592 | OOP 框架 + Demo + 入口 |
| light_ui.cpp | 558 | GLFW 窗口管理 |
| light_record.cpp | 485 | 纯 Lua ORM |
| light_db.cpp | 316 | SQLite3 接口 |
| light_data.cpp | 306 | 二进制缓冲区 |
| light_antidebug.cpp | 132 | 反调试检测 |
| cc_core.cpp | 101 | 断言/日志/DllMain |
| render_backend.cpp | 88 | Mat4 矩阵 + 渲染后端 |
| **合计** | **~6745** | **引擎核心 C++ 代码** |

---

## 10. 优势与特点

1. **极致轻量**: 核心 DLL + Lua VM 即可运行完整游戏
2. **脚本驱动**: 游戏逻辑全部由 Lua 编写，热重载友好
3. **全平台覆盖**: 6 个平台 CI/CD 自动构建
4. **安全纵深**: 加密 + 反调试 + 混淆 + 静默异常多层防护
5. **逆向还原精度**: 函数地址、字节大小精确对应原始二进制
6. **自包含**: 第三方库均以源码或单头文件嵌入，无外部依赖
7. **OOP 完整性**: 嵌入式 Lua OOP 框架支持继承、多态、运算符重载
8. **动态加载**: FFmpeg 运行时按需加载，无 FFmpeg 环境也可运行基础功能

---

## 11. 待改进方向

- **渲染管线**: 从 OpenGL 1.x/2.x 固定管线升级到现代 Shader 管线
- **物理引擎**: 集成 Box2D 或 Chipmunk2D
- **音频增强**: 完善 FFmpeg → PCM 回退路径，添加空间音效
- **ECS 架构**: 引入 Entity-Component-System 提升大规模场景性能
- **资源管理**: 统一异步资源加载框架
- **文档完善**: 生成完整 Lua API 参考文档
