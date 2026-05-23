/**
 * @file light_ui.cpp
 * @brief Light.UI + Light.UI.Window 模块 — 基于 PlatformWindow (SDL3)
 *
 * Window API (12 函数, Lua 接口完全兼容历史版本):
 *   Open(w, h, title)    — 创建窗口, 默认标题 "ChocoLight Engine"
 *   Close()              — 标记窗口关闭
 *   ID()                 — 获取原生窗口句柄数字
 *   Get/SetWidth         — 宽度存取
 *   Get/SetHeight        — 高度存取
 *   Get/SetDimensions    — 尺寸存取 (返回 w,h 两值)
 *   SetVSync(bool)       — 垂直同步
 *   __call()             — 事件循环步进 (Clear → Draw → Update → Swap)
 *   __tostring()         — "Light.UI.Window"
 *
 * UI.Resume — 主事件泵 (每帧调用): PollEvent → 分发到 Lua 回调 → __call
 *
 * 事件模型 (与历史 GLFW 版本保持兼容):
 *   GLFW: 推模式 (回调在 glfwPollEvents 内自动触发)
 *   SDL3: 拉模式 (Resume 拉取事件, 手动分发到 Lua 回调)
 *   Lua 回调签名/语义不变, 现有脚本无需改动
 */

#include "light.h"
#include "light_antidebug.h"
#include "render_backend.h"
#include "batch_renderer.h"
#include "lit_batch_renderer.h"  // Phase E.2.3 — 2D Lit 批渲染器
#include "hdr_renderer.h"             // Phase E.3.2 — HDR 离屏管线
#include "taa_renderer.h"             // Phase F.0 — TAA jitter injection hook
#include "bloom_renderer.h"           // Phase E.4.2 — Bloom 后处理
#include "auto_exposure_renderer.h"   // Phase E.5.2 — Auto Exposure (Eye Adaptation)
#include "lens_dirt_renderer.h"       // Phase E.6.2 — Lens Dirt
#include "streak_renderer.h"          // Phase E.6.2 — Streak (Anamorphic Flare)
#include "lens_flare_renderer.h"      // Phase E.7.2 — Lens Flare (Ghost + Halo + Chromatic)
#include "ssao_renderer.h"              // Phase E.8.2 — SSAO (屏幕空间环境光遮蔽)
#include "ssr_renderer.h"               // Phase E.9 — SSR (屏幕空间反射)
#include "motion_blur_renderer.h"       // Phase E.15 — Velocity-driven Motion Blur
#include "asset_loader.h"               // Phase G.1 — 异步资源加载
#include "light_audio_backend.h"
#include "light_platform_net.h"
#include "platform_window.h"
#include "light_time.h"                 // Phase H.0 — Tick-Render 解耦 (LT::TickRender)
#include <cstdint>

// Input 模块事件处理 (定义在 light_input.cpp)
extern void InputProcessEvent(const PlatformWindow::Event& ev);
#include <cstdlib>
#include <cstdio>      // Phase G.1.3 — atexit 审计钩子用 fputs/fprintf 直写 stderr

#ifdef __ANDROID__
#include <SDL3/SDL.h>
#include <jni.h>
#endif

// 渲染后端为空时的 GL 兼容 (Legacy 路径)
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
#include <GLES3/gl3.h>
#elif defined(CHOCO_PLATFORM_IOS)
#include <OpenGLES/ES3/gl.h>
#elif defined(_WIN32)
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

// windows.h 宏污染清理 (CreateWindow → CreateWindowA)
#ifdef CreateWindow
#undef CreateWindow
#endif

// Phase F.0.11 — 录屏 tick hook (实现在 light_graphics.cpp)
extern "C" void Light_Graphics_RecordTickHook(int win_w, int win_h);
// Phase F.0.11.6.1.A7 — 录屏指示器 OSD (实现在 light_graphics.cpp)
//   注意调用顺序: RecordTickHook 之后, SwapBuffers 之前 — 确保 readback 不含 OSD
extern "C" void Light_Graphics_DrawRecordOSD(int win_w, int win_h);

// ==================== 全局状态 ====================

static bool       g_platformInited = false;
static void*      g_mainWindow     = nullptr;   // PlatformWindow 不透明句柄
static void*      g_glContext      = nullptr;
// Phase G.1: 去 static, 供 asset_loader.cpp 等模块在主线程 dispatch Lua callback
lua_State*        g_callbackL      = nullptr;   // 持续保存的 Lua state (用于回调)
static int        g_windowRef      = LUA_NOREF; // Window Lua 实例的注册表索引

// 鼠标位置缓存 (SDL3 鼠标按钮事件已自带坐标, 但单独 Move 事件也保持此缓存供其他地方查询)
static double g_mouseX = 0.0;
static double g_mouseY = 0.0;

/// 平台层初始化 (懒加载, 同时初始化反调试)
static bool EnsurePlatform() {
    if (g_platformInited) return true;
    if (PlatformWindow::Init()) {
        g_platformInited = true;
        LightAntiDebug::Init();
        return true;
    }
    CC::Log(CC::LOG_ERROR, "PlatformWindow init failed");
    return false;
}

// ==================== 2D 正交投影辅助 ====================

static void SetupOrthoProjection() {
    if (!g_mainWindow) return;
    int winW = 0, winH = 0;
    int fbW = 0, fbH = 0;
    PlatformWindow::GetWindowSize(g_mainWindow, &winW, &winH);
    PlatformWindow::GetFramebufferSize(g_mainWindow, &fbW, &fbH);

    if (g_render) {
        g_render->SetViewport(0, 0, fbW, fbH);
        g_render->LoadOrtho(0, (float)winW, (float)winH, 0, -1.0f, 1.0f);
    } else {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, winW, winH, 0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
#endif
    }
}

// ==================== Lua 事件分发 ====================

/// 调用 Window:OnKey(key, scanCode, action, mods)
static void DispatchOnKey(lua_State* L, int key, int scancode, int action, int mods) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnKey");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);                        // self
        lua_pushinteger(L, key);
        lua_pushinteger(L, scancode);
        lua_pushinteger(L, action);
        lua_pushinteger(L, mods);
        if (lua_pcall(L, 5, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnKey: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnMouseButton(x, y, button, action, mods)
static void DispatchOnMouseButton(lua_State* L, double x, double y, int button, int action, int mods) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnMouseButton");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushinteger(L, button);
        lua_pushinteger(L, action);
        lua_pushinteger(L, mods);
        if (lua_pcall(L, 6, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnMouseButton: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnMousePosition(x, y)
static void DispatchOnMousePosition(lua_State* L, double x, double y) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnMousePosition");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        if (lua_pcall(L, 3, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnMousePosition: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnTextInput(text)  — IME 已提交的 UTF-8 文本
static void DispatchOnTextInput(lua_State* L, const char* text) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnTextInput");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);                        // self
        lua_pushstring(L, text ? text : "");
        if (lua_pcall(L, 2, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnTextInput: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnTextEditing(text, start, length)  — IME 组合态实时反馈
static void DispatchOnTextEditing(lua_State* L, const char* text, int start, int length) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnTextEditing");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);                        // self
        lua_pushstring(L, text ? text : "");
        lua_pushinteger(L, start);
        lua_pushinteger(L, length);
        if (lua_pcall(L, 4, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnTextEditing: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

// ==================== Phase H.0.3 — App 生命周期 dispatch ====================

/// 调用 Window:OnAppEnterBackground()  — iOS/Android 切后台 (auto Pause 已在 hook 内调过)
static void DispatchOnAppEnterBackground(lua_State* L) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnAppEnterBackground");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);   // self
        if (lua_pcall(L, 1, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnAppEnterBackground: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnAppEnterForeground()  — iOS/Android 切回前台 (auto Resume 已在 hook 内调过)
static void DispatchOnAppEnterForeground(lua_State* L) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnAppEnterForeground");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);   // self
        if (lua_pcall(L, 1, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnAppEnterForeground: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

// ==================== Phase AR — Pen 事件 dispatch ====================

/// 调用 Window:OnPenProximity(penId, action)  — action: 1=in, 0=out
static void DispatchOnPenProximity(lua_State* L, int penId, int action) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenProximity");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);                        // self
        lua_pushinteger(L, penId);
        lua_pushinteger(L, action);
        if (lua_pcall(L, 3, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenProximity: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnPenDown(penId, x, y, eraser)
static void DispatchOnPenDown(lua_State* L, int penId, double x, double y, int eraser) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenDown");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushinteger(L, penId);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushinteger(L, eraser);
        if (lua_pcall(L, 5, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenDown: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnPenUp(penId, x, y, eraser)
static void DispatchOnPenUp(lua_State* L, int penId, double x, double y, int eraser) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenUp");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushinteger(L, penId);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        lua_pushinteger(L, eraser);
        if (lua_pcall(L, 5, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenUp: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnPenButton(penId, button, action, x, y)  — action: 1=down, 0=up
static void DispatchOnPenButton(lua_State* L, int penId, int button, int action, double x, double y) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenButton");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushinteger(L, penId);
        lua_pushinteger(L, button);
        lua_pushinteger(L, action);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        if (lua_pcall(L, 6, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenButton: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnPenMotion(penId, x, y)
static void DispatchOnPenMotion(lua_State* L, int penId, double x, double y) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenMotion");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushinteger(L, penId);
        lua_pushnumber(L, x);
        lua_pushnumber(L, y);
        if (lua_pcall(L, 4, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenMotion: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

/// 调用 Window:OnPenAxis(penId, axis, value)
static void DispatchOnPenAxis(lua_State* L, int penId, int axis, float value) {
    if (!L || g_windowRef == LUA_NOREF) return;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnPenAxis");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);
        lua_pushinteger(L, penId);
        lua_pushinteger(L, axis);
        lua_pushnumber(L, value);
        if (lua_pcall(L, 4, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnPenAxis: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }
    lua_settop(L, top);
}

// Phase AR — Timer 事件转发 (由 light_time.cpp 提供实现)
extern "C" void Time_OnTimerEvent(lua_State* L, int timer_id);

/// 帧缓冲尺寸变更 → 更新视口和投影
static void OnFramebufferResize(int width, int height) {
    SetupOrthoProjection();
}

/// 处理一帧内累积的所有事件
static void DispatchEvents(lua_State* L) {
    PlatformWindow::Event ev;
    // 单帧最多处理 256 个事件, 避免事件风暴卡死
    for (int i = 0; i < 256; ++i) {
        if (!PlatformWindow::PollEvent(&ev)) break;
        // 同步到 Input 模块状态快照
        InputProcessEvent(ev);
        switch (ev.type) {
            case PlatformWindow::Event::KeyDown:
                DispatchOnKey(L, ev.keycode, ev.scancode, /*action=*/1, ev.mods);
                break;
            case PlatformWindow::Event::KeyUp:
                DispatchOnKey(L, ev.keycode, ev.scancode, /*action=*/0, ev.mods);
                break;
            case PlatformWindow::Event::MouseDown:
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMouseButton(L, ev.x, ev.y, ev.button, /*action=*/1, /*mods=*/0);
                break;
            case PlatformWindow::Event::MouseUp:
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMouseButton(L, ev.x, ev.y, ev.button, /*action=*/0, /*mods=*/0);
                break;
            case PlatformWindow::Event::MouseMove:
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMousePosition(L, ev.x, ev.y);
                break;
            case PlatformWindow::Event::Resize:
                OnFramebufferResize(ev.width, ev.height);
                break;
            case PlatformWindow::Event::Close:
                // ShouldClose 会在 PollEvent 内部被设置, Resume 主循环会感知
                break;
            case PlatformWindow::Event::TouchDown:
                // 触摸映射为左键鼠标按下 (兼容现有 Lua 脚本)
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMouseButton(L, ev.x, ev.y, /*button=*/0, /*action=*/1, 0);
                break;
            case PlatformWindow::Event::TouchUp:
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMouseButton(L, ev.x, ev.y, /*button=*/0, /*action=*/0, 0);
                break;
            case PlatformWindow::Event::TouchMove:
                g_mouseX = ev.x; g_mouseY = ev.y;
                DispatchOnMousePosition(L, ev.x, ev.y);
                break;
            case PlatformWindow::Event::TextInput:
                DispatchOnTextInput(L, ev.text);
                break;
            case PlatformWindow::Event::TextEditing:
                DispatchOnTextEditing(L, ev.text, ev.text_start, ev.text_length);
                break;
            // Phase AR — Pen 事件
            case PlatformWindow::Event::PenProximity:
                DispatchOnPenProximity(L, ev.penId, ev.penAction);
                break;
            case PlatformWindow::Event::PenDown:
                DispatchOnPenDown(L, ev.penId, ev.x, ev.y, ev.penEraser);
                break;
            case PlatformWindow::Event::PenUp:
                DispatchOnPenUp(L, ev.penId, ev.x, ev.y, ev.penEraser);
                break;
            case PlatformWindow::Event::PenButton:
                DispatchOnPenButton(L, ev.penId, ev.penButton, ev.penAction, ev.x, ev.y);
                break;
            case PlatformWindow::Event::PenMotion:
                DispatchOnPenMotion(L, ev.penId, ev.x, ev.y);
                break;
            case PlatformWindow::Event::PenAxis:
                DispatchOnPenAxis(L, ev.penId, ev.penAxis, ev.penAxisValue);
                break;
            // Phase AR — Timer 事件 (light_time.cpp 提供处理)
            case PlatformWindow::Event::Timer:
                Time_OnTimerEvent(L, ev.penButton);  // 复用 penButton 字段作为 timer_id
                break;
            // Phase H.0.3 — App 生命周期: 先驱动 TickRender 状态机, 再回调 Lua
            case PlatformWindow::Event::AppEnterBackground:
                LT::TickRender::Pause();
                DispatchOnAppEnterBackground(L);
                break;
            case PlatformWindow::Event::AppEnterForeground:
                LT::TickRender::Resume();
                DispatchOnAppEnterForeground(L);
                break;
            default:
                break;
        }
    }
}

// ==================== Window:Open ====================
// 多重载: Open() / Open(w) / Open(w,h) / Open(w,h,title)

static int l_Window_Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int argc = lua_gettop(L);

    int width = 800, height = 600;
    const char* title = "ChocoLight Engine";

    switch (argc) {
        case 4: title  = luaL_checkstring(L, 4);            // fallthrough
        case 3: height = (int)luaL_checkinteger(L, 3);      // fallthrough
        case 2: width  = (int)luaL_checkinteger(L, 2); break;
        default: break;
    }

    if (!EnsurePlatform()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 创建窗口 (PlatformWindow 内部已设置 GL 3.3 Core 属性)
    g_mainWindow = PlatformWindow::CreateWindow(title, width, height, /*fullscreen=*/false);
    if (!g_mainWindow) {
        CC::Log(CC::LOG_ERROR, "Failed to create window %dx%d", width, height);
        lua_pushboolean(L, 0);
        return 1;
    }

    // 创建 GL 上下文
    g_glContext = PlatformWindow::CreateGLContext(g_mainWindow);
    if (!g_glContext) {
        CC::Log(CC::LOG_ERROR, "Failed to create GL context");
        PlatformWindow::DestroyWindow(g_mainWindow);
        g_mainWindow = nullptr;
        lua_pushboolean(L, 0);
        return 1;
    }
    PlatformWindow::MakeCurrent(g_mainWindow, g_glContext);
    PlatformWindow::SetSwapInterval(1);  // VSync on by default

    // 初始化渲染后端 (内部检测 GL 版本, 失败回退 Legacy)
    g_render = CreateRenderBackend();
    if (!g_render) {
        CC::Log(CC::LOG_ERROR, "No render backend available, aborting");
        PlatformWindow::DestroyGLContext(g_glContext);
        PlatformWindow::DestroyWindow(g_mainWindow);
        g_glContext = nullptr;
        g_mainWindow = nullptr;
        lua_pushboolean(L, 0);
        return 1;
    }
    CC::Log(CC::LOG_INFO, "Render backend: %s", g_render->GetName());

    // Phase A7: 初始化 BatchRenderer (与 g_render 绑定, 失败不致命)
    if (!BatchRenderer::Init(g_render)) {
        CC::Log(CC::LOG_WARN, "BatchRenderer init failed, falling back to per-call rendering");
    }
    // Phase E.2.3: 初始化 LitBatchRenderer (2D Lit 批渲染, 独立于 BatchRenderer)
    if (!LitBatchRenderer::Init(g_render)) {
        CC::Log(CC::LOG_WARN, "LitBatchRenderer init failed, lit sprites fall back to per-call rendering");
    }
    // Phase E.3.2: 初始化 HDRRenderer (不自动 Enable, 等 Lua 显式 Light.Graphics.HDR.Enable)
    HDRRenderer::Init(g_render);
    // Phase E.4.2: 初始化 BloomRenderer (不自动 Enable; autoEnable=true 时由 HDR.Enable 联动拉起)
    BloomRenderer::Init(g_render);
    // Phase E.5.2: 初始化 AutoExposureRenderer (不自动 Enable; autoEnable=false 默认 manual exposure)
    AutoExposureRenderer::Init(g_render);
    // Phase E.6.2: 初始化 LensDirt + Streak (默认 autoEnable=false, 手动启用)
    LensDirtRenderer::Init(g_render);
    StreakRenderer::Init(g_render);
    // Phase E.7.2: 初始化 LensFlare (默认 autoEnable=false)
    LensFlareRenderer::Init(g_render);
    // Phase E.8.2: 初始化 SSAO (默认 autoEnable=false)
    SSAORenderer::Init(g_render);
    // Phase E.9: 初始化 SSR (默认 autoEnable=false)
    SSRRenderer::Init(g_render);
    // Phase E.15: 初始化 Motion Blur (默认 autoEnable=false)
    MotionBlurRenderer::Init(g_render);
    // Phase F.0: 初始化 TAA 主管线 (默认 autoEnable=false, 用户主动 Enable)
    TAARenderer::Init(g_render);

    // Phase H.0: 初始化 Tick-Render 解耦 — 重置 accumulator + lastTime
    // 不影响 Phase AR Light.Time 现有 SDL_AddTimer 路径
    LT::TickRender::Init();

    // Phase G.1: 启动异步资源加载 worker (失败不致命, 内部 fallback 同步加载)
    // Phase G.1.1: 传入主窗口/主 GL ctx, 内部 probe Shared GL Context;
    //              probe 失败仍正常启动 worker, 走主线程上传路径
    if (!AssetLoader::Init(g_mainWindow, g_glContext)) {
        CC::Log(CC::LOG_WARN, "AssetLoader init failed, async loads fall back to sync path");
    }

    // 初始化音频后端
    if (!AudioBackend::Init()) {
        CC::Log(CC::LOG_WARN, "AudioBackend: init failed, audio will be unavailable");
    }

    // 初始化网络后端
    if (!PlatformNet::Init()) {
        CC::Log(CC::LOG_WARN, "PlatformNet: init failed, network will be unavailable");
    }

    // 初始视口 + 2D 正交投影
    SetupOrthoProjection();

    // 初始颜色
    g_render->SetColor(1.0f, 1.0f, 1.0f, 1.0f);

    // 保存 Lua 状态和 Window 实例引用 (用于事件分发)
    g_callbackL = L;
    lua_pushvalue(L, 1);
    g_windowRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // 调用 OnOpen 回调
    lua_getfield(L, 1, "OnOpen");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "OnOpen: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    CC::Log(CC::LOG_INFO, "Window opened: %dx%d '%s'", width, height, title);
    return 0;
}

// ==================== Window:Close ====================

static int l_Window_Close(lua_State* L) {
    if (g_mainWindow) {
        PlatformWindow::SetShouldClose(g_mainWindow, true);
    }
    return 0;
}

// ==================== Window:ID ====================

static int l_Window_ID(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(intptr_t)g_mainWindow);
    return 1;
}

// ==================== Window:GetWidth / GetHeight / GetDimensions ====================

// Phase F.0.11.6 — 给 light_graphics.cpp::l_Graphics_RecordMP4 用的窗口尺寸 extern
extern "C" int Light_GetWindowWidth() {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetWindowSize(g_mainWindow, &w, &h);
    return w;
}
extern "C" int Light_GetWindowHeight() {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetWindowSize(g_mainWindow, &w, &h);
    return h;
}

extern "C" int Light_GetFramebufferWidth() {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetFramebufferSize(g_mainWindow, &w, &h);
    return w;
}

extern "C" int Light_GetFramebufferHeight() {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetFramebufferSize(g_mainWindow, &w, &h);
    return h;
}

static int l_Window_GetDPIScale(lua_State* L) {
    int winW = 1, winH = 1;
    int fbW = 1, fbH = 1;
    if (g_mainWindow) {
        PlatformWindow::GetWindowSize(g_mainWindow, &winW, &winH);
        PlatformWindow::GetFramebufferSize(g_mainWindow, &fbW, &fbH);
    }
    float scale = (winW > 0) ? (float)fbW / (float)winW : 1.0f;
    lua_pushnumber(L, scale);
    return 1;
}

static int l_Window_GetWidth(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, w);
    return 1;
}

static int l_Window_GetHeight(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, h);
    return 1;
}

static int l_Window_GetDimensions(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) PlatformWindow::GetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

// ==================== Window:SetWidth / SetHeight / SetDimensions ====================

static int l_Window_SetWidth(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 2);
    if (g_mainWindow) {
        int _, h;
        PlatformWindow::GetWindowSize(g_mainWindow, &_, &h);
        PlatformWindow::SetWindowSize(g_mainWindow, w, h);
    }
    return 0;
}

static int l_Window_SetHeight(lua_State* L) {
    int h = (int)luaL_checkinteger(L, 2);
    if (g_mainWindow) {
        int w, _;
        PlatformWindow::GetWindowSize(g_mainWindow, &w, &_);
        PlatformWindow::SetWindowSize(g_mainWindow, w, h);
    }
    return 0;
}

static int l_Window_SetDimensions(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    if (g_mainWindow) PlatformWindow::SetWindowSize(g_mainWindow, w, h);
    return 0;
}

// ==================== Window:SetVSync ====================

static int l_Window_SetVSync(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int vsync = lua_toboolean(L, 2);
    if (g_mainWindow) {
        PlatformWindow::MakeCurrent(g_mainWindow, g_glContext);
        PlatformWindow::SetSwapInterval(vsync ? 1 : 0);
    }
    lua_pushboolean(L, vsync);
    return 1;
}

// ==================== Window:__call ====================
// Phase H.0 重构 — Tick-Render 解耦.
//
// 严格顺序 (CONSENSUS §2.6):
//   1) LT::TickRender::BeginFrame  (累积 frameTime)
//   2) while ShouldStepFixed:
//        a. 物理 auto-step (Box2D + Bullet, 仅 SetAutoStep(true) 的 World)
//        b. Lua: OnFixedUpdate(fixedDt)
//        c. ConsumeFixedStep
//   3) LT::TickRender::FinalizeFrame  (alpha + accumulator clamp)
//   4) BeginFrame + AssetLoader::Tick + Batch/HDR/TAA::Begin + TAA::ApplyJitter
//   5) Lua: Draw  (旧)
//   6) Lua: Update(frameTime)  (旧, dt = wall-clock)
//   7) Lua: OnRender(alpha, frameTime)  (新 H.0; 仅当 Lua 定义时调)
//   8) Batch/HDR/TAA::End + g_render->EndFrame
//   9) RecordTickHook + DrawRecordOSD
//  10) SwapBuffers

// Phase H.0 — 调 Lua 回调 helper (减少 6 处 lua_getfield+pcall+log 重复).
// windowIdx: Window self table 在 lua 栈的索引 (通常为 1)
// argc: 0 / 1 / 2; 通过 numArgs 控制 push 几个 arg
static void CallLuaWindowCallback_(lua_State* L, int windowIdx,
                                    const char* name, int argc,
                                    double arg1 = 0.0, double arg2 = 0.0) {
    lua_getfield(L, windowIdx, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushvalue(L, windowIdx);
    if (argc >= 1) lua_pushnumber(L, arg1);
    if (argc >= 2) lua_pushnumber(L, arg2);
    if (lua_pcall(L, 1 + argc, 0, 0)) {
        CC::Log(CC::LOG_ERROR, "%s: %s", name, lua_tostring(L, -1));
        lua_pop(L, 1);
    }
}

// Phase H.0 — 物理 auto-step 桥接.
// 实现位于 light_time.cpp (Light_PhysicsAutoStepAll → LT::PhysicsRegistry::StepAllAuto).
// 物理子系统 (Box2D / Bullet) 通过 LT::PhysicsRegistry::RegisterWorld 注入回调,
// 主循环每个 fixed step 调一次. 列表为空 (无 World 注册) 时零开销早退.
extern "C" void Light_PhysicsAutoStepAll(double dt);

static int l_Window_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    if (!g_mainWindow || PlatformWindow::ShouldClose(g_mainWindow)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // ---------- Phase H.0 步骤 1: BeginFrame ----------
    LT::TickRender::BeginFrame();

    // ---------- Phase H.0 步骤 2: Fixed-step 累积器循环 ----------
    while (LT::TickRender::ShouldStepFixed()) {
        const double fixedDt = LT::TickRender::GetFixedDt();
        // 2a) 物理 auto-step (T4A/T4B 实现)
        Light_PhysicsAutoStepAll(fixedDt);
        // 2b) Lua OnFixedUpdate(fixedDt)
        CallLuaWindowCallback_(L, 1, "OnFixedUpdate", 1, fixedDt);
        // 2c) 消费 1 步
        LT::TickRender::ConsumeFixedStep();
    }

    // ---------- Phase H.0 步骤 3: FinalizeFrame ----------
    LT::TickRender::FinalizeFrame();
    const double alpha     = LT::TickRender::GetAlpha();
    const double frameTime = LT::TickRender::GetLastFrameTime();   // 已 clamp

    // ---------- 步骤 4: 渲染上下文 setup ----------
    if (g_render) {
        g_render->BeginFrame(0, 0, 0, 1);
    }
    // Phase G.1: 主线程 drain async-asset 结果, 上传 GL + 标 Future ready
    AssetLoader::Tick();
    if (BatchRenderer::IsInited())    BatchRenderer::BeginFrame();
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::BeginFrame();
    // Phase E.3.2: HDR 启用时切到 HDR RT (所有 Draw 绘到 RGBA16F 离屏)
    if (HDRRenderer::IsEnabled())     HDRRenderer::BeginScene();
    // Phase F.0: TAA 启用时注入 sub-pixel jitter 到 backend (raster 用 jittered projection)
    //   GetProjection() 仍返 unjittered → SSR/SSAO/velocity 零改动
    //   需在 BeginScene 后 (确保 HDR FBO 已绑) + 用户 Draw 之前
    if (TAARenderer::IsEnabled())     TAARenderer::ApplyJitter();

    // ---------- 步骤 5: Lua Draw (旧) ----------
    CallLuaWindowCallback_(L, 1, "Draw", 0);

    // ---------- 步骤 6: Lua Update(frameTime) (旧, wall-clock dt) ----------
    CallLuaWindowCallback_(L, 1, "Update", 1, frameTime);

    // ---------- 步骤 7 (Phase H.0): Lua OnRender(alpha, frameTime) (新) ----------
    CallLuaWindowCallback_(L, 1, "OnRender", 2, alpha, frameTime);

    // 结束帧 + 交换缓冲区
    // 顺序: Lit 批先 EndFrame (依赖 BatchRenderer 不破坏 Lit shader state),
    //       详见 light_graphics.cpp::l_DrawLit 中的双向 Flush 需求.
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::EndFrame();
    if (BatchRenderer::IsInited())    BatchRenderer::EndFrame();
    // Phase E.3.2: HDR EndScene — 解绑 HDR RT + ACES tonemap 到 default fb.
    // 必须在两个 BatchRenderer::EndFrame 之后 (保证 HDR RT 内容已全部 flush),
    // 在 g_render->EndFrame() 和 SwapBuffers 之前 (tonemap 结果就在 default fb 上).
    if (HDRRenderer::IsEnabled())     HDRRenderer::EndScene();
    if (g_render) g_render->EndFrame();

    // Phase F.0.11 — 录屏 hook (PNG sequence 输出)
    // 必须在 g_render->EndFrame() 之后, SwapBuffers 之前: default fb 内容已就绪,
    // 但尚未 swap 出去, 读 GL_BACK 是确定的本帧画面.
    // hook 内部检查 g_record.active, inactive 时是廉价 no-op.
    {
        int win_w = 0, win_h = 0;
        PlatformWindow::GetWindowSize(g_mainWindow, &win_w, &win_h);
        Light_Graphics_RecordTickHook(win_w, win_h);
        // Phase F.0.11.6.1.A7: OSD 红点闪烁 — 在 readback (RecordTickHook) 之后绘制,
        //   保证 mp4 内不含 OSD; 仅在屏幕显示给用户视觉反馈 (录屏中).
        Light_Graphics_DrawRecordOSD(win_w, win_h);
    }

    PlatformWindow::SwapBuffers(g_mainWindow);

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Window:__tostring ====================

static int l_Window_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.UI.Window");
    return 1;
}

// ==================== 退出审计 (Phase G.1.3 加固) ====================
// 在进程 atexit 阶段自检 window / render / AssetLoader / platform 状态。
// 正常路径 (PerformWindowShutdown_ 已被走过): 全部 nullptr/false → 静默。
// 绕过清理路径: 输出 stderr 警告, 帮助开发者发现自己的 sample 退出路径错误。
//
// 注意: 此 hook 在 main() 返回后、静态析构前运行 (C atexit 语义).
//   - 不能用 CC::Log (依赖未知)
//   - 不能调 GL / Lua (上下文已失效)
//   - 仅安全的 fputs/fprintf 到 stderr
namespace {

static void WindowLifecycleAudit_() {
    int n = 0;
    const char* warns[8] = {nullptr};
    if (g_mainWindow)               warns[n++] = "g_mainWindow not destroyed";
    if (g_glContext)                warns[n++] = "g_glContext not destroyed";
    if (g_render)                   warns[n++] = "g_render not deleted (renderer may leak)";
    if (g_platformInited)           warns[n++] = "PlatformWindow::Shutdown not called (SDL3 still inited)";
    if (g_windowRef != LUA_NOREF)   warns[n++] = "g_windowRef still held in Lua registry";
    if (AssetLoader::IsRunning())   warns[n++] = "AssetLoader worker thread still running (joinable thread will std::terminate at static dtor)";

    if (n == 0) return;   // 正常路径: 静默

    // 用 fputs/fprintf 直写 stderr, 避免依赖任何可能已析构的子系统
    fputs("\n[ChocoLight] WARNING: window lifecycle audit failed at process exit:\n", stderr);
    for (int i = 0; i < n; ++i) {
        fprintf(stderr, "  - %s\n", warns[i]);
    }
    fputs("[ChocoLight] Likely cause: main loop exited without going through UI.Loop / UI.Resume\n", stderr);
    fputs("              cleanup path (e.g. early `break`, exception, or os.exit before window close).\n", stderr);
    fputs("[ChocoLight] Hint: ensure user calls self:Close() inside Update/Draw, and use the\n", stderr);
    fputs("              standard pattern `while UI.Loop() do UI.Resume() end` without breaking out early.\n", stderr);
    fputs("[ChocoLight] See docs/API_REFERENCE.md (Light.UI section) for the correct lifecycle.\n", stderr);
    fflush(stderr);
}

// 用静态 ctor 注册 atexit 钩子; 仅依赖 <cstdlib> 已 include.
struct WindowLifecycleAuditor {
    WindowLifecycleAuditor() { atexit(&WindowLifecycleAudit_); }
};
static WindowLifecycleAuditor g_lifecycleAuditor;

} // namespace

// ==================== 窗口关闭统一清理路径 (Phase G.1.x fix) ====================
// 历史问题: 此前清理逻辑只在 l_UI_Resume 的 ShouldClose 分支内执行。
// 但 sample 通用模板 `while UI.Loop() do UI.Resume() end` 在 UI.Loop=false 时
// 直接退出 while, 此后不会再调 UI.Resume → 清理永远不跑 → worker thread / GL ctx
// 等资源未释放 → 进程退出时 std::thread::~thread() joinable 触发 std::terminate
// (Windows 上表现为 STATUS_STACK_BUFFER_OVERRUN, 0xC0000409)。
//
// 修复: 抽为独立 helper, l_UI_Loop 与 l_UI_Resume 在任一路径首次检测到 ShouldClose
// 都调用此函数; 内部用 g_mainWindow != nullptr 作为幂等 guard, 避免重复执行。
static void PerformWindowShutdown_(lua_State* L) {
    if (!g_mainWindow) return;   // 幂等: 已清理过则直接返回

    if (g_windowRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, g_windowRef);
        g_windowRef = LUA_NOREF;
    }
    // Phase G.1: 关 AssetLoader (worker join + 清队列). 必须在 g_render 销毁前
    // 完成最后一次 Tick (Tick 仍可调 backend->CreateTexture). 但出于稳健考虑,
    // 这里不再 Tick — Shutdown 直接把 pending future 标 Error.
    AssetLoader::Shutdown();
    PlatformNet::Shutdown();
    AudioBackend::Shutdown();
    // Phase H.0: Tick-Render 解耦关闭 (重置 accumulator / PhysicsRegistry 留给 World 析构自清)
    LT::TickRender::Shutdown();
    // Phase F.0: TAA 依赖 HDR sceneTex + velocity + dilation; 管线最末端 → 最先关闭
    TAARenderer::Shutdown();
    // Phase E.15: Motion Blur 依赖 HDR sceneTex + velocityTex; 最先关闭 (管线末端)
    MotionBlurRenderer::Shutdown();
    // Phase E.9: SSR 依赖 HDR RT + G-buffer normal; 最先关闭 (管线末端, 在 SSAO 之前)
    SSRRenderer::Shutdown();
    // Phase E.8.2: SSAO 依赖 HDR RT depth; 最先关闭 (管线末端)
    SSAORenderer::Shutdown();
    // Phase E.7.2: LensFlare 依赖 Bloom + HDR; 最先关闭 (管线末端)
    LensFlareRenderer::Shutdown();
    // Phase E.6.2: LensFx 依赖 backend + Bloom; 最先关闭 (反初始化顺序)
    StreakRenderer::Shutdown();
    LensDirtRenderer::Shutdown();
    // Phase E.5.2: AutoExposureRenderer 依赖 backend、先于 Bloom/HDR 关闭 (luminance RT 独立)
    AutoExposureRenderer::Shutdown();
    // Phase E.4.2: BloomRenderer 依赖 backend、先于 HDR 关闭 (pyramid 拍拭不注册 depth RBO map)
    BloomRenderer::Shutdown();
    HDRRenderer::Shutdown();
    LitBatchRenderer::Shutdown();
    BatchRenderer::Shutdown();
    if (g_render) {
        g_render->Shutdown();
        delete g_render;
        g_render = nullptr;
    }
    PlatformWindow::DestroyGLContext(g_glContext);
    PlatformWindow::DestroyWindow(g_mainWindow);
    g_glContext      = nullptr;
    g_mainWindow     = nullptr;
    PlatformWindow::Shutdown();
    g_platformInited = false;
}

// ==================== UI.Loop ====================

static int l_UI_Loop(lua_State* L) {
    // 检测 ShouldClose: 若窗口已经标记关闭, 立刻执行一次清理 (幂等)
    // 这样兼容 sample 写法 `while UI.Loop() do UI.Resume() end` — 即使后续 UI.Resume
    // 不再被调用, 清理路径也必然执行一次, 避免 worker thread 未 join 的崩溃。
    if (g_mainWindow && PlatformWindow::ShouldClose(g_mainWindow)) {
        PerformWindowShutdown_(L);
    }
    lua_pushboolean(L, g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow));
    return 1;
}

// ==================== Phase H.0.2 — 单帧执行 helper ====================
// 抽出 l_UI_Resume / l_UI_RunBrowserMainLoop / BrowserMainLoopFrame_ 共用的单帧逻辑.
// 不处理 ShouldClose; 调用方自行检查.
// 不操作 Lua 栈 (Pcall 内自管栈; 异常已 Log 不外抛).
static void RunSingleFrame_(lua_State* L) {
    // 1) 拉取并分发事件 (SDL3 拉模式)
    DispatchEvents(L);

    if (!g_mainWindow) return;

    // 2) 反调试周期检查
    LightAntiDebug::Check();
    float af = LightAntiDebug::GetAnomalyFactor();
    bool skipRender = (af > 0.0f && (rand() % 100) < (int)(af * 30.0f));

    // 3) 调用 Window:__call (内部走 H.0 主循环 11 步: BeginFrame / fixed / FinalizeFrame / Draw / OnRender / Swap)
    if (g_windowRef != LUA_NOREF && !skipRender) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
        lua_getfield(L, -1, "__call");
        if (lua_isfunction(L, -1)) {
            lua_pushvalue(L, -2);  // self
            lua_pushvalue(L, -3);  // Window table 作为 arg
            if (lua_pcall(L, 2, 1, 0)) {
                CC::Log(CC::LOG_ERROR, "RunSingleFrame: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            lua_pop(L, 1);  // 丢弃返回值
        } else {
            lua_pop(L, 1);
        }
        lua_pop(L, 1);  // 弹出 Window table
    }
}

// ==================== UI.Resume ====================
// 主事件泵: 拉取 SDL 事件 → 分发到 Lua 回调 → 调用 Window:__call

static int l_UI_Resume(lua_State* L) {
    if (!g_platformInited) {
        lua_pushboolean(L, 1);  // 平台未初始化时保持存活
        return 1;
    }

    if (g_mainWindow && PlatformWindow::ShouldClose(g_mainWindow)) {
        // 窗口关闭 → 走统一清理路径 (与 l_UI_Loop 共用, 幂等)
        // 注: ShouldClose 检测前不需要先 DispatchEvents, 因为 l_UI_Loop 调用前已 dispatch 过.
        //     此分支主要捕获本帧之前已设的关闭标志.
        PerformWindowShutdown_(L);
        lua_pushboolean(L, 0);
        return 1;
    }

    if (!g_mainWindow) {
        // 窗口未创建仍允许调 (event-only mode); 拉一次事件即返
        DispatchEvents(L);
        lua_pushboolean(L, 0);
        return 1;
    }

    // 标准路径: 单帧执行
    RunSingleFrame_(L);

    // RunSingleFrame_ 内可能触发 OnClose → ShouldClose=true; 此处再查一次确保紧 close 即返 false
    if (PlatformWindow::ShouldClose(g_mainWindow)) {
        PerformWindowShutdown_(L);
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase H.0.2 — Browser Main Loop ====================
// Web 平台: emscripten_set_main_loop_arg(frame, L, 0, 1) — 浏览器异步驱动, 永不返回
// Native:   阻塞 while 等价 `while UI.Loop() do UI.Resume() end`
//
// 加性 API: 32+ 老 sample 不调用此函数, 仍走 ASYNCIFY 路径; 新 sample 可一行替代 while 循环.

#ifdef __EMSCRIPTEN__
// 浏览器每帧 callback. arg = lua_State*.
// 内部检查 ShouldClose; 关闭后 CancelMainLoop + Shutdown.
static void BrowserMainLoopFrame_(void* arg) {
    lua_State* L = (lua_State*)arg;
    if (!g_mainWindow || PlatformWindow::ShouldClose(g_mainWindow)) {
        PlatformWindow::CancelMainLoop();
        PerformWindowShutdown_(L);
        return;
    }
    RunSingleFrame_(L);
}
#endif

static int l_UI_RunBrowserMainLoop(lua_State* L) {
    if (!g_platformInited) {
        // 平台未初始化 → no-op
        return 0;
    }
#ifdef __EMSCRIPTEN__
    // 浏览器异步主循环 (simulate_infinite_loop=1: 永不返回)
    // 浏览器后台标签页时自动暂停 callback → 节能
    PlatformWindow::RunMainLoop(&BrowserMainLoopFrame_, L);
    // 不可达
    return 0;
#else
    // Native: 阻塞循环 = 等价 lua `while UI.Loop() do UI.Resume() end`
    while (g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow)) {
        RunSingleFrame_(L);
    }
    PerformWindowShutdown_(L);
    return 0;
#endif
}

// ==================== Window:StartTextInput / StopTextInput / IsTextInputActive (Phase AQ) ====================
// Lua: window:StartTextInput()  — 启用文本输入事件 (移动端弹软键盘)
// Lua: window:StartTextInput({type="email", capitalization="sentences", autocorrect=true, multiline=false})
// 类型枚举字符串: "text" / "name" / "email" / "username" / "password" / "password_visible" / "number" / "number_password" / "number_password_visible"
// 大小写枚举: "none" / "sentences" / "words" / "letters"

static int ParseTextInputType(const char* s) {
    if (!s) return -1;
    if (strcmp(s, "text") == 0)               return 0;
    if (strcmp(s, "name") == 0)               return 1;
    if (strcmp(s, "email") == 0)              return 2;
    if (strcmp(s, "username") == 0)           return 3;
    if (strcmp(s, "password") == 0)           return 4;
    if (strcmp(s, "password_hidden") == 0)    return 4;
    if (strcmp(s, "password_visible") == 0)   return 5;
    if (strcmp(s, "number") == 0)             return 6;
    if (strcmp(s, "number_password") == 0)        return 7;
    if (strcmp(s, "number_password_hidden") == 0) return 7;
    if (strcmp(s, "number_password_visible") == 0)return 8;
    return -1;
}

static int ParseCapitalization(const char* s) {
    if (!s) return -1;
    if (strcmp(s, "none") == 0)      return 0;
    if (strcmp(s, "sentences") == 0) return 1;
    if (strcmp(s, "words") == 0)     return 2;
    if (strcmp(s, "letters") == 0)   return 3;
    return -1;
}

static int l_Window_StartTextInput(lua_State* L) {
    if (!g_mainWindow) { lua_pushboolean(L, 0); return 1; }

    // 在调用者提供 table 时走带属性路径
    if (lua_istable(L, 2)) {
        PlatformWindow::TextInputProps props;

        // type 可以传 string 枚举或 integer (SDL_TEXTINPUT_TYPE_*)
        lua_getfield(L, 2, "type");
        if (lua_isnumber(L, -1)) {
            props.type = (int)lua_tointeger(L, -1);
        } else if (lua_isstring(L, -1)) {
            props.type = ParseTextInputType(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        // capitalization 同上
        lua_getfield(L, 2, "capitalization");
        if (lua_isnumber(L, -1)) {
            props.capitalization = (int)lua_tointeger(L, -1);
        } else if (lua_isstring(L, -1)) {
            props.capitalization = ParseCapitalization(lua_tostring(L, -1));
        }
        lua_pop(L, 1);

        // autocorrect 布尔
        lua_getfield(L, 2, "autocorrect");
        if (!lua_isnil(L, -1)) {
            props.autocorrect = lua_toboolean(L, -1) ? 1 : 0;
        }
        lua_pop(L, 1);

        // multiline 布尔
        lua_getfield(L, 2, "multiline");
        if (!lua_isnil(L, -1)) {
            props.multiline = lua_toboolean(L, -1) ? 1 : 0;
        }
        lua_pop(L, 1);

        bool ok = PlatformWindow::StartTextInputWithProps(g_mainWindow, props);
        lua_pushboolean(L, ok ? 1 : 0);
        return 1;
    }

    bool ok = PlatformWindow::StartTextInput(g_mainWindow);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

static int l_Window_StopTextInput(lua_State* L) {
    if (g_mainWindow) PlatformWindow::StopTextInput(g_mainWindow);
    return 0;
}

static int l_Window_IsTextInputActive(lua_State* L) {
    bool active = g_mainWindow ? PlatformWindow::IsTextInputActive(g_mainWindow) : false;
    lua_pushboolean(L, active ? 1 : 0);
    return 1;
}

// Lua: window:SetTextInputArea(x, y, w, h, [cursor])
static int l_Window_SetTextInputArea(lua_State* L) {
    int x      = (int)luaL_checkinteger(L, 2);
    int y      = (int)luaL_checkinteger(L, 3);
    int w      = (int)luaL_checkinteger(L, 4);
    int h      = (int)luaL_checkinteger(L, 5);
    int cursor = (int)luaL_optinteger(L, 6, 0);
    if (g_mainWindow) PlatformWindow::SetTextInputArea(g_mainWindow, x, y, w, h, cursor);
    return 0;
}

static int l_Window_ClearComposition(lua_State* L) {
    if (g_mainWindow) PlatformWindow::ClearComposition(g_mainWindow);
    return 0;
}

static int l_Window_IsScreenKeyboardShown(lua_State* L) {
    bool shown = g_mainWindow ? PlatformWindow::IsScreenKeyboardShown(g_mainWindow) : false;
    lua_pushboolean(L, shown ? 1 : 0);
    return 1;
}

// ==================== Window:SetOrientation ====================
// Lua: Window:SetOrientation("landscape"|"portrait"|"auto")
// Android: JNI 调用 setRequestedOrientation
// 其他平台: no-op
static int l_Window_SetOrientation(lua_State* L) {
#ifdef __ANDROID__
    const char* mode = luaL_checkstring(L, 2);
    // Android ActivityInfo 常量
    int orient = 6; // 默认 SCREEN_ORIENTATION_SENSOR_LANDSCAPE
    if (strcmp(mode, "portrait") == 0)       orient = 7;  // SENSOR_PORTRAIT
    else if (strcmp(mode, "auto") == 0)      orient = 10; // FULL_SENSOR
    else if (strcmp(mode, "landscape") == 0) orient = 6;  // SENSOR_LANDSCAPE

    JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
    jobject activity = (jobject)SDL_GetAndroidActivity();
    if (env && activity) {
        jclass cls = env->GetObjectClass(activity);
        jmethodID mid = env->GetMethodID(cls, "setRequestedOrientation", "(I)V");
        if (mid) env->CallVoidMethod(activity, mid, orient);
        env->DeleteLocalRef(cls);
    }
#else
    (void)L;
#endif
    return 0;
}

// ==================== luaopen 注册 ====================

static const luaL_Reg ui_funcs[] = {
    {"Loop",   l_UI_Loop},
    {"Resume", l_UI_Resume},
    // Phase H.0.2 — 跨平台一行主循环 (web: emscripten_set_main_loop_arg; native: 阻塞 while)
    {"RunBrowserMainLoop", l_UI_RunBrowserMainLoop},
    {NULL, NULL}
};

int luaopen_Light_UI(lua_State* L) {
    LT::RegisterModule(L, "UI", ui_funcs);

    // Phase AQ - TextInput / IME 常量 (与 SDL_TEXTINPUT_TYPE_* / SDL_CAPITALIZE_* 数字一致)
    // 用户也可用字符串枚举: {type="email", capitalization="sentences"}, 这些常量是数字版本
    lua_pushinteger(L, 0); lua_setfield(L, -2, "TEXTINPUT_TYPE_TEXT");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "TEXTINPUT_TYPE_NAME");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "TEXTINPUT_TYPE_EMAIL");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "TEXTINPUT_TYPE_USERNAME");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "TEXTINPUT_TYPE_PASSWORD_HIDDEN");
    lua_pushinteger(L, 5); lua_setfield(L, -2, "TEXTINPUT_TYPE_PASSWORD_VISIBLE");
    lua_pushinteger(L, 6); lua_setfield(L, -2, "TEXTINPUT_TYPE_NUMBER");
    lua_pushinteger(L, 7); lua_setfield(L, -2, "TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN");
    lua_pushinteger(L, 8); lua_setfield(L, -2, "TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE");
    lua_pushinteger(L, 0); lua_setfield(L, -2, "CAPITALIZATION_NONE");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "CAPITALIZATION_SENTENCES");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "CAPITALIZATION_WORDS");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "CAPITALIZATION_LETTERS");

    // Phase AR - Pen 输入状态位 (与 SDL_PEN_INPUT_* 一致)
    lua_pushinteger(L, 1u << 0);  lua_setfield(L, -2, "PEN_INPUT_DOWN");
    lua_pushinteger(L, 1u << 1);  lua_setfield(L, -2, "PEN_INPUT_BUTTON_1");
    lua_pushinteger(L, 1u << 2);  lua_setfield(L, -2, "PEN_INPUT_BUTTON_2");
    lua_pushinteger(L, 1u << 3);  lua_setfield(L, -2, "PEN_INPUT_BUTTON_3");
    lua_pushinteger(L, 1u << 4);  lua_setfield(L, -2, "PEN_INPUT_BUTTON_4");
    lua_pushinteger(L, 1u << 5);  lua_setfield(L, -2, "PEN_INPUT_BUTTON_5");
    lua_pushinteger(L, 1u << 30); lua_setfield(L, -2, "PEN_INPUT_ERASER_TIP");

    // Phase AR - Pen 轴 ID (与 SDL_PenAxis 枚举一致)
    lua_pushinteger(L, 0); lua_setfield(L, -2, "PEN_AXIS_PRESSURE");
    lua_pushinteger(L, 1); lua_setfield(L, -2, "PEN_AXIS_XTILT");
    lua_pushinteger(L, 2); lua_setfield(L, -2, "PEN_AXIS_YTILT");
    lua_pushinteger(L, 3); lua_setfield(L, -2, "PEN_AXIS_DISTANCE");
    lua_pushinteger(L, 4); lua_setfield(L, -2, "PEN_AXIS_ROTATION");
    lua_pushinteger(L, 5); lua_setfield(L, -2, "PEN_AXIS_SLIDER");
    lua_pushinteger(L, 6); lua_setfield(L, -2, "PEN_AXIS_TANGENTIAL_PRESSURE");
    lua_pushinteger(L, 7); lua_setfield(L, -2, "PEN_AXIS_COUNT");
    return 1;
}

// Window — 12 函数 (与 IDA 注册表精确匹配)
int luaopen_Light_UI_Window(lua_State* L) {
    luaopen_Light_UI(L);

    lua_pushstring(L, "Window");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Window");
        lua_createtable(L, 0, 0);

        const luaL_Reg win_funcs[] = {
            {"Open",          l_Window_Open},
            {"Close",         l_Window_Close},
            {"ID",            l_Window_ID},
            {"GetWidth",      l_Window_GetWidth},
            {"GetHeight",     l_Window_GetHeight},
            {"GetDimensions", l_Window_GetDimensions},
            {"SetWidth",      l_Window_SetWidth},
            {"SetHeight",     l_Window_SetHeight},
            {"SetDimensions", l_Window_SetDimensions},
            {"SetVSync",      l_Window_SetVSync},
            {"GetDPIScale",   l_Window_GetDPIScale},
            {"SetOrientation", l_Window_SetOrientation},
            // Phase AQ — TextInput / IME (6 个方法)
            {"StartTextInput",        l_Window_StartTextInput},
            {"StopTextInput",         l_Window_StopTextInput},
            {"IsTextInputActive",     l_Window_IsTextInputActive},
            {"SetTextInputArea",      l_Window_SetTextInputArea},
            {"ClearComposition",      l_Window_ClearComposition},
            {"IsScreenKeyboardShown", l_Window_IsScreenKeyboardShown},
            {"__call",        l_Window_Call},
            {"__tostring",    l_Window_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, win_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Window");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
