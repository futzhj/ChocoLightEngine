# Phase F.0.11.6 MP4 Recording — ACCEPTANCE 文档

> **阶段**：6A Workflow — 阶段 5 Acceptance（验收）
> **基线**：PLAN_PhaseF_0_11_6.md (方案 A 完整实施)
> **实施记录**：FINAL_PhaseF_0_11_6.md
> **验收日期**：2026-05-17

---

## 1. 功能验收

### 1.1 FFmpegLib 扩展
- [x] `ffmpeg_common.h` 新增 14 个 encoder/muxer/avutil 函数指针
  - encoder: avcodec_find_encoder / avcodec_find_encoder_by_name / avcodec_send_frame / avcodec_receive_packet / avcodec_parameters_from_context / av_packet_rescale_ts
  - muxer: avformat_alloc_output_context2 / avformat_new_stream / avformat_write_header / av_interleaved_write_frame / av_write_trailer / avio_open / avio_closep / avformat_free_context
  - avutil: av_frame_get_buffer / av_frame_make_writable / av_dict_set / av_dict_free / av_opt_set / av_opt_set_int
- [x] `light_av.cpp::LoadFFmpeg` 加 LOAD_FUNC 调用; 缺失符号不让 LoadFFmpeg 整体失败 (老版 DLL 容错)
- [x] 编译通过 (4 demos build clean)

### 1.2 MP4 Encoder 状态机 (record_mp4.cpp)
- [x] `record_mp4.h` namespace 4 函数: Open / WriteRGBA / Close / IsActive
- [x] H.264 编码器查找 (FF_AV_CODEC_ID_H264 / libx264 / h264_nvenc fallback chain)
- [x] mp4 muxer (`avformat_alloc_output_context2(.., "mp4", path)`)
- [x] AVCodecContext 配置: width/height/pix_fmt(YUV420P)/bit_rate/gop/time_base/framerate (av_opt_set_int 跨 ABI 稳定)
- [x] libx264 私有选项: preset=medium / crf=23 (via av_dict_set + avcodec_open2)
- [x] sws_scale 流水线 (RGBA → YUV420P)
- [x] B-frame 关闭 (max_b_frames=0) 简化 pts 处理
- [x] `Close()` 含 flush (send NULL frame + drain receive_packet) + write_trailer + avio_closep
- [x] cleanup_all() 全错误路径资源释放 (Open 失败时不留 leak)

### 1.3 RecordState 扩展
- [x] `RecordState::mode` 字段 (0=PNG, 1=MP4)
- [x] tick hook 内 mp4 分支: ReadbackDefaultFB → RecordMP4::WriteRGBA, 失败时自动 stop + 报错日志
- [x] `StopRecord` mp4 路径: RecordMP4::Close() flush + 写 trailer

### 1.4 Lua API
- [x] `Light.Graphics.RecordMP4(path, opts?)` — opts 表支持 fps/bitrate/max_frames/frame_skip
- [x] `Light.Graphics.GetRecordMode()` — 返 0/1 标识当前录屏模式
- [x] 注册到 graphics_funcs[]
- [x] 错误处理: 非法 fps / frame_skip / 录屏冲突 / 窗口未打开 / FFmpeg DLL 缺失 → nil + err

### 1.5 Smoke 增量 (4 检查点)
- [x] RecordMP4 / GetRecordMode 在 fn surface
- [x] GetRecordMode 默认 = 0 (idle)
- [x] RecordMP4(fps=0) → nil + err 含 "fps"
- [x] RecordMP4(frame_skip=0) → nil + err 含 "frame_skip"
- [x] RecordMP4 headless → nil + err (FFmpeg DLL 不可用 / 窗口未开)

**Smoke 结果**: **42 PASS / 0 FAIL** (旧 36 + 6 新)

### 1.6 Demo 集成
- [x] `demo_taau` 增 **M 键**: toggle MP4 录屏 (默认 30 fps + 5 Mbps); 再按 M 停止 → 输出 `taau_record.mp4`
- [x] HUD 键位提示更新

---

## 2. 兼容性验收 (零回归)

| Demo | 启动 | warn/error/fail/undef |
|---|---|---|
| `demo_ssr` | ✅ | 0 |
| `demo_taa_split2` | ✅ | 0 |
| `demo_taau` | ✅ | 0 |
| `demo_multi_hdr_pip` | ✅ | 0 |

| Smoke | PASS | FAIL |
|---|---|---|
| TAA smoke | 171 | 0 |
| Screenshot smoke | 42 (+6 新 F.0.11.6) | 0 |

**关键不变量**: PNG sequence 路径 (mode=0) 行为完全未变, Lua 调 `RecordPNGSequence` 不走 mp4 分支.

---

## 3. 设计决策回顾

### 3.1 av_opt_set_int 跨 ABI 稳定
为避免 AVCodecContext 字段直写 (FFmpeg 4-7 偏移有变化), 用 `av_opt_set_int(ctx, "width", w, 0)` 通过 AVOption API 设置. AVOption 名称 (`width`/`height`/`b`/`g`/`pix_fmt`/`time_base`/`framerate`/`max_b_frames`) 是 FFmpeg ABI 稳定契约, 跨版本安全.

### 3.2 B-frame 禁用 (max_b_frames=0)
B-frame 模式下 packet 顺序 ≠ 显示顺序, pts/dts 必须正确 rescale. 我们禁用 B-frame:
- 优点: pts = frame_index 直接用 (无 rescale 需求)
- 优点: latency 低 (encoder buffer 1 frame max), Close flush 更简单
- 代价: 编码效率略低 (~5-10% 码率上升). 对屏幕录制可接受.

### 3.3 stream->codecpar 偏移探针
AVStream::codecpar 在结构体中的偏移随 FFmpeg 版本变化 (4.x ~80 字节, 5.x ~88 字节, 6.x 不同). 用与 video_backend_ffmpeg.cpp::ProbeCodecPar 同模式的探针: 扫描 56-256 字节范围找 `codec_type == 0 (VIDEO)` 的指针. SEH (`__try`/`__except`) 处理无效指针访问.

### 3.4 AVFormatContext::pb 偏移
AVFormatContext 头部 4 个指针 (av_class / iformat / oformat / priv_data) 共 32 字节, 第 5 个字段是 `AVIOContext *pb`. 跨版本稳定 (头部布局 FFmpeg 自 1.x 起未变).

### 3.5 单 instance 录屏 (无并发)
`g_record` 单例 + RecordMP4 单例: 同时只能 1 个录屏 (PNG 或 MP4 二选一). 这与 PNG 路径一致. 录屏中调 RecordMP4 拒绝 + err.

---

## 4. 真机验证 (待用户)

⏳ 用户需要在 demo_taau / demo_multi_hdr_pip 真机内:

1. **配置 FFmpeg DLL**: 把 `avformat-59.dll` / `avcodec-59.dll` / `avutil-57.dll` / `swscale-9.dll` 复制到 `samples/demo_taau/lib/` 目录 (与现有视频解码 DLL 同位置)
2. 按 **M 键**: 应看到日志 `[demo_taau] RecordMP4 started -> taau_record.mp4`
3. 录制几秒, 再按 M 停止: 应看到 `[demo_taau] StopRecord: N frames written to taau_record.mp4`
4. 用 VLC / Windows Media Player / 浏览器打开 `taau_record.mp4`: 应正常播放
5. `ffprobe taau_record.mp4`: 验证 stream H.264, container mp4, fps 30, 时长正确

**预期失败模式**:
- FFmpeg DLL 缺失 → "RecordMP4: FFmpeg DLL not available" log
- libx264 未编译进 FFmpeg DLL → "H.264 encoder not available" log
- 窗口尺寸非偶数 → "w/h must be even" log

---

## 5. 验收结论

**核心交付**: FFmpeg encoder 动态加载扩展 + record_mp4.cpp 状态机 (sws RGBA→YUV420 + H.264 编码 + B-frame flush) + Lua bridge + demo 集成.

**验收级别**:
- ✅ **代码层**: PASS (Release build clean, smoke 42/0)
- ✅ **兼容性**: PASS (4 demo 零回归; PNG sequence 路径未变)
- ⏳ **真机功能**: 待用户配置 FFmpeg DLL 后按 M 验证 mp4 文件生成 + 播放正常

**结论**: F.0.11.6 **代码层通过验收**, 进入用户真机配置 FFmpeg DLL + 验证生成 mp4 文件阶段。

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 验收提交 — 代码层 PASS |
