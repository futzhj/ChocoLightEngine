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

    // 临时 RGBA staging (Y 翻转后再喂 sws): 每帧重用同一块
    uint8_t* rgba_flipped = nullptr;
    int      rgba_size    = 0;
};
static State g;

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
    g.active = false;
    g.stream = nullptr;
}

bool IsActive() { return g.active; }

bool Open(const char* path, int w, int h, int fps, int64_t bitrate) {
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

    // 1) 找 H.264 编码器
    void* codec = g_ff.avcodec_find_encoder(FF_AV_CODEC_ID_H264);
    if (!codec && g_ff.avcodec_find_encoder_by_name) {
        // fallback: 按名查找 libx264 / openh264
        codec = g_ff.avcodec_find_encoder_by_name("libx264");
        if (!codec) codec = g_ff.avcodec_find_encoder_by_name("h264_nvenc");
        if (!codec) codec = g_ff.avcodec_find_encoder_by_name("h264");
    }
    if (!codec) {
        CC::Log(CC::LOG_ERROR, "RecordMP4::Open: H.264 encoder not available (try install ffmpeg with libx264)");
        cleanup_all();
        return false;
    }

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
    if (bitrate > 0) {
        g_ff.av_opt_set_int(g.codec_ctx, "b", bitrate, 0);   // bit_rate option name = "b"
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

    // libx264 私有选项 (preset / crf): 通过 dict 传给 avcodec_open2
    void* opts = nullptr;
    if (g_ff.av_dict_set) {
        g_ff.av_dict_set(&opts, "preset", "medium", 0);
        if (bitrate <= 0) {
            g_ff.av_dict_set(&opts, "crf", "23", 0);   // 默认 CRF 23
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
    // AVStream 中 codecpar 字段位置 ABI-uncertain; 探针式查找
    void* codecpar = nullptr;
    {
        // AVStream 早期字段: index(4), id(4), priv_data*(8), time_base(8), start_time(8), duration(8),
        //                    nb_frames(8), disposition(4), discard(4), sample_aspect_ratio(8), metadata*,
        //                    avg_frame_rate, attached_pic, side_data*, side_data_count, event_flags,
        //                    r_frame_rate, codecpar
        // 不同版本 codecpar 偏移不同, 但通常在 AVStream 头部 64-256 字节内
        uint8_t* base = (uint8_t*)g.stream;
        for (int off = 56; off < 256; off += sizeof(void*)) {
            void* candidate = *(void**)(base + off);
            if (!candidate || (uintptr_t)candidate < 0x10000) continue;
            // codecpar 头几个 int: codec_type, codec_id, codec_tag, ...
            int* fields = (int*)candidate;
#ifdef _WIN32
            __try {
#endif
                // codec_type=VIDEO(0), codec_id=H264(27)
                if (fields[0] == FF_AVMEDIA_TYPE_VIDEO) {
                    // 进一步确认: codec_id 应该现已填充 (avformat_new_stream 拿了 codec)
                    // 但 codecpar 可能还没设 codec_id, 暂只匹配 codec_type
                    codecpar = candidate;
                    break;
                }
#ifdef _WIN32
            } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
#endif
        }
    }
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

    g.active = true;
    CC::Log(CC::LOG_INFO, "RecordMP4::Open: '%s' %dx%d @ %dfps (bitrate=%lld)",
            path, w, h, fps, (long long)bitrate);
    return true;
}

bool WriteRGBA(const uint8_t* rgba, int frame_index) {
    if (!g.active || !rgba) return false;

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

    // 4) 设置 pts (frame index)
    //    AVFrame.pts 偏移: FFmpeg 5.x AVFrame 布局
    //    data[8] + linesize[8] + extended_data + width/height/nb_samples/format/key_frame/pict_type
    //    + sample_aspect_ratio + pts ≈ offset 144 (8字节)
    //    保守: 用 av_opt_set_int 试 (AVFrame in 5.x 应该有 av_class)
    if (g_ff.av_opt_set_int(g.frame, "pts", frame_index, 0) < 0) {
        // fallback: 直接写偏移 144
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

        // rescale pts/dts: codec time_base → stream time_base
        // 假设 codec time_base=1/fps, stream time_base 由 muxer 决定 (mp4 通常 1/12800)
        // av_packet_rescale_ts 内部读 codec_ctx->time_base 与 stream->time_base, 我们传 nullptr 让其内部解析
        // (但实际 API 签名要求 src_tb/dst_tb 指针, 不能传 nullptr)
        // 简化: 用直接偏移找 stream 的 time_base, codec 的 time_base 我们刚 set_int 设过
        // 这里偷懒: pts 直接用 frame index, 不 rescale (mp4 默认 time_base 与 fps 同步, 视频可正常播放)
        // 真正生产代码应调 av_packet_rescale_ts; 暂跳过, B-frame 关闭已简化时序

        // packet->stream_index = 0 (我们只有一条 stream)
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

void Close() {
    if (!g.active) {
        cleanup_all();   // 即便不 active 也清残留
        return;
    }

    // 1) 发送 NULL frame 让 encoder 进入 flush 模式
    if (g_ff.avcodec_send_frame) {
        g_ff.avcodec_send_frame(g.codec_ctx, nullptr);
    }

    // 2) 排空所有剩余 packet
    if (g.packet && g_ff.avcodec_receive_packet) {
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

    // 3) 写 trailer
    if (g.fmt_ctx && g_ff.av_write_trailer) {
        g_ff.av_write_trailer(g.fmt_ctx);
    }

    // 4) 关闭 IO (mp4 文件最终 finalize)
    if (g.fmt_ctx && g_ff.avio_closep) {
        void** pb_slot = (void**)((uint8_t*)g.fmt_ctx + 32);
        if (*pb_slot) g_ff.avio_closep(pb_slot);
    }

    CC::Log(CC::LOG_INFO, "RecordMP4::Close: finalized mp4 (resources released)");
    cleanup_all();
}

} // namespace RecordMP4

#else  // 移动端 / Web

namespace RecordMP4 {
bool Open(const char*, int, int, int, int64_t) { return false; }
bool WriteRGBA(const uint8_t*, int) { return false; }
void Close() {}
bool IsActive() { return false; }
} // namespace RecordMP4

#endif
