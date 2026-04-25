#pragma once
/**
 * @file light_antidebug.h
 * @brief Engine-level anti-debugging protection
 * @note 5 detection methods + silent anomaly strategy
 *       Windows: 5 层检测 (IsDebuggerPresent, RemoteDebugger, DebugPort, Timing, HW BP)
 *       其他平台: 空实现 (Init/Check/GetAnomalyFactor 全部为无操作)
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
