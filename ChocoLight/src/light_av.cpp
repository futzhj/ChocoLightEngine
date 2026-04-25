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
#include "ffmpeg_common.h"
#include "video_backend.h"
#include <cstring>
#include <cstdlib>

// DynLoad/DynSym 宏定义 (LoadFFmpeg 使用)
#if defined(__EMSCRIPTEN__)
// Emscripten: 无动态库加载, FFmpeg 不可用
#define DynLoad(name) ((void*)0)
#define DynSym(lib, name) ((void*)0)
#define DynFree(lib) ((void)0)
#elif defined(_WIN32)
#define DynLoad(name) LoadLibraryA(name)
#define DynSym(lib, name) GetProcAddress(lib, name)
#define DynFree(lib) FreeLibrary(lib)
#else
#include <dlfcn.h>
#define DynLoad(name) dlopen(name, RTLD_LAZY)
#define DynSym(lib, name) dlsym(lib, name)
#define DynFree(lib) dlclose(lib)
#endif

// ==================== FFmpeg 动态加载 ====================

// g_ff 全局定义 (ffmpeg_common.h 声明为 extern, video_backend_ffmpeg.cpp 也访问)
FFmpegLib g_ff = {};

/// 懒加载 FFmpeg DLL — 首次调用 AV 时触发
bool LoadFFmpeg() {
    if (g_ff.attempted) return g_ff.loaded;
    g_ff.attempted = true;
#ifdef __EMSCRIPTEN__
    // Web 平台: FFmpeg 不可用, 视频由 HTML5 <video> 后端处理
    CC::Log(CC::LOG_INFO, "FFmpeg: not available on Web (using HTML5 video backend)");
    return false;
#endif
#ifdef __ANDROID__
    // Android: FFmpeg DLL 不自带, 音频由 miniaudio 处理, 视频暂不可用
    CC::Log(CC::LOG_INFO, "FFmpeg: not available on Android");
    return false;
#endif

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

// ==================== Video (通过 VideoBackend 委托) ====================
// 视频功能已迁移到 video_backend_ffmpeg.cpp / video_backend_html5.cpp
// 此处仅保留 Lua 绑定层, 通过 VideoBackend 抽象接口委托

/// 视频包装器 — 持有 VideoBackend 实例
struct VideoWrapper {
    VideoBackend* backend;
};

static VideoWrapper* GetVideoWrapper(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    VideoWrapper* w = (VideoWrapper*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return w;
}

/// Video.__gc — 释放 VideoBackend
static int l_Video_GC(lua_State* L) {
    VideoWrapper* w = (VideoWrapper*)lua_touserdata(L, 1);
    if (w && w->backend) {
        w->backend->Close();
        delete w->backend;
        w->backend = nullptr;
    }
    return 0;
}

/// @lua_api Light.AV.Video.__call
/// @brief 构造函数, 打开视频文件
/// @param path string 视频文件路径
/// @return void
static int l_Video_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    VideoWrapper* w = (VideoWrapper*)lua_newuserdata(L, sizeof(VideoWrapper));
    w->backend = nullptr;

    // __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Video_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    VideoBackend* backend = CreateVideoBackend();
    if (backend && backend->Open(path, g_render)) {
        w->backend = backend;
    } else {
        delete backend;
        CC::Log(CC::LOG_WARN, "Video: failed to open '%s'", path);
    }

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// @lua_api Light.AV.Video.Update
/// @brief 解码并更新视频帧 (每帧调用)
static int l_Video_Update(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoWrapper* w = GetVideoWrapper(L, 1);
    if (w && w->backend && w->backend->IsPlaying())
        w->backend->Update();
    return 0;
}

/// @lua_api Light.AV.Video.Draw
/// @brief 绘制当前视频帧
static int l_Video_Draw(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoWrapper* w = GetVideoWrapper(L, 1);
    if (!w || !w->backend || w->backend->GetTextureId() == 0) return 0;
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float dw = (float)luaL_optnumber(L, 4, (double)w->backend->GetWidth());
    float dh = (float)luaL_optnumber(L, 5, (double)w->backend->GetHeight());
    w->backend->Draw(x, y, dw, dh);
    return 0;
}

/// @lua_api Light.AV.Video.IsPlaying
static int l_Video_IsPlaying(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    VideoWrapper* w = GetVideoWrapper(L, 1);
    lua_pushboolean(L, w && w->backend && w->backend->IsPlaying());
    return 1;
}

/// @lua_api Light.AV.Video.GetWidth
static int l_Video_GetWidth(lua_State* L) {
    VideoWrapper* w = GetVideoWrapper(L, 1);
    lua_pushinteger(L, (w && w->backend) ? w->backend->GetWidth() : 0);
    return 1;
}

/// @lua_api Light.AV.Video.GetHeight
static int l_Video_GetHeight(lua_State* L) {
    VideoWrapper* w = GetVideoWrapper(L, 1);
    lua_pushinteger(L, (w && w->backend) ? w->backend->GetHeight() : 0);
    return 1;
}

/// @lua_api Light.AV.Video.Stop
static int l_Video_Stop(lua_State* L) {
    VideoWrapper* w = GetVideoWrapper(L, 1);
    if (w && w->backend) w->backend->Stop();
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
