/**
 * @file platform_window_sdl3.cpp
 * @brief PlatformWindow 的 SDL3 实现
 * @note 仅在此文件中包含 SDL3 头文件, 引擎其他模块不依赖 SDL3
 */

#include "platform_window.h"
#include "light.h"

#include <SDL3/SDL.h>
#include <cstring>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// windows.h (被 SDL.h/light.h 间接拉入) 将 CreateWindow 定义为宏,
// 必须在所有 #include 之后 undef, 否则函数定义会被宏展开破坏
#ifdef CreateWindow
#undef CreateWindow
#endif

// Phase AR — Light.Time 提供的 Timer 事件类型查询函数
// (在全局作用域声明; extern "C" 不允许出现在函数内部块中)
extern "C" Uint32 Time_GetTimerEventType();

namespace PlatformWindow {

// ==================== 内部状态 ====================

static bool s_initialized = false;

// ==================== 键码映射 (SDL_Keycode → GLFW_KEY_*) ====================
// 保持 Lua 脚本对 OnKey 回调的键码值兼容 (旧脚本基于 GLFW 键码)

// GLFW 键码常量 (摘自 glfw3.h, 避免依赖 GLFW 头文件)
namespace GLFW_KEY {
    constexpr int SPACE         = 32;
    constexpr int APOSTROPHE    = 39;
    constexpr int COMMA         = 44;
    constexpr int MINUS         = 45;
    constexpr int PERIOD        = 46;
    constexpr int SLASH         = 47;
    constexpr int N_0           = 48;  // 0..9
    constexpr int SEMICOLON     = 59;
    constexpr int EQUAL         = 61;
    constexpr int A             = 65;  // A..Z
    constexpr int LEFT_BRACKET  = 91;
    constexpr int BACKSLASH     = 92;
    constexpr int RIGHT_BRACKET = 93;
    constexpr int GRAVE_ACCENT  = 96;
    constexpr int ESCAPE        = 256;
    constexpr int ENTER         = 257;
    constexpr int TAB           = 258;
    constexpr int BACKSPACE     = 259;
    constexpr int INSERT        = 260;
    constexpr int DELETE_KEY    = 261;
    constexpr int RIGHT         = 262;
    constexpr int LEFT          = 263;
    constexpr int DOWN          = 264;
    constexpr int UP            = 265;
    constexpr int PAGE_UP       = 266;
    constexpr int PAGE_DOWN     = 267;
    constexpr int HOME          = 268;
    constexpr int END           = 269;
    constexpr int CAPS_LOCK     = 280;
    constexpr int SCROLL_LOCK   = 281;
    constexpr int NUM_LOCK      = 282;
    constexpr int PRINT_SCREEN  = 283;
    constexpr int PAUSE         = 284;
    constexpr int F1            = 290;  // F1..F25
    constexpr int LEFT_SHIFT    = 340;
    constexpr int LEFT_CONTROL  = 341;
    constexpr int LEFT_ALT      = 342;
    constexpr int LEFT_SUPER    = 343;
    constexpr int RIGHT_SHIFT   = 344;
    constexpr int RIGHT_CONTROL = 345;
    constexpr int RIGHT_ALT     = 346;
    constexpr int RIGHT_SUPER   = 347;
    constexpr int MENU          = 348;
}

/// SDL_Keycode → GLFW key 映射
static int SDLKeyToGLFW(SDL_Keycode kc) {
    using namespace GLFW_KEY;
    // ASCII 范围: 字母数字直接对应
    if (kc >= 'a' && kc <= 'z') return A + (kc - 'a');     // 注意 GLFW 用大写 A=65
    if (kc >= 'A' && kc <= 'Z') return A + (kc - 'A');
    if (kc >= '0' && kc <= '9') return N_0 + (kc - '0');

    switch (kc) {
        case SDLK_SPACE:        return SPACE;
        case SDLK_APOSTROPHE:   return APOSTROPHE;
        case SDLK_COMMA:        return COMMA;
        case SDLK_MINUS:        return MINUS;
        case SDLK_PERIOD:       return PERIOD;
        case SDLK_SLASH:        return SLASH;
        case SDLK_SEMICOLON:    return SEMICOLON;
        case SDLK_EQUALS:       return EQUAL;
        case SDLK_LEFTBRACKET:  return LEFT_BRACKET;
        case SDLK_BACKSLASH:    return BACKSLASH;
        case SDLK_RIGHTBRACKET: return RIGHT_BRACKET;
        case SDLK_GRAVE:        return GRAVE_ACCENT;
        case SDLK_ESCAPE:       return ESCAPE;
        case SDLK_RETURN:       return ENTER;
        case SDLK_TAB:          return TAB;
        case SDLK_BACKSPACE:    return BACKSPACE;
        case SDLK_INSERT:       return INSERT;
        case SDLK_DELETE:       return DELETE_KEY;
        case SDLK_RIGHT:        return RIGHT;
        case SDLK_LEFT:         return LEFT;
        case SDLK_DOWN:         return DOWN;
        case SDLK_UP:           return UP;
        case SDLK_PAGEUP:       return PAGE_UP;
        case SDLK_PAGEDOWN:     return PAGE_DOWN;
        case SDLK_HOME:         return HOME;
        case SDLK_END:          return END;
        case SDLK_CAPSLOCK:     return CAPS_LOCK;
        case SDLK_SCROLLLOCK:   return SCROLL_LOCK;
        case SDLK_NUMLOCKCLEAR: return NUM_LOCK;
        case SDLK_PRINTSCREEN:  return PRINT_SCREEN;
        case SDLK_PAUSE:        return PAUSE;
        case SDLK_LSHIFT:       return LEFT_SHIFT;
        case SDLK_LCTRL:        return LEFT_CONTROL;
        case SDLK_LALT:         return LEFT_ALT;
        case SDLK_LGUI:         return LEFT_SUPER;
        case SDLK_RSHIFT:       return RIGHT_SHIFT;
        case SDLK_RCTRL:        return RIGHT_CONTROL;
        case SDLK_RALT:         return RIGHT_ALT;
        case SDLK_RGUI:         return RIGHT_SUPER;
        case SDLK_APPLICATION:  return MENU;
        default: break;
    }
    // F 系列: SDLK_F1 = 0x4000003A 起步
    if (kc >= SDLK_F1 && kc <= SDLK_F12) return F1 + (kc - SDLK_F1);
    return 0; // 未识别
}

/// SDL_Keymod → GLFW 修饰键位掩码 (Shift=1, Ctrl=2, Alt=4, Super=8)
static int SDLModToGLFW(SDL_Keymod mod) {
    int r = 0;
    if (mod & SDL_KMOD_SHIFT) r |= 1;
    if (mod & SDL_KMOD_CTRL)  r |= 2;
    if (mod & SDL_KMOD_ALT)   r |= 4;
    if (mod & SDL_KMOD_GUI)   r |= 8;
    return r;
}

/// SDL 鼠标按键 → GLFW 编号 (Left=0, Right=1, Middle=2)
static int SDLMouseButtonToGLFW(Uint8 btn) {
    switch (btn) {
        case SDL_BUTTON_LEFT:   return 0;
        case SDL_BUTTON_RIGHT:  return 1;
        case SDL_BUTTON_MIDDLE: return 2;
        default: return btn - 1; // X1=3, X2=4
    }
}

// ==================== 生命周期 ====================

bool Init() {
    if (s_initialized) return true;

    // ==================== SDL_HINT 配置 (Phase A2) ====================
    // 性能/兼容/IME 调优, 必须在 SDL_Init 之前调用才生效

    // 双缓冲: 桌面/移动统一启用 (默认即开启, 显式声明意图)
    SDL_SetHint(SDL_HINT_VIDEO_DOUBLE_BUFFER, "1");

    // 鼠标焦点点击穿透: 焦点切换时第一次点击也可被处理 (UI/编辑器场景必需)
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");

    // 屏保抑制: 长时间静止画面也不触发屏保 (游戏/视频播放场景)
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");

    // App ID: Wayland/某些 Linux 桌面要求, 用于任务栏图标分组
    SDL_SetHint(SDL_HINT_APP_ID, "com.chocolight.engine");

    // IME: 启用候选词/合成文本 UI (中文/日文/韩文输入必需)
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition,candidates");

    // VSync: 默认关闭, 由用户通过 Window:SetVSync 控制
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "0");

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        CC::Log(CC::LOG_ERROR, "PlatformWindow: SDL_Init failed: %s", SDL_GetError());
        return false;
    }
    s_initialized = true;
    CC::Log(CC::LOG_INFO, "PlatformWindow: SDL3 initialized (gamepad enabled, IME UI on, app_id=com.chocolight.engine)");
    return true;
}

void Shutdown() {
    if (!s_initialized) return;
    SDL_Quit();
    s_initialized = false;
}

// ==================== 窗口管理 ====================

void* CreateWindow(const char* title, int w, int h, bool fullscreen) {
    if (!s_initialized) return nullptr;

    // 设置 OpenGL 属性 (在创建窗口之前)
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
    // 移动/Web: GLES 3.0 (= WebGL2)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#else
    // 桌面: GL 3.3 Core (引擎渲染层会回退到 2.1 兼容模式)
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#endif
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN;

    SDL_Window* win = SDL_CreateWindow(title, w, h, flags);
    if (!win) {
        CC::Log(CC::LOG_ERROR, "PlatformWindow: SDL_CreateWindow failed: %s", SDL_GetError());
        return nullptr;
    }
    return (void*)win;
}

void DestroyWindow(void* win) {
    if (win) SDL_DestroyWindow((SDL_Window*)win);
}

void GetWindowSize(void* win, int* w, int* h) {
    if (win) SDL_GetWindowSize((SDL_Window*)win, w, h);
}

void SetWindowSize(void* win, int w, int h) {
    if (win) SDL_SetWindowSize((SDL_Window*)win, w, h);
}

void GetFramebufferSize(void* win, int* w, int* h) {
    if (win) SDL_GetWindowSizeInPixels((SDL_Window*)win, w, h);
}

bool ShouldClose(void* win) {
    if (!win) return true;
    // SDL3 没有内置 should_close 标志, 用 window properties 实现
    SDL_PropertiesID props = SDL_GetWindowProperties((SDL_Window*)win);
    return SDL_GetBooleanProperty(props, "should_close", false);
}

void SetShouldClose(void* win, bool close) {
    if (!win) return;
    SDL_PropertiesID props = SDL_GetWindowProperties((SDL_Window*)win);
    SDL_SetBooleanProperty(props, "should_close", close);
}

// ==================== OpenGL 上下文 ====================

void* CreateGLContext(void* win) {
    if (!win) return nullptr;
    SDL_GLContext ctx = SDL_GL_CreateContext((SDL_Window*)win);
    if (!ctx) {
        CC::Log(CC::LOG_ERROR, "PlatformWindow: SDL_GL_CreateContext failed: %s", SDL_GetError());
    }
    return (void*)ctx;
}

void DestroyGLContext(void* ctx) {
    if (ctx) SDL_GL_DestroyContext((SDL_GLContext)ctx);
}

void MakeCurrent(void* win, void* ctx) {
    if (win && ctx) SDL_GL_MakeCurrent((SDL_Window*)win, (SDL_GLContext)ctx);
}

// 来自 light_io.cpp 的 drain 入口 (无 IO 使用时立即返回 0)
extern "C" int Light_IO_DrainQueue();

void SwapBuffers(void* win) {
    if (win) SDL_GL_SwapWindow((SDL_Window*)win);
    // 每帧自动 drain Light.IO 异步加载完成回调, 用户无需手动调 Light.IO.Poll
    Light_IO_DrainQueue();
}

void* GetGLProcAddress(const char* name) {
    return (void*)SDL_GL_GetProcAddress(name);
}

void SetSwapInterval(int interval) {
    SDL_GL_SetSwapInterval(interval);
}

// ==================== 事件 ====================

bool PollEvent(Event* out) {
    if (!out) return false;
    SDL_Event e;
    if (!SDL_PollEvent(&e)) return false;

    *out = {}; // 清零
    switch (e.type) {
        case SDL_EVENT_KEY_DOWN:
            out->type     = Event::KeyDown;
            out->keycode  = SDLKeyToGLFW(e.key.key);
            out->scancode = (int)e.key.scancode;
            out->mods     = SDLModToGLFW(e.key.mod);
            return true;

        case SDL_EVENT_KEY_UP:
            out->type     = Event::KeyUp;
            out->keycode  = SDLKeyToGLFW(e.key.key);
            out->scancode = (int)e.key.scancode;
            out->mods     = SDLModToGLFW(e.key.mod);
            return true;

        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            out->type   = Event::MouseDown;
            out->button = SDLMouseButtonToGLFW(e.button.button);
            out->x      = e.button.x;
            out->y      = e.button.y;
            return true;

        case SDL_EVENT_MOUSE_BUTTON_UP:
            out->type   = Event::MouseUp;
            out->button = SDLMouseButtonToGLFW(e.button.button);
            out->x      = e.button.x;
            out->y      = e.button.y;
            return true;

        case SDL_EVENT_MOUSE_MOTION:
            out->type = Event::MouseMove;
            out->x    = e.motion.x;
            out->y    = e.motion.y;
            return true;

        case SDL_EVENT_MOUSE_WHEEL:
            out->type = Event::MouseWheel;
            out->dx   = e.wheel.x;
            out->dy   = e.wheel.y;
            return true;

        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            out->type   = Event::Resize;
            out->width  = e.window.data1;
            out->height = e.window.data2;
            return true;

        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            out->type = Event::Close;
            // 同时设置 should_close 标志, 让 ShouldClose() 返回 true
            {
                SDL_Window* w = SDL_GetWindowFromID(e.window.windowID);
                if (w) {
                    SDL_PropertiesID props = SDL_GetWindowProperties(w);
                    SDL_SetBooleanProperty(props, "should_close", true);
                }
            }
            return true;

        case SDL_EVENT_FINGER_DOWN:
            out->type    = Event::TouchDown;
            out->touchId = (int)e.tfinger.fingerID;
            out->x       = e.tfinger.x;
            out->y       = e.tfinger.y;
            return true;

        case SDL_EVENT_FINGER_UP:
            out->type    = Event::TouchUp;
            out->touchId = (int)e.tfinger.fingerID;
            out->x       = e.tfinger.x;
            out->y       = e.tfinger.y;
            return true;

        case SDL_EVENT_FINGER_MOTION:
            out->type    = Event::TouchMove;
            out->touchId = (int)e.tfinger.fingerID;
            out->x       = e.tfinger.x;
            out->y       = e.tfinger.y;
            return true;

        case SDL_EVENT_TEXT_INPUT:
            out->type = Event::TextInput;
            std::strncpy(out->text, e.text.text, sizeof(out->text) - 1);
            return true;

        case SDL_EVENT_TEXT_EDITING:
            out->type        = Event::TextEditing;
            std::strncpy(out->text, e.edit.text ? e.edit.text : "", sizeof(out->text) - 1);
            out->text_start  = (int)e.edit.start;
            out->text_length = (int)e.edit.length;
            return true;

        // ==================== Pen 数字笔事件 (Phase AR) ====================
        case SDL_EVENT_PEN_PROXIMITY_IN:
            out->type      = Event::PenProximity;
            out->penId     = (int)e.pproximity.which;
            out->penAction = 1;
            return true;
        case SDL_EVENT_PEN_PROXIMITY_OUT:
            out->type      = Event::PenProximity;
            out->penId     = (int)e.pproximity.which;
            out->penAction = 0;
            return true;
        case SDL_EVENT_PEN_DOWN:
            out->type      = Event::PenDown;
            out->penId     = (int)e.ptouch.which;
            out->x         = e.ptouch.x;
            out->y         = e.ptouch.y;
            out->penEraser = e.ptouch.eraser ? 1 : 0;
            return true;
        case SDL_EVENT_PEN_UP:
            out->type      = Event::PenUp;
            out->penId     = (int)e.ptouch.which;
            out->x         = e.ptouch.x;
            out->y         = e.ptouch.y;
            out->penEraser = e.ptouch.eraser ? 1 : 0;
            return true;
        case SDL_EVENT_PEN_BUTTON_DOWN:
            out->type      = Event::PenButton;
            out->penId     = (int)e.pbutton.which;
            out->penButton = (int)e.pbutton.button;
            out->penAction = 1;
            out->x         = e.pbutton.x;
            out->y         = e.pbutton.y;
            return true;
        case SDL_EVENT_PEN_BUTTON_UP:
            out->type      = Event::PenButton;
            out->penId     = (int)e.pbutton.which;
            out->penButton = (int)e.pbutton.button;
            out->penAction = 0;
            out->x         = e.pbutton.x;
            out->y         = e.pbutton.y;
            return true;
        case SDL_EVENT_PEN_MOTION:
            out->type  = Event::PenMotion;
            out->penId = (int)e.pmotion.which;
            out->x     = e.pmotion.x;
            out->y     = e.pmotion.y;
            return true;
        case SDL_EVENT_PEN_AXIS:
            out->type         = Event::PenAxis;
            out->penId        = (int)e.paxis.which;
            out->penAxis      = (int)e.paxis.axis;
            out->penAxisValue = e.paxis.value;
            out->x            = e.paxis.x;
            out->y            = e.paxis.y;
            return true;

        case SDL_EVENT_QUIT:
            out->type = Event::Quit;
            return true;

        // ==================== 手柄事件 ====================
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            out->type      = Event::GamepadButton;
            out->gamepadId = (int)e.gbutton.which;
            out->gpButton  = (int)e.gbutton.button;
            out->gpAction  = 1;
            return true;

        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            out->type      = Event::GamepadButton;
            out->gamepadId = (int)e.gbutton.which;
            out->gpButton  = (int)e.gbutton.button;
            out->gpAction  = 0;
            return true;

        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            out->type        = Event::GamepadAxis;
            out->gamepadId   = (int)e.gaxis.which;
            out->gpAxis      = (int)e.gaxis.axis;
            out->gpAxisValue = (float)e.gaxis.value / 32767.0f;
            return true;

        case SDL_EVENT_GAMEPAD_ADDED:
            out->type      = Event::GamepadConnect;
            out->gamepadId = (int)e.gdevice.which;
            out->gpAction  = 1;
            // 自动打开手柄
            SDL_OpenGamepad(e.gdevice.which);
            CC::Log(CC::LOG_INFO, "Gamepad connected: %d", (int)e.gdevice.which);
            return true;

        case SDL_EVENT_GAMEPAD_REMOVED:
            out->type      = Event::GamepadConnect;
            out->gamepadId = (int)e.gdevice.which;
            out->gpAction  = 0;
            CC::Log(CC::LOG_INFO, "Gamepad disconnected: %d", (int)e.gdevice.which);
            return true;

        default:
            // Phase AR — 动态识别 Light.Time.AddTimer 注册的用户事件类型
            // (g_timerEventType 在 luaopen_Light_Time 中通过 SDL_RegisterEvents 申请,
            //  Time_GetTimerEventType() 在文件顶部以 extern "C" 全局声明)
            {
                Uint32 timerType = ::Time_GetTimerEventType();
                if (timerType != 0 && e.type == timerType) {
                    out->type      = Event::Timer;
                    out->penButton = (int)e.user.code;  // 复用 penButton 字段存 timer_id
                    return true;
                }
            }
            // 未识别的事件: 返回 None, 让调用者继续轮询
            out->type = Event::None;
            return true;
    }
}

// ==================== 文本输入 / IME (Phase AQ) ====================

bool StartTextInput(void* win) {
    if (!win) return false;
    return SDL_StartTextInput(static_cast<SDL_Window*>(win));
}

bool StartTextInputWithProps(void* win, const TextInputProps& props) {
    if (!win) return false;
    SDL_Window* w = static_cast<SDL_Window*>(win);

    // 所有 props 都是默认 -> 走快速路径, 免创建 Properties
    if (props.type < 0 && props.capitalization < 0 &&
        props.autocorrect < 0 && props.multiline < 0) {
        return SDL_StartTextInput(w);
    }

    // 构造 SDL_PropertiesID
    SDL_PropertiesID p = SDL_CreateProperties();
    if (p == 0) return SDL_StartTextInput(w);  // 降级: Props 创建失败仍然启动输入

    if (props.type >= 0) {
        SDL_SetNumberProperty(p, SDL_PROP_TEXTINPUT_TYPE_NUMBER, props.type);
    }
    if (props.capitalization >= 0) {
        SDL_SetNumberProperty(p, SDL_PROP_TEXTINPUT_CAPITALIZATION_NUMBER, props.capitalization);
    }
    if (props.autocorrect >= 0) {
        SDL_SetBooleanProperty(p, SDL_PROP_TEXTINPUT_AUTOCORRECT_BOOLEAN, props.autocorrect != 0);
    }
    if (props.multiline >= 0) {
        SDL_SetBooleanProperty(p, SDL_PROP_TEXTINPUT_MULTILINE_BOOLEAN, props.multiline != 0);
    }

    bool ok = SDL_StartTextInputWithProperties(w, p);
    SDL_DestroyProperties(p);
    return ok;
}

void StopTextInput(void* win) {
    if (!win) return;
    SDL_StopTextInput(static_cast<SDL_Window*>(win));
}

bool IsTextInputActive(void* win) {
    if (!win) return false;
    return SDL_TextInputActive(static_cast<SDL_Window*>(win));
}

void SetTextInputArea(void* win, int x, int y, int w, int h, int cursor) {
    if (!win) return;
    SDL_Rect rect = { x, y, w, h };
    SDL_SetTextInputArea(static_cast<SDL_Window*>(win), &rect, cursor);
}

void ClearComposition(void* win) {
    if (!win) return;
    SDL_ClearComposition(static_cast<SDL_Window*>(win));
}

bool IsScreenKeyboardShown(void* win) {
    if (!win) return false;
    return SDL_ScreenKeyboardShown(static_cast<SDL_Window*>(win));
}

// ==================== 计时 ====================

double GetTime() {
    static Uint64 s_freq = 0;
    static Uint64 s_start = 0;
    if (s_freq == 0) {
        s_freq  = SDL_GetPerformanceFrequency();
        s_start = SDL_GetPerformanceCounter();
    }
    Uint64 now = SDL_GetPerformanceCounter();
    return (double)(now - s_start) / (double)s_freq;
}

void Sleep(int ms) {
    if (ms > 0) SDL_Delay((Uint32)ms);
}

// ==================== Web 主循环 ====================

#ifdef __EMSCRIPTEN__
void RunMainLoop(void (*frame)(void*), void* userdata) {
    emscripten_set_main_loop_arg(frame, userdata, 0, 1);
}

void CancelMainLoop() {
    emscripten_cancel_main_loop();
}
#endif

} // namespace PlatformWindow
