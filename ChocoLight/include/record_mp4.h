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
/// @param encoder_pref  编码器偏好 (Phase F.0.11.6.1.A5):
///                      nullptr / "" / "auto" — 自动 (NVENC > libx264 > AMF, 默认行为)
///                      "libx264"             — 强制软件编码
///                      "h264_nvenc"          — 强制 NVIDIA GPU 硬编 (无 GPU 则 Open 失败)
///                      "h264_amf"            — 强制 AMD GPU 硬编 (无 GPU 则 Open 失败)
///                      "software"            — "libx264" 别名 (语义清晰)
/// @param gop_size GOP 大小 / 关键帧间隔 (Phase F.0.11.6.2.A8):
///                 0 (默认) = fps × 2 (每 2 秒 1 关键帧, 平衡 seek 和压缩比)
///                 1 = 全 I 帧 (无 P 帧, 文件最大但任意 seek)
///                 较大值 = 文件更小但 seek 粒度粗
/// @return true=成功, false=失败 (FFmpeg 缺失 / encoder 不可用 / 文件创建失败)
bool Open(const char* path, int w, int h, int fps, int64_t bitrate,
          const char* encoder_pref = nullptr,
          int gop_size = 0);

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

/// Phase F.0.11.6.1.A4 — 取消 Acquire 拿到的 slot (不推进 tail, 不增 count).
/// 适用场景: AcquireWriteSlot 后, Readback / GPU 操作失败, 不希望 worker 编码无效数据.
/// 实现: 由于主线程是 mp4 录屏唯一 producer, Acquire 未 Commit 时 tail 还在原位,
///       直接 do nothing 即可 — 下次 AcquireWriteSlot 仍返同一 slot, 数据被覆盖.
/// @note 必须紧跟在 AcquireWriteSlot 返非 nullptr 后调用; 与 CommitWriteSlot 二选一.
void CancelWriteSlot();

/// 关闭 mp4: stop worker → join → flush encoder → 写 trailer → 释放资源
void Close();

/// 是否当前 active (Open 成功后到 Close 之前)
bool IsActive();

// ============================================================
// Phase F.0.11.6.2 — 录屏控制扩展
// ============================================================

/// Phase F.0.11.6.2.A10 — 暂停录制 (主线程跳过 Acquire/Commit, ring 不进新帧).
///   pts 不前进 (暂停期间 mp4 时间线无缝衔接, 类似剪辑跳过空白段).
///   仅当 IsActive() 时有效, 否则 no-op.
/// @note 与 stop_flag 独立: paused=true 时 Acquire 直接返 nullptr;
///       生效路径由调用方 (light_graphics RecordTickHook) 主动查 IsPaused() 提前 return.
void PauseRecord();

/// Phase F.0.11.6.2.A10 — 恢复录制. 与 Pause 配对; no-op 若未暂停.
void ResumeRecord();

/// Phase F.0.11.6.2.A10 — 查询是否暂停 (Open 后 / Close 前才有意义).
bool IsPaused();

/// Phase F.0.11.6.2.A11 — 设置 mp4 文件大小上限 (字节). 超限时不自动停, 仅供脚本查询.
///   0 = 无限 (默认); > 0 = 上限字节数 (例如 100 * 1024 * 1024 = 100 MB)
///   主线程脚本通过 GetStats 查 bytes_written, 自行决定何时调 StopRecord.
void SetMaxSizeBytes(int64_t max_bytes);

/// Phase F.0.11.6.2.A12 — 录屏统计快照.
/// @param frames_encoded   [out] worker 已编码并写盘的帧数 (B-frame 延迟已 flush)
/// @param bytes_written    [out] 已写入 mp4 文件的字节数 (累加每个 packet.size)
/// @param max_size_bytes   [out] 当前 max_size_bytes 设定值 (0=无限)
/// @param encoder_name_out [out, optional] 编码器名 (例: "libx264" / "h264_nvenc"), nullable
/// @param encoder_name_cap [in] encoder_name_out 缓冲容量 (含 \0), <= 0 时跳过
/// @param paused           [out] 当前是否处于暂停
/// @return true=Open 后, false=未 Open / Close 后 (out 参数置 0/false/空串)
bool GetStats(int64_t* frames_encoded, int64_t* bytes_written, int64_t* max_size_bytes,
              char* encoder_name_out, int encoder_name_cap, bool* paused);

} // namespace RecordMP4
