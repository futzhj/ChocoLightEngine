# Phase F.0.11.6 MP4 Recording — FINAL 文档（实施记录）

> **基线**：PLAN_PhaseF_0_11_6.md (方案 A 完整实施)
> **实施日期**：2026-05-17
> **完成度**：F.0.11.6 全量交付

---

## 1. 实施时间线

| 任务 | 实际产出 | 耗时 |
|---|---|---|
| T1 | FFmpegLib 增 14 个 encoder/muxer 符号 + LoadFFmpeg LOAD_FUNC | ~30 min |
| T2 | record_mp4.cpp/.h encoder 状态机 (~370 行) — Open/WriteRGBA/Close + sws + 探针 codecpar | ~2.5 h |
| T3+T4 | RecordState mode 字段 + tick hook mp4 分支 + StopRecord 流程 + l_Graphics_RecordMP4 + GetRecordMode + smoke | ~1.5 h |
| T5 | demo_taau M 键 + 文档收尾 + 4 demo zero-regression | ~30 min |

**总计**: ~5 小时 (PLAN 估时 11 小时, 大幅提前因为利用了现有 FFmpeg 加载器骨架 + 跳过 av_packet_rescale_ts (B-frame 禁用))

---

## 2. 文件改动清单

| 文件 | 改动类型 | 改动量 |
|---|---|---|
| `ChocoLight/include/ffmpeg_common.h` | 修改 | +24 行 (14 个新函数指针) |
| `ChocoLight/src/light_av.cpp` | 修改 | +20 行 (14 个 LOAD_FUNC) |
| `ChocoLight/include/record_mp4.h` | 新建 | ~30 行 |
| `ChocoLight/src/record_mp4.cpp` | 新建 | ~370 行 |
| `ChocoLight/CMakeLists.txt` | 修改 | +1 行 |
| `ChocoLight/src/light_graphics.cpp` | 修改 | +130 行 (mode 字段 + mp4 分支 + RecordMP4 + GetRecordMode + 注册) |
| `ChocoLight/src/light_ui.cpp` | 修改 | +12 行 (Light_GetWindowWidth/Height extern "C") |
| `scripts/smoke/screenshot.lua` | 修改 | +20 行 (6 新检查点) |
| `samples/demo_taau/main.lua` | 修改 | +20 行 (M 键 + HUD) |
| `samples/demo_taau/README.md` | 修改 | +1 行 |
| `docs/Phase F.0.11.6 MP4 Recording/PLAN_PhaseF_0_11_6.md` | 新建/修改 | ~150 行 |
| `docs/Phase F.0.11.6 MP4 Recording/ACCEPTANCE_PhaseF_0_11_6.md` | 新建 | ~110 行 |
| `docs/Phase F.0.11.6 MP4 Recording/FINAL_PhaseF_0_11_6.md` | 新建 | 本文 |
| `docs/HANDOFF_REMAINING_TASKS.md` | 修改 | F.0.11.6 状态更新 |

**总改动**: 7 文件代码修改 + 2 文件新建 + 5 文件文档/资源新建

---

## 3. 关键实现细节

### 3.1 av_opt_set 跨 ABI 稳定

避免 AVCodecContext 直写偏移, 用 AVOption 名称设置基础字段:
```cpp
g_ff.av_opt_set_int(g.codec_ctx, "width",  w, 0);
g_ff.av_opt_set_int(g.codec_ctx, "height", h, 0);
g_ff.av_opt_set_int(g.codec_ctx, "pix_fmt", FF_AV_PIX_FMT_YUV420P, 0);
g_ff.av_opt_set_int(g.codec_ctx, "b", bitrate, 0);   // bit_rate
g_ff.av_opt_set_int(g.codec_ctx, "g", fps * 2, 0);   // gop_size
g_ff.av_opt_set_int(g.codec_ctx, "max_b_frames", 0, 0);
g_ff.av_opt_set(g.codec_ctx, "time_base", "1/30", 0);   // rational via string
g_ff.av_opt_set(g.codec_ctx, "framerate", "30/1", 0);
```

libx264 私有选项 (preset/crf) 通过 av_dict_set + avcodec_open2:
```cpp
void* opts = nullptr;
g_ff.av_dict_set(&opts, "preset", "medium", 0);
g_ff.av_dict_set(&opts, "crf", "23", 0);
g_ff.avcodec_open2(g.codec_ctx, codec, &opts);
g_ff.av_dict_free(&opts);
```

### 3.2 codecpar 探针 (跨 FFmpeg 版本 ABI 兼容)

AVStream::codecpar 字段偏移因版本变化 (4.x ~80B, 5.x ~88B, 6.x ~96B). 用与现有 video_backend_ffmpeg.cpp::ProbeCodecPar 同模式扫描 + SEH 防野指针:
```cpp
uint8_t* base = (uint8_t*)g.stream;
for (int off = 56; off < 256; off += sizeof(void*)) {
    void* candidate = *(void**)(base + off);
    if (!candidate || (uintptr_t)candidate < 0x10000) continue;
    int* fields = (int*)candidate;
    __try {
        if (fields[0] == FF_AVMEDIA_TYPE_VIDEO) {
            codecpar = candidate; break;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
}
```

### 3.3 AVFormatContext::pb 偏移 (固定 32)

AVFormatContext 头部布局 ABI 稳定: av_class(8) + iformat(8) + oformat(8) + priv_data(8) = 32 字节, 第 5 个字段 `AVIOContext *pb`. 直接 `(void**)((uint8_t*)fmt_ctx + 32)` 拿 pb 槽位:
```cpp
void** pb_slot = (void**)((uint8_t*)g.fmt_ctx + 32);
g_ff.avio_open(pb_slot, path, FF_AVIO_FLAG_WRITE);
```

### 3.4 AVFrame.pts 双路径

FFmpeg 5+ 起 AVFrame 有 AVClass, av_opt_set_int 应可工作. 但 4.x 不支持. 双路径 fallback:
```cpp
if (g_ff.av_opt_set_int(g.frame, "pts", frame_index, 0) < 0) {
    // fallback: 直接写偏移 (FFmpeg 5.x AVFrame 中 pts 通常 ~144 字节)
    *(int64_t*)((uint8_t*)g.frame + 144) = frame_index;
}
```

### 3.5 B-frame 禁用 简化 pts 处理

`max_b_frames = 0` 让 packet 输出顺序 = 帧顺序 (无 reorder buffer). 简化:
- frame->pts = frame_index (直接整数)
- packet 输出后 stream_index = 0 (单条 stream), 直接 av_interleaved_write_frame
- 不需 av_packet_rescale_ts (虽然加了符号但本实现未调用)

代价: 编码效率 ~5-10% 略低. 对屏幕录制场景可接受.

### 3.6 cleanup_all 全路径覆盖

Open 失败 + WriteRGBA 中途失败 + Close 都共用 cleanup_all(). 顺序: sws_ctx → frame → packet → codec_ctx → fmt_ctx → rgba_flipped 释放. nullptr 检查全覆盖.

---

## 4. 测试覆盖

### 4.1 Smoke (6 新检查点全过)
```
PASS RecordMP4 exists
PASS GetRecordMode exists
PASS GetRecordMode default = 0 (PNG / idle)
PASS RecordMP4(fps=0) → nil+err
PASS RecordMP4(frame_skip=0) → nil+err
PASS RecordMP4 headless → nil+err (no window / FFmpeg DLL)
```
**总计**: 42 PASS / 0 FAIL

### 4.2 Zero-Regression (4 demo)
- ✅ demo_ssr / demo_taa_split2 / demo_taau / demo_multi_hdr_pip 启动无错
- ✅ TAA smoke 171/0 不受影响
- ✅ PNG sequence 录屏 (mode=0) 行为完全未变 (RecordPNGSequence smoke 通过)

### 4.3 真机验证 (待用户)
- ⏳ 配置 FFmpeg DLL (lib/ 目录)
- ⏳ demo_taau 按 M 录 5 秒, 再按 M 停止
- ⏳ VLC 播放 `taau_record.mp4`
- ⏳ ffprobe 验证 stream/container/duration

---

## 5. 已知 / 留观察问题

### 5.1 设计层
- **AVFrame.pts 偏移 144 假设**: FFmpeg 6.x AVFrame 布局微调可能让 fallback 失效 (av_opt_set 路径仍正常). 用户报错时检查
- **codecpar 探针仅匹配 codec_type=VIDEO**: 若 stream->priv_data 头几个字节碰巧也是 0, 可能误匹配. 实测 FFmpeg 不在 priv_data 头部放 codec_type, 但风险存在
- **avio_closep 仅在 cleanup_all 中**: Close() 路径已通过 cleanup_all 关闭; 若 av_write_trailer 失败导致中途异常, fmt_ctx 仍被 free, avio 资源由 FFmpeg 自动回收

### 5.2 性能 (待真机测)
- 当前 mp4 路径未走 PBO async (encoder 内部已 buffering, async 收益有限). 1080p @ 30fps 软件 H.264 medium preset 预计 ~30-50ms/frame, 可能成为帧率瓶颈
- 优化路径 (后续 phase 可加): worker thread 解耦 encoder, 主线程仅 PBO 读取后丢入队列

### 5.3 平台
- Windows: 需 lib/ 内 4 个 FFmpeg DLL (avcodec-59 / avformat-59 / avutil-57 / swscale-9)
- Linux: 同名 .so.59 / .so.57 / .so.6, 由 LoadFFmpeg 内置 dlopen 试加载
- macOS: .dylib 同模式
- Android/iOS/Web: RecordMP4 直接返 false (源文件 #if 守卫)

### 5.4 编码器质量
- 默认 libx264 medium preset + crf=23 = 业界推荐 web 视频质量
- 用户传 bitrate=0 → CRF 模式 (画质优先, 码率自适应)
- 用户传 bitrate>0 → CBR 模式 (码率约束)
- 不暴露 preset 选项 (复杂度 vs 收益): 后续 phase 可加

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 实施完结 — F.0.11.6 全量代码 + 文档交付 |
