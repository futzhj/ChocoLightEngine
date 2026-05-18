/**
 * @file record_mp4.h
 * @brief Phase F.0.11.6 — MP4 H.264 录屏 (FFmpeg libavcodec/libavformat 动态加载)
 *
 * 设计:
 *   - 复用 light_av.cpp 的 g_ff (FFmpegLib 动态加载器), 不静态链 FFmpeg
 *   - 扩展 encoder/muxer 符号: avcodec_find_encoder / avformat_alloc_output_context2 等
 *   - sws_scale 把 OpenGL RGBA → YUV420p (H.264 要求)
 *   - 每帧由 light_graphics.cpp 录制循环调 RecordMP4_WriteRGBA(), 内部异步推 packet
 *   - StopRecord 时调 RecordMP4_Close(), 自动 flush B-frame + 写 trailer
 *
 * 失败模式:
 *   - FFmpeg DLL 缺失 → Open 返 false (调用方走 PNG 回退或报错)
 *   - 编码器 libx264 不可用 → 同上
 *   - 编码器配置失败 → Open 返 false + 内部清理已分配资源
 */

#pragma once

#include <cstdint>

namespace RecordMP4 {

/// 打开 mp4 编码器并写文件头.
/// @param path     输出文件路径 (推荐 .mp4 扩展名)
/// @param w, h     视频宽高 (像素), 必须 > 0 且偶数 (libx264 要求)
/// @param fps      帧率 (例如 30, 60)
/// @param bitrate  目标码率 bit/s (例如 5_000_000 = 5 Mbps); 0 = libx264 自动 (CRF 模式)
/// @return true=成功, false=失败 (FFmpeg 缺失 / encoder 不可用 / 文件创建失败)
bool Open(const char* path, int w, int h, int fps, int64_t bitrate);

/// 写入一帧 RGBA8 数据 (与 RecordPNG 同模式, OpenGL bottom-left 原点 — 内部翻转).
/// @param rgba   长度 w*h*4 字节, 当前帧的 RGBA8
/// @param frame_index  帧序号 (从 0 开始, 用于 pts)
/// @return true=成功 (含 encoder buffered 但未写盘 — B-frame 延迟); false=失败
/// @note F.0.11.6.1.A1 起内部转 Acquire+memcpy+Commit; 主线程仍 1 次 8MB memcpy.
///       新代码推荐用 AcquireWriteSlot + CommitWriteSlot 的 zero-copy 路径 (省 1 次 memcpy).
bool WriteRGBA(const uint8_t* rgba, int frame_index);

/// Phase F.0.11.6.1.A1 — Zero-copy ring buffer 写入路径.
/// 主线程持有返回的指针后, 直接把 readback 数据 (例如 glReadPixels / PBO map) 写入此 buffer,
/// 之后调 CommitWriteSlot() 通知 worker 编码. 比 WriteRGBA 省一次 8MB memcpy.
///
/// @param frame_index  帧序号 (用于 pts)
/// @return  指向 ring buffer 当前 tail slot 的 RGBA8 buffer (容量 w*h*4 字节);
///          失败返 nullptr (录屏未 active 或 stop_flag 已设).
/// @note 阻塞: 若 ring 已满 (kRingSize=16 帧 in flight), 主线程在此等 worker 出队 (back-pressure).
/// @note 配对调用: AcquireWriteSlot 返非 nullptr 后, **必须**调 CommitWriteSlot, 否则 ring 永远卡死.
uint8_t* AcquireWriteSlot(int frame_index);

/// Phase F.0.11.6.1.A1 — 推进 ring tail + 通知 worker.
/// 必须在 AcquireWriteSlot 返非 nullptr 之后调用; 不可重复调.
void CommitWriteSlot();

/// 关闭 mp4: stop worker → join → flush encoder → 写 trailer → 释放资源
void Close();

/// 是否当前 active (Open 成功后到 Close 之前)
bool IsActive();

} // namespace RecordMP4
