/**
 * @file cc_core.cpp
 * @brief CC 命名空间核心工具 — Assert / 日志 / DllMain
 * @note 还原自 Light.dll 地址范围 0x180001000-0x180004000
 */

#include "light.h"

// ==================== 全局状态 ====================

/// 控制台输出句柄 (对应 IDA 中 hConsoleOutput @ 0x1802E0678)
static HANDLE g_hConsoleOutput = INVALID_HANDLE_VALUE;

/// 原始控制台文本属性 (对应 word_1802E0680)
static WORD g_wOriginalAttributes = 0;

/// 时间戳缓冲区 (对应 byte_1802E0688)
static char g_timeBuffer[32] = {0};

// ==================== 控制台初始化 ====================

/// @brief 初始化控制台句柄和原始颜色属性
static void InitConsole() {
    if (g_hConsoleOutput != INVALID_HANDLE_VALUE) return;
    
    g_hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_hConsoleOutput, &csbi)) {
        g_wOriginalAttributes = csbi.wAttributes;
    }
}

// ==================== CC::Assert ====================
// 原始地址: 0x180003790 — 导出序号 1

void CC::Assert(bool condition, const char* msg, const char* file, int line) {
    if (!condition) {
        // 调用日志输出 "%s on [%s:%d]" 然后 abort
        CC::Log(LOG_ERROR, "%s on [%s:%d]", msg, file, line);
        abort();
    }
}

// ==================== CC::Log ====================
// 原始地址: 0x180003C40

void CC::Log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    // 获取当前时间戳
    __time64_t now;
    _time64(&now);
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    _localtime64_s(&tm_val, &now);
    strftime(g_timeBuffer, sizeof(g_timeBuffer), "[%Y-%m-%d][%H:%M:%S] ", &tm_val);

    InitConsole();

    switch (level) {
        case LOG_INFO:
            // [I] — 不改变颜色
            std::cout << "[I]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
            break;
        
        case LOG_WARN:
            // [W] — 黄色 (保留原始属性中的低4位中的黄色分量)
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes & 6);
            std::cout << "[W]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes);
            break;
        
        case LOG_ERROR:
            // [E] — 红色
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes & 4);
            std::cout << "[E]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes);
            break;
    }

    va_end(args);
}

// ==================== DllMain ====================
// 原始地址: DllEntryPoint @ 0x18024DE6C

BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitConsole();
    }
    return TRUE;
}
