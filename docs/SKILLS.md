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

## S6. GLFW 窗口管理与事件循环

**描述**: 基于 GLFW 实现跨平台窗口创建、输入事件处理、帧循环管理。

**技术要点**:
- 窗口创建多重载 (`Open()` / `Open(w)` / `Open(w,h)` / `Open(w,h,title)`)
- GL 上下文创建: 3.3 Core → 2.1 兼容自动回退
- GLFW 回调 → Lua 方法转发 (`OnKey/OnMouseButton/OnMousePosition`)
- 帧循环: `Resume()` → `PollEvents` → `AntiDebug::Check` → `Window:__call` (清屏→Draw→Update(dt)→SwapBuffers)
- 窗口关闭时自动清理: 渲染/音频/网络后端 Shutdown

**涉及文件**: `light_ui.cpp`

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

## 技能矩阵总览

| 编号 | 技能 | 领域 | 难度 |
|------|------|------|------|
| S1 | Lua OOP 框架 | 语言设计 | ⭐⭐⭐ |
| S2 | C++/Lua 桥接 | 系统编程 | ⭐⭐⭐ |
| S3 | GL 渲染后端 | 图形学 | ⭐⭐⭐⭐ |
| S4 | UTF-8 文本渲染 | 图形学 | ⭐⭐⭐ |
| S5 | FFmpeg 动态加载 | 音视频 | ⭐⭐⭐⭐⭐ |
| S6 | GLFW 窗口管理 | 系统编程 | ⭐⭐ |
| S7 | HTTP/WebSocket | 网络编程 | ⭐⭐⭐⭐ |
| S8 | SQLite + ORM | 数据库 | ⭐⭐⭐ |
| S9 | 游戏格式逆向 | 逆向工程 | ⭐⭐⭐⭐ |
| S10 | 反调试+加密 | 安全 | ⭐⭐⭐⭐ |
| S11 | 跨平台 CI/CD | DevOps | ⭐⭐⭐ |
| S12 | 二进制缓冲区 | 系统编程 | ⭐⭐ |
| S13 | IDA 逆向还原 | 逆向工程 | ⭐⭐⭐⭐⭐ |
