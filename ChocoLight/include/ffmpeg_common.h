/**
 * @file ffmpeg_common.h
 * @brief FFmpeg 动态加载器 — 共享类型定义
 * @note FFmpegLib 在 light_av.cpp 中定义, video_backend_ffmpeg.cpp 通过 extern 访问
 *       DynLoad/DynSym/DynFree 仅在 light_av.cpp 内部使用
 */
#pragma once

#include <cstdint>
#include <cstddef>

#ifdef _WIN32
#include <windows.h>
typedef HMODULE DynLib;
#else
typedef void* DynLib;
#endif

// ==================== FFmpeg 函数指针集合 ====================

struct FFmpegLib {
    DynLib  hFormat;   // avformat-59
    DynLib  hCodec;    // avcodec-59
    DynLib  hUtil;     // avutil-57
    DynLib  hResample; // swresample-4
    DynLib  hScale;    // swscale-6
    bool    loaded;
    bool    attempted; // 防止重复尝试加载

    // --- avformat ---
    void* (*avformat_open_input)(void** ctx, const char* url, void* fmt, void** opts);
    int   (*avformat_find_stream_info)(void* ctx, void** opts);
    void  (*avformat_close_input)(void** ctx);
    int   (*av_find_best_stream)(void* ctx, int mediaType, int wantedIdx, int relIdx, void** codec, int flags);
    int   (*av_read_frame)(void* ctx, void* pkt);

    // --- avcodec ---
    void* (*avcodec_alloc_context3)(void* codec);
    int   (*avcodec_parameters_to_context)(void* ctx, void* params);
    int   (*avcodec_open2)(void* ctx, void* codec, void** opts);
    int   (*avcodec_send_packet)(void* ctx, void* pkt);
    int   (*avcodec_receive_frame)(void* ctx, void* frame);
    void  (*avcodec_free_context)(void** ctx);
    void* (*avcodec_find_decoder)(int id);

    // --- avutil ---
    void* (*av_frame_alloc)();
    void  (*av_frame_free)(void** frame);
    void* (*av_packet_alloc)();
    void  (*av_packet_free)(void** pkt);
    void  (*av_packet_unref)(void* pkt);
    int   (*av_samples_get_buffer_size)(int* linesize, int channels, int nb_samples, int fmt, int align);
    void* (*av_malloc)(size_t size);
    void  (*av_free)(void* ptr);

    // --- swscale (视频色彩空间转换) ---
    void* (*sws_getContext)(int srcW, int srcH, int srcFmt,
                           int dstW, int dstH, int dstFmt,
                           int flags, void* srcFilter, void* dstFilter, double* param);
    int   (*sws_scale)(void* ctx, const uint8_t* const srcSlice[],
                       const int srcStride[], int srcSliceY, int srcSliceH,
                       uint8_t* const dst[], const int dstStride[]);
    void  (*sws_freeContext)(void* ctx);
    int   (*av_image_get_buffer_size)(int pixFmt, int w, int h, int align);
    int   (*av_image_fill_arrays)(uint8_t* dst_data[4], int dst_linesize[4],
                                 const uint8_t* src, int pixFmt, int w, int h, int align);

    // --- swresample (音频格式转换) ---
    void* (*swr_alloc_set_opts)(void* s, int64_t out_ch_layout, int out_sample_fmt, int out_sample_rate,
                                int64_t in_ch_layout, int in_sample_fmt, int in_sample_rate,
                                int log_offset, void* log_ctx);
    int   (*swr_init)(void* s);
    int   (*swr_convert)(void* s, uint8_t** out, int out_count,
                         const uint8_t** in, int in_count);
    void  (*swr_free)(void** s);

    // ===== Phase F.0.11.6 — encoder / muxer =====
    // avcodec encoder
    void* (*avcodec_find_encoder)(int id);
    void* (*avcodec_find_encoder_by_name)(const char* name);
    int   (*avcodec_send_frame)(void* ctx, const void* frame);
    int   (*avcodec_receive_packet)(void* ctx, void* pkt);
    int   (*avcodec_parameters_from_context)(void* params, void* ctx);
    void  (*av_packet_rescale_ts)(void* pkt, void* tb_src, void* tb_dst);

    // avformat muxer
    int   (*avformat_alloc_output_context2)(void** out_ctx, void* fmt, const char* fmt_name, const char* path);
    void* (*avformat_new_stream)(void* ctx, void* codec);
    int   (*avformat_write_header)(void* ctx, void** opts);
    int   (*av_interleaved_write_frame)(void* ctx, void* pkt);
    int   (*av_write_trailer)(void* ctx);
    int   (*avio_open)(void** s, const char* url, int flags);
    int   (*avio_closep)(void** s);
    void  (*avformat_free_context)(void* ctx);

    // avutil frame helpers (frame buffer 分配, 编码侧需要)
    int   (*av_frame_get_buffer)(void* frame, int align);
    int   (*av_frame_make_writable)(void* frame);
    int   (*av_dict_set)(void** pm, const char* key, const char* value, int flags);
    void  (*av_dict_free)(void** pm);

    // av_opt_set 系列: 用 string key 设置 AVCodecContext / AVFormatContext 字段, 跨 ABI 版本稳定
    //   Phase F.0.11.6: 编码器 ctx 字段 (fps/bitrate/preset 等) 通过此接口设置, 避开直接写 ABI 不稳定的结构体偏移
    int   (*av_opt_set)(void* obj, const char* name, const char* val, int search_flags);
    int   (*av_opt_set_int)(void* obj, const char* name, int64_t val, int search_flags);
};

/// 全局 FFmpeg 库实例 (light_av.cpp 定义, 非 static)
extern FFmpegLib g_ff;

/// 懒加载 FFmpeg DLL — 首次调用时触发 (light_av.cpp 定义)
bool LoadFFmpeg();
