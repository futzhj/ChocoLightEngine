/**
 * @file cc_core.cpp
 * @brief CC 命名空间核心工具 — Assert / 日志 / DllMain
 * @note 还原自 Light.dll 地址范围 0x180001000-0x180004000
 */

#include "light.h"

// ==================== 全局状态 ====================

/// 时间戳缓冲区 (对应 byte_1802E0688)
static char g_timeBuffer[32] = {0};

#ifdef _WIN32
/// 控制台输出句柄 (对应 IDA 中 hConsoleOutput @ 0x1802E0678)
static HANDLE g_hConsoleOutput = INVALID_HANDLE_VALUE;
/// 原始控制台文本属性 (对应 word_1802E0680)
static WORD g_wOriginalAttributes = 0;
#endif

// ==================== 控制台初始化 ====================

static void InitConsole() {
#ifdef _WIN32
    if (g_hConsoleOutput != INVALID_HANDLE_VALUE) return;
    g_hConsoleOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(g_hConsoleOutput, &csbi)) {
        g_wOriginalAttributes = csbi.wAttributes;
    }
#endif
}

/// 获取当前时间戳字符串
static void UpdateTimestamp() {
#ifdef _WIN32
    __time64_t now;
    _time64(&now);
    struct tm tm_val;
    memset(&tm_val, 0, sizeof(tm_val));
    _localtime64_s(&tm_val, &now);
#else
    time_t now = time(nullptr);
    struct tm tm_val;
    localtime_r(&now, &tm_val);
#endif
    strftime(g_timeBuffer, sizeof(g_timeBuffer), "[%Y-%m-%d][%H:%M:%S] ", &tm_val);
}

// ==================== CC::Assert ====================
// 原始地址: 0x180003790 — 导出序号 1

void CC::Assert(bool condition, const char* msg, const char* file, int line) {
    if (!condition) {
        CC::Log(LOG_ERROR, "%s on [%s:%d]", msg, file, line);
        abort();
    }
}

// ==================== CC::Log ====================
// 原始地址: 0x180003C40

void CC::Log(LogLevel level, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    UpdateTimestamp();
    InitConsole();

    switch (level) {
        case LOG_INFO:
            std::cout << "[I]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
            break;
        
        case LOG_WARN:
#ifdef _WIN32
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes & 6);
#else
            fprintf(stdout, "\033[33m"); // 黄色 ANSI
#endif
            std::cout << "[W]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
#ifdef _WIN32
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes);
#else
            fprintf(stdout, "\033[0m");
#endif
            break;
        
        case LOG_ERROR:
#ifdef _WIN32
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes & 4);
#else
            fprintf(stdout, "\033[31m"); // 红色 ANSI
#endif
            std::cout << "[E]" << g_timeBuffer;
            vfprintf(stdout, fmt, args);
            std::cout << std::endl;
#ifdef _WIN32
            SetConsoleTextAttribute(g_hConsoleOutput, g_wOriginalAttributes);
#else
            fprintf(stdout, "\033[0m");
#endif
            break;
    }

    va_end(args);
}

// ==================== DllMain (Windows only) ====================
// 原始地址: DllEntryPoint @ 0x18024DE6C

#ifdef _WIN32
BOOL APIENTRY DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved) {
    if (fdwReason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinstDLL);
        InitConsole();
    }
    return TRUE;
}
#endif
