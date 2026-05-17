# Phase F.0.11.6 MP4 Recording (FFmpeg libavcodec) — PLAN 文档

> **阶段**：6A Workflow 合并版 (ALIGNMENT + DESIGN + TASK)
> **目标**：在现有 PNG sequence 录屏基础上, 增加直接编码 H.264 MP4 输出能力 (libavcodec/libavformat)
> **基线**：Phase F.0.11.7 完结 (commit pending, 2026-05-17)
> **创建日期**：2026-05-17

---

## 1. 背景

`Light.Graphics.RecordPNGSequence(dir, max_frames, frame_skip)` 已能录制无损 PNG 序列, 用户需要用外部 FFmpeg 合成 mp4. 改造目标:
- `Light.Graphics.RecordMP4(path, opts)` 直接输出 mp4, 省去 PNG 中间步骤
- 利用现有 PBO 异步 readback (Phase F.0.11.2) 减少 GPU 阻塞
- 复用 `light_av.cpp` 中已有的 FFmpeg 动态加载器 (扩展为含编码器符号)

## 2. 范围与权衡

**改动规模** (大):
- FFmpegLib 扩展 ~12 个编码器/muxer 函数指针 (avcodec_find_encoder / avformat_alloc_output_context2 / avformat_new_stream / avio_open / avformat_write_header / avcodec_send_frame / avcodec_receive_packet / av_interleaved_write_frame / av_write_trailer / avcodec_parameters_from_context / sws_getContext / sws_scale)
- 新建 `record_mp4.cpp` 实现编码器状态机 (约 400-500 行)
- 修改 RecordState 增 mp4-mode 分支 (frame 来源 = PBO async 读出的 RGBA → sws_scale 转 YUV420p → avcodec 编码)
- Lua bridge 增 2 API: `RecordMP4 / GetRecordMode`
- 错误处理 ~15 路径 (FFmpeg 加载失败 / 编码器不存在 / 格式不支持 / 写盘失败 / encoder flush 失败 etc.)

**风险与复杂度**:
1. **DLL 依赖**: 用户必须自备 FFmpeg DLL (avcodec/avformat/avutil/swscale). 当前 `light_av.cpp` 已实现懒加载 + 失败容错; 复用此路径
2. **编码器选择**: H.264 软件 (libx264) vs 硬件 (NVENC/QSV/AMF). 软件 → 跨平台保证, 硬件 → 性能但需检测. MVP 用软件 H.264
3. **像素格式转换**: OpenGL RGBA8 → YUV420p (FFmpeg 默认). 必须用 swscale, 否则 H.264 不接受 RGB 输入
4. **时间戳**: pts/dts 计算, 涉及 stream time_base, 容易出错
5. **B-frame 缓冲**: encoder 可能延迟输出 packet (B-frame), 录制结束时必须 flush
6. **跨平台**:
   - Windows: 现有 DLL 加载路径 OK
   - Linux: .so 加载 OK
   - macOS: .dylib OK
   - Android/iOS/Web: FFmpeg 不可用 → API 返 nil + err (已有约定)

**估算工时**: 2-3 天 (含 FFmpeg 加载器扩展 + encoder 状态机 + 错误处理 + 真机视频文件验证)

## 3. 替代方案 (用户决策点)

**方案 A**: 完整 F.0.11.6 实施 (上述 2-3 天工作量)
**方案 B**: 推迟到独立 Phase, 当前用 PNG sequence + 文档化 FFmpeg 后处理命令行 (用户体验略差但维护成本低)
**方案 C**: MVP (只支持 H.264 软件编码, 默认参数, 不暴露 quality/bitrate; 估时缩到 1-1.5 天)

## 4. 设计 (若选 A 或 C)

### 4.1 FFmpeg 符号扩展

ffmpeg_common.h `FFmpegLib` 增:
```cpp
// avcodec encoder
void* (*avcodec_find_encoder)(int id);
void* (*avcodec_find_encoder_by_name)(const char* name);
int   (*avcodec_send_frame)(void* ctx, const void* frame);
int   (*avcodec_receive_packet)(void* ctx, void* pkt);
int   (*avcodec_parameters_from_context)(void* params, void* ctx);

// avformat muxer
int   (*avformat_alloc_output_context2)(void** out_ctx, void* fmt, const char* fmt_name, const char* path);
void* (*avformat_new_stream)(void* ctx, void* codec);
int   (*avformat_write_header)(void* ctx, void** opts);
int   (*av_interleaved_write_frame)(void* ctx, void* pkt);
int   (*av_write_trailer)(void* ctx);
int   (*avio_open)(void** s, const char* url, int flags);
int   (*avio_closep)(void** s);
void  (*avformat_free_context)(void* ctx);

// avutil frame helpers
int   (*av_frame_get_buffer)(void* frame, int align);
int   (*av_frame_make_writable)(void* frame);
int64_t (*av_rescale_q)(int64_t a, void* bq, void* cq);

// swscale (已有 sws_getContext / sws_scale / sws_freeContext, 复用)
```

### 4.2 RecordMP4 状态机

```
RecordMP4(path, opts) {
    if (!LoadFFmpeg()) return error;
    if (!avcodec_find_encoder(H264)) return error;

    state.fmt_ctx = avformat_alloc_output_context2(.., "mp4", path);
    state.video_st = avformat_new_stream(fmt_ctx, codec);
    state.codec_ctx = avcodec_alloc_context3(codec);
    // 配置: width / height / time_base / framerate / bit_rate / pix_fmt=YUV420P
    avcodec_open2(codec_ctx, codec, ...);
    avcodec_parameters_from_context(stream->codecpar, codec_ctx);

    state.sws_ctx = sws_getContext(w, h, RGBA, w, h, YUV420P, BILINEAR, ...);
    state.frame = av_frame_alloc();
    state.frame->format = YUV420P; state.frame->width = w; state.frame->height = h;
    av_frame_get_buffer(frame, 32);

    avio_open(&fmt_ctx->pb, path, AVIO_FLAG_WRITE);
    avformat_write_header(fmt_ctx, NULL);
}

每帧 (复用 RecordState 路径, PBO async 读 RGBA 后):
    av_frame_make_writable(frame);
    sws_scale(sws_ctx, rgba_data, ..., frame->data, ...);
    frame->pts = frame_index;
    avcodec_send_frame(codec_ctx, frame);
    while (avcodec_receive_packet(...) >= 0) {
        av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
        av_interleaved_write_frame(fmt_ctx, pkt);
    }

StopRecord:
    avcodec_send_frame(codec_ctx, NULL);   // flush
    drain receive_packet 循环
    av_write_trailer(fmt_ctx);
    avio_closep(&fmt_ctx->pb);
    avformat_free_context(fmt_ctx);
    sws_freeContext(sws_ctx);
    av_frame_free(&frame);
    avcodec_free_context(&codec_ctx);
```

### 4.3 Lua API

```lua
local ok = Light.Graphics.RecordMP4("out.mp4", {
    fps        = 30,        -- default 60
    bit_rate   = 8000000,   -- 8 Mbps, default 5 Mbps
    preset     = "medium",  -- libx264 preset: ultrafast/veryfast/fast/medium/slow
    crf        = 23,        -- 0..51, lower=better quality, default 23
    frame_skip = 1,         -- 与 PNG 录屏一致
    max_frames = 0,         -- 0 = unlimited
})

Light.Graphics.StopRecord()  -- 已有, 自动 flush + 写 trailer
```

## 5. 验收门槛 (若实施)

- ✅ FFmpeg DLL 缺失时返 nil + err (容错)
- ✅ 真机生成 mp4 用 VLC / 浏览器 / Windows Media Player 能正常播放
- ✅ Linux ffprobe 解析: stream H.264 + container mp4 + 正确 fps / 时长
- ✅ B-frame 模式下 StopRecord 正确 flush 不丢最后帧
- ✅ 与 PNG sequence 路径互不干扰 (二选一)

## 6. 决策结果

**用户拍板 (2026-05-17): 方案 A — 完整实施**.

实施任务拆分:

| 任务 | 内容 | 估时 |
|---|---|---|
| T1 | FFmpegLib + LoadFFmpeg 扩展 ~12 个 encoder/muxer 符号 | 1.5h |
| T2 | mp4 encoder 状态机封装 (record_mp4.cpp): open / write_frame_rgba / flush_close + sws RGBA→YUV420p | 4h |
| T3 | RecordState 增 mp4 模式分支 + PBO 异步读取后路由到 encoder | 2h |
| T4 | Lua bridge: l_Graphics_RecordMP4 + GetRecordMode + smoke 5-6 检查点 | 1.5h |
| T5 | build + 4 demo zero-regression + 真机 mp4 文件验证 + ACCEPTANCE/FINAL | 2h |

**总估时**: ~11 小时 (约 1.5 天工时).
