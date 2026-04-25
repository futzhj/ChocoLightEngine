/**
 * @file light_antidebug.cpp
 * @brief Engine-level anti-debugging: 5 detection methods + silent anomaly
 *
 * Detection methods:
 *   1. IsDebuggerPresent()
 *   2. CheckRemoteDebuggerPresent()
 *   3. NtQueryInformationProcess (ProcessDebugPort)
 *   4. Timing check (QueryPerformanceCounter)
 *   5. Hardware breakpoint detection (DR0-DR7 via GetThreadContext)
 *
 * Strategy: "Silent anomaly" — don't crash, subtly corrupt rendering/audio
 */

#ifdef _WIN32

#include "light_antidebug.h"
#include <windows.h>
#include <winternl.h>

// NtQueryInformationProcess function pointer
typedef NTSTATUS(NTAPI *pNtQueryInfoProc)(HANDLE, ULONG, PVOID, ULONG, PULONG);

namespace LightAntiDebug {

static float g_anomaly = 0.0f;      // Anomaly intensity (0=clean, 1.0=max)
static int g_checkCount = 0;        // Total checks performed
static int g_detectCount = 0;       // Detections triggered
static LARGE_INTEGER g_qpcFreq;     // QPC frequency
static LARGE_INTEGER g_lastQpc;     // Last QPC value for timing checks

void Init() {
    g_anomaly = 0.0f;
    g_checkCount = 0;
    g_detectCount = 0;
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_lastQpc);
}

// ---- Individual detection methods ----

static bool checkIsDebuggerPresent() {
    return IsDebuggerPresent() != FALSE;
}

static bool checkRemoteDebugger() {
    BOOL present = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &present);
    return present != FALSE;
}

static bool checkDebugPort() {
    static pNtQueryInfoProc ntQuery = nullptr;
    static bool resolved = false;
    if (!resolved) {
        HMODULE ntdll = GetModuleHandleA("ntdll.dll");
        if (ntdll)
            ntQuery = (pNtQueryInfoProc)GetProcAddress(ntdll, "NtQueryInformationProcess");
        resolved = true;
    }
    if (!ntQuery) return false;

    // ProcessDebugPort = 7
    DWORD_PTR debugPort = 0;
    NTSTATUS status = ntQuery(GetCurrentProcess(), 7, &debugPort, sizeof(debugPort), nullptr);
    return (status == 0 && debugPort != 0);
}

static bool checkTimingAnomaly() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Time since last check
    double elapsed = (double)(now.QuadPart - g_lastQpc.QuadPart) / g_qpcFreq.QuadPart;
    g_lastQpc = now;
    // If a single frame takes > 2 seconds, suspicious (breakpoint/single-step)
    // Only trigger after first few checks to allow initial loading
    return (g_checkCount > 5 && elapsed > 2.0);
}

static bool checkHardwareBreakpoints() {
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
    if (GetThreadContext(GetCurrentThread(), &ctx)) {
        // DR0-DR3 hold breakpoint addresses; non-zero = HW breakpoint set
        return (ctx.Dr0 != 0 || ctx.Dr1 != 0 || ctx.Dr2 != 0 || ctx.Dr3 != 0);
    }
    return false;
}

// ---- Public interface ----

bool Check() {
    g_checkCount++;

    // Run checks periodically (not every call, to reduce overhead)
    // Stagger different checks across different intervals
    bool detected = false;

    if (g_checkCount % 60 == 0)
        detected |= checkIsDebuggerPresent();
    if (g_checkCount % 90 == 1)
        detected |= checkRemoteDebugger();
    if (g_checkCount % 120 == 2)
        detected |= checkDebugPort();
    if (g_checkCount % 30 == 0)
        detected |= checkTimingAnomaly();
    if (g_checkCount % 150 == 3)
        detected |= checkHardwareBreakpoints();

    if (detected) {
        g_detectCount++;
        // Gradually increase anomaly (caps at 1.0)
        g_anomaly += 0.05f;
        if (g_anomaly > 1.0f) g_anomaly = 1.0f;
    } else if (g_anomaly > 0.0f) {
        // Slowly decay anomaly when clean (makes it harder to identify trigger)
        g_anomaly -= 0.001f;
        if (g_anomaly < 0.0f) g_anomaly = 0.0f;
    }

    return !detected;
}

float GetAnomalyFactor() {
    return g_anomaly;
}

} // namespace LightAntiDebug

#elif defined(__ANDROID__)

// ==================== Android 反调试 ====================
// 检测方法:
//   1. TracerPid (/proc/self/status)
//   2. ptrace(PTRACE_TRACEME) 自占位
//   3. 时间异常检测 (clock_gettime)

#include "light_antidebug.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <time.h>
#include <unistd.h>
#include <sys/ptrace.h>

namespace LightAntiDebug {

static float g_anomaly = 0.0f;
static int g_checkCount = 0;
static int g_detectCount = 0;
static uint64_t g_lastTimeNs = 0;
static bool g_ptraceOccupied = false;

static uint64_t getNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void Init() {
    g_anomaly = 0.0f;
    g_checkCount = 0;
    g_detectCount = 0;
    g_lastTimeNs = getNowNs();
    // ptrace 自占位 (阻止其他调试器附加)
    if (ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) == 0) {
        g_ptraceOccupied = true;
    }
}

// 检查 /proc/self/status 中的 TracerPid
static bool checkTracerPid() {
    FILE* f = fopen("/proc/self/status", "r");
    if (!f) return false;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "TracerPid:", 10) == 0) {
            int pid = atoi(line + 10);
            fclose(f);
            return pid != 0;
        }
    }
    fclose(f);
    return false;
}

// 时间异常: 单帧 > 2 秒 → 可能断点暂停
static bool checkTimingAnomaly() {
    uint64_t now = getNowNs();
    double elapsed = (double)(now - g_lastTimeNs) / 1e9;
    g_lastTimeNs = now;
    return (g_checkCount > 5 && elapsed > 2.0);
}

// 检查调试器进程名 (通过 /proc/pid/cmdline)
static bool checkDebuggerProcess() {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", getppid());
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char cmdline[256] = {};
    fread(cmdline, 1, sizeof(cmdline) - 1, f);
    fclose(f);
    // 常见调试器进程名
    if (strstr(cmdline, "gdb") || strstr(cmdline, "lldb") ||
        strstr(cmdline, "frida") || strstr(cmdline, "ida")) {
        return true;
    }
    return false;
}

bool Check() {
    g_checkCount++;
    bool detected = false;

    if (g_checkCount % 60 == 0)
        detected |= checkTracerPid();
    if (g_checkCount % 30 == 0)
        detected |= checkTimingAnomaly();
    if (g_checkCount % 120 == 2)
        detected |= checkDebuggerProcess();

    if (detected) {
        g_detectCount++;
        g_anomaly += 0.05f;
        if (g_anomaly > 1.0f) g_anomaly = 1.0f;
    } else if (g_anomaly > 0.0f) {
        g_anomaly -= 0.001f;
        if (g_anomaly < 0.0f) g_anomaly = 0.0f;
    }

    return !detected;
}

float GetAnomalyFactor() { return g_anomaly; }

} // namespace LightAntiDebug

#elif defined(CHOCO_PLATFORM_IOS)

// ==================== iOS 反调试 ====================
// 检测方法:
//   1. sysctl P_TRACED 标志
//   2. 时间异常检测 (clock_gettime)
//   3. 环境变量检测 (DYLD_INSERT_LIBRARIES)

#include "light_antidebug.h"
#include <cstring>
#include <cstdlib>
#include <time.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>

namespace LightAntiDebug {

static float g_anomaly = 0.0f;
static int g_checkCount = 0;
static int g_detectCount = 0;
static uint64_t g_lastTimeNs = 0;

static uint64_t getNowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

void Init() {
    g_anomaly = 0.0f;
    g_checkCount = 0;
    g_detectCount = 0;
    g_lastTimeNs = getNowNs();
}

// sysctl 检测 P_TRACED 标志
static bool checkSysctlDebugger() {
    struct kinfo_proc info = {};
    size_t infoSize = sizeof(info);
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &info, &infoSize, nullptr, 0) == 0) {
        return (info.kp_proc.p_flag & P_TRACED) != 0;
    }
    return false;
}

// 时间异常
static bool checkTimingAnomaly() {
    uint64_t now = getNowNs();
    double elapsed = (double)(now - g_lastTimeNs) / 1e9;
    g_lastTimeNs = now;
    return (g_checkCount > 5 && elapsed > 2.0);
}

// DYLD_INSERT_LIBRARIES 注入检测 (Frida/Cycript 常用)
static bool checkDyldInjection() {
    const char* env = getenv("DYLD_INSERT_LIBRARIES");
    return (env != nullptr && env[0] != '\0');
}

bool Check() {
    g_checkCount++;
    bool detected = false;

    if (g_checkCount % 60 == 0)
        detected |= checkSysctlDebugger();
    if (g_checkCount % 30 == 0)
        detected |= checkTimingAnomaly();
    if (g_checkCount % 120 == 2)
        detected |= checkDyldInjection();

    if (detected) {
        g_detectCount++;
        g_anomaly += 0.05f;
        if (g_anomaly > 1.0f) g_anomaly = 1.0f;
    } else if (g_anomaly > 0.0f) {
        g_anomaly -= 0.001f;
        if (g_anomaly < 0.0f) g_anomaly = 0.0f;
    }

    return !detected;
}

float GetAnomalyFactor() { return g_anomaly; }

} // namespace LightAntiDebug

#else // 其他平台 (Linux/macOS 桌面等)

#include "light_antidebug.h"

namespace LightAntiDebug {
void  Init() {}
bool  Check() { return true; }
float GetAnomalyFactor() { return 0.0f; }
} // namespace LightAntiDebug

#endif
