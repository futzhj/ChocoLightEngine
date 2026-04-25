<!--
 * @Author: Antigravity
 * @LastEditors: 炽热
 * @Date: 2026-04-25 20:52:42
 * @LastEditTime: 2026-04-25 21:15:55
-->
# ACCEPTANCE — SDL3 迁移验收记录

## M1: 桌面三平台 SDL3 迁移 ✅

| 任务 | 状态 | 交付物 | 验证 |
|------|------|--------|------|
| T1.1 SDL3 FetchContent | ✅ | CMakeLists.txt | cmake configure 成功 |
| T1.2 PlatformWindow 接口 | ✅ | platform_window.h | 编译通过 |
| T1.3 SDL3 实现 | ✅ | platform_window_sdl3.cpp | 桌面运行正常 |
| T1.4 RenderBackend 抽象 | ✅ | render_backend.h/.cpp, render_gl33.cpp, render_legacy.cpp | 渲染正常 |
| T1.5 light_ui.cpp 适配 | ✅ | light_ui.cpp 修改 | 窗口创建+事件+主循环 |
| T1.6 GLFW 移除 | ✅ | 删除 GLFW 依赖, 更新 cmake | 无 GLFW 残留 |
| T1.7 桌面 CI | ✅ | build-templates.yml | Win/Linux/macOS 绿色 |

**M1 完成提交**: `30e8770` (2025-04-25)

---

## M2: Web/Emscripten + VideoBackend 抽象 ✅

| 任务 | 状态 | 交付物 | 验证 |
|------|------|--------|------|
| T2.1 VideoBackend 接口 | ✅ | video_backend.h | 编译通过 |
| T2.2 FFmpeg 后端抽取 | ✅ | video_backend_ffmpeg.cpp + ffmpeg_common.h | 桌面编译通过 |
| T2.3 light_av.cpp 集成 | ✅ | light_av.cpp 重构 (1422→724行) | Lua API 不变 |
| T2.4 HTML5 视频后端 | ✅ | video_backend_html5.cpp | EM_JS 桥接完成 |
| T2.5 GLES3 渲染适配 | ✅ | render_gl33.cpp 条件编译 | Shader 300 es + WebGL2 纹理 |
| T2.6 Web CI | ✅ | CMakeLists.txt + build-templates.yml | CI 验证中 |

### M2 关键改动清单

**新文件**:
- `ChocoLight/include/video_backend.h` — VideoBackend 抽象接口
- `ChocoLight/include/ffmpeg_common.h` — FFmpegLib 共享定义
- `ChocoLight/src/video_backend_ffmpeg.cpp` — FFmpeg 桌面视频后端
- `ChocoLight/src/video_backend_html5.cpp` — HTML5 Web 视频后端

**修改文件**:
- `ChocoLight/src/light_av.cpp` — 视频 Lua 绑定改为 VideoBackend 委托
- `ChocoLight/src/render_gl33.cpp` — GLES3/WebGL2 条件编译
- `ChocoLight/src/light_ui.cpp` — Emscripten GL 头文件
- `ChocoLight/src/light_network.cpp` — Emscripten 空存根
- `ChocoLight/src/light_platform_net.cpp` — Emscripten 空存根
- `ChocoLight/CMakeLists.txt` — Emscripten 条件编译体系
- `.github/workflows/build-templates.yml` — Web 构建添加 ChocoLight

**M2 提交**: `b966586` (T2.1-T2.3) + `3f34f38` (T2.4-T2.6)

---

## M3: Android SDL3 适配 ✅

| 任务 | 状态 | 交付物 | 验证 |
|------|------|--------|------|
| T3.1 CMakeLists Android | ✅ | CMakeLists.txt (SDL3 SHARED, Light STATIC, GLES3) | CI 验证中 |
| T3.2 GLES3 渲染适配 | ✅ | render_gl33.cpp (单独 swizzle, 无 GL_TEXTURE_SWIZZLE_RGBA) | 编译时验证 |
| T3.3 VideoBackend 存根 | ✅ | video_backend_ffmpeg.cpp (Android→nullptr) | 编译时验证 |
| T3.4 模板 + CI | ✅ | templates/android-sdl3/ + CI 更新 | CI 验证中 |

### M3 关键交付物

**新目录 `templates/android-sdl3/`**:
- `app/build.gradle` — AGP 8.2, NDK 27, minSdk 24, GLES 3.0
- `app/src/main/cpp/CMakeLists.txt` — Lumen(static) + ChocoLight(static) + SDL3(shared)
- `app/src/main/cpp/main.cpp` — SDL_main 入口, Lua VM 初始化
- `ChocoLightActivity.java` — 继承 SDLActivity
- `setup_sdl3_java.sh` — SDL3 Java 源码提取脚本

**引擎改动**:
- CMakeLists: Android 条件 (SDL3 SHARED, 无 glad/libuv, GLES3+EGL 链接)
- 源码: `__ANDROID__` 守卫 (light_av, light_network, light_platform_net, light_ui, render_gl33, video_backend_ffmpeg)

**M3 提交**: `333e26e`

---

## M4: iOS SDL3 适配 — 待开始
