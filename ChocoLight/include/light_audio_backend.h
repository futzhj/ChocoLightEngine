/**
 * @file light_audio_backend.h
 * @brief ChocoLight 音频后端抽象接口 (miniaudio 实现)
 * @note 提供文件播放和 PCM 数据播放两种模式
 *       miniaudio 不支持的格式由 FFmpeg 解码后通过 LoadPCM 播放
 */

#pragma once

#include <cstdint>

// 不透明音频句柄
struct AudioHandle;

// ==================== AudioBackend 接口 ====================

namespace AudioBackend {

/// 初始化音频引擎 (在窗口创建后调用一次)
bool Init();

/// 关闭音频引擎 (程序退出时调用)
void Shutdown();

/// 从文件加载音频 (miniaudio 原生支持: WAV, MP3, FLAC)
/// 返回 nullptr 表示格式不支持或文件不存在
AudioHandle* LoadFile(const char* path);

/// 从 PCM 数据加载音频 (用于 FFmpeg 解码后的回退路径)
/// format: 1=u8, 2=s16, 3=s32, 5=f32
/// channels: 声道数, sampleRate: 采样率
AudioHandle* LoadPCM(const void* data, uint64_t frameCount,
                     int format, int channels, int sampleRate);

/// 播放 (从头开始或从暂停处继续)
void Play(AudioHandle* h);

/// 暂停
void Pause(AudioHandle* h);

/// 停止 (重置到开头)
void Stop(AudioHandle* h);

/// 设置音量 (0.0 ~ 1.0+)
void SetVolume(AudioHandle* h, float vol);

/// 获取音量
float GetVolume(AudioHandle* h);

/// 是否正在播放
bool IsPlaying(AudioHandle* h);

/// 释放音频资源
void Free(AudioHandle* h);

} // namespace AudioBackend
