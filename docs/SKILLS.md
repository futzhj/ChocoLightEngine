# ChocoLight Engine — 技能清单 (Skills)

> 基于项目源码分析提取的核心技术能力

---

## S1. Lua 嵌入式 OOP 框架设计

**描述**: 设计并实现纯 Lua 面向对象编程框架，通过 metatable 和原型链提供类继承、实例化、运算符重载等完整 OOP 能力。

**技术要点**:
- 原型链继承 (`getmetatable` 链式查找)
- `__index` / `__newindex` 拦截实现属性访问控制
- 运算符重载 (`__add/__sub/__mul/__div/__concat`)
- 方法缓存 (`SetMethodCache`) 优化查找性能
- Mixin 混入模式、Clone 克隆、Cast 类型转换
- 嵌入 C++ 二进制作为常量字符串, `luaL_loadstring` 初始化

**涉及文件**: `light_module.cpp`

---

## S2. C++17 ↔ Lua 桥接层开发

**描述**: 通过 Lua C API 实现 C++ 功能模块到 Lua 脚本的完整桥接，包括函数注册、userdata 生命周期管理、回调转发。

**技术要点**:
- `luaL_Reg` 函数表注册 + `luaL_setfuncs` 批量绑定
- `lua_newuserdata` + `__gc` 元表实现 RAII 资源管理
- `luaL_ref` / `luaL_unref` 实现 Lua 对象引用持有
- 多重载参数解析 (`lua_gettop` + `switch`)
- `lua_pcall` 错误处理 + 日志记录
- 全局表 (`Light`) 模块化注册模式 (`LT::RegisterModule`)

**涉及文件**: `light.h`, `light_module.cpp`, 所有 `light_*.cpp`

---

## S3. OpenGL 渲染后端抽象与 2D 图形引擎

**描述**: 实现跨 GL 版本的渲染后端抽象层，支持 2D/3D 基元绘制、矩阵变换栈、纹理管理、FBO 离屏渲染。

**技术要点**:
- 运行时 GL 版本检测 (3.3 Core → 2.1 Legacy 自动回退)
- `RenderBackend` 接口抽象 (PushMatrix/PopMatrix/DrawArrays/BindTexture 等)
- `Mat4` 矩阵运算 (Identity/Ortho/Translate/Rotate/Scale/Multiply)
- 2D 正交投影 (原点左上角, Y 轴向下)
- 12 种绘图基元 (Draw/DrawQuad/Print/Line/Triangle/Rectangle/RoundedRectangle/Quad/Polygon/Arc/Circle/DrawSprite)
- FBO Canvas 离屏渲染
- Scissor 裁剪测试

**涉及文件**: `render_backend.cpp`, `render_backend.h`, `light_graphics.cpp`

---

## S4. UTF-8 文本渲染与动态字形缓存

**描述**: 实现支持中英文混排的 UTF-8 文本渲染系统，基于 stb_truetype 生成字形位图并动态缓存为 GL 纹理。

**技术要点**:
- UTF-8 多字节序列解码 (1-4 字节)
- stb_truetype 字形光栅化
- 字形缓存表 (codepoint → GL 纹理 ID)
- 懒加载: 首次渲染时创建纹理
- 支持自定义字体 (`Light.Graphics.Font`)
- 水平排版偏移计算

**涉及文件**: `light_graphics.cpp`

---

## S5. FFmpeg 动态加载与音视频管线

**描述**: 运行时动态加载 FFmpeg DLL (避免编译期依赖)，实现音频解码/播放和视频解码/渲染的完整管线。

**技术要点**:
- `LoadLibraryA` + `GetProcAddress` 解析 30+ FFmpeg 函数指针
- 多版本兼容: `av_packet_alloc` 在 avcodec/avutil 间探测
- 懒加载: 首次使用 AV 模块时触发
- DLL 搜索路径: `lib/` 子目录优先 → 当前目录回退
- **安全探针** (`ProbeCodecPar`): 在 AVStream 内存中扫描 codecpar 指针，避免依赖 FFmpeg 结构体布局
- 音频探针 (`ProbeAudioParams`): 通过匹配已知采样率值定位 sample_rate/channels
- swscale YUV→RGBA 色彩空间转换
- swresample 音频格式重采样
- waveOut 环形缓冲区音频同步 (64 槽 × 4096 样本)
- miniaudio 优先 + FFmpeg 回退的双音频后端

**涉及文件**: `light_av.cpp`, `light_audio_backend.h`

---

## S6. SDL3 窗口管理与平台抽象层

**描述**: 基于 SDL3 实现跨平台窗口创建、GL 上下文管理、事件处理、帧循环，替代原 GLFW 方案以支持 Android/iOS 原生平台。

**技术要点**:
- **PlatformWindow 抽象层**: 统一事件模型 (`PlatformWindow::Event`)，SDL3 后端实现
- 窗口创建多重载 + GL 上下文: 3.3 Core → ES 3.0 → 2.1 自动回退
- SDL3 拉模式事件 → 统一 Event 结构 → 分发到 Input 和 Lua 回调
- 帧循环: `Resume()` → `PollEvent` → `AntiDebug::Check` → `Window:__call`
- **Android JNI 集成**: `Window:SetOrientation()` 运行时屏幕方向控制
- 窗口关闭自动清理: 渲染/音频/网络后端 Shutdown

**涉及文件**: `light_ui.cpp`, `platform_window.h`, `platform_window_sdl3.cpp`

---

## S7. HTTP/WebSocket 网络栈

**描述**: 基于 libuv 实现 HTTP 1.1 和 WebSocket (RFC 6455) 的客户端/服务器，以及嵌入式 Lua Web 微框架。

**技术要点**:
- HTTP 1.1 请求/响应解析
- WebSocket 握手 (SHA-1 + Base64)
- WebSocket 帧编解码 (掩码、分片、控制帧)
- libuv 异步 I/O 事件循环
- 嵌入式 Lua Web 框架 (路由/会话/WebSocket 聊天)
- 客户端 + 服务器双模式

**涉及文件**: `light_network.cpp`

---

## S8. SQLite3 集成与纯 Lua ORM

**描述**: C++ 层提供 SQLite3 原生接口，Lua 层实现完整 ORM 框架，支持 CRUD、条件查询、事务、分页。

**技术要点**:
- C++ SQLite3 绑定 (Execute/Escape/Blob/TypeName)
- WAL 模式优化并发性能
- 14 种字段类型映射 (Serial ~ TimeStamp)
- `Where` 条件构建器 (Same/Greater/Less + Or 变体, 链式调用)
- `Record` 单行 CRUD (Insert/Update/Delete, 自动主键定位)
- `Records` 表级操作 (Table 自动建表, Fetch/FetchPage/Find/FindOne/Count/PageCount)
- 事务 (Begin/Commit/RollBack)
- SQL 历史记录 (`__history`)

**涉及文件**: `light_db.cpp`, `light_record.cpp`

---

## S9. 游戏资源格式逆向与解析

**描述**: 逆向并实现网易系游戏资源格式 (WDF/WAS/NEM) 的解析、解码和渲染。

**技术要点**:
- **WDF (PFDW)**: 资源包格式，FNV-1a 哈希索引，字节翻转 + XOR 0x5A 解码
- **WAS (0x5053)**: 精灵帧格式，RLE 压缩，RGBA 像素还原，帧数组 + hotspot
- **NEM**: 2D 网格地图，障碍物数据，基础寻路
- 资源提取 API: GetRaw/GetTGA/GetImage/GetAudio/GetSpriteSheet
- `__gc` 自动资源释放

**涉及文件**: `light_plugins.cpp`

---

## S10. 多层反调试与脚本保护

**描述**: 实现 5 层反调试检测 + 静默异常策略 + CLPK 脚本加密，构建纵深防护体系。

**技术要点**:
- **5 层检测**: IsDebuggerPresent → CheckRemoteDebuggerPresent → NtQueryInformationProcess(ProcessDebugPort) → 时序异常 → 硬件断点(DR0-DR3)
- **静默异常策略**: 不崩溃，渐进增大 anomalyFactor → 随机跳帧 → 调试体验恶化
- **CLPK 加密格式**: 76 字节头 + XOR 双密钥体系 (Master Key + Script Key)
- **密钥混淆**: XOR-with-index 存储，运行时还原
- **跨平台**: DLL 主密钥 (Windows) + 移动端主密钥 (Android/iOS)
- 二进制名验证 (`CheckBinaryName`)

**涉及文件**: `light_antidebug.cpp`, `light_antidebug.h`, `light_crypto.h`, `choco_crypt.h`

---

## S11. 跨平台 CI/CD 与模板工程

**描述**: 构建 6 平台全覆盖的 CI/CD 流水线和平台模板工程。

**技术要点**:
- GitHub Actions 矩阵构建 (windows-latest / ubuntu-latest / macos-latest)
- Emscripten WASM 编译 (emsdk 3.1.51)
- Android NDK + Gradle 集成 (JDK 17)
- iOS Xcode + CMake (arm64, deployment target 16.4, unsigned)
- macOS Universal Binary (x86_64 + arm64)
- Tag 触发自动 Release (tar.gz 归档)
- 平台模板: Android Gradle / iOS CMake+Xcode / Web HTML

**涉及文件**: `.github/workflows/build-templates.yml`, `templates/`

---

## S12. 二进制缓冲区管理

**描述**: 为 Lua 脚本层提供高效的二进制数据操作能力，支持类型化读写和原始指针交互。

**技术要点**:
- `std::vector<uint8_t>` 底层存储
- 栈式操作 (Push/Pop) + 随机访问 (Insert/Delete/At)
- 头部操作 (Shift/Unshift)
- 类型化数据推入: 整数 (8/16/32/64 有符号/无符号)、浮点、双精度、字节
- 原始指针暴露 (`GetPointer` → `lightuserdata`)
- placement new + `__gc` 析构的 userdata 生命周期管理

**涉及文件**: `light_data.cpp`

---

## S13. IDA 逆向工程与源码还原

**描述**: 从编译后的 DLL 二进制文件通过 IDA Pro 反编译，逐字节还原 C++ 源码和嵌入 Lua 脚本。

**技术要点**:
- IDA 反编译结果到可编译 C++ 源码的转换
- 嵌入数据提取 (OOP 框架 7276 字节、Demo 脚本 2359 字节、ORM 脚本)
- 函数地址标注 (如 `sub_1800A53F0`)
- 结构体布局推断 (AVContext/VideoContext/WDFData 等)
- 安全探针方式避免依赖内部结构体布局
- 字节大小精确匹配 (userdata 64 字节等)

**涉及文件**: 所有源文件中的 `@note 还原自` 注释

---

---

## S14. 统一输入管理器

**描述**: 实现键盘/鼠标/触摸/手柄的统一输入系统，支持虚拟动作映射，跨桌面和移动平台。

**技术要点**:
- 纯 POD 静态数据结构（避免 Android .so 加载时全局构造函数问题）
- 键盘: 512 键状态数组 + 帧按下检测 (当前帧 vs 上帧)
- 鼠标: 按钮状态 + 坐标 + 滚轮增量
- 触摸: 10 点多触控，槽位分配/释放
- 手柄: SDL3 Gamepad API，4 手柄并发，按钮/轴/热插拔
- 动作映射: `AddAction(name, {key, mouseBtn, gamepadBtn})` → `IsActionDown(name)`
- **Android 精简版**: #ifdef __ANDROID__ 编译 ~90 行精简实现，避免 MuMu .so 段布局崩溃

**涉及文件**: `light_input.cpp`

---

## S15. 2D 粒子系统

**描述**: 实现 GPU 友好的 2D 粒子发射器，支持可配置的粒子属性和批量渲染。

**技术要点**:
- 发射器参数: 位置/速度/加速度/生命周期/颜色渐变/缩放
- 对象池管理: 固定大小粒子池，避免运行时分配
- 批量渲染: 合并粒子绘制调用
- Lua API: `Particles.New/Emit/Update/Draw/SetProperty`

**涉及文件**: `light_particles.cpp`

---

## S16. Tilemap 瓦片地图

**描述**: 实现 2D 瓦片地图加载、渲染和碰撞查询。

**技术要点**:
- 多图层支持
- 瓦片集纹理管理
- 视口裁剪 (只渲染可见区域)
- Lua API: `Tilemap.New/Load/Draw/GetTile/SetTile`

**涉及文件**: `light_tilemap.cpp`

---

## S17. Box2D 物理引擎集成

**描述**: 集成 Box2D v3 物理引擎，提供 Lua 层的刚体模拟和碰撞检测。

**技术要点**:
- Box2D v3 API (与 v2 API 完全不同)
- World/Body/Shape/Joint 生命周期管理
- FetchAdd CMake 自动拉取 Box2D 源码
- Android 平台排除 (v3 不支持 x86_64 翻译层)
- Lua API: `Physics.NewWorld/CreateBody/AddShape/Step/RayCast`

**涉及文件**: `light_physics.cpp`, `CMakeLists.txt`

---

## S18. 轻量 ECS 实体组件系统

**描述**: 实现轻量级 Entity-Component-System 架构，用于中等规模场景管理。

**技术要点**:
- Entity: 整数 ID + 组件位掩码
- Component: 类型化数据存储
- System: 按组件过滤实体，批量处理
- Lua API: `ECS.CreateEntity/AddComponent/GetComponent/RegisterSystem/Update`

**涉及文件**: `light_ecs.cpp`

---

## S19. Android NDK 适配与 .so 崩溃排查

**描述**: 解决 Android NDK 构建中的 ABI 兼容性、.so 加载崩溃、模拟器 GPU 驱动问题。

**技术要点**:
- **MuMu RenderThread 崩溃根因**: .so 代码/数据段大小改变内存布局，触发 `pthread_mutex_lock on destroyed mutex`
- 解决方案: Android 用 #ifdef 编译精简版模块（~90 行 vs ~470 行）
- Box2D v3 `Unsupported CPU` 错误: 限制 ABI 为 arm64-v8a + x86_64
- `aligned_alloc` 兼容性: minSdk 提升到 28
- 二分法排查: 逐模块启用/禁用定位崩溃源
- JNI 集成: 运行时调用 Activity 方法 (屏幕方向)

**涉及文件**: `light_input.cpp`, `CMakeLists.txt`, `AndroidManifest.xml`, `main.cpp`

---

## S20. 跨平台渲染后端抽象

**描述**: 设计并实现渲染后端抽象层，支持 GL 3.3 Core (桌面) / GLES 3.0 (移动) / GL 2.1 Legacy (回退) 三级自动切换。

**技术要点**:
- `RenderBackend` 接口: DrawArrays/BindTexture/SetColor/PushMatrix/PopMatrix 等
- `GL33Backend`: Shader 管线 (VAO/VBO/Uniform)
- `LegacyBackend`: 固定管线 (glBegin/glEnd)
- 运行时 GL 版本探测 + 自动选择
- `Mat4` 矩阵运算: Identity/Ortho/Translate/Rotate/Scale/Multiply

**涉及文件**: `render_backend.cpp`, `render_backend.h`, `render_gl33.cpp`

---

## S21. Lumen + MSVC `lua_error` 安全模式

**核心**: 在 ChocoLight C++ Lua 模块中，`luaL_error` / `lua_error` 内部走 `longjmp`，与 MSVC `/GS` stack cookie 在某些栈布局下会触发崩溃（Phase AV.x `AddSampler` 调试中通过 6 次 CI iteration 锁定）。

### 风险模式（同时满足三条即高危）

1. **C++ 编译目标**（`light_*.cpp`，非 .c）
2. **函数体内 ≥ 8 字节的 `char[N]` 局部数组** —— MSVC 自动注入 `/GS` stack cookie
3. **`luaL_error` / `lua_error` 路径含 `%s` 或被多次调用**

### 症状

- CI 进程 exit code 1，无 stderr 错误信息，无 dmp
- 不一定第一次就崩，可能第 N 次 raise 才崩
- 改用 `snprintf + lua_pushstring + lua_error` 绕开 `lua_pushvfstring` 不能解决
- 把 `std::string` 改 `char[N]` 也不能解决（恰恰是触发 `/GS` 注入的根因）

### 安全模式表

| 模式 | 安全性 | 案例 |
|------|--------|------|
| `luaL_error(L, "...%d...", int)` 函数无 char[] | ✅ | `SetJointName` |
| `luaL_error(L, "...%s...", const char*)` 函数无 char[] | ✅ | `light_loadso.cpp` |
| `luaL_error(L, "...%s...", const char*)` 函数有 `char[N]` | ⚠️⚠️⚠️ | **不要用** |
| `lua_pushnil + lua_pushstring + return 2` | ✅✅✅ | **首选**，`AddSampler` / `LoadGLTF` / `Sound.Load` |

### 推荐 Helper（trivial-dtor 友好）

```cpp
static int ErrorReturn(lua_State* L, const char* msg) {
    lua_pushnil(L);
    lua_pushstring(L, msg);
    return 2;
}
static int ErrorReturnF(lua_State* L, const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    buf[sizeof(buf) - 1] = '\0';
    return ErrorReturn(L, buf);
}
```

### 通用规则

> ChocoLight C++ Lua 模块的错误处理 **首选** `return nil, err` 模式（与 `LoadGLTF` / `Sound.Load` / `LoadObject` 同构）。`luaL_error` 仅在函数体内**无 `char[N]` 局部数组**且**不可能多次 raise** 时才使用。

### CI bisect 方法论（适用于所有 longjmp 类崩溃）

1. **Lua 端 print 探针**：精确定位崩溃在哪个 pcall 之前
2. **C++ 端逐路径 `return 0`**：消除变量逐一确认嫌疑
3. **格式化 vs 非格式化**：排除 `lua_pushvfstring` 嫌疑
4. **首次 raise vs N 次 raise**：确认与调用次序的相关性

每轮 5-10 分钟 CI，6 轮即可锁定。**遇到 longjmp 类崩溃，第一时间考虑绕开（`return nil+err`）而非根治**——根治需要 WinDbg 本地复现 + 引擎源码深度调试，工程性价比远低于绕开。

**涉及文件**: `light_animation.cpp`（`l_Clip_AddSampler` + `ErrorReturn` / `ErrorReturnF` helper）；详细 6 次 iteration 记录见 `docs/Phase AV 骨骼动画/ACCEPTANCE_PhaseAV.md` Phase AV.x §4.1 / §5.1。

---

## 技能矩阵总览

| 编号 | 技能 | 领域 | 难度 | 版本 |
|------|------|------|------|------|
| S1 | Lua OOP 框架 | 语言设计 | ⭐⭐⭐ | v0.1 |
| S2 | C++/Lua 桥接 | 系统编程 | ⭐⭐⭐ | v0.1 |
| S3 | GL 渲染后端 | 图形学 | ⭐⭐⭐⭐ | v0.1 |
| S4 | UTF-8 文本渲染 | 图形学 | ⭐⭐⭐ | v0.1 |
| S5 | FFmpeg 动态加载 | 音视频 | ⭐⭐⭐⭐⭐ | v0.1 |
| S6 | SDL3 窗口+平台抽象 | 系统编程 | ⭐⭐⭐ | v0.2 |
| S7 | HTTP/WebSocket | 网络编程 | ⭐⭐⭐⭐ | v0.1 |
| S8 | SQLite + ORM | 数据库 | ⭐⭐⭐ | v0.1 |
| S9 | 游戏格式逆向 | 逆向工程 | ⭐⭐⭐⭐ | v0.1 |
| S10 | 反调试+加密 | 安全 | ⭐⭐⭐⭐ | v0.1 |
| S11 | 跨平台 CI/CD | DevOps | ⭐⭐⭐ | v0.1 |
| S12 | 二进制缓冲区 | 系统编程 | ⭐⭐ | v0.1 |
| S13 | IDA 逆向还原 | 逆向工程 | ⭐⭐⭐⭐⭐ | v0.1 |
| S14 | 统一输入管理器 | 游戏系统 | ⭐⭐⭐ | v0.3 |
| S15 | 2D 粒子系统 | 图形学 | ⭐⭐⭐ | v0.3 |
| S16 | Tilemap 瓦片地图 | 游戏系统 | ⭐⭐⭐ | v0.3 |
| S17 | Box2D 物理集成 | 游戏系统 | ⭐⭐⭐⭐ | v0.3 |
| S18 | 轻量 ECS | 架构设计 | ⭐⭐⭐ | v0.3 |
| S19 | Android 适配排查 | 平台工程 | ⭐⭐⭐⭐ | v0.3 |
| S20 | 渲染后端抽象 | 图形学 | ⭐⭐⭐⭐ | v0.2 |
| S21 | Lumen+MSVC `lua_error` 安全模式 | 调试/系统编程 | ⭐⭐⭐⭐⭐ | v0.3 |
