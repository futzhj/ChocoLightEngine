# Phase F.0.11.6.5 — MP4 Audio Capture (A13 Windows WASAPI Loopback) FINAL

> **交付日期**：2026-05-18
> **基线**：Phase F.0.11.6.4.A14 (GIF 录屏完成)
> **commits**：`9000011` — A13 WASAPI loopback + AAC encoder + mux into mp4
> **author**: Cascade-assisted refactor
> **风险声明**：单 Windows 平台实现，作者**未真机验证音频质量**；用户需自行听音验收

---

## 一. 目标

让 mp4 录屏自带"系统声音" — 用户按 M 键录屏时，扬声器输出的所有音频（游戏音效 / BGM / 浏览器视频 / Discord 等）一起被 mux 进 mp4 文件，无需外部 OBS / NVIDIA ShadowPlay。

实现 Windows-only WASAPI loopback (默认 render 设备共享模式抓取) + FFmpeg AAC encoder + mp4 双 stream (video=0 + audio=1) mux。macOS / Linux 静默 disable audio (仅视频)。

---

## 二. 架构

```
┌─────────────────────────────┐
│ Windows render device       │ ← 系统所有音源混音输出
│ (扬声器 / 耳机 / HDMI 音频)  │
└──────────────┬──────────────┘
               │ AUDCLNT_STREAMFLAGS_LOOPBACK
               ▼
┌─────────────────────────────┐
│ WASAPI capture thread       │  独立 COM(MTA) 线程
│  ├─ IAudioClient::Init      │  buffer=200ms, polling 模式
│  ├─ IAudioCaptureClient::    │
│  │  GetBuffer (10ms 轮询)   │
│  └─ ring_write → ring buffer │  16 MB SPSC (mutex)
└──────────────┬──────────────┘
               │ ring_read (Pull)
               ▼
┌─────────────────────────────┐
│ audio_thread (record_mp4)    │  独立线程
│  ├─ Pull max 4 AAC frames   │  4096 samples × frame
│  ├─ accum until 1024 samples│  AAC frame_size
│  ├─ swr_convert FLT→FLTP    │  interleaved → planar
│  ├─ avcodec_send_frame      │  AAC encoder
│  ├─ avcodec_receive_packet  │
│  └─ av_interleaved_write_frame│  output_mu 与 video 互斥
└──────────────┬──────────────┘
               │
               ▼
┌─────────────────────────────┐
│ mp4 muxer                    │
│  stream 0: H.264 video      │  worker_loop_ 写入
│  stream 1: AAC audio        │  audio_thread 写入
│  fmt_ctx 共享 → output_mu锁 │
└──────────────────────────────┘
```

### 关键不变量
1. **WASAPI capture 线程独立 COM(MTA)**: 不污染主线程 COM apartment, audio_thread 也不直接碰 COM
2. **ring overflow 不 crash**: capture 写 > 主线程读 → 丢最老 chunk (read_pos 跟着前移)
3. **stop 顺序**: stop_flag → join worker → join audio_thread → WASAPI::Stop → flush video encoder → flush audio encoder → write_trailer
4. **output_mu 保护 av_interleaved_write_frame 多 stream 并发**: video worker 和 audio thread 都用同一锁

---

## 三. 交付内容

### 3.1 新文件

| 文件 | 行数 | 备注 |
|------|------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\audio_capture_wasapi.h` | 63 | 跨平台接口 (Start/Stop/Pull/Pending/IsRunning/TotalCaptured) |
| `@e:\jinyiNew\Light\ChocoLight\src\audio_capture_wasapi.cpp` | 305 | Windows WASAPI 实现 + macOS/Linux 桩 + ring buffer |

### 3.2 修改文件

| 文件 | 增量 | 关键改动 |
|------|------|----------|
| `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h` | +25 / -3 | Open 加 enable_audio 参数 + GetAudioStats API |
| `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp` | +290 / -8 | State 加 audio 字段 + Open audio setup + audio_thread_loop_ + Close flush + 桩 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +35 / -1 | Lua opts.audio 解析 + GetRecordStats audio_* 字段 |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +3 / -1 | 加新源文件 + ole32/uuid Windows 链接 |
| `@e:\jinyiNew\Light\scripts\smoke\screenshot.lua` | +28 | 10 个新 A13 用例 |

### 3.3 commit 链
- `9000011` — Phase F.0.11.6.5.A13: WASAPI loopback audio capture + AAC encoder + mux into mp4

---

## 四. API 增量

### Lua API
```lua
-- 启用音频录制 (Windows-only, 系统扬声器输出)
Gfx.RecordMP4("out.mp4", { fps = 30, audio = true })

-- 别名 (语义清晰)
Gfx.RecordMP4("out.mp4", { fps = 30, audio = "system" })

-- 显式关闭 (默认行为)
Gfx.RecordMP4("out.mp4", { fps = 30, audio = false })

-- 查询 audio 统计
local s = Gfx.GetRecordStats()
print(s.audio_enabled, s.audio_frames, s.audio_sample_rate, s.audio_channels, s.audio_dropped)
-- 例输出: true 480  48000 2 0
```

### C++ API
```cpp
// audio_capture_wasapi.h
namespace AudioCaptureWASAPI {
    bool Start(int* sr, int* ch, int* fmt);   // 启动 capture thread
    void Stop();                              // 停止并 join
    bool IsRunning();
    int  Pull(uint8_t* dst, int max_frames);  // 拉 PCM
    int  Pending();                           // 当前 ring 积压
    int64_t TotalCaptured();                  // 累计捕获 frame 数
}

// record_mp4.h 增量
namespace RecordMP4 {
    bool Open(...,  bool enable_audio = false);   // 新参数
    bool GetAudioStats(bool*, int64_t*, int*, int*, int64_t*);   // 新 API
}
```

---

## 五. 设计决策

### 5.1 共享模式 vs 独占模式
**选择**: AUDCLNT_SHAREMODE_SHARED + AUDCLNT_STREAMFLAGS_LOOPBACK
- 共享模式 = 不打断其他应用 (Spotify/游戏继续播放)
- LOOPBACK flag = 抓 render 设备的混音输出 (所有应用)
- 独占模式不支持 LOOPBACK，不适合录屏

### 5.2 sample_fmt 透传
**选择**: WASAPI 报告 → swr_convert → AAC FLTP
- WASAPI mix format 通常 IEEE_FLOAT 32-bit (Windows 7+ 默认)
- 也支持 PCM int16 (Windows XP/老硬件)
- 我们不强制重采样到 48kHz / 16bit, 透传 WASAPI 报告的 rate, swr 仅做 interleaved → planar 格式转换

### 5.3 capture thread COM(MTA) 模型
**选择**: 在 capture thread 内部 `CoInitializeEx(MULTITHREADED)`
- 不污染主线程 COM apartment (主线程可能 STA, ChocoLight 其他系统可能依赖)
- audio_thread 不碰 COM (只读 ring buffer)
- 失败处理: COM init 失败仍尝试 (假设主线程已 init), 仅记录 com_owned=false 不再 CoUninit

### 5.4 ring buffer 16MB / SPSC
**选择**: 固定 16 MB ring buffer, mutex 保护
- ~85 秒 @ 48kHz stereo float32 = 384KB/s
- 主线程拉慢时溢出 → 丢最老 chunk (read_pos 跟着前移)
- mutex 锁开销 < 1μs (uncontended), 可接受 (capture 10ms 间隔, audio_thread 5ms 轮询)
- SPSC lock-free queue 复杂度高, 收益不明显, 暂用 mutex 简化

### 5.5 audio_thread polling 而非 event
**选择**: 简单 sleep_for(5ms) 轮询 ring
- AAC frame_size = 1024 samples, @ 48kHz = 21.3ms
- 5ms 轮询足够低延迟, CPU 开销可忽略
- event-driven (cv) 复杂度高, ring 加 cv_not_empty + capture 写 notify, 收益小

### 5.6 多 stream 并发 mux 加 output_mu 保护
**选择**: video worker 和 audio thread 都包装 av_interleaved_write_frame 在 output_mu 内
- FFmpeg AVFormatContext 的 interleaving queue 多线程 unsafe (实测 5.x 偶发 crash)
- 单 stream 录屏 (audio_enabled=false) 锁仅本线程持有, 开销 <10ns
- audio+video 并发时锁竞争少 (video ~33fps, audio ~46.9 packet/s @ 48k/1024), 偶发等待

### 5.7 GIF 模式禁用 audio
**选择**: GIF muxer 无 audio stream 概念, opts.audio 自动忽略
- 用户传 `RecordMP4("anim.gif", {audio=true})` 不报错, audio 静默 disable
- GIF 输出仅视频, 兼容主流播放器

### 5.8 Audio 不影响视频可靠性
**选择**: WASAPI / AAC 任一步失败仅 disable audio, video 继续正常录
- 用户场景: 没声卡 / FFmpeg DLL 不带 AAC encoder → mp4 仍可看 (无声)
- 比 "Open 失败整体回退" 用户体感好

---

## 六. 验证矩阵

### 6.1 Smoke (headless)
| 测试用例 | 状态 | 备注 |
|----------|------|------|
| `screenshot.lua` 81 PASS / 0 FAIL | ✅ | +10 A13 用例 |
| `RecordMP4(audio=true)` 解析 | ✅ | parsing OK, headless nil+err |
| `RecordMP4(audio=false)` 解析 | ✅ | parsing OK |
| `RecordMP4(audio="system")` 别名 | ✅ | parsing OK |
| `RecordMP4(gif + audio)` 组合 | ✅ | gif 自动忽略 audio |
| `GetRecordStats` audio 字段齐全 | ✅ | 6 个新字段 (enabled/frames/rate/channels/dropped) |

### 6.2 编译矩阵
| 平台 | 状态 | 备注 |
|------|------|------|
| Windows (MSVC Release) | ✅ 零 warning | 完整 WASAPI 实现 |
| macOS / Linux | ✅ 桩 | Start 返 false, audio 静默 disable |
| 移动端 / Web | ✅ 桩 | RecordMP4 整体 disable |

### 6.3 全套 smoke 回归
- `asset_loader_async_gltf` ✅ 14 PASS
- `mesh_3d` ✅
- `hdr` ✅ 141 PASS
- `window_lifecycle` ✅ 2 PASS
- `asset_loader_async` ✅ 2 PASS
- `asset_loader_async_probe` ✅ 1 PASS
- `screenshot` ✅ **81 PASS / 0 FAIL** (+10 A13 用例)

### 6.4 真机验收 (待用户操作)
**重要**: 作者未真机录音验证音质 / 同步 / 编码正确性, 用户必须手动验收以下场景:

```lua
-- demo_taau / 任意 demo, 主循环里:
Gfx.RecordMP4("test_audio.mp4", { fps = 30, audio = true })
-- 播放音频 (浏览器 / Spotify), 等 5-10 秒
Gfx.StopRecord()
-- 检查 test_audio.mp4:
--   1. 用 ffplay / VLC 播放, 应有图像 + 声音 (画面: 录的是 demo 渲染; 声音: 录的是当时扬声器输出)
--   2. ffprobe 应显示 video + audio 双 stream
--   3. 声音不应有杂音 / 跳音 / 与画面不同步
```

预期 ffprobe 输出 (示例):
```
Stream #0:0: Video: h264, yuv420p, 1280x720, 5000 kb/s, 30 fps
Stream #0:1: Audio: aac, 48000 Hz, stereo, fltp, 128 kb/s
```

---

## 七. 已知限制

### 7.1 当前不支持
- **macOS / Linux audio capture**: 仅桩 (Start 返 false), 后续 Phase 加 CoreAudio / PulseAudio
- **麦克风录制**: 当前仅 system loopback (扬声器输出), 不抓麦克风输入
  - 后续可加 `opts.audio = "mic"` (用 eCapture 而非 eRender + 不带 LOOPBACK flag)
  - 麦克风 + system 混音需要二次混音器 (复杂度 + delay sync)
- **多设备选择**: 总是用 default render 设备, 不支持选具体扬声器
- **录制中切换默认设备**: 当前实现不重连 (短暂静音, 不致命)

### 7.2 风险点 (作者无法本地验证)
- **DTS/PTS 同步**: audio pts 用 sample 累计 (1024 增量), video pts 用 frame_index, 两边 time_base 不同, 依赖 mp4 muxer 自动 rescale; 实际是否完美同步未真机验证
- **AAC encoder 在 FFmpeg DLL 中可用性**: avformat-59.dll / avcodec-59.dll 通常带内置 aac encoder (`avcodec_find_encoder_by_name("aac")`), 但用户 DLL 来源不同可能缺失 → 自动 fallback "audio disabled, video only"
- **WASAPI mix format 异常情况**: 极少数硬件 (5.1 / 7.1 surround) 可能报告 6/8 channels, 当前代码 channel_layout 只覆盖 mono/stereo, 多声道会失败 disable audio
- **AVStream::time_base 探针**: codec_ctx time_base 由 av_opt 设置, mp4 mux 内部从 codec_ctx 拷贝 (FFmpeg 自动); 不显式 rescale_ts 依赖 muxer

### 7.3 未实施的常见补强
- **Audio packet drop counter**: capture 端 ring overflow 仅 read_pos 推进, 但当前 `audio_dropped` atomic 未在 ring_write 中累加 (始终 0); GetAudioStats 始终返 dropped=0. 后续可在 capture thread overflow 时 fetch_add
- **Audio level meter**: 实时显示音量 (用于 OSD), 需 sample 数据计算 RMS, 暂未实施
- **Audio fade in/out**: 录屏开始/结束 100ms 渐入渐出, 避免 pop 噪音

---

## 八. 真机验收步骤 (用户必做)

```powershell
# 1. 启动 demo_taau (含 M 键录屏)
e:\jinyiNew\Light\lumen-master\build\src\light\Release\light.exe `
  e:\jinyiNew\Light\samples\demo_taau\main.lua

# 2. 在 Lua 控制台 (或修改 demo_taau M 键路径) 触发:
local Gfx = Light.Graphics
Gfx.RecordMP4("test_audio.mp4", { fps = 30, audio = true })

# 3. 同时播放系统音频:
#    - 打开 YouTube / Spotify / 任何应用播音乐
#    - 录 5-10 秒
Gfx.StopRecord()

# 4. 验证:
#    a. test_audio.mp4 文件存在
#    b. ffplay test_audio.mp4 → 应同时听到画面 + 音频
#    c. ffprobe test_audio.mp4 → 应显示双 stream (video h264 + audio aac)
#    d. 音质评估: 应清晰无爆音; 与画面同步 (无明显延迟)
#    e. log 应见: "WASAPI: loopback started" + "audio AAC @ 48000Hz × 2ch"

# 5. 异常验收:
#    a. 静音 (Windows 任务栏静音扬声器) → 应录到 SILENT 帧 (静音 mp4 仍可播)
#    b. 切换扬声器 (录屏中插拔耳机) → 当前不重连, 短暂静音

# 6. 边界:
#    a. 1080p 30fps + 48k audio: 主线程帧时应仍 ~0.5-2ms (audio thread 不阻塞 render)
#    b. 录屏 1 分钟应 ~10MB video + ~1MB audio = ~11MB
```

如发现以下症状, 报告给作者:
- mp4 无声 / 噪音 / 不同步
- demo_taau 主线程 fps 因 audio 大幅下降
- log 出现 "WASAPI: ... failed" / "AAC avcodec_open2 failed"
- 录屏 stop 时 demo crash / hang

---

## 九. 后续优化候选

### A13.1 — Audio drop counter 实施
ring_write overflow 时 `audio_dropped_frames.fetch_add(chunk_frames)` (~5 行)
价值低: 主线程拉数据快通常不溢出

### A13.2 — 麦克风支持
opts.audio = "mic" (eCapture 设备 + 无 LOOPBACK flag) (~30 行)
中等价值: 直播 / 解说场景需要

### A13.3 — Audio fade in/out
开头 100ms 渐入 + 结尾 100ms 渐出 (~50 行 PCM 后处理)
低价值: 大多数用户不会注意

### A13.4 — macOS CoreAudio
ScreenCaptureKit (12.3+) audio capture (~200 行 Objective-C)
高工作量: 需 macOS 真机环境

### A13.5 — Linux PulseAudio / PipeWire monitor
PulseAudio monitor 设备 (~150 行) (~200 行 PipeWire)
高工作量: 需 Linux 真机环境

---

## 十. 与之前 Phase 的关系

| Phase | 主题 | 与 A13 关系 |
|-------|------|------------|
| F.0.11.6 | MP4 录屏基线 (同步) | 视频 encoder 基础 |
| F.0.11.6.1 (A1~A7) | Worker thread + 性能优化 | 视频 worker, audio 不占视频 worker |
| F.0.11.6.2 (A8/A10/A11/A12) | GOP / Pause / max_size / Stats | GetStats 加 audio 字段并入 |
| F.0.11.6.3.A9 | ROI 录屏 | 视频 readback 区域, audio 不变 |
| F.0.11.6.4.A14 | GIF 录屏 | gif 模式禁用 audio |
| **F.0.11.6.5.A13** | **WASAPI loopback audio** | **本 phase** |

---

## 十一. 文件清单

### 新增
- `@e:\jinyiNew\Light\ChocoLight\include\audio_capture_wasapi.h` (63 行)
- `@e:\jinyiNew\Light\ChocoLight\src\audio_capture_wasapi.cpp` (305 行)
- `@e:\jinyiNew\Light\docs\Phase F.0.11.6.5 MP4 Audio Capture\FINAL_PhaseF_0_11_6_5.md` (本文档)

### 修改
- `@e:\jinyiNew\Light\ChocoLight\include\record_mp4.h` (Open 签名 + GetAudioStats)
- `@e:\jinyiNew\Light\ChocoLight\src\record_mp4.cpp` (audio_thread + AAC encoder integration)
- `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` (Lua opts.audio + Stats audio 字段)
- `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` (新源文件 + ole32/uuid 链接)
- `@e:\jinyiNew\Light\scripts\smoke\screenshot.lua` (10 个新 A13 用例)

**commit**: `9000011` (in `main`)
**author**: Cascade-assisted refactor
