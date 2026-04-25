/**
 * @file light_ui.cpp
 * @brief Light.UI + Light.UI.Window 模块 — 真实 GLFW 窗口管理
 * @note 深度还原自 Light.dll IDA 反编译 + GLFW 3.4 静态链接
 *
 * Window API (12 函数，精确匹配 luaopen_Light_UI_Window 注册表):
 *   Open(w, h, title)    — 创建 GLFW 窗口, 默认标题 "ChocoLight Engine"
 *   Close()              — 关闭窗口
 *   ID()                 — 获取原生窗口句柄
 *   Get/SetWidth         — 宽度存取
 *   Get/SetHeight        — 高度存取
 *   Get/SetDimensions    — 尺寸存取 (返回 w,h 两个值)
 *   SetVSync(bool)       — 垂直同步开关
 *   __call()             — 事件循环步进 (Poll + SwapBuffers)
 *   __tostring()         — "Light.UI.Window"
 *
 * UI.Resume — light.exe 事件泵 (每帧调用)
 */

#include "light.h"
#include "light_antidebug.h"
#include "render_backend.h"
#include "light_audio_backend.h"
#include "light_platform_net.h"
#include <cstdint>

// GLFW 头文件
#include <GLFW/glfw3.h>

// ==================== 全局 GLFW 状态 ====================

static bool g_glfwInitialized = false;
static GLFWwindow* g_mainWindow = nullptr;
static lua_State* g_callbackL = nullptr;  // 回调用的 Lua 状态

// Lua 回调引用 (存储在注册表中的 Window 实例)
static int g_windowRef = LUA_NOREF;

/// GLFW 初始化 (懒加载)
static bool EnsureGLFW() {
    if (g_glfwInitialized) return true;
    if (glfwInit()) {
        g_glfwInitialized = true;
        LightAntiDebug::Init();  // Initialize anti-debug alongside GLFW
        CC::Log(CC::LOG_INFO, "GLFW initialized: %s", glfwGetVersionString());
        return true;
    }
    CC::Log(CC::LOG_ERROR, "GLFW init failed");
    return false;
}

// ==================== GLFW 回调 ====================

/// 键盘回调 → 调用 Window:OnKey(key, scanCode, action, mods)
static void glfw_key_callback(GLFWwindow* win, int key, int scanCode, int action, int mods) {
    if (!g_callbackL || g_windowRef == LUA_NOREF) return;
    lua_State* L = g_callbackL;
    int top = lua_gettop(L);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnKey");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);  // self
        lua_pushinteger(L, key);
        lua_pushinteger(L, scanCode);
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

/// 鼠标按钮回调 → 调用 Window:OnMouseButton(x, y, button, action, mods)
static void glfw_mouse_button_callback(GLFWwindow* win, int button, int action, int mods) {
    if (!g_callbackL || g_windowRef == LUA_NOREF) return;
    lua_State* L = g_callbackL;
    int top = lua_gettop(L);

    double mx, my;
    glfwGetCursorPos(win, &mx, &my);

    lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
    lua_getfield(L, -1, "OnMouseButton");
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, -2);  // self
        lua_pushnumber(L, mx);
        lua_pushnumber(L, my);
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

/// 鼠标移动回调 → 调用 Window:OnMousePosition(x, y)
static void glfw_cursor_pos_callback(GLFWwindow* win, double x, double y) {
    if (!g_callbackL || g_windowRef == LUA_NOREF) return;
    lua_State* L = g_callbackL;
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

/// 设置 2D 正交投影 (原点左上角, Y 轴向下)
static void SetupOrthoProjection(int width, int height) {
    if (g_render) {
        g_render->SetViewport(0, 0, width, height);
        g_render->LoadOrtho(0, (float)width, (float)height, 0, -1.0f, 1.0f);
    } else {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(0, width, height, 0, -1.0, 1.0);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }
}

/// 窗口大小回调 → 更新视口和投影
static void glfw_framebuffer_size_callback(GLFWwindow* win, int width, int height) {
    if (g_render) {
        g_render->SetViewport(0, 0, width, height);
    } else {
        glViewport(0, 0, width, height);
    }
    SetupOrthoProjection(width, height);
}

// ==================== Window:Open ====================
// 还原自 sub_1800A7FD0 — 多重载参数:
// Open()           → 默认宽度, 高600
// Open(w)          → w×600
// Open(w, h)       → w×h, 默认标题
// Open(w, h, title)→ w×h, 自定义标题

static int l_Window_Open(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int argc = lua_gettop(L);

    int width = 800, height = 600;
    const char* title = "ChocoLight Engine";

    // 多重载解析 (还原自 IDA sub_1800A7FD0)
    switch (argc) {
    case 4: title  = luaL_checkstring(L, 4);  // fallthrough
    case 3: height = (int)luaL_checkinteger(L, 3); // fallthrough
    case 2: width  = (int)luaL_checkinteger(L, 2); break;
    default: break;
    }

    if (!EnsureGLFW()) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // ======== 运行时检测: 先尝试 GL 3.3 Core, 失败回退默认 ========
    glfwDefaultWindowHints();
    glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);

    // 策略 1: 尝试 GL 3.3 Core Profile
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif

    g_mainWindow = glfwCreateWindow(width, height, title, nullptr, nullptr);

    if (!g_mainWindow) {
        // 策略 2: 回退到默认兼容模式 (GL 2.1)
        CC::Log(CC::LOG_INFO, "GL 3.3 Core not available, falling back to Legacy");
        glfwDefaultWindowHints();
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
        glfwWindowHint(GLFW_RESIZABLE, GLFW_TRUE);
        g_mainWindow = glfwCreateWindow(width, height, title, nullptr, nullptr);
    }

    if (!g_mainWindow) {
        CC::Log(CC::LOG_ERROR, "Failed to create window %dx%d", width, height);
        lua_pushboolean(L, 0);
        return 1;
    }

    glfwMakeContextCurrent(g_mainWindow);
    glfwSwapInterval(1);  // VSync on by default

    // ======== 初始化渲染后端 (自动检测 GL 版本) ========
    g_render = CreateRenderBackend();
    if (!g_render) {
        CC::Log(CC::LOG_ERROR, "No render backend available, aborting");
        glfwDestroyWindow(g_mainWindow);
        g_mainWindow = nullptr;
        lua_pushboolean(L, 0);
        return 1;
    }
    CC::Log(CC::LOG_INFO, "Render backend: %s", g_render->GetName());

    // ======== 初始化音频后端 ========
    if (!AudioBackend::Init()) {
        CC::Log(CC::LOG_WARN, "AudioBackend: init failed, audio will be unavailable");
    }

    // ======== 初始化网络后端 ========
    if (!PlatformNet::Init()) {
        CC::Log(CC::LOG_WARN, "PlatformNet: init failed, network will be unavailable");
    }

    // 设置回调
    glfwSetKeyCallback(g_mainWindow, glfw_key_callback);
    glfwSetMouseButtonCallback(g_mainWindow, glfw_mouse_button_callback);
    glfwSetCursorPosCallback(g_mainWindow, glfw_cursor_pos_callback);
    glfwSetFramebufferSizeCallback(g_mainWindow, glfw_framebuffer_size_callback);

    // 初始视口 + 2D 正交投影 (通过渲染后端)
    int fbW, fbH;
    glfwGetFramebufferSize(g_mainWindow, &fbW, &fbH);
    SetupOrthoProjection(fbW, fbH);

    // 初始颜色
    g_render->SetColor(1.0f, 1.0f, 1.0f, 1.0f);

    // 保存 Lua 状态和 Window 实例引用 (用于回调)
    g_callbackL = L;
    lua_pushvalue(L, 1);
    g_windowRef = luaL_ref(L, LUA_REGISTRYINDEX);

    // 调用 OnOpen 回调 (如果存在)
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
// 还原自 sub_1800A83C0

static int l_Window_Close(lua_State* L) {
    if (g_mainWindow) {
        glfwSetWindowShouldClose(g_mainWindow, GLFW_TRUE);
    }
    return 0;
}

// ==================== Window:ID ====================
// 还原自 sub_1800A7EE0 — 返回原生窗口句柄数字

static int l_Window_ID(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(intptr_t)g_mainWindow);
    return 1;
}

// ==================== Window:GetWidth ====================
// 还原自 sub_1800A7BC0

static int l_Window_GetWidth(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) glfwGetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, w);
    return 1;
}

// ==================== Window:GetHeight ====================
// 还原自 sub_1800A7AE0

static int l_Window_GetHeight(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) glfwGetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, h);
    return 1;
}

// ==================== Window:GetDimensions ====================
// 还原自 sub_1800A7C40 — 返回 width, height 两个值

static int l_Window_GetDimensions(lua_State* L) {
    int w = 0, h = 0;
    if (g_mainWindow) glfwGetWindowSize(g_mainWindow, &w, &h);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
    return 2;
}

// ==================== Window:SetWidth ====================
// 还原自 sub_1800A82D0

static int l_Window_SetWidth(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 2);
    if (g_mainWindow) {
        int _, h;
        glfwGetWindowSize(g_mainWindow, &_, &h);
        glfwSetWindowSize(g_mainWindow, w, h);
    }
    return 0;
}

// ==================== Window:SetHeight ====================
// 还原自 sub_1800A81D0

static int l_Window_SetHeight(lua_State* L) {
    int h = (int)luaL_checkinteger(L, 2);
    if (g_mainWindow) {
        int w, _;
        glfwGetWindowSize(g_mainWindow, &w, &_);
        glfwSetWindowSize(g_mainWindow, w, h);
    }
    return 0;
}

// ==================== Window:SetDimensions ====================
// 还原自 sub_1800A7D40

static int l_Window_SetDimensions(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    if (g_mainWindow) glfwSetWindowSize(g_mainWindow, w, h);
    return 0;
}

// ==================== Window:SetVSync ====================
// 还原自 sub_1800A8070

static int l_Window_SetVSync(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int vsync = lua_toboolean(L, 2);
    if (g_mainWindow) {
        glfwMakeContextCurrent(g_mainWindow);
        glfwSwapInterval(vsync ? 1 : 0);
    }
    lua_pushboolean(L, vsync);
    return 1;
}

// ==================== Window:__call ====================
// 还原自 sub_1800A8450 — 处理窗口中的 Update/Draw 回调
// 事件循环步进: 清屏 → Draw() → Update(dt) → SwapBuffers

static int l_Window_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    if (!g_mainWindow || glfwWindowShouldClose(g_mainWindow)) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 获取帧时间
    static double lastTime = glfwGetTime();
    double nowTime = glfwGetTime();
    double dt = nowTime - lastTime;
    lastTime = nowTime;

    // 每帧清屏 + 重置矩阵 (通过渲染后端)
    if (g_render) {
        g_render->BeginFrame(0, 0, 0, 1);
    } else {
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glLoadIdentity();
    }

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
    if (g_render) g_render->EndFrame();
    glfwSwapBuffers(g_mainWindow);

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Window:__tostring ====================
// 还原自 sub_1800A8350

static int l_Window_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.UI.Window");
    return 1;
}

// ==================== UI.Loop ====================

static int l_UI_Loop(lua_State* L) {
    lua_pushboolean(L, g_mainWindow && !glfwWindowShouldClose(g_mainWindow));
    return 1;
}

// ==================== UI.Resume ====================
// 事件循环步进 — light.exe 嵌入脚本的主事件泵
// glfwPollEvents → 处理窗口回调 → 检查窗口关闭
// 返回: true = 继续运行, false/nil = 退出

static int l_UI_Resume(lua_State* L) {
    if (!g_glfwInitialized) {
        lua_pushboolean(L, 1);  // GLFW 未初始化时保持存活
        return 1;
    }

    glfwPollEvents();

    if (g_mainWindow) {
        if (glfwWindowShouldClose(g_mainWindow)) {
            // 窗口关闭 → 清理
            if (g_windowRef != LUA_NOREF) {
                luaL_unref(L, LUA_REGISTRYINDEX, g_windowRef);
                g_windowRef = LUA_NOREF;
            }
            // 释放网络后端
            PlatformNet::Shutdown();
            // 释放音频后端
            AudioBackend::Shutdown();
            // 释放渲染后端
            if (g_render) {
                g_render->Shutdown();
                delete g_render;
                g_render = nullptr;
            }
            glfwDestroyWindow(g_mainWindow);
            g_mainWindow = nullptr;
            glfwTerminate();
            g_glfwInitialized = false;
            lua_pushboolean(L, 0);
            return 1;
        }

        // Anti-debug: periodic check in main loop
        LightAntiDebug::Check();

        // Anti-debug anomaly: randomly skip render when debugger detected
        float af = LightAntiDebug::GetAnomalyFactor();
        bool skipRender = (af > 0.0f && (rand() % 100) < (int)(af * 30.0f));

        // Invoke Window:__call (Update + Draw + SwapBuffers)
        if (g_windowRef != LUA_NOREF && !skipRender) {
            lua_rawgeti(L, LUA_REGISTRYINDEX, g_windowRef);
            lua_getfield(L, -1, "__call");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);  // self
                lua_pushvalue(L, -3);  // Window table as arg
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
        lua_pushboolean(L, 0);  // 无窗口 → 退出
    }
    return 1;
}

// ==================== luaopen 注册 ====================

static const luaL_Reg ui_funcs[] = {
    {"Loop",   l_UI_Loop},
    {"Resume", l_UI_Resume},
    {NULL, NULL}
};

int luaopen_Light_UI(lua_State* L) {
    LT::RegisterModule(L, "UI", ui_funcs);
    return 1;
}

// Window — 12 函数精确匹配 IDA 注册表
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
