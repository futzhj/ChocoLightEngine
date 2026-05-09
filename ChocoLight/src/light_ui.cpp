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
#include "light_audio_backend.h"
#include "light_platform_net.h"
#include "platform_window.h"
#include <cstdint>

// Input 模块事件处理 (定义在 light_input.cpp)
extern void InputProcessEvent(const PlatformWindow::Event& ev);
#include <cstdlib>

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

// ==================== 全局状态 ====================

static bool       g_platformInited = false;
static void*      g_mainWindow     = nullptr;   // PlatformWindow 不透明句柄
static void*      g_glContext      = nullptr;
static lua_State* g_callbackL      = nullptr;   // 持续保存的 Lua state (用于回调)
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

static void SetupOrthoProjection(int width, int height) {
    if (g_render) {
        g_render->SetViewport(0, 0, width, height);
        g_render->LoadOrtho(0, (float)width, (float)height, 0, -1.0f, 1.0f);
    } else {
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width, height, 0, -1.0, 1.0);
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

/// 帧缓冲尺寸变更 → 更新视口和投影
static void OnFramebufferResize(int width, int height) {
    if (g_render) {
        g_render->SetViewport(0, 0, width, height);
    }
    SetupOrthoProjection(width, height);
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

    // 初始化音频后端
    if (!AudioBackend::Init()) {
        CC::Log(CC::LOG_WARN, "AudioBackend: init failed, audio will be unavailable");
    }

    // 初始化网络后端
    if (!PlatformNet::Init()) {
        CC::Log(CC::LOG_WARN, "PlatformNet: init failed, network will be unavailable");
    }

    // 初始视口 + 2D 正交投影
    int fbW, fbH;
    PlatformWindow::GetFramebufferSize(g_mainWindow, &fbW, &fbH);
    SetupOrthoProjection(fbW, fbH);

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
// 事件循环步进: BeginFrame → Draw → Update(dt) → EndFrame → SwapBuffers

static int l_Window_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    if (!g_mainWindow || PlatformWindow::ShouldClose(g_mainWindow)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 帧时间
    static double lastTime = PlatformWindow::GetTime();
    double nowTime = PlatformWindow::GetTime();
    double dt = nowTime - lastTime;
    lastTime = nowTime;

    // 清屏 + 重置矩阵
    if (g_render) {
        g_render->BeginFrame(0, 0, 0, 1);
    }
    if (BatchRenderer::IsInited()) BatchRenderer::BeginFrame();

    // Draw 回调
    lua_getfield(L, 1, "Draw");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        if (lua_pcall(L, 1, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "Draw: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // Update 回调
    lua_getfield(L, 1, "Update");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, 1);
        lua_pushnumber(L, dt);
        if (lua_pcall(L, 2, 0, 0)) {
            CC::Log(CC::LOG_ERROR, "Update: %s", lua_tostring(L, -1));
            lua_pop(L, 1);
        }
    } else {
        lua_pop(L, 1);
    }

    // 结束帧 + 交换缓冲区
    if (BatchRenderer::IsInited()) BatchRenderer::EndFrame();
    if (g_render) g_render->EndFrame();
    PlatformWindow::SwapBuffers(g_mainWindow);

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Window:__tostring ====================

static int l_Window_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.UI.Window");
    return 1;
}

// ==================== UI.Loop ====================

static int l_UI_Loop(lua_State* L) {
    lua_pushboolean(L, g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow));
    return 1;
}

// ==================== UI.Resume ====================
// 主事件泵: 拉取 SDL 事件 → 分发到 Lua 回调 → 调用 Window:__call

static int l_UI_Resume(lua_State* L) {
    if (!g_platformInited) {
        lua_pushboolean(L, 1);  // 平台未初始化时保持存活
        return 1;
    }

    // 1. 拉取并分发事件 (SDL3 拉模式, 替代 GLFW glfwPollEvents 的隐式回调)
    DispatchEvents(L);

    if (g_mainWindow) {
        if (PlatformWindow::ShouldClose(g_mainWindow)) {
            // 窗口关闭 → 清理
            if (g_windowRef != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, g_windowRef);
                g_windowRef = LUA_NOREF;
            }
            PlatformNet::Shutdown();
            AudioBackend::Shutdown();
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
            lua_pushboolean(L, 0);
            return 1;
        }

        // 反调试周期检查
        LightAntiDebug::Check();
        float af = LightAntiDebug::GetAnomalyFactor();
        bool skipRender = (af > 0.0f && (rand() % 100) < (int)(af * 30.0f));

        // 调用 Window:__call (Draw + Update + Swap)
        if (g_windowRef != LUA_NOREF && !skipRender) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
            lua_getfield(L, -1, "__call");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);  // self
                lua_pushvalue(L, -3);  // Window table 作为 arg
                if (lua_pcall(L, 2, 1, 0)) {
                    CC::Log(CC::LOG_ERROR, "Resume: %s", lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
                lua_pop(L, 1);  // 丢弃返回值
            } else {
                lua_pop(L, 1);
            }
            lua_pop(L, 1);  // 弹出 Window table
        }

        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
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
