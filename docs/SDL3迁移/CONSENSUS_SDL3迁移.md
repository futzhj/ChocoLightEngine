<!--
 * @Author: 炽热
 * @Date: 2026-04-25
 * @Description: SDL3 全平台迁移最终共识
-->

# CONSENSUS — SDL3 全平台迁移共识

## 一、明确的需求描述

将 ChocoLight 引擎从 GLFW 迁移到 SDL3，扩展平台覆盖至 6 个平台:
- 桌面: **Windows / Linux / macOS** (替换现有 GLFW 实现)
- 移动: **Android / iOS** (新增完整引擎，替换仅 Lua VM 的旧模板)
- 网络: **Web (WASM)** (新增完整引擎)

## 二、已确认的决策

| 决策项 | 选项 | 说明 |
|--------|------|------|
| Q1 SDL3 获取 | **A. 全平台 FetchContent** | CMake 自动从 GitHub 拉取 SDL3 源码编译，CI 缓存优化 |
| Q2 输入事件 | A (默认) | 引擎主循环 `SDL_PollEvent`，分发到 Lua 回调 (`OnKey`/`OnMouseButton`)。Lua API 不变 |
| Q3 引擎打包 | **B. 动态库** | Win: `Light.dll`, Linux: `libLight.so`, macOS: `libLight.dylib`, Android: `libLight.so`, **iOS: `Light.framework` (内嵌)**, Web: WASM 中静态合并 |
| Q4 FFmpeg 视频 | **C. 平台原生 API** | Web: HTML `<video>`, iOS: `AVPlayer`, Android: `MediaPlayer` |
| Q5 反调试 | 空实现 (默认) | 非 Windows 平台 `LightAntiDebug::Check()` 始终返回 true |
| Q6 旧模板 | **替换** | 旧模板归档到 `templates/legacy/`，新建 SDL3 入口 |
| Q7 SDL3 版本 | release-3.2.x (默认) | 稳定版，避开开发分支 breaking change |

## 三、技术实现方案

### 3.1 整体迁移策略

```
┌─────────────────────────────────────────────────┐
│ Lua 脚本层 (零改动)                              │
│  Light.UI.Window:Open() / OnKey / Draw / Update │
└─────────────────────────────────────────────────┘
                    │
                    ▼ (Lua API 完全兼容)
┌─────────────────────────────────────────────────┐
│ ChocoLight 引擎层 (本次改动核心)                  │
│  light_ui.cpp     ← GLFW → SDL3                 │
│  light_av.cpp     ← glfwGetTime → SDL_GetPerformanceCounter │
│  render_*.cpp     ← glfwGetProcAddress → SDL_GL_GetProcAddress │
│  light_video_native.cpp  ← 新增, 平台原生视频   │
└─────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────┐
│ 抽象层 (新增/调整)                               │
│  PlatformWindow (SDL3 封装, 屏蔽平台差异)        │
│  RenderBackend (现有, 保持)                      │
│  AudioBackend (现有, 保持)                       │
│  VideoBackend (新增, 平台原生视频)               │
└─────────────────────────────────────────────────┘
                    │
                    ▼
┌─────────────────────────────────────────────────┐
│ 第三方库                                         │
│  SDL3 (FetchContent) │ miniaudio │ OpenGL/GLES  │
└─────────────────────────────────────────────────┘
```

### 3.2 文件级改动清单

#### 修改 (M1 - 桌面)

```
ChocoLight/src/light_ui.cpp        重写 (~600 行) GLFW → SDL3
ChocoLight/src/light_av.cpp        4 处 glfwGetTime → SDL_GetPerformanceCounter
ChocoLight/src/render_legacy.cpp   1 处 glfwGetProcAddress → SDL_GL_GetProcAddress
ChocoLight/src/render_gl33.cpp     1 处 glad 加载源切换
ChocoLight/CMakeLists.txt          移除 find_package(glfw3), 新增 FetchContent(SDL3)
.github/workflows/build-templates.yml  移除 vcpkg/apt/brew GLFW 安装
```

#### 新增 (M2-M4)

```
ChocoLight/src/light_video_native.cpp  平台原生视频 (Web/iOS/Android)
templates/web/                          新增 Web 模板
  index.html
  build.sh
  CMakeLists.txt
templates/android-sdl3/                 SDL3 Android 模板 (替换旧)
  app/src/main/java/SDLActivity.java   (来自 SDL3 项目)
  app/src/main/cpp/main.cpp
  CMakeLists.txt
templates/ios-sdl3/                     SDL3 iOS 模板 (替换旧)
  main.cpp (SDL_main)
  Info.plist
  CMakeLists.txt
templates/legacy/                       归档原 templates/android, templates/ios
```

### 3.3 SDL3 集成方式 (FetchContent)

```cmake
include(FetchContent)
FetchContent_Declare(
    SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.2.0      # 锁定稳定版
    GIT_SHALLOW TRUE
)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)   # 静态链接到 Light.dll
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(SDL3)

target_link_libraries(Light PRIVATE SDL3::SDL3-static)
```

### 3.4 Lua API 兼容矩阵

| Lua API | 当前实现 | SDL3 后实现 | 行为变化 |
|---------|---------|------------|---------|
| `Light.UI.Window:Open(w,h,title)` | `glfwCreateWindow` | `SDL_CreateWindow + SDL_GL_CreateContext` | ✅ 一致 |
| `OnKey(key, action)` | GLFW 回调推送 | 主循环拉事件分发 | ✅ 一致 (key 码值映射) |
| `OnMouseButton(btn, action)` | 同上 | 同上 | ✅ 一致 |
| `OnMouseMove(x, y)` | 同上 | 同上 | ✅ 一致 |
| `OnResize(w, h)` | 同上 | 同上 | ✅ 一致 |
| `Light.AV.Audio` | miniaudio | miniaudio | ✅ 不变 |
| `Light.AV.Video` (桌面) | FFmpeg 动态加载 | FFmpeg 动态加载 | ✅ 不变 |
| `Light.AV.Video` (Web) | N/A | HTML5 `<video>` 桥接 | 🆕 新增 |
| `Light.AV.Video` (Android) | N/A | `MediaPlayer` JNI | 🆕 新增 |
| `Light.AV.Video` (iOS) | N/A | `AVPlayer` Obj-C++ 桥接 | 🆕 新增 |

## 四、任务边界限制

### 4.1 必须遵循

1. **零 Lua 脚本破坏**: 现有 `Light-0.2.3/lua/main.lua` 在桌面平台必须保持原行为
2. **渲染抽象不破坏**: `RenderBackend` 接口完全不变
3. **音频后端不动**: miniaudio 依然是音频主路径
4. **CI 红绿测试**: M1 完成后桌面 CI 必须绿色，再启动 M2

### 4.2 允许调整

1. SDL3 编译选项可根据平台优化 (如禁用 SDL_audio 减小体积，因为我们用 miniaudio)
2. 移动/Web 的 Lua API 子集可以小一些 (Light.AV.Video 视频部分允许有限制)

## 五、验收标准

### M1 (桌面三平台)

- [ ] Windows: GitHub Actions `build-windows` 绿色，产物含 `Light.dll`
- [ ] Linux: GitHub Actions `build-linux` 绿色，产物含 `libLight.so`
- [ ] macOS: GitHub Actions `build-macos` 绿色，产物含 `libLight.dylib`
- [ ] 本地测试: `Light-0.2.3/lua/main.lua` 在三平台启动后能创建窗口、显示内容、响应键盘
- [ ] 代码层: `git grep -l GLFW` 在 `ChocoLight/src` 中无结果 (除注释)

### M2 (Web)

- [ ] GitHub Actions `build-web` 绿色，产物含 `light.js + light.wasm + index.html`
- [ ] 浏览器打开 `index.html` 能加载并执行 Lua 脚本
- [ ] WebGL2 渲染管线工作 (能显示三角形/矩形)
- [ ] HTML5 video 桥接基础工作 (`Light.AV.Video:New(url):Play()`)

### M3 (Android)

- [ ] GitHub Actions `build-android` 绿色，产出 APK
- [ ] APK 安装到 Android 7.0+ 设备能启动并显示窗口
- [ ] 触摸输入事件正确传递到 Lua `OnMouseButton`
- [ ] GLES3 渲染工作

### M4 (iOS)

- [ ] GitHub Actions `build-ios` 绿色，产出 .app
- [ ] iPhone 模拟器能启动并显示窗口
- [ ] 触摸输入正确传递
- [ ] GLES3 渲染工作

## 六、确认所有不确定性已解决

✅ Q1-Q7 全部已确认  
✅ 技术方案与现有 `RenderBackend` / `AudioBackend` 抽象对齐  
✅ Lua API 兼容性策略明确  
✅ 平台特定代码隔离方案 (条件编译 + 独立文件)  
✅ 风险点和缓解措施已识别  
✅ 4 个 milestone 验收标准具体可测试

**进入阶段 2 (Architect)，生成详细架构设计文档。**
