/**
 * @file light_audio_backend.h
 * @brief ChocoLight 音频后端抽象接口 (miniaudio 实现)
 * @note 提供文件播放和 PCM 数据播放两种模式
 *       miniaudio 不支持的格式由 FFmpeg 解码后通过 LoadPCM 播放
 *
 * Phase AT 扩展: 3D 空间化, SoundGroup (Mixer), Effect (滤波器/Echo), Listener
 */

#pragma once

#include <cstdint>

// 不透明音频句柄
struct AudioHandle;
struct GroupHandle;
struct EffectHandle;

// ==================== AudioBackend 接口 ====================

namespace AudioBackend {

// ---------- 引擎生命周期 ----------

/// 初始化音频引擎 (在窗口创建后调用一次)
bool Init();

/// 关闭音频引擎 (程序退出时调用)
void Shutdown();

// ---------- Sound 加载 ----------

/// 从文件加载音频 (miniaudio 原生支持: WAV, MP3, FLAC)
/// 返回 nullptr 表示格式不支持或文件不存在
AudioHandle* LoadFile(const char* path);

/// 从 PCM 数据加载音频 (用于 FFmpeg 解码后的回退路径)
/// format: 1=u8, 2=s16, 3=s32, 5=f32
/// channels: 声道数, sampleRate: 采样率
AudioHandle* LoadPCM(const void* data, uint64_t frameCount,
                     int format, int channels, int sampleRate);

/// 释放音频资源
void Free(AudioHandle* h);

// ---------- Sound 播放控制 ----------

void Play(AudioHandle* h);
void Pause(AudioHandle* h);
void Stop(AudioHandle* h);  // 停止 + seek 到 0
bool IsPlaying(AudioHandle* h);

void SetVolume(AudioHandle* h, float vol);  // [0, 1+]
float GetVolume(AudioHandle* h);

// ---------- Phase AT: Sound 扩展 ----------

void  SetLooping(AudioHandle* h, bool looping);
bool  GetLooping(AudioHandle* h);

void  SetPitch(AudioHandle* h, float pitch);  // 1.0 = 原速
float GetPitch(AudioHandle* h);

void  SetPan(AudioHandle* h, float pan);  // [-1, 1] (仅 stereo 输出)
float GetPan(AudioHandle* h);

void  SetSpatializationEnabled(AudioHandle* h, bool enabled);
bool  GetSpatializationEnabled(AudioHandle* h);

void  SetPosition(AudioHandle* h, float x, float y, float z);
void  GetPosition(AudioHandle* h, float* x, float* y, float* z);

void  SetVelocity(AudioHandle* h, float x, float y, float z);

/// model: 0=none, 1=inverse, 2=linear, 3=exp
void  SetAttenuationModel(AudioHandle* h, int model);
int   GetAttenuationModel(AudioHandle* h);

void  SetMinDistance(AudioHandle* h, float d);
float GetMinDistance(AudioHandle* h);

void  SetMaxDistance(AudioHandle* h, float d);
float GetMaxDistance(AudioHandle* h);

void  SetRolloff(AudioHandle* h, float r);
float GetRolloff(AudioHandle* h);

// ---------- Phase AT: SoundGroup (Mixer) ----------

/// 创建 SoundGroup. parent=nullptr 表示挂在 master 上
GroupHandle* CreateGroup(GroupHandle* parent);
void         FreeGroup(GroupHandle* g);

void  SetGroupVolume(GroupHandle* g, float v);
float GetGroupVolume(GroupHandle* g);
void  SetGroupPitch(GroupHandle* g, float p);
float GetGroupPitch(GroupHandle* g);

void  GroupPause(GroupHandle* g);
void  GroupResume(GroupHandle* g);
void  GroupStop(GroupHandle* g);

/// 把 sound 关联到 group, nullptr=master. 内部 reroute output bus.
void  SetSoundGroup(AudioHandle* h, GroupHandle* g);

/// 设置 group 父级 (parent=nullptr -> master). 含循环检测, 失败返回 false.
bool  SetGroupParent(GroupHandle* g, GroupHandle* parent);

// ---------- Phase AT: Effect (滤波器 + Echo) ----------

/// channels=0 表示用 engine 默认通道数 (通常 2)
EffectHandle* CreateLowPass(float cutoffHz, int order);          // order: 2..8 偶数
EffectHandle* CreateHighPass(float cutoffHz, int order);
EffectHandle* CreateBandPass(float cutoffHz, int order);
EffectHandle* CreateNotch(float cutoffHz, float q);
EffectHandle* CreatePeak(float cutoffHz, float gainDB, float q);
/// delayMs: 延迟毫秒数; decay: [0, 1) 反馈衰减; wet/dry: [0, 1] 干湿混合
EffectHandle* CreateEcho(int delayMs, float decay, float wet, float dry);
void          FreeEffect(EffectHandle* e);

void  SetEffectEnabled(EffectHandle* e, bool enabled);
bool  GetEffectEnabled(EffectHandle* e);

/// 把 effect 应用到 sound, nullptr 移除 effect. effect 不可被多个 sound 共享.
void  SetSoundEffect(AudioHandle* h, EffectHandle* e);

// ---------- Phase AT: Listener (3D 摄像机) ----------

void SetListenerPosition(int idx, float x, float y, float z);
void GetListenerPosition(int idx, float* x, float* y, float* z);
void SetListenerDirection(int idx, float x, float y, float z);
void GetListenerDirection(int idx, float* x, float* y, float* z);
void SetListenerWorldUp(int idx, float x, float y, float z);
void GetListenerWorldUp(int idx, float* x, float* y, float* z);
void SetListenerVelocity(int idx, float x, float y, float z);
void GetListenerVelocity(int idx, float* x, float* y, float* z);
int  GetListenerCount();

// ---------- Phase AT: Master / Engine ----------

void  SetGlobalVolume(float v);
float GetGlobalVolume();

} // namespace AudioBackend
