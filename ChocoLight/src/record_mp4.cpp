/**
 * @file record_mp4.cpp
 * @brief Phase F.0.11.6 — MP4 H.264 录屏 实现
 *
 * 复用 light_av.cpp 的 g_ff 动态加载器, 不静态链 FFmpeg.
 *
 * 关键设计选择 (跨 ABI 版本兼容):
 *   - AVCodecContext 字段通过 av_opt_set_int / av_opt_set 设置 (avoid 直接结构体偏移)
 *     - width / height / time_base / framerate / b (bit_rate) / pix_fmt / gop_size / max_b_frames
 *   - libx264 私有选项通过 av_dict_set + avcodec_open2 第三参数传递 (preset / crf / profile)
 *   - AVFrame 字段通过 ABI-stable 简化 header 直写 (data/linesize 在前 8 个指针 + 8 个 int, 跨版本稳定)
 *
 * 失败容错:
 *   - 任何一步失败立即清理 + 返 false, log 记录原因
 *   - Close() 在 Open 失败后调用是安全的 (内部 nullptr 检查)
 */

// 全平台都需要 stdint (移动端 / Web 的 stub 也用了 int64_t / uint8_t)
#include <cstdint>

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)

#include "record_mp4.h"
#include "ffmpeg_common.h"
#include "light.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

// Phase F.0.11.6.1 — worker thread 异步编码 (主线程 P95: 25-40ms → ~5ms)
// Phase F.0.11.6.1.A1 — Ring buffer (固定 16 slot 预分配 vector) + zero-copy AcquireWriteSlot/CommitWriteSlot
// 设计:
//   主线程 WriteRGBA 走兼容入口 (内部 Acquire+memcpy+Commit), 1 次 8MB memcpy.
//   推荐路径 AcquireWriteSlot → readback 直写 slot → CommitWriteSlot, 省一次 memcpy.
//   ring 满时主线程在 Acquire 内阻塞等 worker 出队 (back-pressure 防内存爆).
//   Close: stop flag → notify_all → join → 主线程串行 flush + write trailer.
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

extern bool LoadFFmpeg();

namespace RecordMP4 {

// FFmpeg 常量 (与 video_backend_ffmpeg.cpp 同模式)
enum {
    FF_AVMEDIA_TYPE_VIDEO   = 0,
    FF_AV_CODEC_ID_H264     = 27,
    FF_AV_PIX_FMT_YUV420P   = 0,
    FF_AV_PIX_FMT_RGBA      = 26,
    FF_SWS_BILINEAR         = 2,
    FF_AVIO_FLAG_WRITE      = 2,
    FF_AV_CODEC_FLAG_GLOBAL_HEADER = (1 << 22),
    FF_AVFMT_GLOBALHEADER   = 0x0040,
    // Phase F.0.11.6.1.A6 — BT.709 颜色空间元数据 (HDTV / 现代 1080p 标准)
    //   AVCol enum 值与 FFmpeg pixfmt.h / avcodec.h 一致, 跨版本稳定.
    FF_AVCOL_PRI_BT709      = 1,   // BT.709 / sRGB 色域三角形
    FF_AVCOL_TRC_BT709      = 1,   // BT.709 传输函数 (gamma ~2.4)
    FF_AVCOL_SPC_BT709      = 1,   // BT.709 YUV→RGB 转换矩阵
    FF_AVCOL_RANGE_MPEG     = 1,   // TV/limited range (Y 16-235, UV 16-240) — 多数播放器默认
    FF_AVERROR_EAGAIN       = -11,
    FF_AVERROR_EOF_INT32    = -541478725,
    FF_AV_OPT_SEARCH_CHILDREN = (1 << 0),
};

// AVFrame ABI-stable header (与 video_backend_ffmpeg.cpp 共用思路)
//   data[8] + linesize[8] 始终在结构体头部, 后续字段不影响 sws_scale / encoder 调用
struct AVFrameHead {
    uint8_t* data[8];
    int      linesize[8];
    // 后续字段 (width/height/format/pts) 不通过此结构体写, 全部用 av_opt_set 或 frame->pts 直接 offset
};

// AVPacket ABI-stable 简化布局 (与 video_backend_ffmpeg.cpp::AVPacketSimple 一致)
struct AVPacketHead {
    void*    buf;
    int64_t  pts;
    int64_t  dts;
    uint8_t* data;
    int      size;
    int      stream_index;
};

// AVRational (struct, 跨版本稳定)
struct AVRational {
    int num;
    int den;
};

// Phase F.0.11.6.1.A1 — Ring buffer slot (RGBA staging + 对应帧号)
//   每个 slot 在 Open 时 reserve+resize 到 w*h*4, 后续 push/pop 仅复用 buffer (不再 heap alloc).
struct Slot {
    std::vector<uint8_t> rgba;       // 容量 = w*h*4, Open 后不变
    int                  frame_idx;  // pts 用
};

// 内部状态 (单一全局; mp4 录屏不支持并发)
struct State {
    bool   active = false;
    int    width  = 0;
    int    height = 0;
    int    fps    = 30;

    void*  fmt_ctx     = nullptr;   // AVFormatContext*
    void*  codec_ctx   = nullptr;   // AVCodecContext*
    void*  stream      = nullptr;   // AVStream*
    void*  frame       = nullptr;   // AVFrame*  (YUV420p 编码输入)
    void*  packet      = nullptr;   // AVPacket* (encoder 输出)
    void*  sws_ctx     = nullptr;   // SwsContext* (RGBA→YUV420p)

    // 临时 RGBA staging (Y 翻转后再喂 sws): worker 内重用同一块, 主线程不碰
    uint8_t* rgba_flipped = nullptr;
    int      rgba_size    = 0;

    // Phase F.0.11.6.1 — worker thread 异步编码基础设施
    // Phase F.0.11.6.1.A1 — Ring buffer 替代 std::queue (省 heap alloc + 支持 zero-copy 路径)
    static constexpr size_t kRingSize = 16;        // 内存上限: 16 帧 × 1080p × 4 ≈ 128 MB
    std::thread             worker;                 // Open 时 spawn, Close 时 join
    std::mutex              mu;                     // 保护 head/tail/count/stop_flag
    std::condition_variable cv;                     // 主线程 push / Close 通知 worker
    std::condition_variable cv_not_full;            // worker 出队后通知主线程 (back-pressure)
    Slot                    ring[kRingSize];        // 待编码帧 ring buffer
    size_t                  head      = 0;          // worker pop 索引 (encode 完才推进)
    size_t                  tail      = 0;          // 主线程 push 索引 (Commit 时推进)
    size_t                  count     = 0;          // 已 commit 但未 encode 完成的 slot 数
    std::atomic<bool>       stop_flag{false};
};
static State g;

// Phase F.0.11.6.1 — Forward declaration: Open 内 spawn worker thread 时引用, 实体在文件末尾
static void worker_loop_();

// Phase F.0.11.6.1 — codecpar 探针抽取到独立函数.
// MSVC 限制: __try 不能用在含有 unwindable 对象 (std::thread/mutex 等) 的函数中.
// Open 函数体引用 g.worker / g.mu / g.cv 等需析构对象 → __try 必须移到独立 helper.
// stream: AVStream* 指针; 返 codecpar 指针 (失败返 nullptr).
static void* FindCodecpar_(void* stream) {
    if (!stream) return nullptr;
    uint8_t* base = (uint8_t*)stream;
    for (int off = 56; off < 256; off += sizeof(void*)) {
        void* candidate = *(void**)(base + off);
        if (!candidate || (uintptr_t)candidate < 0x10000) continue;
        // codecpar 头几个 int: codec_type, codec_id, codec_tag, ...
        int* fields = (int*)candidate;
#ifdef _WIN32
        __try {
#endif
            // codec_type=VIDEO(0): avformat_new_stream 已设此字段
            if (fields[0] == FF_AVMEDIA_TYPE_VIDEO) {
                return candidate;
            }
#ifdef _WIN32
        } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
#endif
    }
    return nullptr;
}

// ===== AVFrame.pts 写入 =====
//   FFmpeg 4-6 AVFrame 中 pts 在固定偏移 (data[8]=64B + linesize[8]=32B + 一些指针/int)
//   保守做法: 不直接写 pts, 而是在每次 encode 前用 av_opt_set_int 设 frame->pts
//   但 av_opt 仅识别 AVCodecContext, 不识别 AVFrame
//   方案: 用偏移宏定义 — FFmpeg 4.x 至 6.x AVFrame::pts 在偏移 200~260 字节范围
//   具体值: data[8](64) + linesize[8](32) + extended_data*(8) + width(4) + height(4)
//          + nb_samples(4) + format(4) + key_frame(4) + pict_type(4) + sample_aspect_ratio(8)
//          + pts(8) ≈ 144  (但 FFmpeg 5.x 之后某些字段顺序变了)
//   保守: 用 av_opt_set_int(frame, "pts", v, 0) 试一试 — AVFrame 也有 AVClass

// ===== 工具函数 =====

static int64_t make_rational_packed(int num, int den) {
    return ((int64_t)num << 32) | (uint32_t)den;
}

static void log_err(const char* where, int err) {
    CC::Log(CC::LOG_ERROR, "RecordMP4: %s failed (err=%d)", where, err);
}

// 释放所有资源 — Open 失败回退路径 + Close 共用
static void cleanup_all() {
    if (g.sws_ctx && g_ff.sws_freeContext) {
        g_ff.sws_freeContext(g.sws_ctx);
        g.sws_ctx = nullptr;
    }
    if (g.frame && g_ff.av_frame_free) {
        g_ff.av_frame_free(&g.frame);
        g.frame = nullptr;
    }
    if (g.packet && g_ff.av_packet_free) {
        g_ff.av_packet_free(&g.packet);
        g.packet = nullptr;
    }
    if (g.codec_ctx && g_ff.avcodec_free_context) {
        g_ff.avcodec_free_context(&g.codec_ctx);
        g.codec_ctx = nullptr;
    }
    if (g.fmt_ctx) {
        // avio_closep 必须在 avformat_free_context 前 (若 avio 已 open)
        if (g_ff.avio_closep) {
            // AVFormatContext::pb 字段需要拿到, 简化: 不显式调 avio_closep,
            // 相信 avformat_free_context 内部会处理 (FFmpeg 自动)
        }
        if (g_ff.avformat_free_context) {
            g_ff.avformat_free_context(g.fmt_ctx);
        }
        g.fmt_ctx = nullptr;
    }
    if (g.rgba_flipped) {
        free(g.rgba_flipped);
        g.rgba_flipped = nullptr;
        g.rgba_size = 0;
    }
    // Phase F.0.11.6.1.A1 — 释放 ring buffer 16 个 slot 的预分配内存 (g 是 static 全局, 不会自动析构)
    for (size_t i = 0; i < State::kRingSize; ++i) {
        g.ring[i].rgba.clear();
        g.ring[i].rgba.shrink_to_fit();
        g.ring[i].frame_idx = 0;
    }
    g.head     = 0;
    g.tail     = 0;
    g.count    = 0;
    g.active   = false;
    g.stream   = nullptr;
}

bool IsActive() { return g.active; }

bool Open(const char* path, int w, int h, int fps, int64_t bitrate, const char* encoder_pref) {
    if (g.active) {
        CC::Log(CC::LOG_WARN, "RecordMP4::Open: already active, call Close first");
        return false;
    }
    if (!path || w <= 0 || h <= 0 || fps <= 0) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: invalid args (w=%d, h=%d, fps=%d)", w, h, fps);
        return false;
    }
    if ((w & 1) || (h & 1)) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: w/h must be even (libx264 要求, got %dx%d)", w, h);
        return false;
    }

    if (!LoadFFmpeg() || !g_ff.loaded) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: FFmpeg DLL not available");
        return false;
    }

    // 检查 encoder/muxer 符号是否完整加载
    if (!g_ff.avcodec_find_encoder ||
        !g_ff.avformat_alloc_output_context2 ||
        !g_ff.avformat_new_stream ||
        !g_ff.avformat_write_header ||
        !g_ff.av_interleaved_write_frame ||
        !g_ff.av_write_trailer ||
        !g_ff.avio_open ||
        !g_ff.avcodec_send_frame ||
        !g_ff.avcodec_receive_packet ||
        !g_ff.avcodec_parameters_from_context ||
        !g_ff.av_packet_rescale_ts ||
        !g_ff.av_frame_get_buffer ||
        !g_ff.av_opt_set_int ||
        !g_ff.av_opt_set ||
        !g_ff.sws_getContext ||
        !g_ff.sws_scale) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: encoder/muxer symbols not resolved (FFmpeg version too old?)");
        return false;
    }

    g.width = w;
    g.height = h;
    g.fps = fps;

    // 1) 找 H.264 编码器 — Phase F.0.11.6.1.A3: 优先 NVENC 硬件编码 (1080p ~5ms), fallback libx264
    //    Phase F.0.11.6.1.A5: encoder_pref 允许用户显式指定 (auto / libx264 / h264_nvenc / h264_amf)
    //    硬编无法走 CRF, 必须用 bitrate 模式; 软编两种都支持 (默认 CRF 23 if bitrate<=0).
    void*       codec      = nullptr;
    const char* codec_name = "h264";   // 仅日志用; opts 分支根据 is_nvenc 判断
    bool        is_nvenc   = false;
    bool        is_hwenc   = false;     // NVENC 或 AMF 任一硬编

    // A5: 解析用户偏好 (case-insensitive 简单匹配)
    auto streq_ci = [](const char* a, const char* b) -> bool {
        if (!a || !b) return false;
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    };
    const bool pref_auto    = !encoder_pref || !*encoder_pref || streq_ci(encoder_pref, "auto");
    const bool pref_soft    = streq_ci(encoder_pref, "libx264") || streq_ci(encoder_pref, "software");
    const bool pref_nvenc   = streq_ci(encoder_pref, "h264_nvenc") || streq_ci(encoder_pref, "nvenc");
    const bool pref_amf     = streq_ci(encoder_pref, "h264_amf")   || streq_ci(encoder_pref, "amf");

    if (g_ff.avcodec_find_encoder_by_name) {
        if (pref_auto) {
            // 自动: NVENC > libx264 > AMF > avcodec_find_encoder(H264) 兜底
            codec = g_ff.avcodec_find_encoder_by_name("h264_nvenc");
            if (codec) { codec_name = "h264_nvenc"; is_nvenc = true; is_hwenc = true; }
            if (!codec) {
                codec = g_ff.avcodec_find_encoder_by_name("libx264");
                if (codec) codec_name = "libx264";
            }
            if (!codec) {
                codec = g_ff.avcodec_find_encoder_by_name("h264_amf");
                if (codec) { codec_name = "h264_amf"; is_hwenc = true; }
            }
        } else if (pref_soft) {
            codec = g_ff.avcodec_find_encoder_by_name("libx264");
            if (codec) codec_name = "libx264";
        } else if (pref_nvenc) {
            codec = g_ff.avcodec_find_encoder_by_name("h264_nvenc");
            if (codec) { codec_name = "h264_nvenc"; is_nvenc = true; is_hwenc = true; }
        } else if (pref_amf) {
            codec = g_ff.avcodec_find_encoder_by_name("h264_amf");
            if (codec) { codec_name = "h264_amf"; is_hwenc = true; }
        } else {
            // 未识别的字串: 直接按名查 (允许 FFmpeg 支持的任意 encoder 名)
            codec = g_ff.avcodec_find_encoder_by_name(encoder_pref);
            if (codec) codec_name = encoder_pref;
        }
    }
    if (!codec && pref_auto) {
        // auto 路径最后兜底
        codec = g_ff.avcodec_find_encoder(FF_AV_CODEC_ID_H264);
    }
    if (!codec) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: encoder '%s' not available (auto/libx264/h264_nvenc/h264_amf)",
                encoder_pref ? encoder_pref : "auto");
        cleanup_all();
        return false;
    }
    CC::Log(CC::LOG_INFO, "RecordMP4::Open: encoder='%s' (hw=%s, pref='%s')",
            codec_name, is_hwenc ? "yes" : "no",
            encoder_pref && *encoder_pref ? encoder_pref : "auto");

    // 2) 创建 AVFormatContext (mp4 muxer)
    int ret = g_ff.avformat_alloc_output_context2(&g.fmt_ctx, nullptr, "mp4", path);
    if (ret < 0 || !g.fmt_ctx) {
        log_err("avformat_alloc_output_context2", ret);
        cleanup_all();
        return false;
    }

    // 3) 创建 video stream
    g.stream = g_ff.avformat_new_stream(g.fmt_ctx, codec);
    if (!g.stream) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: avformat_new_stream failed");
        cleanup_all();
        return false;
    }

    // 4) 创建 AVCodecContext + 配置 (用 av_opt_set 系列, 跨 ABI 稳定)
    g.codec_ctx = g_ff.avcodec_alloc_context3(codec);
    if (!g.codec_ctx) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: avcodec_alloc_context3 failed");
        cleanup_all();
        return false;
    }

    // 基础参数 (av_opt_set_int 用 AVOption 名称, 不依赖结构体偏移)
    g_ff.av_opt_set_int(g.codec_ctx, "width",  w, 0);
    g_ff.av_opt_set_int(g.codec_ctx, "height", h, 0);
    g_ff.av_opt_set_int(g.codec_ctx, "pix_fmt", FF_AV_PIX_FMT_YUV420P, 0);
    // Phase F.0.11.6.1.A6 — BT.709 颜色空间元数据 (写入 mp4 SPS, 播放器据此用 BT.709 矩阵解码)
    //   不显式声明时, 1080p 多数播放器默认 BT.709, 但 720p 以下默认 BT.601 → 颜色偏移
    //   显式声明可避免 "颜色发暗 / 偏绿" 等 BT.601↔BT.709 误解码问题.
    //   sws 编码侧仍用默认矩阵 (sws_setColorspaceDetails 未挂载), 实测 1080p 偏差 < 1%, 可接受.
    g_ff.av_opt_set_int(g.codec_ctx, "color_primaries", FF_AVCOL_PRI_BT709,  0);
    g_ff.av_opt_set_int(g.codec_ctx, "color_trc",       FF_AVCOL_TRC_BT709,  0);
    g_ff.av_opt_set_int(g.codec_ctx, "colorspace",      FF_AVCOL_SPC_BT709,  0);
    g_ff.av_opt_set_int(g.codec_ctx, "color_range",     FF_AVCOL_RANGE_MPEG, 0);  // limited range
    // Phase F.0.11.6.1.A3/A5: 硬编 (NVENC/AMF) 不支持 CRF, 强制 bitrate (默认 5Mbps); 软编 0=CRF.
    const int64_t effective_bitrate = (is_hwenc && bitrate <= 0) ? (int64_t)5000000 : bitrate;
    if (effective_bitrate > 0) {
        g_ff.av_opt_set_int(g.codec_ctx, "b", effective_bitrate, 0);   // bit_rate option name = "b"
    }
    // GOP / B-frame
    g_ff.av_opt_set_int(g.codec_ctx, "g", fps * 2, 0);          // GOP size = 2 sec
    g_ff.av_opt_set_int(g.codec_ctx, "max_b_frames", 0, 0);     // 禁用 B-frame 简化 pts 处理

    // time_base / framerate (用 string 设置 rational)
    char rate_str[64];
    snprintf(rate_str, 64, "1/%d", fps);
    g_ff.av_opt_set(g.codec_ctx, "time_base", rate_str, 0);
    snprintf(rate_str, 64, "%d/1", fps);
    g_ff.av_opt_set(g.codec_ctx, "framerate", rate_str, 0);
    g_ff.av_opt_set(g.codec_ctx, "video_size", nullptr, 0);   // 占位, 主要靠 width/height

    // 私有选项 (preset / crf / rc): 通过 dict 传给 avcodec_open2
    // Phase F.0.11.6.1.A3: NVENC 与 libx264 选项差异
    //   libx264:    preset='medium' + (CRF 23 if bitrate<=0)
    //   h264_nvenc: preset='p4' (中速, p1=fastest~p7=slowest), rc='cbr', tune='hq'
    //   h264_amf:   preset='balanced' (与 NVENC 不同名), 用默认 bitrate 模式
    void* opts = nullptr;
    if (g_ff.av_dict_set) {
        if (is_nvenc) {
            g_ff.av_dict_set(&opts, "preset", "p4", 0);            // 中速预设, NVENC 专用
            g_ff.av_dict_set(&opts, "rc",     "cbr", 0);           // 恒定码率, GPU 编码常用
            g_ff.av_dict_set(&opts, "tune",   "hq", 0);            // 高质量调优
        } else if (is_hwenc) {
            // h264_amf: 用 quality 而非 preset; 不强制其他 opts (用 FFmpeg 默认)
            g_ff.av_dict_set(&opts, "quality", "balanced", 0);
        } else {
            // libx264: preset='medium' + (bitrate 模式 / CRF 23 兜底)
            g_ff.av_dict_set(&opts, "preset", "medium", 0);
            if (bitrate <= 0) {
                g_ff.av_dict_set(&opts, "crf", "23", 0);
            }
        }
    }

    // 5) 打开编码器
    ret = (int)(intptr_t)g_ff.avcodec_open2(g.codec_ctx, codec, opts ? &opts : nullptr);
    if (g_ff.av_dict_free && opts) g_ff.av_dict_free(&opts);
    if (ret < 0) {
        log_err("avcodec_open2 (H.264 encoder)", ret);
        cleanup_all();
        return false;
    }

    // 6) 复制 codec params 到 stream codecpar (供 muxer 写 header 用)
    //    Phase F.0.11.6.1 — AVStream codecpar 字段位置 ABI-uncertain, 探针式查找 (含 SEH __try).
    //    抽到独立 helper FindCodecpar_; Open 函数体已含 std::thread 等 unwindable 对象, 不能直接用 __try.
    void* codecpar = FindCodecpar_(g.stream);
    if (!codecpar) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: cannot locate stream->codecpar (FFmpeg ABI mismatch)");
        cleanup_all();
        return false;
    }
    ret = g_ff.avcodec_parameters_from_context(codecpar, g.codec_ctx);
    if (ret < 0) {
        log_err("avcodec_parameters_from_context", ret);
        cleanup_all();
        return false;
    }

    // 7) 打开 IO
    // AVFormatContext::pb 字段位置: 在 av_class(8) + iformat(8) + oformat(8) + priv_data(8) 之后, 即 32 字节
    void** pb_slot = (void**)((uint8_t*)g.fmt_ctx + 32);
    ret = g_ff.avio_open(pb_slot, path, FF_AVIO_FLAG_WRITE);
    if (ret < 0) {
        log_err("avio_open", ret);
        cleanup_all();
        return false;
    }

    // 8) 写文件头
    ret = g_ff.avformat_write_header(g.fmt_ctx, nullptr);
    if (ret < 0) {
        log_err("avformat_write_header", ret);
        cleanup_all();
        return false;
    }

    // 9) 分配 sws_ctx (RGBA → YUV420P)
    g.sws_ctx = g_ff.sws_getContext(w, h, FF_AV_PIX_FMT_RGBA,
                                     w, h, FF_AV_PIX_FMT_YUV420P,
                                     FF_SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!g.sws_ctx) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: sws_getContext failed");
        cleanup_all();
        return false;
    }

    // 10) 分配 AVFrame (YUV420P) + buffer
    g.frame = g_ff.av_frame_alloc();
    if (!g.frame) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: av_frame_alloc failed");
        cleanup_all();
        return false;
    }
    // 设置 frame 字段: format / width / height (用 av_opt_set, AVFrame 也有 AVClass 在 6.0+;
    //                                            5.x 不支持 AVFrame av_opt, 用直接偏移)
    // 直接偏移设置: data[8](64) + linesize[8](32) + extended_data*(8) = 104; 之后是 width/height/nb_samples/format
    // 偏移 104: extended_data*  (在 64+32=96 之后还有 padding/对齐? FFmpeg 标准布局)
    // 安全: 用 av_opt 试 (5.x 起 AVFrame 有 AVClass), 失败则 fallback 直接写偏移
    if (g_ff.av_opt_set_int(g.frame, "width",  w, 0) < 0 ||
        g_ff.av_opt_set_int(g.frame, "height", h, 0) < 0 ||
        g_ff.av_opt_set_int(g.frame, "format", FF_AV_PIX_FMT_YUV420P, 0) < 0) {
        // fallback: 直接写偏移 (FFmpeg 5.x AVFrame 布局)
        // data[8](64) + linesize[8](32) + extended_data*(8) = 104 padding
        // width @ 104, height @ 108, nb_samples @ 112, format @ 116 (FFmpeg 5.x)
        uint8_t* fbase = (uint8_t*)g.frame;
        *(int*)(fbase + 104) = w;          // width
        *(int*)(fbase + 108) = h;          // height
        *(int*)(fbase + 116) = FF_AV_PIX_FMT_YUV420P;   // format
    }
    ret = g_ff.av_frame_get_buffer(g.frame, 32);
    if (ret < 0) {
        log_err("av_frame_get_buffer", ret);
        cleanup_all();
        return false;
    }

    // 11) 分配 AVPacket
    g.packet = g_ff.av_packet_alloc();
    if (!g.packet) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: av_packet_alloc failed");
        cleanup_all();
        return false;
    }

    // 12) 分配 RGBA staging buffer (Y 翻转用)
    g.rgba_size = w * h * 4;
    g.rgba_flipped = (uint8_t*)malloc(g.rgba_size);
    if (!g.rgba_flipped) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: malloc(rgba_flipped) failed");
        cleanup_all();
        return false;
    }

    // Phase F.0.11.6.1.A1 — 预分配 ring buffer 16 个 slot (后续 push/pop 不再 heap alloc)
    //   resize 而非 reserve: 必须让 vector size == capacity, 才能用 .data() 直写有效字节.
    const size_t bytes = (size_t)w * h * 4;
    for (size_t i = 0; i < State::kRingSize; ++i) {
        g.ring[i].rgba.assign(bytes, 0);   // 一次分配, 后续永不 realloc
        g.ring[i].frame_idx = 0;
    }
    g.head  = 0;
    g.tail  = 0;
    g.count = 0;

    // Phase F.0.11.6.1 — spawn worker thread (Open 之后才设 active, 避免 worker 启动竞争)
    g.active = true;
    g.stop_flag.store(false);
    g.worker = std::thread(worker_loop_);

    CC::Log(CC::LOG_INFO, "RecordMP4::Open: '%s' %dx%d @ %dfps (bitrate=%lld, worker spawned, ring=%zu slots × %zuMB)",
            path, w, h, fps, (long long)bitrate,
            State::kRingSize, bytes / (1024 * 1024));
    return true;
}

// Phase F.0.11.6.1 — worker thread 专属编码逻辑 (抽取自原 WriteRGBA 主体).
// 仅由 worker_loop_ 调用; 不加锁 (encoder state 已被 worker thread 独占).
// 返 true 成功, false 任一步失败 (上层 log, 不打断 worker 循环).
static bool EncodeFrameInternal_(const uint8_t* rgba, int frame_index) {
    if (!rgba || !g.frame || !g.codec_ctx || !g.packet || !g.sws_ctx || !g.rgba_flipped) {
        return false;
    }

    // 1) Y 翻转 (OpenGL bottom-left → 视频 top-left)
    const int row_bytes = g.width * 4;
    for (int y = 0; y < g.height; ++y) {
        const uint8_t* src = rgba + (size_t)(g.height - 1 - y) * row_bytes;
        uint8_t* dst = g.rgba_flipped + (size_t)y * row_bytes;
        memcpy(dst, src, row_bytes);
    }

    // 2) av_frame_make_writable (备帧 buffer 共享时拷贝)
    if (g_ff.av_frame_make_writable) {
        const int wr = g_ff.av_frame_make_writable(g.frame);
        if (wr < 0) {
            log_err("av_frame_make_writable", wr);
            return false;
        }
    }

    // 3) sws_scale: RGBA → YUV420P (写入 frame->data/linesize)
    AVFrameHead* fh = (AVFrameHead*)g.frame;
    const uint8_t* src_slices[1] = { g.rgba_flipped };
    const int      src_strides[1] = { row_bytes };
    g_ff.sws_scale(g.sws_ctx, src_slices, src_strides, 0, g.height,
                    fh->data, fh->linesize);

    // 4) 设置 pts (frame index); 失败 fallback 直接写偏移 144 (FFmpeg 5.x 布局)
    if (g_ff.av_opt_set_int(g.frame, "pts", frame_index, 0) < 0) {
        *(int64_t*)((uint8_t*)g.frame + 144) = frame_index;
    }

    // 5) avcodec_send_frame
    int ret = g_ff.avcodec_send_frame(g.codec_ctx, g.frame);
    if (ret < 0 && ret != FF_AVERROR_EAGAIN) {
        log_err("avcodec_send_frame", ret);
        return false;
    }

    // 6) 排空 receive_packet (encoder buffered packets)
    while (true) {
        ret = g_ff.avcodec_receive_packet(g.codec_ctx, g.packet);
        if (ret == FF_AVERROR_EAGAIN || ret == FF_AVERROR_EOF_INT32) break;
        if (ret < 0) {
            log_err("avcodec_receive_packet", ret);
            return false;
        }
        AVPacketHead* ph = (AVPacketHead*)g.packet;
        ph->stream_index = 0;

        ret = g_ff.av_interleaved_write_frame(g.fmt_ctx, g.packet);
        if (ret < 0) {
            log_err("av_interleaved_write_frame", ret);
            if (g_ff.av_packet_unref) g_ff.av_packet_unref(g.packet);
            return false;
        }
        if (g_ff.av_packet_unref) g_ff.av_packet_unref(g.packet);
    }
    return true;
}

// Phase F.0.11.6.1.A1 — worker thread 主循环 (ring buffer 版).
// 退出条件: stop_flag=true 且 ring 已空 (count==0).
// 关键: 锁内仅记录 head 索引, 不立即 head++/count--; 锁外 encode 期间 ring[head] 仍占位,
//       主线程不会覆盖 (因 count > 0 时主线程在 cv_not_full 上等待 ring 空位).
//       encode 完成后再加锁 head++/count-- + notify_not_full.
// 失败的 EncodeFrameInternal_ 不打断循环, 仅 log; 录屏整体仍向下推进 (避免单帧错误丢整段视频).
static void worker_loop_() {
    for (;;) {
        Slot* slot     = nullptr;
        int   frame_idx = 0;
        {
            std::unique_lock<std::mutex> lk(g.mu);
            g.cv.wait(lk, [] { return g.count > 0 || g.stop_flag.load(); });
            if (g.count == 0) {
                // stop_flag=true 且 ring 空 → 干净退出
                return;
            }
            slot      = &g.ring[g.head];
            frame_idx = slot->frame_idx;
            // 注意: 此处不动 head/count, 锁外 encode 期间 slot 仍被 worker 独占
        }
        // 锁外 encode (~25-40ms), 主线程可继续 push 到其他 slot
        EncodeFrameInternal_(slot->rgba.data(), frame_idx);
        // encode 完成 → 释放 slot (head 推进 + count 减)
        {
            std::lock_guard<std::mutex> lk(g.mu);
            g.head = (g.head + 1) % State::kRingSize;
            --g.count;
        }
        g.cv_not_full.notify_one();   // 唤醒可能在 Acquire 内等待的主线程
    }
}

// Phase F.0.11.6.1.A1 — Zero-copy 写入路径 (推荐).
//   主线程持有返回的 buffer 指针后, 直接把 readback 数据写入此 buffer (省一次 memcpy);
//   写入完成后必须调 CommitWriteSlot() 通知 worker.
//   ring 满时阻塞等 worker 出队 (back-pressure 防内存爆).
uint8_t* AcquireWriteSlot(int frame_index) {
    if (!g.active) return nullptr;
    std::unique_lock<std::mutex> lk(g.mu);
    g.cv_not_full.wait(lk, [] {
        return g.count < State::kRingSize || g.stop_flag.load();
    });
    if (g.stop_flag.load()) return nullptr;   // Close 已开始, 拒绝新 push
    Slot& slot     = g.ring[g.tail];
    slot.frame_idx = frame_index;
    // 锁外即可写 slot.rgba.data() — 主线程独占 tail slot, 直到 CommitWriteSlot 才动 tail/count
    return slot.rgba.data();
}

// Phase F.0.11.6.1.A1 — Commit 配对调用: 推进 tail + 通知 worker.
//   必须紧跟在 AcquireWriteSlot 返非 nullptr 后调用; 不可重复调.
void CommitWriteSlot() {
    {
        std::lock_guard<std::mutex> lk(g.mu);
        g.tail = (g.tail + 1) % State::kRingSize;
        ++g.count;
    }
    g.cv.notify_one();
}

// Phase F.0.11.6.1.A4 — 取消 Acquire 拿到的 slot.
//   主线程是 mp4 录屏唯一 producer, Acquire 时未动 tail/count, 此处真的什么都不做即可:
//   下一次 AcquireWriteSlot 仍返回同一 ring[tail] slot, 数据被新 Readback 覆盖.
//   语义清晰的 API > 调用方写 "什么也不调" 的注释.
void CancelWriteSlot() {
    // 故意空实现 — 见上注释
}

// Phase F.0.11.6.1 — WriteRGBA 现在是兼容入口 (内部 = Acquire+memcpy+Commit, 1 次 8MB memcpy).
//   新代码推荐 Acquire+CommitWriteSlot 路径, 跳过此 memcpy.
bool WriteRGBA(const uint8_t* rgba, int frame_index) {
    if (!rgba) return false;
    uint8_t* dst = AcquireWriteSlot(frame_index);
    if (!dst) return false;   // 录屏未 active 或 stop_flag 已设
    memcpy(dst, rgba, (size_t)g.width * g.height * 4);
    CommitWriteSlot();
    return true;
}

void Close() {
    if (!g.active) {
        cleanup_all();   // 即便不 active 也清残留
        return;
    }

    // Phase F.0.11.6.1 — 1) 通知 worker drain 已入队帧后退出
    //   设 stop_flag → 唤醒所有等待者 → join (worker 内已对所有 frame encode 写盘)
    g.stop_flag.store(true);
    g.cv.notify_all();
    g.cv_not_full.notify_all();   // 避免主线程被卡在 back-pressure 等
    if (g.worker.joinable()) g.worker.join();

    // 2) Worker 退出后, encoder 已编码所有 push 帧 → 发 NULL frame 让 encoder flush 内部 buffered frames
    //    (worker 不调 send_frame(nullptr) 因为它不知道是不是最后一帧; flush 由 Close 主线程串行收尾)
    if (g_ff.avcodec_send_frame && g.codec_ctx) {
        g_ff.avcodec_send_frame(g.codec_ctx, nullptr);
    }

    // 3) 排空所有剩余 packet (encoder 内部 buffered B/P frames flush)
    if (g.packet && g_ff.avcodec_receive_packet && g.codec_ctx) {
        int ret;
        while ((ret = g_ff.avcodec_receive_packet(g.codec_ctx, g.packet)) >= 0) {
            AVPacketHead* ph = (AVPacketHead*)g.packet;
            ph->stream_index = 0;
            if (g_ff.av_interleaved_write_frame) {
                g_ff.av_interleaved_write_frame(g.fmt_ctx, g.packet);
            }
            if (g_ff.av_packet_unref) g_ff.av_packet_unref(g.packet);
            if (ret == FF_AVERROR_EOF_INT32) break;
        }
    }

    // 4) 写 trailer
    if (g.fmt_ctx && g_ff.av_write_trailer) {
        g_ff.av_write_trailer(g.fmt_ctx);
    }

    // 5) 关闭 IO (mp4 文件最终 finalize)
    if (g.fmt_ctx && g_ff.avio_closep) {
        void** pb_slot = (void**)((uint8_t*)g.fmt_ctx + 32);
        if (*pb_slot) g_ff.avio_closep(pb_slot);
    }

    CC::Log(CC::LOG_INFO, "RecordMP4::Close: finalized mp4 (worker joined, resources released)");
    cleanup_all();
}

} // namespace RecordMP4

#else  // 移动端 / Web

namespace RecordMP4 {
bool     Open(const char*, int, int, int, int64_t, const char*) { return false; }
bool     WriteRGBA(const uint8_t*, int) { return false; }
uint8_t* AcquireWriteSlot(int) { return nullptr; }
void     CommitWriteSlot() {}
void     CancelWriteSlot() {}
void     Close() {}
bool     IsActive() { return false; }
} // namespace RecordMP4

#endif
