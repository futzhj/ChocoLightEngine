/**
 * @file audio_capture_wasapi.h
 * @brief Phase F.0.11.6.5.A13 — Windows WASAPI loopback audio capture
 *
 * 目的: 从默认 render 设备 (扬声器输出) 抓取 PCM 数据, 喂给 MP4 录屏的 AAC encoder.
 * 这样录屏 mp4 自带系统声音 (无需外部 OBS / NVIDIA ShadowPlay).
 *
 * 设计:
 *   - 独立 capture thread, 内部 init/cleanup COM (不污染主线程 COM apartment)
 *   - 共享模式 + LOOPBACK flag → 抓 render 设备的混音输出 (系统所有音源)
 *   - SPSC ring buffer (单生产者 capture thread, 单消费者 record_mp4 audio thread)
 *   - Sample format 透传: 由 GetMixFormat 决定 (通常 IEEE float32 stereo 48000Hz)
 *   - 调用方负责 swr_convert 到 AAC encoder 需要的 fmt (planar float)
 *
 * 跨平台:
 *   - Windows: 完整 WASAPI 实现
 *   - macOS/Linux: Start() 返 false (静默 disable audio, 不影响视频录屏)
 *
 * 失败模式:
 *   - 无声卡 / 默认设备被禁用 → Start 返 false
 *   - COM 初始化失败 → Start 返 false
 *   - 用户在录屏中切换默认设备 → 当前实现不重连 (短暂静音, 不致命)
 */

#pragma once

#include <cstdint>

namespace AudioCaptureWASAPI {

/// 启动 capture thread.
/// @param out_sample_rate   [out] 实际采样率 (通常 48000)
/// @param out_channels      [out] 实际声道数 (通常 2 = stereo)
/// @param out_sample_fmt    [out] FFmpeg AVSampleFormat 值
///                                 AV_SAMPLE_FMT_FLT=3 (interleaved float32), 最常见
///                                 AV_SAMPLE_FMT_S16=1 (interleaved int16), 老格式
/// @return true=启动成功, false=无声卡 / COM 失败 / 非 Windows
bool Start(int* out_sample_rate, int* out_channels, int* out_sample_fmt);

/// 停止 capture thread, 释放资源. 幂等 (重复调用安全).
void Stop();

/// 是否当前在 capture (Start 成功后到 Stop 前).
bool IsRunning();

/// 从 ring buffer 拉取 PCM 数据 (interleaved frames, 每 frame = channels × bytes_per_sample).
/// @param dst              [out] 目标 buffer
/// @param max_frames       期望最多拉取的 frame 数 (1 frame = 全声道一个采样点)
/// @return 实际拉取的 frame 数; 0 = ring 暂无数据 (调用方应自旋等待 / sleep)
/// @note 非阻塞; 主线程或 audio_thread 都可调; 单消费者假设 (不要多线程同时 Pull).
int Pull(uint8_t* dst, int max_frames);

/// 当前 ring buffer 内积压的 frame 数 (用于背压判断).
int Pending();

/// Capture 累计写入 ring 的 frame 数 (含被覆盖丢弃的); ring overflow 时增长但 Pending 不变.
int64_t TotalCaptured();

} // namespace AudioCaptureWASAPI
