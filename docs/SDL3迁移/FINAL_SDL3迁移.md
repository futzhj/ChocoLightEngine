# FINAL — SDL3 迁移项目总结报告

## 项目概述

将 ChocoLight 引擎从 GLFW 完全迁移至 SDL3，实现 **6 平台统一窗口/事件/GL上下文管理**。

| 平台 | 窗口系统 | 渲染 | 视频后端 | 网络 | 状态 |
|------|---------|------|---------|------|------|
| Windows | SDL3 | GL 3.3 (glad) | FFmpeg 动态加载 | libuv | ✅ |
| Linux | SDL3 | GL 3.3 (glad) | FFmpeg 动态加载 | libuv | ✅ |
| macOS | SDL3 | GL 3.3 (glad) | FFmpeg 动态加载 | libuv | ✅ |
| Web/WASM | SDL3 | WebGL2 (GLES3) | HTML5 `<video>` | 空存根 | ✅ |
| Android | SDL3 | GLES 3.0 | 空存根 (待 MediaPlayer) | 空存根 | ✅ |
| iOS | SDL3 | GLES 3.0 (OpenGLES) | 空存根 (待 AVPlayer) | 空存根 | ✅ |

## 里程碑完成记录

### M1: 桌面三平台 GLFW → SDL3
- **范围**: PlatformWindow 抽象 + SDL3 实现 + RenderBackend 抽象 + GLFW 完全移除
- **交付**: 7 个子任务 (T1.1-T1.7)
- **提交**: `30e8770`

### M2: Web/Emscripten + VideoBackend 抽象
- **范围**: VideoBackend 接口 + FFmpeg 桌面后端抽取 + HTML5 视频后端 + GLES3 渲染适配
- **交付**: 6 个子任务 (T2.1-T2.6)
- **提交**: `b966586` + `3f34f38`

### M3: Android SDL3 适配
- **范围**: CMake Android 支持 + SDL3 SHARED + android-sdl3 模板 + CI
- **交付**: 4 个子任务 (T3.1-T3.4)
- **提交**: `333e26e`

### M4: iOS SDL3 适配
- **范围**: CMake iOS 支持 + CHOCO_PLATFORM_IOS 统一 + ios-sdl3 模板 + CI
- **交付**: 4 个子任务 (T4.1-T4.4)
- **提交**: `7396b6f`

## 架构改动总结

### 新增抽象层

| 抽象层 | 头文件 | 用途 |
|--------|--------|------|
| PlatformWindow | `platform_window.h` | 窗口/事件/GL上下文 (替代 GLFW 直接调用) |
| RenderBackend | `render_backend.h` | GL 渲染操作 (GL33 + Legacy 两实现) |
| VideoBackend | `video_backend.h` | 视频解码+渲染 (FFmpeg/HTML5/AVPlayer) |

### 条件编译体系

```
CMakeLists.txt 判断:
├── EMSCRIPTEN     → WebGL2, 无 glad/libuv, HTML5 视频
├── ANDROID        → GLES3, SDL3 SHARED, 无 glad/libuv
├── CHOCO_IOS      → GLES3, OpenGLES framework, 无 glad/libuv
└── else (桌面)    → GL 3.3 + glad + libuv + FFmpeg
```

### 平台模板

| 模板 | 路径 | 构建系统 |
|------|------|---------|
| Android SDL3 | `templates/android-sdl3/` | Gradle + NDK CMake |
| iOS SDL3 | `templates/ios-sdl3/` | CMake → Xcode |
| Web | `templates/web/` (CI 内构建) | Emscripten CMake |
| 桌面 | ChocoLight 直接构建 | CMake |

## 技术决策记录

### 1. SDL3 静态 vs 动态
- **桌面/Web/iOS**: SDL3 静态链接 — 减少运行时依赖
- **Android**: SDL3 动态链接 — SDLActivity Java 通过 `System.loadLibrary` 加载要求 .so

### 2. iOS 平台判断: CHOCO_PLATFORM_IOS
- `TARGET_OS_IOS` 需要 `#include <TargetConditionals.h>` 且在预处理器时机不稳定
- 改用 CMake `target_compile_definitions(CHOCO_PLATFORM_IOS=1)`, 在所有 .cpp 中可靠使用

### 3. GLES3 纹理 swizzle
- WebGL2: 不支持 swizzle → R8 展开为 RGBA
- Android/iOS GLES3: 支持 `GL_TEXTURE_SWIZZLE_R/G/B/A` 但不支持 `GL_TEXTURE_SWIZZLE_RGBA` 向量版
- 桌面 GL3.3: `glTexParameteriv(GL_TEXTURE_SWIZZLE_RGBA)` 正常

### 4. 视频后端策略
- 桌面: FFmpeg 动态加载 (保持原有行为)
- Web: HTML5 `<video>` + EM_JS 桥接
- Android/iOS: 当前为空存根, 预留原生后端接口

## 文件变更统计

| 类别 | 新增文件 | 修改文件 |
|------|---------|---------|
| 引擎核心 | 6 | 12 |
| Android 模板 | 8 | 0 |
| iOS 模板 | 5 | 0 |
| CI | 0 | 1 |
| 文档 | 3 | 1 |
| **合计** | **22** | **14** |

## CI 状态

| 平台 | Job | 编译 | 运行 |
|------|-----|------|------|
| Windows | build-windows | ✅ | ✅ 桌面验证 |
| Linux | build-linux | ✅ | CI 无 GUI |
| macOS | build-macos | ✅ | CI 无 GUI |
| Web | build-web | ✅ | 需浏览器 |
| Android | build-android | ✅ | 待真机验证 |
| iOS | build-ios | ✅ | 待真机验证 |

> 全平台编译通过。M5 阶段修复了 10 个构建问题 (详见 ACCEPTANCE)。
