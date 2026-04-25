#pragma once
/**
 * @file light_antidebug.h
 * @brief Engine-level anti-debugging protection
 * @note Silent anomaly strategy — 检测调试器后渐进式干扰渲染/音频
 *       Windows: 5 层检测 (IsDebuggerPresent, RemoteDebugger, DebugPort, Timing, HW BP)
 *       Android: 3 层检测 (TracerPid, ptrace 自占位, Timing, 调试器进程名)
 *       iOS:     3 层检测 (sysctl P_TRACED, Timing, DYLD_INSERT_LIBRARIES 注入)
 *       其他平台: 空实现
 */

namespace LightAntiDebug {

// Initialize anti-debug (call once at engine startup)
void Init();

// Periodic check (call from main loop, ~every 60 frames)
// Returns true if environment is clean, false if debugger detected
bool Check();

// Get anomaly factor: 0.0 = clean, >0.0 = apply anomalies
// Gradually increases on repeated detections
float GetAnomalyFactor();

} // namespace LightAntiDebug
