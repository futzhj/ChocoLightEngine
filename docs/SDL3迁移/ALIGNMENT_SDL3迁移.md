<!--
 * @Author: 炽热
 * @Date: 2026-04-25
 * @Description: SDL3 全平台迁移对齐文档
-->

# ALIGNMENT — ChocoLight 引擎全平台迁移到 SDL3

## 一、项目上下文分析

### 1.1 现有架构

ChocoLight 是 Lua 游戏引擎，核心 VM 为 Lumen (Lua 5.1 C++17 重实现)。引擎层由 C++ 实现的 `Light.dll/so/dylib` 提供以下 Lua 模块：

| 模块 | 当前依赖 | 跨平台情况 |
|------|---------|-----------|
| `Light.UI.Window` | **GLFW** (窗口/输入/事件) | 仅桌面 |
| `Light.Graphics` | **OpenGL** 2.x/3.3 (经渲染后端抽象) | 仅桌面 (GL 桌面版) |
| `Light.AV` | FFmpeg (动态加载) + miniaudio + waveOut(Win) | 桌面已跨平台 |
| `Light.DB.SQLite` | SQLite3 静态链接 | 已跨平台 |
| `Light.Network` | libuv (静态链接) | 已跨平台 |
| `Light.Record` | 纯 Lua | 已跨平台 |
| `Light.Plugins` | WDF/NEM 解析 (纯 C++) | 已跨平台 |
| `Light.Data` | std::vector | 已跨平台 |
| `Light.AntiDebug` | Win32 API | 仅 Windows (其他平台空实现) |

### 1.2 当前 CI/CD 状态

| 平台 | Lumen VM | ChocoLight 引擎 |
|------|---------|----------------|
| Windows x64 | ✅ | ✅ (vcpkg + GLFW) |
| Linux x64 | ✅ | ✅ (apt + GLFW) |
| macOS Universal | ✅ | ✅ (brew + GLFW) |
| Web (WASM) | ✅ | ❌ |
| Android APK | ✅ | ❌ (仅 JNI 跑 Lua) |
| iOS .app | ✅ | ❌ (仅 UIKit 跑 Lua) |

### 1.3 GLFW 在源码中的耦合点

通过代码搜索定位的全部依赖点：

```
include/render_backend.h   → 接口层 (无 GLFW 直接调用)
src/light_ui.cpp           → GLFW 窗口/输入/事件 (核心，~558 行)
src/light_av.cpp           → glfwGetTime (4 处)
src/render_legacy.cpp      → glfwGetProcAddress
src/render_gl33.cpp        → glad + glfwGetProcAddress
CMakeLists.txt             → find_package(glfw3) / glfw3.lib
```

## 二、原始需求

> 全升级为 SDL3 → 启动 M1-M4 任务，全面迁移 SDL3

四个里程碑（用户已认可路线图）:
- **M1**: 桌面三平台 SDL3 替换 GLFW (CI 验证 Win/Linux/macOS 仍能跑)
- **M2**: Web 模板 (Emscripten + SDL3 + WebGL2)
- **M3**: Android 模板 (SDL3 Activity + GLES3)
- **M4**: iOS 模板 (SDL3 UIViewController + GLES3)

## 三、边界确认 (任务范围)

### 3.1 范围内 (In Scope)

- ✅ GLFW 完全移除，替换为 SDL3
- ✅ `light_ui.cpp` 的 Window 模块用 SDL3 重写
- ✅ `light_av.cpp` 中 `glfwGetTime` → `SDL_GetPerformanceCounter`
- ✅ 渲染层 GL 函数加载: `glfwGetProcAddress` → `SDL_GL_GetProcAddress`
- ✅ CMakeLists.txt 改用 SDL3
- ✅ 桌面 CI/CD 切换 SDL3
- ✅ Web/Android/iOS 模板从 "纯 Lua VM" 升级为 "完整 ChocoLight 引擎"

### 3.2 范围外 (Out of Scope)

- ❌ **渲染 API 不变** (保留 OpenGL/GLES 经 RenderBackend 抽象)
- ❌ **不切换到 SDL_GPU** (避免渲染层重写 ~3000 行)
- ❌ **音频不切换** (保留 miniaudio，已跨平台)
- ❌ **不重写 FFmpeg 集成** (动态加载机制保持)
- ❌ **Lumen VM 本身不动** (仅引擎层)
- ❌ **Lua API 行为不变** (`Light.UI.Window:Open()` 签名不变)

## 四、需求理解 (对现有项目的理解)

### 4.1 关键设计原则

1. **Lua API 完全兼容**: 现有 Lua 脚本 (如 `Light-0.2.3/lua/main.lua`) 不需要改动
2. **渲染抽象保持**: `RenderBackend` 接口不动，只换 GL 函数加载源
3. **平台特定文件分离**: 移动/Web 平台特殊代码用条件编译或独立文件隔离
4. **依赖管理统一**: SDL3 通过 vcpkg/系统包/源码三种方式获取，CMakeLists.txt 统一处理

### 4.2 SDL3 vs GLFW API 映射 (核心)

| GLFW | SDL3 | 备注 |
|------|------|------|
| `glfwInit()` | `SDL_Init(SDL_INIT_VIDEO\|SDL_INIT_EVENTS)` | |
| `glfwCreateWindow()` | `SDL_CreateWindow()` + `SDL_GL_CreateContext()` | |
| `glfwPollEvents()` | `while(SDL_PollEvent(&e))` | |
| `glfwSwapBuffers()` | `SDL_GL_SwapWindow()` | |
| `glfwGetTime()` | `SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency()` | 单位: 秒 |
| `glfwGetProcAddress()` | `SDL_GL_GetProcAddress()` | |
| `glfwSetKeyCallback` | 事件循环中处理 `SDL_EVENT_KEY_DOWN` | 推模式 → 拉模式 |
| `glfwSetMouseButtonCallback` | `SDL_EVENT_MOUSE_BUTTON_*` | |
| `glfwSetWindowSizeCallback` | `SDL_EVENT_WINDOW_RESIZED` | |
| `glfwGetFramebufferSize` | `SDL_GetWindowSizeInPixels` | |

### 4.3 平台特殊处理

| 平台 | 上下文创建 | 入口点 | 注意事项 |
|------|----------|-------|---------|
| Windows/Linux/macOS | OpenGL 3.3 Core + 回退 GL 2.1 | `main()` | 不变 |
| **Web** | WebGL2 (= GLES 3.0) | `main()` + `emscripten_set_main_loop` | 主循环必须返回，用 `emscripten_set_main_loop` |
| **Android** | GLES 3.0 | `SDL_main` (SDL3 接管) | NDK + Java Activity 由 SDL3 提供 |
| **iOS** | GLES 3.0 (注意: Apple 已弃用但仍可用) 或保留 OpenGL | `SDL_main` | 也可用 SDL_GPU+Metal (但范围外) |

## 五、疑问澄清 (关键决策点)

### Q1: SDL3 依赖获取方式

SDL3 在不同平台的获取策略有差异，需要决定：

| 方案 | 优点 | 缺点 |
|------|------|------|
| **A. 全平台 CMake FetchContent** | 一处配置，自动从 GitHub 拉取源码编译 | 首次 CI 编译慢 (~3 分钟) |
| **B. 包管理器** (vcpkg/apt/brew) + CMake FetchContent (Web/移动) | 桌面快 | 配置分散 |
| **C. 仓库 vendor SDL3 源码** | 离线可用 | 仓库膨胀 (~20MB 源码) |

**推荐: A** — 一致性最好，CI 缓存可优化编译时间

### Q2: 输入事件模型

GLFW 是回调模式，SDL3 是事件队列模式。Lua 层 `OnKey` / `OnMouseButton` 回调如何对接？

| 方案 | 实现 |
|------|------|
| **A. 引擎主循环拉事件，分发到 Lua 回调** | 改 `light_ui.cpp` 的 `Resume()`, 在循环里 `SDL_PollEvent` 后调用 Lua 回调 |
| **B. 暴露 SDL 事件给 Lua** | Lua 层主动 `Light.UI.PollEvent()` |

**推荐: A** — 保持 Lua API 兼容，现有脚本不用改

### Q3: 移动/Web 平台 ChocoLight 引擎打包方式

桌面是动态库 (`.dll/.so/.dylib`)，移动/Web 怎么打包？

| 方案 | 实现 |
|------|------|
| **A. 静态库**: 移动/Web 把 ChocoLight 静态链接到主 app/wasm | 单一二进制，无加载问题 |
| **B. 动态库**: 仍然产出 `.so/.dylib`，运行时加载 | iOS 不允许动态加载第三方库；Android 可行但复杂 |

**推荐: A** — iOS App Store 政策强制要求静态链接

### Q4: 移动/Web 平台 FFmpeg 处理

`light_av.cpp` 动态加载 FFmpeg，这在移动/Web 不可行：

| 方案 | 实现 |
|------|------|
| **A. 移动/Web 禁用 Video，保留 Audio (miniaudio)** | `Light.AV.Video` 在这些平台返回 nil |
| **B. 静态链接 FFmpeg** | 包体增加 ~10MB+ |
| **C. Web 用 `<video>` 元素 + Lua 桥接，移动用平台原生 API** | 工作量大 |

**推荐: A** — M2-M4 阶段先保证基本功能，FFmpeg 视频可作后续 TODO

### Q5: 反调试模块的处理

`Light.AntiDebug` 仅 Windows 实现，在其他平台是空实现。新增的 Web/Android/iOS 是否需要补全？

**推荐: 保持空实现** — 这些平台有各自的安全机制 (Apple 签名、Android 验证)，反调试不是必需

### Q6: AndroidManifest/Info.plist 集成

SDL3 需要：
- Android: SDL3 提供的 `SDLActivity.java` (extends Activity)
- iOS: SDL3 自动接管 main，提供 `SDL_main`

现有模板的 `LightActivity.java` 和 `main.m` (Lua-only) 是保留还是替换？

**推荐: 替换** — M3/M4 完全切到 SDL3 入口，旧模板归档为 `templates/legacy/`

### Q7: SDL3 版本

SDL3 当前最新稳定版是 **3.2.x** (2025)，需要确认使用版本：

| 选项 | 说明 |
|------|------|
| SDL3 3.2.x (release-3.2.x 分支) | 稳定，推荐 |
| SDL3 main (开发分支) | 最新功能，可能有 breaking change |

**推荐: SDL3 3.2.x release 分支**

## 六、需要用户确认的决策点

请回答以下问题以进入下一阶段:

1. **Q1 SDL3 获取方式**: A (全平台 FetchContent) / B (包管理器+FetchContent混合) / C (vendor 源码)
2. **Q3 移动/Web 引擎打包**: A (静态库) / B (动态库)
3. **Q4 FFmpeg 处理**: A (移动/Web 禁用 Video) / B (静态链接) / C (用平台原生)
4. **Q6 旧模板处理**: 替换 / 保留并新建
5. **是否同意以上其他默认推荐**: Q2-A / Q5-空实现 / Q7-3.2.x release

## 七、风险评估

| 风险 | 等级 | 缓解措施 |
|------|------|---------|
| `light_ui.cpp` 重写引入回归 bug | 高 | M1 先在桌面三平台完整测试，对比 demo 行为 |
| SDL3 vcpkg/apt 版本陈旧 | 中 | 优先 FetchContent，确保 3.2.x |
| iOS GLES 弃用 | 低 | Apple 仍允许使用，未来再迁移 SDL_GPU+Metal |
| 移动平台 FFmpeg 视频缺失 | 中 | 用户已知，明确写入 TODO |
| Web 主循环模型不同 | 中 | M2 阶段单独适配 `emscripten_set_main_loop` |
| 编译时间增加 | 低 | CI 缓存 SDL3 build artifact |
