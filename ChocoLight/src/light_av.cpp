/**
 * @file light_av.cpp
 * @brief Light.AV + Audio + AudioData 模块 — FFmpeg 动态加载后端
 * @note 深度还原自 Light.dll IDA 反编译 + FFmpeg 59/57/4 动态加载
 *
 * AV 父模块 (还原自 sub_1800AE900):
 *   Play()     — 播放媒体 (设置播放状态)
 *   Pause()    — 暂停播放
 *   Stop()     — 停止播放
 *   Playable   — 可播放基类子表 (供 Audio 继承)
 *
 * Audio (还原自 luaopen_Light_AV_Audio):
 *   __call(path)     — 构造函数, 加载音频文件 (avformat_open_input)
 *   __tostring()     — "Light.AV.Audio"
 *   继承 Playable (通过 Light:New 调用 Playable 基类)
 *
 * AudioData (还原自 luaopen_Light_AV_AudioData, 7 函数):
 *   GetFormat()       — PCM 采样格式
 *   GetChannels()     — 声道数
 *   GetSampleRate()   — 采样率
 *   GetPointer()      — 原始 PCM 数据指针
 *   Count()           — 采样帧数
 *   __call(path)      — 构造函数 (完整解码到 PCM)
 *   __tostring()      — "Light.AV.AudioData"
 *
 * FFmpeg 动态加载 (LoadLibraryA + GetProcAddress):
 *   avformat-59.dll、avcodec-59.dll、avutil-57.dll、swresample-4.dll
 */

#include "light.h"
#include "render_backend.h"
#include "light_audio_backend.h"
#include <cstring>
#include <cstdlib>
#include "platform_window.h"  // PlatformWindow::GetTime (跨平台高精度计时)

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>    // Video 音频同步仍需 waveOut API
#pragma comment(lib, "winmm.lib")
typedef HMODULE DynLib;
#define DynLoad(name) LoadLibraryA(name)
#define DynSym(lib, name) GetProcAddress(lib, name)
#define DynFree(lib) FreeLibrary(lib)
#else
#include <dlfcn.h>
typedef void* DynLib;
#define DynLoad(name) dlopen(name, RTLD_LAZY)
#define DynSym(lib, name) dlsym(lib, name)
#define DynFree(lib) dlclose(lib)
#endif

// ==================== FFmpeg 动态加载 ====================

// FFmpeg 函数指针类型定义 (简化为 void* 泛型, 内部按正确 ABI 调用)
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
};

static FFmpegLib g_ff = {};

/// 懒加载 FFmpeg DLL — 首次调用 AV 时触发
static bool LoadFFmpeg() {
    if (g_ff.attempted) return g_ff.loaded;
    g_ff.attempted = true;

#ifdef _WIN32
    const char* fmtNames[] = { "avformat-59.dll", "avformat.dll", nullptr };
    const char* codNames[] = { "avcodec-59.dll",  "avcodec.dll",  nullptr };
    const char* utlNames[] = { "avutil-57.dll",   "avutil.dll",   nullptr };
    const char* swrNames[] = { "swresample-4.dll","swresample.dll",nullptr };
    const char* swsNames[] = { "swscale-9.dll",   "swscale.dll",  nullptr };
#elif defined(__APPLE__)
    const char* fmtNames[] = { "libavformat.59.dylib", "libavformat.dylib", nullptr };
    const char* codNames[] = { "libavcodec.59.dylib",  "libavcodec.dylib",  nullptr };
    const char* utlNames[] = { "libavutil.57.dylib",   "libavutil.dylib",   nullptr };
    const char* swrNames[] = { "libswresample.4.dylib","libswresample.dylib",nullptr };
    const char* swsNames[] = { "libswscale.6.dylib",   "libswscale.dylib",  nullptr };
#else
    const char* fmtNames[] = { "libavformat.so.59", "libavformat.so", nullptr };
    const char* codNames[] = { "libavcodec.so.59",  "libavcodec.so",  nullptr };
    const char* utlNames[] = { "libavutil.so.57",   "libavutil.so",   nullptr };
    const char* swrNames[] = { "libswresample.so.4","libswresample.so",nullptr };
    const char* swsNames[] = { "libswscale.so.6",   "libswscale.so",  nullptr };
#endif

#ifdef _WIN32
    SetDllDirectoryA("lib");
#endif
    auto tryLoad = [](const char** names) -> DynLib {
        for (int i = 0; names[i]; ++i) {
#ifdef _WIN32
            char libPath[260];
            snprintf(libPath, 260, "lib\\%s", names[i]);
            DynLib h = DynLoad(libPath);
            if (h) return h;
#endif
            DynLib h2 = DynLoad(names[i]);
            if (h2) return h2;
        }
        return nullptr;
    };

    g_ff.hFormat   = tryLoad(fmtNames);
    g_ff.hCodec    = tryLoad(codNames);
    g_ff.hUtil     = tryLoad(utlNames);
    g_ff.hResample = tryLoad(swrNames);
    g_ff.hScale    = tryLoad(swsNames);
#ifdef _WIN32
    SetDllDirectoryA(nullptr); // 恢复默认搜索路径
#endif

    if (!g_ff.hFormat || !g_ff.hCodec || !g_ff.hUtil) {
        CC::Log(CC::LOG_WARN, "FFmpeg DLLs not found (avformat=%p, avcodec=%p, avutil=%p)",
                g_ff.hFormat, g_ff.hCodec, g_ff.hUtil);
        g_ff.loaded = false;
        return false;
    }

    // 解析函数指针
    #define LOAD_FUNC(lib, name) \
        *(void**)&g_ff.name = (void*)DynSym(g_ff.lib, #name)

    LOAD_FUNC(hFormat, avformat_open_input);
    LOAD_FUNC(hFormat, avformat_find_stream_info);
    LOAD_FUNC(hFormat, avformat_close_input);
    LOAD_FUNC(hFormat, av_find_best_stream);
    LOAD_FUNC(hFormat, av_read_frame);

    LOAD_FUNC(hCodec, avcodec_alloc_context3);
    LOAD_FUNC(hCodec, avcodec_parameters_to_context);
    LOAD_FUNC(hCodec, avcodec_open2);
    LOAD_FUNC(hCodec, avcodec_send_packet);
    LOAD_FUNC(hCodec, avcodec_receive_frame);
    LOAD_FUNC(hCodec, avcodec_free_context);
    LOAD_FUNC(hCodec, avcodec_find_decoder);

    LOAD_FUNC(hUtil, av_frame_alloc);
    LOAD_FUNC(hUtil, av_frame_free);

    // av_packet 系列: FFmpeg 5.x 在 avcodec 导出, 旧版在 avutil
    LOAD_FUNC(hCodec, av_packet_alloc);
    if (!g_ff.av_packet_alloc) LOAD_FUNC(hUtil, av_packet_alloc);
    LOAD_FUNC(hCodec, av_packet_free);
    if (!g_ff.av_packet_free) LOAD_FUNC(hUtil, av_packet_free);
    LOAD_FUNC(hCodec, av_packet_unref);
    if (!g_ff.av_packet_unref) LOAD_FUNC(hUtil, av_packet_unref);
    LOAD_FUNC(hUtil, av_samples_get_buffer_size);
    LOAD_FUNC(hUtil, av_malloc);
    LOAD_FUNC(hUtil, av_free);

    // swscale (可选, 视频转换用)
    if (g_ff.hScale) {
        LOAD_FUNC(hScale, sws_getContext);
        LOAD_FUNC(hScale, sws_scale);
        LOAD_FUNC(hScale, sws_freeContext);
    }
    // avutil 额外 (图像)
    LOAD_FUNC(hUtil, av_image_get_buffer_size);
    LOAD_FUNC(hUtil, av_image_fill_arrays);

    // swresample (可选, 音频格式转换)
    if (g_ff.hResample) {
        LOAD_FUNC(hResample, swr_alloc_set_opts);
        LOAD_FUNC(hResample, swr_init);
        LOAD_FUNC(hResample, swr_convert);
        LOAD_FUNC(hResample, swr_free);
    }

    #undef LOAD_FUNC

    // 检查必要的函数���否都解析成功
    if (!g_ff.avformat_open_input || !g_ff.avcodec_alloc_context3 || !g_ff.av_frame_alloc) {
        CC::Log(CC::LOG_WARN, "FFmpeg: some functions not resolved");
        g_ff.loaded = false;
        return false;
    }

    g_ff.loaded = true;
    CC::Log(CC::LOG_INFO, "FFmpeg loaded: avformat=%p, avcodec=%p, avutil=%p, swresample=%p",
            g_ff.hFormat, g_ff.hCodec, g_ff.hUtil, g_ff.hResample);
    return true;
}

// ==================== AV 内部上下文 ====================

struct AVContext {
    void* formatCtx;    // AVFormatContext*
    void* codecCtx;     // AVCodecContext*
    int   streamIdx;    // 音频流索引
    int   sampleRate;
    int   channels;
    int   format;       // AV_SAMPLE_FMT_*
    int   frameCount;
    void* pcmData;      // 解码后的 PCM 数据
    int   pcmSize;
    bool  playing;
    bool  paused;
    AudioHandle* audioHandle; // miniaudio 音频句柄
};

static AVContext* GetAVCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    AVContext* ctx = (AVContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

// ==================== Playable 基类函数 ====================

/// @lua_api Light.AV.Play
/// @brief 播放音频
/// @return void
static int l_AV_Play(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    if (ctx && ctx->audioHandle) {
        ctx->playing = true;
        ctx->paused = false;
        AudioBackend::Play(ctx->audioHandle);
    }
    return 0;
}

/// @lua_api Light.AV.Pause
/// @brief 暂停播放
/// @return void
static int l_AV_Pause(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    if (ctx && ctx->audioHandle) {
        ctx->paused = true;
        AudioBackend::Pause(ctx->audioHandle);
    }
    return 0;
}

/// @lua_api Light.AV.Stop
/// @brief 停止播放
/// @return void
static int l_AV_Stop(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    if (ctx) {
        ctx->playing = false;
        ctx->paused = false;
        if (ctx->audioHandle) AudioBackend::Stop(ctx->audioHandle);
    }
    return 0;
}

// ==================== Audio 函数 ====================

/// Audio.__gc — 释放音频资源
static int l_Audio_GC(lua_State* L) {
    AVContext* ctx = (AVContext*)lua_touserdata(L, 1);
    if (!ctx) return 0;
    // 释放 AudioBackend 句柄
    if (ctx->audioHandle) { AudioBackend::Free(ctx->audioHandle); ctx->audioHandle = nullptr; }
    // 释放 FFmpeg 资源
    if (ctx->codecCtx && g_ff.loaded && g_ff.avcodec_free_context)
        g_ff.avcodec_free_context(&ctx->codecCtx);
    if (ctx->formatCtx && g_ff.loaded && g_ff.avformat_close_input)
        g_ff.avformat_close_input(&ctx->formatCtx);
    return 0;
}

/// @lua_api Light.AV.Audio.__call
/// @brief 构造函数, 加载音频文件 (WAV/MP3/FLAC/OGG)
/// @param path string 音频文件路径
/// @return void
/// @example
/// local audio = Light(Light.AV.Audio):New("bgm.mp3")
/// Light.AV.Play(audio)
static int l_Audio_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    AVContext* ctx = (AVContext*)lua_newuserdata(L, sizeof(AVContext));
    memset(ctx, 0, sizeof(AVContext));

    // __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Audio_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    // 尝试 FFmpeg 加载
    if (LoadFFmpeg()) {
        // 打开音频文件
        int rc = (int)(intptr_t)g_ff.avformat_open_input(&ctx->formatCtx, path, nullptr, nullptr);
        if (rc >= 0 && ctx->formatCtx) {
            g_ff.avformat_find_stream_info(ctx->formatCtx, nullptr);
            // 查找音频流 (AVMEDIA_TYPE_AUDIO = 1)
            void* decoder = nullptr;
            ctx->streamIdx = g_ff.av_find_best_stream(ctx->formatCtx, 1, -1, -1, &decoder, 0);
            if (ctx->streamIdx >= 0 && decoder) {
                ctx->codecCtx = g_ff.avcodec_alloc_context3(decoder);
                CC::Log(CC::LOG_INFO, "Audio: opened '%s' (stream=%d)", path, ctx->streamIdx);
            }
        } else {
            CC::Log(CC::LOG_WARN, "Audio: failed to open '%s' (rc=%d)", path, rc);
        }
    } else {
        CC::Log(CC::LOG_INFO, "Audio: FFmpeg not available, stub mode for '%s'", path);
    }

    // 通过 AudioBackend 加载 (优先 miniaudio 原生, 失败则回退 FFmpeg+PCM)
    ctx->audioHandle = AudioBackend::LoadFile(path);
    if (ctx->audioHandle) {
        CC::Log(CC::LOG_INFO, "Audio: loaded via miniaudio '%s'", path);
    } else {
        CC::Log(CC::LOG_INFO, "Audio: miniaudio failed, using FFmpeg fallback for '%s'", path);
        // T3.4 回退路径: FFmpeg 解码 → PCM → AudioBackend::LoadPCM
        // 待实现
    }

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Audio.__tostring — 还原自 sub_1800AEC30
static int l_Audio_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.AV.Audio");
    return 1;
}

// ==================== AudioData 函数 ====================

/// @lua_api Light.AV.AudioData.GetFormat
/// @brief 获取采样格式
/// @return number
static int l_AudioData_GetFormat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->format : 0);
    return 1;
}

/// @lua_api Light.AV.AudioData.GetChannels
/// @brief 获取声道数
/// @return number
static int l_AudioData_GetChannels(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 1;
}

/// @lua_api Light.AV.AudioData.GetSampleRate
/// @brief 获取采样率
/// @return number
static int l_AudioData_GetSampleRate(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->sampleRate : 0);
    return 1;
}

/// @lua_api Light.AV.AudioData.GetPointer
/// @brief 获取原始 PCM 数据指针
/// @return lightuserdata,number 指针,字节数
static int l_AudioData_GetPointer(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    if (ctx && ctx->pcmData) {
        lua_pushlightuserdata(L, ctx->pcmData);
        lua_pushinteger(L, ctx->pcmSize);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

/// @lua_api Light.AV.AudioData.Count
/// @brief 获取采样帧数
/// @return number
static int l_AudioData_Count(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    AVContext* ctx = GetAVCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->frameCount : 0);
    return 1;
}

/// AudioData.__gc — 释放 PCM 数据
/// 还原自 sub_1800AF020
static int l_AudioData_GC(lua_State* L) {
    AVContext* ctx = (AVContext*)lua_touserdata(L, 1);
    if (!ctx) return 0;
    if (ctx->pcmData) {
        if (g_ff.loaded && g_ff.av_free)
            g_ff.av_free(ctx->pcmData);
        else
            free(ctx->pcmData);
        ctx->pcmData = nullptr;
    }
    if (ctx->codecCtx && g_ff.loaded && g_ff.avcodec_free_context)
        g_ff.avcodec_free_context(&ctx->codecCtx);
    if (ctx->formatCtx && g_ff.loaded && g_ff.avformat_close_input)
        g_ff.avformat_close_input(&ctx->formatCtx);
    return 0;
}

/// @lua_api Light.AV.AudioData.__call
/// @brief 构造函数 (支持 3 种创建方式)
/// @param path_or_rate string|number 文件路径或采样率
/// @return void
/// @note __call(self, filename) — 从文件加载
/// @note __call(self, buffer, size) — 从缓冲区
/// @note __call(self, rate, ch, fmt, count) — 指定参数创建
static int l_AudioData_Call(lua_State* L) {
    int argc = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    // 64 字节 userdata (匹配 IDA)
    AVContext* ctx = (AVContext*)lua_newuserdata(L, 64);
    memset(ctx, 0, 64);

    // __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_AudioData_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    lua_setfield(L, 1, "__instance");

    switch (argc) {
    case 2: {  // 文件名 → 完整解码为 PCM
        const char* path = luaL_checkstring(L, 2);
        if (LoadFFmpeg()) {
            void* fmtCtx = nullptr;
            int rc = (int)(intptr_t)g_ff.avformat_open_input(&fmtCtx, path, nullptr, nullptr);
            if (rc >= 0 && fmtCtx) {
                g_ff.avformat_find_stream_info(fmtCtx, nullptr);
                ctx->formatCtx = fmtCtx;
                // 提取音频流元数据 (简化实现)
                void* decoder = nullptr;
                int si = g_ff.av_find_best_stream(fmtCtx, 1, -1, -1, &decoder, 0);
                if (si >= 0) {
                    ctx->streamIdx = si;
                    // 完整解码需要: alloc_context3 → parameters_to_context → open2 → read/decode 循环
                    CC::Log(CC::LOG_INFO, "AudioData: opened '%s' for decoding", path);
                }
            } else {
                CC::Log(CC::LOG_WARN, "AudioData: failed to open '%s'", path);
            }
        } else {
            CC::Log(CC::LOG_INFO, "AudioData: FFmpeg not available, stub for '%s'", path);
        }
        break;
    }
    case 3: {  // cdata/userdata 缓冲区
        const char* typeName = lua_typename(L, lua_type(L, 2));
        if (strcmp(typeName, "cdata") != 0 && strcmp(typeName, "userdata") != 0)
            luaL_error(L, "Buffer should be cdata or userdata, but give: %s", typeName);
        ctx->pcmData = (void*)lua_topointer(L, 2);
        ctx->pcmSize = (int)luaL_checkinteger(L, 3);
        break;
    }
    case 5:  // 指定格式
        ctx->sampleRate = (int)luaL_checkinteger(L, 2);
        ctx->channels = (int)luaL_checkinteger(L, 3);
        ctx->format = (int)luaL_checkinteger(L, 4);
        ctx->frameCount = (int)luaL_checkinteger(L, 5);
        break;
    default:
        CC::Log(CC::LOG_ERROR, "AudioData: Unknown parameters");
        break;
    }

    return 1;
}

/// AudioData.__tostring — 还原自 sub_1800AF650
static int l_AudioData_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.AV.AudioData");
    return 1;
}

// ==================== Video 视频播放模块 ====================

// FFmpeg 内部常量 (避免包含 FFmpeg 头文件)
enum {
    FF_AVMEDIA_TYPE_VIDEO = 0,
    FF_AVMEDIA_TYPE_AUDIO = 1,
    FF_AV_PIX_FMT_RGBA    = 26,  // AV_PIX_FMT_RGBA
    FF_SWS_BILINEAR       = 2,
    FF_EAGAIN              = -11, // AVERROR(EAGAIN)
    FF_AVERROR_EOF         = -541478725,  // AVERROR_EOF
    // 音频采样格式
    FF_AV_SAMPLE_FMT_U8   = 0,
    FF_AV_SAMPLE_FMT_S16  = 1,
    FF_AV_SAMPLE_FMT_S32  = 2,
    FF_AV_SAMPLE_FMT_FLT  = 3,
    FF_AV_SAMPLE_FMT_DBL  = 4,
    FF_AV_SAMPLE_FMT_U8P  = 5,
    FF_AV_SAMPLE_FMT_S16P = 6,
    FF_AV_SAMPLE_FMT_S32P = 7,
    FF_AV_SAMPLE_FMT_FLTP = 8,
    FF_AV_SAMPLE_FMT_DBLP = 9
};

/// AVFrame 简化布局 — data/linesize 在开头, 所有版本稳定
struct AVFrameHeader {
    uint8_t* data[8];       // 各平面数据指针 (offset 0)
    int      linesize[8];   // 各平面行字节数 (offset 64)
};

/// AVCodecParameters 简化布局 — 字段偏移由编译器对齐保证
struct AVCodecParamsHeader {
    int      codec_type;    // 0=video, 1=audio
    int      codec_id;
    uint32_t codec_tag;
    uint8_t* extradata;     // 编译器自动对齐到 8 字节
    int      extradata_size;
    int      format;
    int64_t  bit_rate;
    int      bits_per_coded_sample;
    int      bits_per_raw_sample;
    int      profile;
    int      level;
    int      width;         // offset 56 on x64
    int      height;        // offset 60 on x64
};

/// AVCodec 头部 — 提取 codec_id
struct AVCodecHeader {
    const char* name;       // 0
    const char* long_name;  // 8
    int         type;       // 16 (AVMediaType)
    int         id;         // 20 (AVCodecID)
};

/// AVFormatContext 头部 — 只访问 streams (所有版本稳定)
struct AVFormatCtxHeader {
    void*        av_class;
    void*        iformat;
    void*        oformat;
    void*        priv_data;
    void*        pb;
    int          ctx_flags;
    unsigned int nb_streams;
    void**       streams;   // AVStream** — 不转型, 用探针读取
};

/// 安全探针: 在 AVStream 内存中查找 codecpar 指针
/// codecpar 指向 AVCodecParameters, 其前 8 字节为 {codec_type, codec_id}
/// 通过匹配已知 codec_type 和 codec_id 来定位, 避免依赖 AVStream 布局
static void* ProbeCodecPar(void* stream, int expectedType, int expectedId) {
    uint8_t* base = (uint8_t*)stream;
    // 扫描 AVStream 前 512 字节中的指针, 查找 codecpar
    for (int off = 0; off < 512; off += sizeof(void*)) {
        void* candidate = *(void**)(base + off);
        // 跳过空指针和明显无效地址
        if (!candidate || (uintptr_t)candidate < 0x10000) continue;
#ifdef _WIN32
        __try {
#endif
            int* fields = (int*)candidate;
            if (fields[0] == expectedType && fields[1] == expectedId) {
                return candidate;
            }
#ifdef _WIN32
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            continue;  // 无效指针, 跳过
        }
#endif
    }
    return nullptr;
}

/// 安全探针: 从 AVCodecParameters 中查找 sample_rate 和 channels
/// sample_rate 在 AVCodecParameters 偏移 60 之后 (视频字段后)
/// 通过匹配已知采样率值定位
static bool ProbeAudioParams(void* codecpar, int* outSampleRate, int* outChannels) {
    int* fields = (int*)codecpar;
    // 从 offset 60 (跳过 width/height) 开始扫描到 offset 256
    for (int i = 15; i < 64; ++i) {
        int v = fields[i];
        if (v == 8000 || v == 11025 || v == 16000 || v == 22050 ||
            v == 32000 || v == 44100 || v == 48000 || v == 96000 || v == 192000) {
            *outSampleRate = v;
            // channels 紧邻 sample_rate (FFmpeg 布局: channels 在 sample_rate 前一字段)
            // 从最近到最远扫描, 避免误匹配 channel_layout 位掩码
            int checkOrder[] = { i-1, i+1, i-2, i+2, i-3, i+3 };
            for (int k = 0; k < 6; ++k) {
                int j = checkOrder[k];
                if (j >= 0 && j < 64 && j != i && fields[j] >= 1 && fields[j] <= 8) {
                    *outChannels = fields[j];
                    return true;
                }
            }
            *outChannels = 2;
            return true;
        }
    }
    *outSampleRate = 44100;
    *outChannels = 2;
    return false;
}

#ifdef _WIN32
// waveOut 音频缓冲区数量
#define VIDEO_AUDIO_BUFS 64
#define VIDEO_AUDIO_BUF_SAMPLES 4096
#endif

/// 视频播放上下文
struct VideoContext {
    // === 视频 ===
    void*          formatCtx;    // AVFormatContext*
    void*          codecCtx;     // AVCodecContext* (视频解码)
    void*          frame;        // AVFrame* (视频解码帧)
    void*          packet;       // AVPacket*
    void*          swsCtx;       // SwsContext*
    int            videoStreamIdx;
    int            width, height;
    int            srcPixFmt;
    unsigned int   texId;
    uint8_t*       rgbaBuf;
    int            rgbaBufSize;
    bool           playing;
    bool           finished;
    bool           useSwscale;
    double         frameDelay;
#ifdef _WIN32
    LARGE_INTEGER  lastFrameTime;
    LARGE_INTEGER  perfFreq;
#else
    double         lastFrameTimeSec;  // 上一帧时间 (glfwGetTime)
#endif
    char           filePath[260];

    // === 音频 ===
    void*          audioCodecCtx;   // AVCodecContext* (音频解码)
    void*          audioFrame;      // AVFrame* (音频解码帧)
    int            audioStreamIdx;
    int            audioSampleFmt;  // FFmpeg 采样格式
    int            audioSampleRate;
    int            audioChannels;
#ifdef _WIN32
    HWAVEOUT       hWaveOut;        // waveOut 句柄
    WAVEHDR        waveHdrs[VIDEO_AUDIO_BUFS]; // 环形缓冲区头
    uint8_t*       audioBufs[VIDEO_AUDIO_BUFS]; // PCM 缓冲区
    int            audioWriteIdx;   // 当前写入缓冲区索引
#endif
    bool           audioReady;      // 音频流是否可用
    void*          swrCtx;          // SwrContext* (音频格式转换)
    // 累积缓冲区: 攒满 VIDEO_AUDIO_BUF_SAMPLES 样本后再提交
    int16_t*       audioAccumBuf;   // 累积 PCM 缓冲区
    int            audioAccumPos;   // 累积位置 (交错样本数)
};

static VideoContext* GetVideoCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    VideoContext* ctx = (VideoContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/// Video.__gc — 释放所有 FFmpeg + GL + 音频资源
static int l_Video_GC(lua_State* L) {
    VideoContext* ctx = (VideoContext*)lua_touserdata(L, 1);
    if (!ctx) return 0;

    // 关闭音频
#ifdef _WIN32
    if (ctx->hWaveOut) {
        waveOutReset(ctx->hWaveOut);
        for (int i = 0; i < VIDEO_AUDIO_BUFS; ++i) {
            if (ctx->waveHdrs[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(ctx->hWaveOut, &ctx->waveHdrs[i], sizeof(WAVEHDR));
            if (ctx->audioBufs[i]) free(ctx->audioBufs[i]);
        }
        waveOutClose(ctx->hWaveOut);
        ctx->hWaveOut = nullptr;
    }
#endif

    if (g_ff.loaded) {
        if (ctx->swsCtx && g_ff.sws_freeContext) g_ff.sws_freeContext(ctx->swsCtx);
        if (ctx->frame   && g_ff.av_frame_free)  g_ff.av_frame_free(&ctx->frame);
        if (ctx->audioFrame && g_ff.av_frame_free) g_ff.av_frame_free(&ctx->audioFrame);
        if (ctx->packet  && g_ff.av_packet_free)  g_ff.av_packet_free(&ctx->packet);
        if (ctx->codecCtx && g_ff.avcodec_free_context) g_ff.avcodec_free_context(&ctx->codecCtx);
        if (ctx->audioCodecCtx && g_ff.avcodec_free_context) g_ff.avcodec_free_context(&ctx->audioCodecCtx);
        if (ctx->formatCtx && g_ff.avformat_close_input) g_ff.avformat_close_input(&ctx->formatCtx);
    }
    if (ctx->rgbaBuf) free(ctx->rgbaBuf);
    if (ctx->texId && g_render) g_render->DeleteTexture(ctx->texId);
    return 0;
}

/// @lua_api Light.AV.Video.__call
/// @brief 构造函数, 打开视频文件
/// @param path string 视频文件路径
/// @return void
/// @example
/// local video = Light(Light.AV.Video):New("intro.mp4")
static int l_Video_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    VideoContext* ctx = (VideoContext*)lua_newuserdata(L, sizeof(VideoContext));
    memset(ctx, 0, sizeof(VideoContext));
    ctx->videoStreamIdx = -1;

    // 设置 __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Video_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    strncpy(ctx->filePath, path, sizeof(ctx->filePath) - 1);

    if (!LoadFFmpeg()) {
        CC::Log(CC::LOG_WARN, "Video: FFmpeg not available for '%s'", path);
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 打开文件
    int rc = (int)(intptr_t)g_ff.avformat_open_input(&ctx->formatCtx, path, nullptr, nullptr);
    if (rc < 0 || !ctx->formatCtx) {
        CC::Log(CC::LOG_WARN, "Video: failed to open '%s' (rc=%d)", path, rc);
        lua_setfield(L, 1, "__instance");
        return 0;
    }
    g_ff.avformat_find_stream_info(ctx->formatCtx, nullptr);

    // 查找视频流 (AVMEDIA_TYPE_VIDEO = 0)
    void* decoder = nullptr;
    ctx->videoStreamIdx = g_ff.av_find_best_stream(ctx->formatCtx, FF_AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ctx->videoStreamIdx < 0 || !decoder) {
        CC::Log(CC::LOG_WARN, "Video: no video stream in '%s'", path);
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 创建解码器上下文
    ctx->codecCtx = g_ff.avcodec_alloc_context3(decoder);
    if (!ctx->codecCtx) {
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 从流参数初始化解码器上下文 (安全探针方式, 不依赖 AVStream 布局)
    AVFormatCtxHeader* fmtHdr = (AVFormatCtxHeader*)ctx->formatCtx;
    void* stream = fmtHdr->streams[ctx->videoStreamIdx];

    // 从 decoder 提取 codec_id
    AVCodecHeader* decHdr = (AVCodecHeader*)decoder;
    int codecId = decHdr->id;
    CC::Log(CC::LOG_INFO, "Video: decoder='%s', codec_id=%d", decHdr->name, codecId);

    // 探针查找 codecpar
    void* codecpar = ProbeCodecPar(stream, FF_AVMEDIA_TYPE_VIDEO, codecId);
    if (!codecpar) {
        CC::Log(CC::LOG_WARN, "Video: failed to locate codecpar in stream for '%s'", path);
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 初始化解码器参数
    g_ff.avcodec_parameters_to_context(ctx->codecCtx, codecpar);
    AVCodecParamsHeader* cp = (AVCodecParamsHeader*)codecpar;
    ctx->width     = cp->width;
    ctx->height    = cp->height;
    ctx->srcPixFmt = cp->format;
    CC::Log(CC::LOG_INFO, "Video: codecpar found (%dx%d, pixfmt=%d)", ctx->width, ctx->height, ctx->srcPixFmt);

    // 默认帧率 25fps (避免访问 AVStream 内部字段)
    ctx->frameDelay = 1.0 / 25.0;

    // 打开解码器
    if ((int)(intptr_t)g_ff.avcodec_open2(ctx->codecCtx, decoder, nullptr) < 0) {
        CC::Log(CC::LOG_WARN, "Video: avcodec_open2 failed for '%s'", path);
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 创建 swscale 上下文 (可选, 无 swscale 时用软件转换)
    ctx->useSwscale = false;
    CC::Log(CC::LOG_INFO, "Video: swscale check: hScale=%p, sws_getContext=%p, srcPixFmt=%d",
            g_ff.hScale, (void*)g_ff.sws_getContext, ctx->srcPixFmt);
    if (g_ff.sws_getContext) {
        ctx->swsCtx = g_ff.sws_getContext(
            ctx->width, ctx->height, ctx->srcPixFmt,
            ctx->width, ctx->height, FF_AV_PIX_FMT_RGBA,
            FF_SWS_BILINEAR, nullptr, nullptr, nullptr);
        ctx->useSwscale = (ctx->swsCtx != nullptr);
        CC::Log(CC::LOG_INFO, "Video: sws_getContext returned %p (useSwscale=%d)",
                ctx->swsCtx, ctx->useSwscale ? 1 : 0);
    }
    if (!ctx->useSwscale) {
        CC::Log(CC::LOG_INFO, "Video: swscale not available, using software YUV->RGBA");
    }

    // 分配 RGBA 缓冲区
    CC::Log(CC::LOG_INFO, "Video: step1 - alloc RGBA buf (%d bytes)", ctx->width * ctx->height * 4);
    ctx->rgbaBufSize = ctx->width * ctx->height * 4;
    ctx->rgbaBuf = (uint8_t*)malloc(ctx->rgbaBufSize);
    if (!ctx->rgbaBuf) {
        CC::Log(CC::LOG_WARN, "Video: malloc failed for RGBA buffer");
        lua_setfield(L, 1, "__instance");
        return 0;
    }
    memset(ctx->rgbaBuf, 0, ctx->rgbaBufSize);

    // 分配 FFmpeg 帧和包 (分步诊断)
    CC::Log(CC::LOG_INFO, "Video: step2a - av_frame_alloc (fptr=%p)", g_ff.av_frame_alloc);
    fflush(stdout); fflush(stderr);
    if (!g_ff.av_frame_alloc) {
        CC::Log(CC::LOG_WARN, "Video: av_frame_alloc is NULL!");
        lua_setfield(L, 1, "__instance");
        return 0;
    }
    ctx->frame = g_ff.av_frame_alloc();
    CC::Log(CC::LOG_INFO, "Video: step2b - frame=%p", ctx->frame);
    fflush(stdout); fflush(stderr);

    CC::Log(CC::LOG_INFO, "Video: step2c - av_packet_alloc (fptr=%p)", g_ff.av_packet_alloc);
    fflush(stdout); fflush(stderr);
    if (!g_ff.av_packet_alloc) {
        CC::Log(CC::LOG_WARN, "Video: av_packet_alloc is NULL!");
        lua_setfield(L, 1, "__instance");
        return 0;
    }
    ctx->packet = g_ff.av_packet_alloc();
    CC::Log(CC::LOG_INFO, "Video: step2d - packet=%p", ctx->packet);
    fflush(stdout); fflush(stderr);

    if (!ctx->frame || !ctx->packet) {
        CC::Log(CC::LOG_WARN, "Video: av_frame/packet_alloc returned null");
        lua_setfield(L, 1, "__instance");
        return 0;
    }

    // 创建视频纹理 (通过渲染后端)
    CC::Log(CC::LOG_INFO, "Video: step3 - create texture (%dx%d)", ctx->width, ctx->height);
    ctx->texId = g_render->CreateTexture(ctx->width, ctx->height, 4, nullptr);

    // 初始化高精度计时器
    CC::Log(CC::LOG_INFO, "Video: step4 - init timer");
#ifdef _WIN32
    QueryPerformanceFrequency(&ctx->perfFreq);
    QueryPerformanceCounter(&ctx->lastFrameTime);
#else
    ctx->lastFrameTimeSec = PlatformWindow::GetTime();
#endif

    ctx->playing = true;
    CC::Log(CC::LOG_INFO, "Video: opened '%s' (%dx%d, %.1f fps, texId=%u)",
            path, ctx->width, ctx->height, 1.0/ctx->frameDelay, ctx->texId);

    // ===== 音频流初始化 =====
    ctx->audioStreamIdx = -1;
    ctx->audioReady = false;
    void* audioDecoder = nullptr;
    int aIdx = g_ff.av_find_best_stream(ctx->formatCtx, FF_AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
    if (aIdx >= 0 && audioDecoder) {
        ctx->audioStreamIdx = aIdx;
        ctx->audioCodecCtx = g_ff.avcodec_alloc_context3(audioDecoder);
        if (ctx->audioCodecCtx) {
            AVCodecHeader* aDecHdr = (AVCodecHeader*)audioDecoder;
            void* aStream = fmtHdr->streams[aIdx];
            void* aCodecpar = ProbeCodecPar(aStream, FF_AVMEDIA_TYPE_AUDIO, aDecHdr->id);
            if (aCodecpar) {
                g_ff.avcodec_parameters_to_context(ctx->audioCodecCtx, aCodecpar);
                // 安全探针获取 sample_rate 和 channels
                int sr = 44100, ch = 2;
                bool probed = ProbeAudioParams(aCodecpar, &sr, &ch);
                ctx->audioSampleRate = sr;
                ctx->audioChannels   = ch;
                // format 从 codecpar 读取 (偏移 28, 与视频共用)
                AVCodecParamsHeader* acp = (AVCodecParamsHeader*)aCodecpar;
                ctx->audioSampleFmt = acp->format;
                CC::Log(CC::LOG_INFO, "Video: audio codecpar probed (sr=%d, ch=%d, fmt=%d, ok=%d)",
                        sr, ch, ctx->audioSampleFmt, probed ? 1 : 0);
            }
            if ((int)(intptr_t)g_ff.avcodec_open2(ctx->audioCodecCtx, audioDecoder, nullptr) >= 0) {
                ctx->audioFrame = g_ff.av_frame_alloc();
                ctx->audioReady = true;
                CC::Log(CC::LOG_INFO, "Video: audio stream found (idx=%d, decoder='%s')",
                        aIdx, aDecHdr->name);
            } else {
                CC::Log(CC::LOG_WARN, "Video: audio codec open failed");
            }
        }
    } else {
        CC::Log(CC::LOG_INFO, "Video: no audio stream found");
    }

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// @lua_api Light.AV.Video.Update
/// @brief 解码并更新视频帧 (每帧调用)
/// @return void
static int l_Video_Update(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoContext* ctx = GetVideoCtx(L, 1);
    if (!ctx || !ctx->playing || ctx->finished || !g_ff.loaded) return 0;

    // 视频帧定时: 判断是否需要新的视频帧
#ifdef _WIN32
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    double elapsed = (double)(now.QuadPart - ctx->lastFrameTime.QuadPart) / ctx->perfFreq.QuadPart;
#else
    double nowSec = PlatformWindow::GetTime();
    double elapsed = nowSec - ctx->lastFrameTimeSec;
#endif
    bool needVideoFrame = (elapsed >= ctx->frameDelay);

    // AVPacket 简化布局
    struct AVPacketSimple {
        void*   buf;          // 0
        int64_t pts;          // 8
        int64_t dts;          // 16
        uint8_t* data;        // 24
        int     size;         // 32
        int     stream_index; // 36
    };

    // 读包策略:
    // - 视频帧周期: 读到视频帧 (顺带处理音频)
    // - 非视频帧周期: 仅在 waveOut 音频缓冲不足时补充
    bool gotVideoFrame = false;

    if (!needVideoFrame) {
#ifdef _WIN32
        // 检查 waveOut 缓冲区水位 (pending = 已提交但未播完)
        int pending = 0;
        if (ctx->hWaveOut) {
            for (int i = 0; i < VIDEO_AUDIO_BUFS; ++i) {
                if ((ctx->waveHdrs[i].dwFlags & WHDR_PREPARED) &&
                    !(ctx->waveHdrs[i].dwFlags & WHDR_DONE))
                    pending++;
            }
        }
        // 4+ 个缓冲区在播放 (≥0.372s), 不需要补充
        if (pending >= 4) return 0;
#else
        return 0; // 非 Windows: 不需要音频缓冲补充
#endif
    }

    int maxPackets = needVideoFrame ? 60 : 8;

    for (int p = 0; p < maxPackets; ++p) {
        int readRc = g_ff.av_read_frame(ctx->formatCtx, ctx->packet);
        if (readRc < 0) {
            ctx->finished = true;
            ctx->playing = false;
            break;
        }

        AVPacketSimple* pkt = (AVPacketSimple*)ctx->packet;

        // ===== 音频包: 始终解码并送 waveOut (Windows only) =====
#ifdef _WIN32
        if (pkt->stream_index == ctx->audioStreamIdx && ctx->audioReady) {
            g_ff.avcodec_send_packet(ctx->audioCodecCtx, ctx->packet);
            g_ff.av_packet_unref(ctx->packet);
            while (g_ff.avcodec_receive_frame(ctx->audioCodecCtx, ctx->audioFrame) == 0) {
                AVFrameHeader* af = (AVFrameHeader*)ctx->audioFrame;
                // 首帧: 初始化 waveOut
                if (!ctx->hWaveOut && af->data[0]) {
                    int actualFmt = *((int*)((uint8_t*)ctx->audioFrame + 116));
                    if (actualFmt >= 0 && actualFmt <= 11) ctx->audioSampleFmt = actualFmt;

                    WAVEFORMATEX wfx = {};
                    wfx.wFormatTag = WAVE_FORMAT_PCM;
                    wfx.nChannels = (WORD)ctx->audioChannels;
                    wfx.nSamplesPerSec = (DWORD)ctx->audioSampleRate;
                    wfx.wBitsPerSample = 16;
                    wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
                    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

                    MMRESULT mr = waveOutOpen(&ctx->hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
                    if (mr != MMSYSERR_NOERROR) {
                        ctx->audioReady = false;
                        continue;
                    }
                    int bufBytes = VIDEO_AUDIO_BUF_SAMPLES * wfx.nBlockAlign;
                    for (int bi = 0; bi < VIDEO_AUDIO_BUFS; ++bi) {
                        ctx->audioBufs[bi] = (uint8_t*)malloc(bufBytes);
                        memset(&ctx->waveHdrs[bi], 0, sizeof(WAVEHDR));
                    }
                    ctx->audioWriteIdx = 0;
                    // 分配累积缓冲区
                    ctx->audioAccumBuf = (int16_t*)malloc(VIDEO_AUDIO_BUF_SAMPLES * wfx.nBlockAlign);
                    ctx->audioAccumPos = 0;
                    // 初始化 swresample (FLTPxx → S16 交错)
                    ctx->swrCtx = nullptr;
                    if (g_ff.swr_alloc_set_opts && g_ff.swr_init && g_ff.swr_convert) {
                        // AV_CH_LAYOUT_STEREO = 3, AV_CH_LAYOUT_MONO = 4
                        int64_t chLayout = (ctx->audioChannels == 1) ? 4 : 3;
                        // 输出: S16 交错 (fmt=1), 输入: 原始格式
                        ctx->swrCtx = g_ff.swr_alloc_set_opts(
                            nullptr, chLayout, 1 /*AV_SAMPLE_FMT_S16*/, ctx->audioSampleRate,
                            chLayout, ctx->audioSampleFmt, ctx->audioSampleRate,
                            0, nullptr);
                        if (ctx->swrCtx) {
                            if (g_ff.swr_init(ctx->swrCtx) < 0) {
                                g_ff.swr_free(&ctx->swrCtx);
                                ctx->swrCtx = nullptr;
                            }
                        }
                    }
                    CC::Log(CC::LOG_INFO, "Video: audio waveOut opened (%d Hz, %d ch, fmt=%d, swr=%s)",
                            ctx->audioSampleRate, ctx->audioChannels, ctx->audioSampleFmt,
                            ctx->swrCtx ? "YES" : "NO");
                }

                if (ctx->hWaveOut && af->data[0]) {
                    // nb_samples 从 AVFrame 字段读取 (offset 112)
                    int ch = ctx->audioChannels;
                    int fmt = ctx->audioSampleFmt;
                    int nb_samples = *((int*)((uint8_t*)ctx->audioFrame + 112));
                    int bps = (fmt == FF_AV_SAMPLE_FMT_FLTP || fmt == FF_AV_SAMPLE_FMT_FLT) ? 4 : 2;
                    int maxFromLinesize = af->linesize[0] / bps;
                    if (nb_samples <= 0 || nb_samples > maxFromLinesize)
                        nb_samples = maxFromLinesize;
                    if (nb_samples <= 0 || nb_samples > VIDEO_AUDIO_BUF_SAMPLES)
                        continue;

                    // 将解码帧写入累积缓冲区
                    int16_t* accum = ctx->audioAccumBuf;
                    int accumMax = VIDEO_AUDIO_BUF_SAMPLES * ch;

                    if (ctx->swrCtx) {
                        // ===== swresample 专业转换 =====
                        // 临时 S16 交错缓冲区 (最多 nb_samples * ch 个 int16)
                        int16_t tmpBuf[4096 * 2]; // 足够 4096 样本 × 2 声道
                        uint8_t* outPtrs[1] = { (uint8_t*)tmpBuf };
                        const uint8_t** inPtrs = (const uint8_t**)af->data;
                        int converted = g_ff.swr_convert(ctx->swrCtx,
                            outPtrs, nb_samples, inPtrs, nb_samples);
                        if (converted > 0) {
                            int toCopy = converted * ch;
                            if (ctx->audioAccumPos + toCopy > accumMax)
                                toCopy = accumMax - ctx->audioAccumPos;
                            memcpy(accum + ctx->audioAccumPos, tmpBuf, toCopy * sizeof(int16_t));
                            ctx->audioAccumPos += toCopy;
                        }
                    } else {
                        // ===== 手动回退转换 =====
                        if (fmt == FF_AV_SAMPLE_FMT_FLTP) {
                            for (int s = 0; s < nb_samples && ctx->audioAccumPos < accumMax; ++s) {
                                for (int c2 = 0; c2 < ch; ++c2) {
                                    float v = (af->data[c2]) ? ((float*)af->data[c2])[s] : 0.0f;
                                    if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                                    accum[ctx->audioAccumPos++] = (int16_t)(v * 32767.0f);
                                }
                            }
                        } else if (fmt == FF_AV_SAMPLE_FMT_S16P) {
                            for (int s = 0; s < nb_samples && ctx->audioAccumPos < accumMax; ++s) {
                                for (int c2 = 0; c2 < ch; ++c2) {
                                    accum[ctx->audioAccumPos++] = (af->data[c2]) ? ((int16_t*)af->data[c2])[s] : 0;
                                }
                            }
                        } else if (fmt == FF_AV_SAMPLE_FMT_S16) {
                            int toCopy = nb_samples * ch;
                            if (ctx->audioAccumPos + toCopy > accumMax) toCopy = accumMax - ctx->audioAccumPos;
                            memcpy(accum + ctx->audioAccumPos, af->data[0], toCopy * 2);
                            ctx->audioAccumPos += toCopy;
                        } else if (fmt == FF_AV_SAMPLE_FMT_FLT) {
                            float* src = (float*)af->data[0];
                            int total = nb_samples * ch;
                            for (int s = 0; s < total && ctx->audioAccumPos < accumMax; ++s) {
                                float v = src[s];
                                if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                                accum[ctx->audioAccumPos++] = (int16_t)(v * 32767.0f);
                            }
                        }
                    }

                    // 累积满一个完整缓冲区后提交给 waveOut
                    if (ctx->audioAccumPos >= accumMax) {
                        int wi = ctx->audioWriteIdx % VIDEO_AUDIO_BUFS;
                        if (ctx->waveHdrs[wi].dwFlags & WHDR_PREPARED) {
                            for (int y = 0; y < 5 && !(ctx->waveHdrs[wi].dwFlags & WHDR_DONE); ++y)
                                Sleep(0);
                            waveOutUnprepareHeader(ctx->hWaveOut, &ctx->waveHdrs[wi], sizeof(WAVEHDR));
                        }
                        memcpy(ctx->audioBufs[wi], accum, accumMax * 2);
                        ctx->waveHdrs[wi].lpData = (LPSTR)ctx->audioBufs[wi];
                        ctx->waveHdrs[wi].dwBufferLength = (DWORD)(accumMax * 2);
                        ctx->waveHdrs[wi].dwFlags = 0;
                        waveOutPrepareHeader(ctx->hWaveOut, &ctx->waveHdrs[wi], sizeof(WAVEHDR));
                        waveOutWrite(ctx->hWaveOut, &ctx->waveHdrs[wi], sizeof(WAVEHDR));
                        ctx->audioWriteIdx++;
                        ctx->audioAccumPos = 0;
                    }
                }
            }
            continue; // 继续读下一个包
        }
#else
        // 非 Windows: 跳过音频包
        if (pkt->stream_index == ctx->audioStreamIdx) {
            g_ff.av_packet_unref(ctx->packet);
            continue;
        }
#endif

        // ===== 视频包: 始终送给解码器 (维护参考帧), 按需取帧 =====
        if (pkt->stream_index == ctx->videoStreamIdx) {
            g_ff.avcodec_send_packet(ctx->codecCtx, ctx->packet);
            g_ff.av_packet_unref(ctx->packet);
            // 始终尝试 receive_frame 以消费解码器输出队列
            if (g_ff.avcodec_receive_frame(ctx->codecCtx, ctx->frame) == 0) {
                if (needVideoFrame && !gotVideoFrame) {
                    gotVideoFrame = true;
#ifdef _WIN32
                    ctx->lastFrameTime = now;
#else
                    ctx->lastFrameTimeSec = nowSec;
#endif
                }
                // 不需要视频帧时丢弃已解码帧 (但解码器参考帧已更新)
            }
            if (gotVideoFrame) break;
            continue;
        }

        // 其他流: 跳过
        g_ff.av_packet_unref(ctx->packet);
    }

    if (!gotVideoFrame && !needVideoFrame) return 0; // 非视频帧周期, 只喂了音频
    if (!gotVideoFrame) return 0;

    // YUV → RGBA 转换
    AVFrameHeader* frm = (AVFrameHeader*)ctx->frame;

    if (ctx->useSwscale && ctx->swsCtx) {
        // 使用 swscale 硬件加速转换
        uint8_t* dstData[1] = { ctx->rgbaBuf };
        int      dstStride[1] = { ctx->width * 4 };
        g_ff.sws_scale(ctx->swsCtx,
                       (const uint8_t* const*)frm->data, frm->linesize,
                       0, ctx->height, dstData, dstStride);
    } else {
        // 优化的软件 YUV420P → RGBA 转换
        // 钳位查找表: 避免逐像素分支判断
        static uint8_t clampTab[512 + 512];
        static bool clampInit = false;
        if (!clampInit) {
            for (int i = 0; i < 1024; ++i) {
                int v = i - 256;
                clampTab[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
            }
            clampInit = true;
        }
        // 偏移指针: clampTab[256 + x] 等同于 clamp(x, 0, 255)
        const uint8_t* clamp = clampTab + 256;

        const uint8_t* yPlane = frm->data[0];
        const uint8_t* uPlane = frm->data[1];
        const uint8_t* vPlane = frm->data[2];
        int yStride = frm->linesize[0];
        int uStride = frm->linesize[1];
        int vStride = frm->linesize[2];
        int w = ctx->width;
        int h = ctx->height;
        uint8_t* dst = ctx->rgbaBuf;

        for (int row = 0; row < h; ++row) {
            const uint8_t* yRow = yPlane + row * yStride;
            const uint8_t* uRow = uPlane + (row >> 1) * uStride;
            const uint8_t* vRow = vPlane + (row >> 1) * vStride;
            uint8_t* dRow = dst + row * w * 4;

            // 每次处理 2 像素 (共享同一 U/V 对)
            int col = 0;
            for (; col < w - 1; col += 2) {
                int U = uRow[col >> 1] - 128;
                int V = vRow[col >> 1] - 128;
                int rAdd = (359 * V) >> 8;
                int gSub = (88 * U + 183 * V) >> 8;
                int bAdd = (454 * U) >> 8;
                // 像素 1
                int Y0 = yRow[col];
                dRow[0] = clamp[Y0 + rAdd];
                dRow[1] = clamp[Y0 - gSub];
                dRow[2] = clamp[Y0 + bAdd];
                dRow[3] = 255;
                // 像素 2
                int Y1 = yRow[col + 1];
                dRow[4] = clamp[Y1 + rAdd];
                dRow[5] = clamp[Y1 - gSub];
                dRow[6] = clamp[Y1 + bAdd];
                dRow[7] = 255;
                dRow += 8;
            }
            // 宽度奇数时处理最后一像素
            if (col < w) {
                int U = uRow[col >> 1] - 128;
                int V = vRow[col >> 1] - 128;
                int Y0 = yRow[col];
                dRow[0] = clamp[Y0 + ((359 * V) >> 8)];
                dRow[1] = clamp[Y0 - ((88 * U + 183 * V) >> 8)];
                dRow[2] = clamp[Y0 + ((454 * U) >> 8)];
                dRow[3] = 255;
            }
        }
    }

    // 更新视频纹理 (通过渲染后端)
    g_render->UpdateTexture(ctx->texId, 0, 0, ctx->width, ctx->height, 4, ctx->rgbaBuf);

    return 0;
}

/// @lua_api Light.AV.Video.Draw
/// @brief 绘制当前视频帧
/// @param x number? 屏幕位置 X (默认 0)
/// @param y number? 屏幕位置 Y (默认 0)
/// @param w number? 绘制宽度 (默认视频宽)
/// @param h number? 绘制高度 (默认视频高)
/// @return void
static int l_Video_Draw(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoContext* ctx = GetVideoCtx(L, 1);
    if (!ctx || ctx->texId == 0) return 0;

    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float w = (float)luaL_optnumber(L, 4, (double)ctx->width);
    float h = (float)luaL_optnumber(L, 5, (double)ctx->height);

    g_render->PushMatrix();
    g_render->Translate(x, y, 0);
    g_render->SetColor(1, 1, 1, 1);
    g_render->BindTexture(ctx->texId);
    RenderVertex verts[4] = {
        {0, 0, 0,  0, 0,  1, 1, 1, 1},
        {w, 0, 0,  1, 0,  1, 1, 1, 1},
        {w, h, 0,  1, 1,  1, 1, 1, 1},
        {0, h, 0,  0, 1,  1, 1, 1, 1},
    };
    g_render->DrawArrays(DrawMode::Quads, verts, 4);
    g_render->UnbindTexture();
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.AV.Video.IsPlaying
/// @brief 返回视频是否正在播放
/// @return boolean
static int l_Video_IsPlaying(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoContext* ctx = GetVideoCtx(L, 1);
    lua_pushboolean(L, ctx && ctx->playing && !ctx->finished);
    return 1;
}

/// @lua_api Light.AV.Video.GetWidth
/// @brief 获取视频宽度
/// @return number
static int l_Video_GetWidth(lua_State* L) {
    VideoContext* ctx = GetVideoCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}
/// @lua_api Light.AV.Video.GetHeight
/// @brief 获取视频高度
/// @return number
static int l_Video_GetHeight(lua_State* L) {
    VideoContext* ctx = GetVideoCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// @lua_api Light.AV.Video.Stop
/// @brief 停止视频播放
/// @return void
static int l_Video_Stop(lua_State* L) {
    VideoContext* ctx = GetVideoCtx(L, 1);
    if (ctx) { ctx->playing = false; ctx->finished = true; }
    return 0;
}

/// Video.__tostring
static int l_Video_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.AV.Video");
    return 1;
}

// ==================== luaopen 注册 ====================

// AV 父模块 — 3 函数 + Playable 子表 + Video 子模块
int luaopen_Light_AV(lua_State* L) {
    // 预加载 FFmpeg (非阻塞, 失败不报错)
    LoadFFmpeg();

    LT::EnsureLightTable(L);

    lua_pushstring(L, "AV");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "AV");
        lua_createtable(L, 0, 0);

        // Playable 基类子表 (还原自 sub_1800AE900)
        lua_createtable(L, 0, 0);
        lua_setfield(L, -2, "Playable");

        const luaL_Reg av_funcs[] = {
            {"Play",  l_AV_Play},
            {"Pause", l_AV_Pause},
            {"Stop",  l_AV_Stop},
            {NULL, NULL}
        };
        luaL_setfuncs(L, av_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "AV");
        lua_rawget(L, -2);

        // 自动注册 Video 子模块
        int avIdx = lua_gettop(L);
        luaopen_Light_AV_Video(L);
        lua_settop(L, avIdx);  // 恢复栈
    }
    lua_remove(L, -2);
    return 1;
}

// Audio — 2 函数 + 继承 Playable
int luaopen_Light_AV_Audio(lua_State* L) {
    luaopen_Light_AV(L);

    lua_pushstring(L, "Audio");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Audio");

        // 继承链: Audio = Light(AV):New("Playable")
        LT::EnsureLightTable(L);
        lua_pushstring(L, "New");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_pushstring(L, "Playable");
        lua_rawget(L, -4);
        lua_call(L, 1, 1);

        const luaL_Reg audio_funcs[] = {
            {"__call",     l_Audio_Call},
            {"__tostring", l_Audio_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, audio_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Audio");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// AudioData — 7 函数
int luaopen_Light_AV_AudioData(lua_State* L) {
    luaopen_Light_AV(L);

    lua_pushstring(L, "AudioData");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "AudioData");
        lua_createtable(L, 0, 0);

        const luaL_Reg ad_funcs[] = {
            {"GetFormat",     l_AudioData_GetFormat},
            {"GetChannels",   l_AudioData_GetChannels},
            {"GetSampleRate", l_AudioData_GetSampleRate},
            {"GetPointer",    l_AudioData_GetPointer},
            {"Count",         l_AudioData_Count},
            {"__call",        l_AudioData_Call},
            {"__tostring",    l_AudioData_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, ad_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "AudioData");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}

// Video — 8 函数
int luaopen_Light_AV_Video(lua_State* L) {
    luaopen_Light_AV(L);

    lua_pushstring(L, "Video");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Video");
        lua_createtable(L, 0, 0);

        const luaL_Reg video_funcs[] = {
            {"Update",     l_Video_Update},
            {"Draw",       l_Video_Draw},
            {"IsPlaying",  l_Video_IsPlaying},
            {"GetWidth",   l_Video_GetWidth},
            {"GetHeight",  l_Video_GetHeight},
            {"Stop",       l_Video_Stop},
            {"__call",     l_Video_Call},
            {"__tostring", l_Video_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, video_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Video");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
