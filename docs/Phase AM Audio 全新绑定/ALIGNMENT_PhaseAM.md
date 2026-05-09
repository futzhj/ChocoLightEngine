# Phase AM — Light.Audio 全新绑定 — Alignment

## 0. 目标

新建 `Light.Audio` Lua 模块,直接暴露 SDL3 SDL_audio.h 的低延迟音频 device + AudioStream API,作为现有 `Light.AV.Audio` (miniaudio 文件解码) 的互补层。

## 1. 现状

- `@e:\jinyiNew\Light\ChocoLight\src\light_audio_backend.cpp` 是 miniaudio 后端,**无 luaopen 入口**(C++ 层 `AudioBackend::Play/Pause/Stop/SetVolume`,被 `Light.AV.Audio` 间接使用)
- **没有 luaopen_Light_Audio**:全新模块
- SDL_audio.h API 共 **56 fns**(经 grep 统计)

## 2. 绑定策略

| 类别 | fns | 备注 |
|---|---|---|
| Drivers | 3 | GetNumAudioDrivers / GetAudioDriver / GetCurrentAudioDriver |
| 设备发现/查询 | 8 | GetAudioPlaybackDevices / GetAudioRecordingDevices / GetAudioDeviceName / GetAudioDeviceFormat / GetAudioDeviceChannelMap / IsAudioDevicePhysical / IsAudioDevicePlayback / GetAudioDeviceGain |
| 设备控制 | 6 | OpenAudioDevice / CloseAudioDevice / PauseAudioDevice / ResumeAudioDevice / AudioDevicePaused / SetAudioDeviceGain |
| 流创建/销毁 | 2 | CreateAudioStream / DestroyAudioStream |
| 流配置 | 10 | Get/SetAudioStreamFormat / Frequency / Gain / Input/OutputChannelMap |
| 流绑定 | 5 | BindAudioStream / BindAudioStreams / UnbindAudioStream / UnbindAudioStreams / GetAudioStreamDevice |
| 流 IO | 6 | Put/GetAudioStreamData / GetAudioStreamAvailable / GetAudioStreamQueued / ClearAudioStream / FlushAudioStream |
| 流 lock | 2 | LockAudioStream / UnlockAudioStream |
| 流 device 控制 | 3 | PauseAudioStreamDevice / ResumeAudioStreamDevice / AudioStreamDevicePaused |
| 简化打开 | 1 | OpenAudioDeviceStream (无 callback) |
| WAV 加载 | 1 | LoadWAV (返 spec + binary string) |
| 音频工具 | 4 | MixAudio / ConvertAudioSamples / GetAudioFormatName / GetSilenceValueForFormat |
| **总计** | **51** | |

**Out-of-scope** (5 fns):
- SetAudioPostmixCallback / SetAudioStreamGetCallback / SetAudioStreamPutCallback (Lua callback wrapper 系统未引入)
- LoadWAV_IO (需要 SDL_IOStream wrapper, 用 LoadWAV 即可)
- GetAudioStreamProperties (Properties 系统无 Lua 封装)

## 3. 常量 (16)

- AudioFormat enum (12): UNKNOWN/U8/S8/S16LE/S16BE/S32LE/S32BE/F32LE/F32BE + S16/S32/F32 (host endian)
- 设备默认 (2): AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFF, AUDIO_DEVICE_DEFAULT_RECORDING = 0xFFFFFFFE
- 格式掩码 (4): AUDIO_MASK_BITSIZE / AUDIO_MASK_FLOAT / AUDIO_MASK_BIG_ENDIAN / AUDIO_MASK_SIGNED

## 4. 关键设计决策

- **AudioDeviceID** (Uint32) → lua_Number; 用 `0xFFFFFFFF` 不会丢精度 (53-bit double 容)
- **SDL_AudioStream*** → lightuserdata handle (Joystick/Gamepad 协议复用)
- **SDL_AudioSpec** ↔ Lua table `{format, channels, freq}` 双向映射
- **PCM data** ↔ Lua string (字节数据);`PutAudioStreamData(stream, "raw bytes")` / `GetAudioStreamData(stream, max_bytes)` → string
- **SDL_INIT_AUDIO** lazy init(EnsureAudioSubsystem 与其他 Phase 一致)
- **不破坏 light_audio_backend.cpp**: miniaudio 与 SDL3 audio 是独立后端,可并存(两者都不会 init 对方的 subsystem)

## 5. 验收

- 51 fns + 16 const 全部注册
- nil/invalid handle/id 边界路径全过
- 真机若无 audio 设备(headless CI): smoke 优雅降级 (设备数=0/相关 fn 返回 false+err 不崩溃)
- CI 6 平台绿
- 8 sibling-phase smoke 不破坏

## 6. 工作量

- light_audio.cpp 新建 ~1000 行
- CMakeLists.txt 注册一行
- light.cpp 模块表注册一行
- scripts/smoke/audio.lua 新建 ~200 行

无需中断询问,共识完成,直接实施。
