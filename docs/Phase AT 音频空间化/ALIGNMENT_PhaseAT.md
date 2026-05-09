# Phase AT — Audio 3D 空间化 + Mixer (Sound Group) — 对齐文档

> **6A Stage 1**: Align (Phase AT)
>
> **现状**: 现有 2 套音频路径 — `Light.Audio` (SDL3 stream 低层, 51 fns) + `AudioBackend` (miniaudio engine, ~10 内部 fns 但未暴露给 Lua)。
>
> **范围**: 暴露 miniaudio 高层 Sound 资源到 Lua, 加 3D 空间化 (位置/距离衰减/Listener/多普勒) + Mixer (SoundGroup 通道组) + 立体声 panning + pitch shifting。

---

## 1. 现状回顾

### 1.1 已有: `Light.Audio` (SDL3 stream)

51 fns 覆盖底层 PCM 流: device 控制, AudioStream 创建/绑定/IO/callback, WAV 加载, MixAudio/ConvertAudioSamples 工具。

**适合**: 低延迟实时 PCM 处理 (synthesizer / 录音处理 / 低延迟回声等)。
**不适合**: 加载 mp3/ogg, 3D 空间化, group 管理 (没有概念)。

### 1.2 已有: `AudioBackend` (miniaudio, 未暴露 Lua)

```cpp
// light_audio_backend.cpp 已有 (~150 行)
namespace AudioBackend {
    bool Init() / Shutdown();
    AudioHandle* LoadFile(path);          // 自动识别 wav/mp3/ogg/flac (miniaudio 内置)
    AudioHandle* LoadPCM(data, ...);
    Play / Pause / Stop / SetVolume / GetVolume / IsPlaying / Free
}
```

**问题**: 这套接口**没有 Lua 入口**! 也没有 3D / pan / pitch / loop / group。

### 1.3 已有: 旧 Lua API 在哪?

之前的 `Light.Audio.Load/Play/...` (LOVE2D 风格) 似乎不存在了, 只有 `Light.Audio` 的 SDL3 PCM stream API。需要确认 audio.lua / 类似 demo 中怎么播放。

---

## 2. Phase AT 范围

### 2.1 In-Scope (本次必做)

#### A. 新模块 `Light.Audio.Sound` (高层 sound 资源, 基于 miniaudio)

```lua
-- 加载
local s = Light.Audio.Sound.Load(path) -> Sound|nil [, err]
  -- 自动识别 wav/mp3/ogg/flac (miniaudio 内置 stb_vorbis/dr_mp3/dr_flac/dr_wav)

local s = Light.Audio.Sound.LoadPCM(data_str, format, channels, sampleRate) -> Sound|nil
  -- format: "u8" | "s16" | "s32" | "f32"

-- 基本播放控制 (与现有 AudioBackend 一致, 但暴露到 Lua)
s:Play()
s:Pause()
s:Stop()
s:IsPlaying() -> bool

-- 音量 / 循环 / 音调
s:SetVolume(v) / GetVolume() -> v             -- [0, 1+]
s:SetLooping(bool) / GetLooping() -> bool
s:SetPitch(p) / GetPitch() -> p               -- 1.0 = 原速, 2.0 = 高八度

-- 立体声 (2D)
s:SetPan(pan) / GetPan() -> pan               -- [-1, 1], 仅 stereo 输出有效

-- 3D 空间化
s:SetPosition(x, y, z) / GetPosition() -> x,y,z
s:Set3DEnabled(bool) / Get3DEnabled() -> bool
s:SetAttenuationModel(model)                  -- "none"|"inverse"|"linear"|"exp"
s:GetAttenuationModel() -> string
s:SetMinDistance(d) / GetMinDistance() -> d   -- 衰减开始距离, 默认 1.0
s:SetMaxDistance(d) / GetMaxDistance() -> d   -- 完全衰减距离, 默认 FLT_MAX
s:SetRolloff(r) / GetRolloff() -> r           -- 衰减系数, 默认 1.0
s:SetVelocity(vx, vy, vz)                     -- 多普勒效应

-- SoundGroup 关联
s:SetGroup(group | nil)                       -- 加入 group, 继承 volume/pitch

-- 生命周期
s:Delete() / __gc                              -- 释放 GPU/CPU 资源
s:__tostring                                   -- "Sound(<path>, vol=..., playing=...)"
```

#### B. 新模块 `Light.Audio.SoundGroup` (Mixer 多通道组)

```lua
local g = Light.Audio.SoundGroup.New() -> SoundGroup
g:SetVolume(v) / GetVolume() -> v             -- group 总音量, 与子 sound 相乘
g:SetPitch(p) / GetPitch() -> p
g:Pause() / Resume()                          -- 一次暂停所有子 sound (实际是 group node 暂停)
g:Stop()                                       -- 停止所有子
g:GetSoundCount() -> int                       -- 当前归属此 group 的 sound 数
g:Delete() / __gc
g:__tostring
```

应用场景:
- "music" group 单独控制背景音乐音量
- "sfx" group 单独控制音效音量
- "voice" group 单独控制对话音量

#### C. `Light.Audio` 模块加 Listener API

```lua
Light.Audio.SetListenerPosition(x, y, z, [listenerIdx=0])
Light.Audio.SetListenerDirection(dirX, dirY, dirZ, [listenerIdx=0])
Light.Audio.SetListenerWorldUp(upX, upY, upZ, [listenerIdx=0])
Light.Audio.SetListenerVelocity(vx, vy, vz, [listenerIdx=0])  -- 多普勒
Light.Audio.GetListenerCount() -> int (默认 1)

-- 全局衰减默认
Light.Audio.SetGlobalVolume(v)              -- engine 总音量 (master)
Light.Audio.GetGlobalVolume() -> v
```

#### D. AudioBackend C++ 接口扩展

```cpp
// 已有: Init/Shutdown/LoadFile/LoadPCM/Play/Pause/Stop/SetVolume/GetVolume/IsPlaying/Free

// Phase AT 新增 (~25 fns)
void SetLooping(AudioHandle*, bool);
bool GetLooping(AudioHandle*);
void SetPitch(AudioHandle*, float);
float GetPitch(AudioHandle*);
void SetPan(AudioHandle*, float);  // [-1, 1]
float GetPan(AudioHandle*);

void Set3DEnabled(AudioHandle*, bool);
void SetPosition(AudioHandle*, float x, float y, float z);
void GetPosition(AudioHandle*, float* x, float* y, float* z);
void SetAttenuationModel(AudioHandle*, int model);  // 0=none/1=inverse/2=linear/3=exp
void SetMinDistance(AudioHandle*, float);
void SetMaxDistance(AudioHandle*, float);
void SetRolloff(AudioHandle*, float);
void SetVelocity(AudioHandle*, float x, float y, float z);

// SoundGroup
struct GroupHandle;
GroupHandle* CreateGroup();
void FreeGroup(GroupHandle*);
void SetGroupVolume(GroupHandle*, float);
void SetGroupPitch(GroupHandle*, float);
void GroupPause(GroupHandle*);
void GroupResume(GroupHandle*);
void GroupStop(GroupHandle*);
void SetSoundGroup(AudioHandle*, GroupHandle*);  // nil = 默认 master

// Listener
void SetListenerPosition(int idx, float x, float y, float z);
void SetListenerDirection(int idx, float x, float y, float z);
void SetListenerWorldUp(int idx, float x, float y, float z);
void SetListenerVelocity(int idx, float x, float y, float z);

// Master
void SetGlobalVolume(float);
float GetGlobalVolume();
```

### 2.2 Out-of-Scope (留 AT.x 后续)

- ❌ **Audio Effects (低/高通/带通滤波, 混响, 延迟)** — 用 ma_node_graph + ma_xxx_node, 工作量 ~500 行
- ❌ **多 Listener (split-screen)** — 当前默认 listener 0 即可
- ❌ **Spatializer plugins (HRTF / binaural)** — miniaudio 不内置, 需第三方
- ❌ **Audio recording / 麦克风输入** — 现有 SDL3 path 已可用
- ❌ **Streaming (大文件流式播放)** — miniaudio 默认全量加载, 大文件留后续

---

## 3. 决策点 Q1~Q5

### Q1 — Sound 与 SDL3 Audio 关系

**A** (推荐): Sound 完全独立 (基于 miniaudio engine), 与 Light.Audio (SDL3) 平行  
**B**: Sound 复用 SDL3 后端  

**A**: SDL3 不提供高层 sound + 3D, miniaudio 提供。两套各自服务不同场景。

### Q2 — 衰减模型枚举

**A** (推荐): 字符串 "none" / "inverse" / "linear" / "exp"  
**B**: 整数常量

**A**: 与之前 Material 的 mode 一致风格。

### Q3 — SoundGroup 嵌套

**A**: 一层 (sound -> group -> master)  
**B** (推荐): 任意嵌套 (group 可以有 group 父级)

**B**: miniaudio 原生支持任意嵌套 (`ma_sound_group_set_parent`), 加这一句即可,代价低。

### Q4 — 默认 listener 数

**A** (推荐): 1 个 (listener_idx=0 自动激活)  
**B**: N 个 (split-screen)

**A**: 99% 游戏场景 1 listener; multi-listener 复杂且很少见。

### Q5 — Sound:Delete 与 Lua __gc 关系

**A** (推荐): __gc 自动释放, 与 Mesh/Material 一致  
**B**: 必须显式 Delete

**A**: 减少用户出错。

---

## 4. 工作量估算

| 子模块 | C++ 行 |
|---|---|
| AudioBackend 接口扩展 (~25 fns + GroupHandle struct) | ~250 |
| Light.Audio.Sound 模块 (Lua userdata + ~25 方法) | ~450 |
| Light.Audio.SoundGroup 模块 (~10 方法) | ~150 |
| Light.Audio Listener + GlobalVolume API (~6 fns) | ~100 |
| smoke `audio_3d_mixer.lua` (5 阶段) | ~120 |
| 文档 (ALIGN/CONSENSUS/ACCEPTANCE) | - |
| **合计** | **~1070 行** |

预期 ~6h, 1 commit, 6 平台 CI 验证。

---

## 5. 关键约束

- **零破坏 Light.Audio 现有 51 fns** — 仅新增 ~6 个 Listener + GlobalVolume 函数
- **headless 友好** — Sound.Load 在无音频设备时返回 nil + err (miniaudio 已 guard, 但需测)
- **跨平台** — miniaudio 内置 wav/mp3/ogg/flac decoder, 无新增依赖
- **不引入新第三方库** — miniaudio.h 已存在 (third_party/miniaudio.h)

---

## 6. 待确认决策

请用户确认 Q1~Q5:

1. **全部推荐 (Q1=A, Q2=A, Q3=B 嵌套, Q4=A, Q5=A)** — 标准方案
2. **简化方案 (无嵌套 group, 无 velocity)** — 缩到 ~800 行
3. **全套 (含 Audio Effects)** — 工作量~1500+ 行
4. **跳过 Phase AT, 转其他**
