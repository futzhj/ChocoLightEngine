/**
 * @file platform_window.h
 * @brief 跨平台窗口/输入/事件抽象层 (基于 SDL3)
 * @note 屏蔽 SDL3/GLFW 等底层差异，对引擎层提供统一接口
 *
 * 设计原则:
 *   - 不暴露 SDL3 类型到引擎层 (使用 void* 不透明指针)
 *   - 事件采用拉取模式 (PollEvent), 由引擎主循环驱动
 *   - 计时使用高精度单调时钟, 单位: 秒
 *   - 仅主线程调用安全
 *
 * 平台覆盖: Windows / Linux / macOS / Android / iOS / Web (Emscripten)
 */
#pragma once

#include <cstdint>

// Windows.h 定义了 CreateWindow / CreateWindowA / CreateWindowW 为宏,
// 会破坏同名函数声明。在此主动 undef。
#ifdef CreateWindow
#undef CreateWindow
#endif
#ifdef CreateWindowA
#undef CreateWindowA
#endif
#ifdef CreateWindowW
#undef CreateWindowW
#endif

namespace PlatformWindow {

// ==================== 事件类型 ====================

/// @brief 跨平台事件，由 PollEvent 拉取
struct Event {
    enum Type {
        None        = 0,
        KeyDown     = 1,
        KeyUp       = 2,
        MouseDown   = 3,
        MouseUp     = 4,
        MouseMove   = 5,
        MouseWheel  = 6,
        Resize      = 7,    // 窗口像素尺寸变化 (HiDPI 已转换为像素)
        Close       = 8,    // 窗口关闭请求
        TouchDown   = 9,    // 移动平台
        TouchUp     = 10,
        TouchMove   = 11,
        TextInput   = 12,   // UTF-8 文本输入 (IME)
        Quit        = 13    // 应用退出
    } type = None;

    // 注意: 不使用 union 以避免 ABI 复杂性, 字段直接列出
    int    keycode    = 0;       ///< KeyDown/KeyUp: 兼容 GLFW 的键码值 (GLFW_KEY_*)
    int    scancode   = 0;       ///< KeyDown/KeyUp: 平台扫描码
    int    mods       = 0;       ///< KeyDown/KeyUp: 修饰键位掩码 (Shift=1, Ctrl=2, Alt=4)
    int    button     = 0;       ///< MouseDown/MouseUp: 0=Left, 1=Right, 2=Middle
    double x          = 0.0;     ///< Mouse*/Touch*: 像素坐标
    double y          = 0.0;
    double dx         = 0.0;     ///< MouseWheel: 滚动增量 (横/纵)
    double dy         = 0.0;
    int    width      = 0;       ///< Resize: 新宽度 (像素)
    int    height     = 0;       ///< Resize: 新高度 (像素)
    int    touchId    = 0;       ///< Touch*: 多指触摸 ID
    char   text[32]   = {0};     ///< TextInput: UTF-8 字符串
};

// ==================== 生命周期 ====================

/// @brief 初始化平台层 (创建窗口前必须调用)
/// @return true=成功, false=失败 (CC::Log 输出错误)
bool Init();

/// @brief 释放平台层资源
void Shutdown();

// ==================== 窗口管理 ====================

/// @brief 创建窗口
/// @param title 窗口标题 (UTF-8)
/// @param w 像素宽度
/// @param h 像素高度
/// @param fullscreen 是否全屏
/// @return 不透明窗口句柄, nullptr=失败
void* CreateWindow(const char* title, int w, int h, bool fullscreen);

/// @brief 销毁窗口 (会同时释放 GL 上下文)
void DestroyWindow(void* win);

/// @brief 获取窗口逻辑尺寸 (DIP, 与 SetWindowSize 单位一致)
void GetWindowSize(void* win, int* w, int* h);

/// @brief 设置窗口尺寸
void SetWindowSize(void* win, int w, int h);

/// @brief 获取帧缓冲尺寸 (实际像素, HiDPI 下 ≥ 窗口尺寸)
void GetFramebufferSize(void* win, int* w, int* h);

/// @brief 查询窗口是否被请求关闭
bool ShouldClose(void* win);

/// @brief 标记窗口为应关闭状态
void SetShouldClose(void* win, bool close);

// ==================== OpenGL 上下文 ====================

/// @brief 创建 OpenGL 上下文 (桌面: GL 3.3 Core, 移动/Web: GLES 3.0)
/// @return 不透明 GL 上下文句柄, nullptr=失败
void* CreateGLContext(void* win);

/// @brief 销毁 GL 上下文
void DestroyGLContext(void* ctx);

/// @brief 将上下文绑定到当前线程
void MakeCurrent(void* win, void* ctx);

/// @brief 交换前后缓冲区 (Present)
void SwapBuffers(void* win);

/// @brief 获取 GL 函数地址 (跨平台 wglGetProcAddress/glXGetProcAddress)
/// @return 函数指针, nullptr=不支持
void* GetGLProcAddress(const char* name);

/// @brief 设置垂直同步 (1=开, 0=关, -1=自适应)
void SetSwapInterval(int interval);

// ==================== 事件 ====================

/// @brief 拉取一个事件
/// @param out 输出事件 (调用者拥有)
/// @return true=成功取得事件, false=队列空
bool PollEvent(Event* out);

// ==================== 计时 ====================

/// @brief 获取应用启动以来的时间, 单位: 秒, 高精度单调时钟
double GetTime();

/// @brief 阻塞当前线程一段时间, 单位: 毫秒
void Sleep(int ms);

// ==================== Web 主循环 (M2) ====================

#ifdef __EMSCRIPTEN__
/// @brief 注册主循环回调 (Emscripten 模式下不能用普通 while 循环)
/// @param frame 每帧调用的函数
/// @param userdata 透传参数
/// @note 此函数会立即返回, 由浏览器驱动循环
void RunMainLoop(void (*frame)(void*), void* userdata);

/// @brief 取消注册主循环
void CancelMainLoop();
#endif

} // namespace PlatformWindow
