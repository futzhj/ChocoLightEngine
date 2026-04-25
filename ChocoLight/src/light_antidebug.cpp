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

#else // !_WIN32

// 非 Windows 平台: 空实现 (无反调试)
#include "light_antidebug.h"

namespace LightAntiDebug {
void  Init() {}
bool  Check() { return true; }
float GetAnomalyFactor() { return 0.0f; }
} // namespace LightAntiDebug

#endif // _WIN32
