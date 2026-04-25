<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 18:44:04
 * @LastEditTime: 2026-04-25 19:22:40
-->
# 待办事项 — ChocoLight 引擎升级

## 优先级: 高

### 1. 运行时功能验证
- **内容**: 启动引擎, 验证渲染/音频/网络模块实际运行是否正常
- **操作**: 运行 Lua 测试脚本, 验证 Graphics.Draw / AV.Play / Http.Open 等核心 API
- **风险**: 编译通过不等于运行正确, libuv 异步回调时序可能与原同步行为有差异

### 2. 网络回调时序验证
- **内容**: libuv 异步模型与原 WinSock 同步 select 行为可能有差异
- **关注点**: 
  - `Http.Open` 现在是异步的, `OnConnect` 回调在下一次 `Poll()` 触发
  - 原代码中 `Open` 返回时连接已建立, 新代码需要等回调
  - Lua 脚本如果依赖 `Open()` 后立即 `SendRequest()`, 需要改为在 `OnConnect` 回调中发送
- **操作**: 检查所有 Lua 脚本中的网络调用模式

### 3. FFmpeg 音频解码集成 (T3.4)
- **内容**: 将 FFmpeg 解码的 PCM 数据通过 `AudioBackend::LoadPCM` 喂入 miniaudio
- **前置**: 需要确认 FFmpeg DLL 可用环境下的解码流程
- **当前状态**: 框架已预留, `AudioBackend::LoadPCM` 接口已定义但未被调用

## 优先级: 中

### 4. HttpContext 内存管理
- **内容**: `HttpContext` 包含 `std::string recvBuf`, 在 `lua_newuserdata` 中通过 placement new 构造
- **风险**: Lua GC 回收 userdata 时不会调用 `std::string` 析构函数, 可能内存泄漏
- **修复**: 为 Http 添加 `__gc` 元方法, 在其中手动调用 `recvBuf.~string()`

### 5. GL33Backend 着色器错误处理
- **内容**: 如果着色器编译失败, 当前直接 abort, 应回退到 LegacyGLBackend
- **操作**: 在 `GL33Backend::Init` 中捕获着色器编译错误, 返回 false

### 6. WebSocket 帧分片处理
- **内容**: 当前 WebSocket 帧解析假设单帧完整接收, 大消息可能跨多次 read
- **操作**: 在 `recvBuf` 中积累数据, 解析完整帧后才分发

## 优先级: 低

### 7. libuv 升级策略
- **内容**: 当前固定 v1.48.0, 后续可考虑 CMake FetchContent 自动更新
- **操作**: 无需立即操作

### 8. ~~跨平台编译测试~~ ✅ 已完成
- **内容**: ~~当前只在 Windows MSVC 编译~~ → 已完成跨平台适配
- **已完成**:
  - `CMakeLists.txt` 支持 Windows/Linux/macOS 三平台构建
  - 源码跨平台适配: `cc_core.cpp` (POSIX 时间+ANSI色彩), `light_antidebug.cpp` (非Windows空实现), `light_av.cpp` (dlopen/dlsym), `light_db.cpp` (守卫), `render_legacy.cpp` (glfwGetProcAddress), `light.h` (导出宏)
  - CI/CD `build-templates.yml` 新增 ChocoLight 引擎构建 (Win: Light.dll, Linux: libLight.so, macOS: libLight.dylib)
- **待验证**: 推送后观察 CI 是否全平台编译通过

## 缺少的配置

| 项目 | 说明 | 操作指引 |
|------|------|----------|
| FFmpeg DLL | 音频解码回退依赖 | 将 `avformat-*.dll`, `avcodec-*.dll`, `avutil-*.dll`, `swresample-*.dll` 放入引擎运行目录 |
| Lua 测试脚本 | 验证各模块运行时行为 | 编写基础测试: 窗口→绘图→音频→网络→关闭 |
