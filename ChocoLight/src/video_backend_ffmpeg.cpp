/**
 * @file video_backend_ffmpeg.cpp
 * @brief VideoBackend FFmpeg 实现 — 桌面平台 (Win/Linux/macOS)
 * @note 从 light_av.cpp 视频部分抽取, 通过 extern g_ff 共享 FFmpeg 加载器
 *       仅桌面平台编译, Web/Android/iOS 使用各自原生后端
 */

// 桌面平台编译守卫
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)

#include "video_backend.h"
#include "ffmpeg_common.h"
#include "render_backend.h"
#include "platform_window.h"
#include "light.h"

#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#endif

// windows.h 宏污染清理
#ifdef CreateWindow
#undef CreateWindow
#endif

// ==================== FFmpeg 内部常量 ====================

enum {
    FF_AVMEDIA_TYPE_VIDEO = 0,
    FF_AVMEDIA_TYPE_AUDIO = 1,
    FF_AV_PIX_FMT_RGBA    = 26,
    FF_SWS_BILINEAR       = 2,
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

// ==================== FFmpeg ABI 简化结构体 ====================

/// AVFrame 头部 — data/linesize 在开头, 所有版本稳定
struct AVFrameHeader {
    uint8_t* data[8];
    int      linesize[8];
};

/// AVCodecParameters 简化布局
struct AVCodecParamsHeader {
    int      codec_type;
    int      codec_id;
    uint32_t codec_tag;
    uint8_t* extradata;
    int      extradata_size;
    int      format;
    int64_t  bit_rate;
    int      bits_per_coded_sample;
    int      bits_per_raw_sample;
    int      profile;
    int      level;
    int      width;
    int      height;
};

/// AVCodec 头部
struct AVCodecHeader {
    const char* name;
    const char* long_name;
    int         type;
    int         id;
};

/// AVFormatContext 头部
struct AVFormatCtxHeader {
    void*        av_class;
    void*        iformat;
    void*        oformat;
    void*        priv_data;
    void*        pb;
    int          ctx_flags;
    unsigned int nb_streams;
    void**       streams;
};

/// AVPacket 简化布局
struct AVPacketSimple {
    void*    buf;
    int64_t  pts;
    int64_t  dts;
    uint8_t* data;
    int      size;
    int      stream_index;
};

// ==================== 探针函数 ====================

/// 安全探针: 在 AVStream 内存中查找 codecpar 指针
static void* ProbeCodecPar(void* stream, int expectedType, int expectedId) {
    uint8_t* base = (uint8_t*)stream;
    for (int off = 0; off < 512; off += sizeof(void*)) {
        void* candidate = *(void**)(base + off);
        if (!candidate || (uintptr_t)candidate < 0x10000) continue;
#ifdef _WIN32
        __try {
#endif
            int* fields = (int*)candidate;
            if (fields[0] == expectedType && fields[1] == expectedId) {
                return candidate;
            }
#ifdef _WIN32
        } __except(EXCEPTION_EXECUTE_HANDLER) { continue; }
#endif
    }
    return nullptr;
}

/// 安全探针: 从 AVCodecParameters 中查找 sample_rate 和 channels
static bool ProbeAudioParams(void* codecpar, int* outSampleRate, int* outChannels) {
    int* fields = (int*)codecpar;
    for (int i = 15; i < 64; ++i) {
        int v = fields[i];
        if (v == 8000 || v == 11025 || v == 16000 || v == 22050 ||
            v == 32000 || v == 44100 || v == 48000 || v == 96000 || v == 192000) {
            *outSampleRate = v;
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

// ==================== waveOut 常量 (Windows 视频音频) ====================

#ifdef _WIN32
#define VIDEO_AUDIO_BUFS        64
#define VIDEO_AUDIO_BUF_SAMPLES 4096
#endif

// ==================== VideoBackendFFmpeg ====================

class VideoBackendFFmpeg : public VideoBackend {
    RenderBackend* m_render = nullptr;

    // 视频解码
    void*      m_formatCtx    = nullptr;
    void*      m_codecCtx     = nullptr;
    void*      m_frame        = nullptr;
    void*      m_packet       = nullptr;
    void*      m_swsCtx       = nullptr;
    int        m_videoStreamIdx = -1;
    int        m_width  = 0;
    int        m_height = 0;
    int        m_srcPixFmt = 0;
    uint32_t   m_texId  = 0;
    uint8_t*   m_rgbaBuf = nullptr;
    int        m_rgbaBufSize = 0;
    bool       m_playing  = false;
    bool       m_finished = false;
    bool       m_useSwscale = false;
    double     m_frameDelay = 0.04;
    double     m_lastTimeSec = 0.0;

    // 视频音频流
    void*      m_audioCodecCtx = nullptr;
    void*      m_audioFrame    = nullptr;
    int        m_audioStreamIdx = -1;
    int        m_audioSampleFmt  = 0;
    int        m_audioSampleRate = 44100;
    int        m_audioChannels   = 2;
    bool       m_audioReady = false;
    void*      m_swrCtx    = nullptr;
    int16_t*   m_audioAccumBuf = nullptr;
    int        m_audioAccumPos = 0;

#ifdef _WIN32
    HWAVEOUT   m_hWaveOut = nullptr;
    WAVEHDR    m_waveHdrs[VIDEO_AUDIO_BUFS] = {};
    uint8_t*   m_audioBufs[VIDEO_AUDIO_BUFS] = {};
    int        m_audioWriteIdx = 0;
#endif

    // YUV→RGBA 转换
    void ConvertFrame();
    // waveOut 音频处理 (Windows)
    void ProcessAudioPacket(void* frame);

public:
    ~VideoBackendFFmpeg() override { Close(); }

    bool Open(const char* path, RenderBackend* render) override;
    void Close() override;
    void Update() override;
    uint32_t GetTextureId() const override { return m_texId; }
    void Draw(float x, float y, float w, float h) override;
    bool IsPlaying()  const override { return m_playing && !m_finished; }
    bool IsFinished() const override { return m_finished; }
    void Stop() override { m_playing = false; m_finished = true; }
    int  GetWidth()   const override { return m_width; }
    int  GetHeight()  const override { return m_height; }
};

// ==================== Open ====================

bool VideoBackendFFmpeg::Open(const char* path, RenderBackend* render) {
    m_render = render;
    if (!LoadFFmpeg()) {
        CC::Log(CC::LOG_WARN, "Video: FFmpeg not available for '%s'", path);
        return false;
    }

    // 打开文件
    int rc = (int)(intptr_t)g_ff.avformat_open_input(&m_formatCtx, path, nullptr, nullptr);
    if (rc < 0 || !m_formatCtx) {
        CC::Log(CC::LOG_WARN, "Video: failed to open '%s' (rc=%d)", path, rc);
        return false;
    }
    g_ff.avformat_find_stream_info(m_formatCtx, nullptr);

    // 查找视频流
    void* decoder = nullptr;
    m_videoStreamIdx = g_ff.av_find_best_stream(m_formatCtx, FF_AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (m_videoStreamIdx < 0 || !decoder) {
        CC::Log(CC::LOG_WARN, "Video: no video stream in '%s'", path);
        return false;
    }

    // 创建解码器上下文
    m_codecCtx = g_ff.avcodec_alloc_context3(decoder);
    if (!m_codecCtx) return false;

    // 从流参数初始化解码器 (安全探针方式)
    AVFormatCtxHeader* fmtHdr = (AVFormatCtxHeader*)m_formatCtx;
    void* stream = fmtHdr->streams[m_videoStreamIdx];

    AVCodecHeader* decHdr = (AVCodecHeader*)decoder;
    CC::Log(CC::LOG_INFO, "Video: decoder='%s', codec_id=%d", decHdr->name, decHdr->id);

    void* codecpar = ProbeCodecPar(stream, FF_AVMEDIA_TYPE_VIDEO, decHdr->id);
    if (!codecpar) {
        CC::Log(CC::LOG_WARN, "Video: failed to locate codecpar for '%s'", path);
        return false;
    }

    g_ff.avcodec_parameters_to_context(m_codecCtx, codecpar);
    AVCodecParamsHeader* cp = (AVCodecParamsHeader*)codecpar;
    m_width     = cp->width;
    m_height    = cp->height;
    m_srcPixFmt = cp->format;
    CC::Log(CC::LOG_INFO, "Video: codecpar (%dx%d, pixfmt=%d)", m_width, m_height, m_srcPixFmt);

    m_frameDelay = 1.0 / 25.0; // 默认 25fps

    // 打开解码器
    if ((int)(intptr_t)g_ff.avcodec_open2(m_codecCtx, decoder, nullptr) < 0) {
        CC::Log(CC::LOG_WARN, "Video: avcodec_open2 failed for '%s'", path);
        return false;
    }

    // swscale 上下文 (可选)
    m_useSwscale = false;
    if (g_ff.sws_getContext) {
        m_swsCtx = g_ff.sws_getContext(
            m_width, m_height, m_srcPixFmt,
            m_width, m_height, FF_AV_PIX_FMT_RGBA,
            FF_SWS_BILINEAR, nullptr, nullptr, nullptr);
        m_useSwscale = (m_swsCtx != nullptr);
    }
    if (!m_useSwscale)
        CC::Log(CC::LOG_INFO, "Video: swscale not available, using software YUV->RGBA");

    // RGBA 缓冲区
    m_rgbaBufSize = m_width * m_height * 4;
    m_rgbaBuf = (uint8_t*)malloc(m_rgbaBufSize);
    if (!m_rgbaBuf) return false;
    memset(m_rgbaBuf, 0, m_rgbaBufSize);

    // FFmpeg 帧和包
    if (!g_ff.av_frame_alloc || !g_ff.av_packet_alloc) return false;
    m_frame  = g_ff.av_frame_alloc();
    m_packet = g_ff.av_packet_alloc();
    if (!m_frame || !m_packet) return false;

    // GL 纹理
    m_texId = m_render->CreateTexture(m_width, m_height, 4, nullptr);

    // 计时
    m_lastTimeSec = PlatformWindow::GetTime();
    m_playing = true;

    CC::Log(CC::LOG_INFO, "Video: opened '%s' (%dx%d, %.1f fps, texId=%u)",
            path, m_width, m_height, 1.0/m_frameDelay, m_texId);

    // ===== 音频流初始化 =====
    m_audioStreamIdx = -1;
    m_audioReady = false;
    void* audioDecoder = nullptr;
    int aIdx = g_ff.av_find_best_stream(m_formatCtx, FF_AVMEDIA_TYPE_AUDIO, -1, -1, &audioDecoder, 0);
    if (aIdx >= 0 && audioDecoder) {
        m_audioStreamIdx = aIdx;
        m_audioCodecCtx = g_ff.avcodec_alloc_context3(audioDecoder);
        if (m_audioCodecCtx) {
            AVCodecHeader* aDecHdr = (AVCodecHeader*)audioDecoder;
            void* aStream = fmtHdr->streams[aIdx];
            void* aCodecpar = ProbeCodecPar(aStream, FF_AVMEDIA_TYPE_AUDIO, aDecHdr->id);
            if (aCodecpar) {
                g_ff.avcodec_parameters_to_context(m_audioCodecCtx, aCodecpar);
                int sr = 44100, ch = 2;
                ProbeAudioParams(aCodecpar, &sr, &ch);
                m_audioSampleRate = sr;
                m_audioChannels   = ch;
                AVCodecParamsHeader* acp = (AVCodecParamsHeader*)aCodecpar;
                m_audioSampleFmt = acp->format;
            }
            if ((int)(intptr_t)g_ff.avcodec_open2(m_audioCodecCtx, audioDecoder, nullptr) >= 0) {
                m_audioFrame = g_ff.av_frame_alloc();
                m_audioReady = true;
                CC::Log(CC::LOG_INFO, "Video: audio stream (idx=%d, decoder='%s')",
                        aIdx, aDecHdr->name);
            }
        }
    }

    return true;
}

// ==================== Close ====================

void VideoBackendFFmpeg::Close() {
#ifdef _WIN32
    if (m_hWaveOut) {
        waveOutReset(m_hWaveOut);
        for (int i = 0; i < VIDEO_AUDIO_BUFS; ++i) {
            if (m_waveHdrs[i].dwFlags & WHDR_PREPARED)
                waveOutUnprepareHeader(m_hWaveOut, &m_waveHdrs[i], sizeof(WAVEHDR));
            if (m_audioBufs[i]) { free(m_audioBufs[i]); m_audioBufs[i] = nullptr; }
        }
        waveOutClose(m_hWaveOut);
        m_hWaveOut = nullptr;
    }
#endif
    if (m_audioAccumBuf) { free(m_audioAccumBuf); m_audioAccumBuf = nullptr; }
    if (m_swrCtx && g_ff.loaded && g_ff.swr_free) g_ff.swr_free(&m_swrCtx);

    if (g_ff.loaded) {
        if (m_swsCtx && g_ff.sws_freeContext) g_ff.sws_freeContext(m_swsCtx);
        if (m_frame   && g_ff.av_frame_free)   g_ff.av_frame_free(&m_frame);
        if (m_audioFrame && g_ff.av_frame_free) g_ff.av_frame_free(&m_audioFrame);
        if (m_packet  && g_ff.av_packet_free)   g_ff.av_packet_free(&m_packet);
        if (m_codecCtx && g_ff.avcodec_free_context) g_ff.avcodec_free_context(&m_codecCtx);
        if (m_audioCodecCtx && g_ff.avcodec_free_context) g_ff.avcodec_free_context(&m_audioCodecCtx);
        if (m_formatCtx && g_ff.avformat_close_input) g_ff.avformat_close_input(&m_formatCtx);
    }
    if (m_rgbaBuf) { free(m_rgbaBuf); m_rgbaBuf = nullptr; }
    if (m_texId && m_render) { m_render->DeleteTexture(m_texId); m_texId = 0; }
    m_playing = false;
    m_finished = true;
}

// ==================== ConvertFrame (YUV→RGBA) ====================

void VideoBackendFFmpeg::ConvertFrame() {
    AVFrameHeader* frm = (AVFrameHeader*)m_frame;

    if (m_useSwscale && m_swsCtx) {
        uint8_t* dstData[1]  = { m_rgbaBuf };
        int      dstStride[1] = { m_width * 4 };
        g_ff.sws_scale(m_swsCtx,
                       (const uint8_t* const*)frm->data, frm->linesize,
                       0, m_height, dstData, dstStride);
        return;
    }

    // 软件 YUV420P → RGBA (钳位查找表优化)
    static uint8_t clampTab[1024];
    static bool clampInit = false;
    if (!clampInit) {
        for (int i = 0; i < 1024; ++i) {
            int v = i - 256;
            clampTab[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
        clampInit = true;
    }
    const uint8_t* clamp = clampTab + 256;

    const uint8_t* yPlane = frm->data[0];
    const uint8_t* uPlane = frm->data[1];
    const uint8_t* vPlane = frm->data[2];
    int yStride = frm->linesize[0];
    int uStride = frm->linesize[1];
    int vStride = frm->linesize[2];
    int w = m_width, h = m_height;
    uint8_t* dst = m_rgbaBuf;

    for (int row = 0; row < h; ++row) {
        const uint8_t* yRow = yPlane + row * yStride;
        const uint8_t* uRow = uPlane + (row >> 1) * uStride;
        const uint8_t* vRow = vPlane + (row >> 1) * vStride;
        uint8_t* dRow = dst + row * w * 4;

        int col = 0;
        for (; col < w - 1; col += 2) {
            int U = uRow[col >> 1] - 128;
            int V = vRow[col >> 1] - 128;
            int rAdd = (359 * V) >> 8;
            int gSub = (88 * U + 183 * V) >> 8;
            int bAdd = (454 * U) >> 8;
            int Y0 = yRow[col];
            dRow[0] = clamp[Y0 + rAdd]; dRow[1] = clamp[Y0 - gSub];
            dRow[2] = clamp[Y0 + bAdd]; dRow[3] = 255;
            int Y1 = yRow[col + 1];
            dRow[4] = clamp[Y1 + rAdd]; dRow[5] = clamp[Y1 - gSub];
            dRow[6] = clamp[Y1 + bAdd]; dRow[7] = 255;
            dRow += 8;
        }
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

// ==================== ProcessAudioPacket (Windows waveOut) ====================

void VideoBackendFFmpeg::ProcessAudioPacket(void* /*audioFrameRaw*/) {
#ifdef _WIN32
    AVFrameHeader* af = (AVFrameHeader*)m_audioFrame;

    // 首帧: 初始化 waveOut
    if (!m_hWaveOut && af->data[0]) {
        int actualFmt = *((int*)((uint8_t*)m_audioFrame + 116));
        if (actualFmt >= 0 && actualFmt <= 11) m_audioSampleFmt = actualFmt;

        WAVEFORMATEX wfx = {};
        wfx.wFormatTag = WAVE_FORMAT_PCM;
        wfx.nChannels = (WORD)m_audioChannels;
        wfx.nSamplesPerSec = (DWORD)m_audioSampleRate;
        wfx.wBitsPerSample = 16;
        wfx.nBlockAlign = wfx.nChannels * wfx.wBitsPerSample / 8;
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        MMRESULT mr = waveOutOpen(&m_hWaveOut, WAVE_MAPPER, &wfx, 0, 0, CALLBACK_NULL);
        if (mr != MMSYSERR_NOERROR) { m_audioReady = false; return; }

        int bufBytes = VIDEO_AUDIO_BUF_SAMPLES * wfx.nBlockAlign;
        for (int bi = 0; bi < VIDEO_AUDIO_BUFS; ++bi) {
            m_audioBufs[bi] = (uint8_t*)malloc(bufBytes);
            memset(&m_waveHdrs[bi], 0, sizeof(WAVEHDR));
        }
        m_audioWriteIdx = 0;
        m_audioAccumBuf = (int16_t*)malloc(VIDEO_AUDIO_BUF_SAMPLES * wfx.nBlockAlign);
        m_audioAccumPos = 0;

        // swresample 初始化
        m_swrCtx = nullptr;
        if (g_ff.swr_alloc_set_opts && g_ff.swr_init && g_ff.swr_convert) {
            int64_t chLayout = (m_audioChannels == 1) ? 4 : 3;
            m_swrCtx = g_ff.swr_alloc_set_opts(
                nullptr, chLayout, 1, m_audioSampleRate,
                chLayout, m_audioSampleFmt, m_audioSampleRate, 0, nullptr);
            if (m_swrCtx && g_ff.swr_init(m_swrCtx) < 0) {
                g_ff.swr_free(&m_swrCtx); m_swrCtx = nullptr;
            }
        }
    }

    if (!m_hWaveOut || !af->data[0]) return;

    int ch  = m_audioChannels;
    int fmt = m_audioSampleFmt;
    int nb_samples = *((int*)((uint8_t*)m_audioFrame + 112));
    int bps = (fmt == FF_AV_SAMPLE_FMT_FLTP || fmt == FF_AV_SAMPLE_FMT_FLT) ? 4 : 2;
    int maxFromLinesize = af->linesize[0] / bps;
    if (nb_samples <= 0 || nb_samples > maxFromLinesize) nb_samples = maxFromLinesize;
    if (nb_samples <= 0 || nb_samples > VIDEO_AUDIO_BUF_SAMPLES) return;

    int16_t* accum = m_audioAccumBuf;
    int accumMax = VIDEO_AUDIO_BUF_SAMPLES * ch;

    if (m_swrCtx) {
        int16_t tmpBuf[4096 * 2];
        uint8_t* outPtrs[1] = { (uint8_t*)tmpBuf };
        const uint8_t** inPtrs = (const uint8_t**)af->data;
        int converted = g_ff.swr_convert(m_swrCtx, outPtrs, nb_samples, inPtrs, nb_samples);
        if (converted > 0) {
            int toCopy = converted * ch;
            if (m_audioAccumPos + toCopy > accumMax) toCopy = accumMax - m_audioAccumPos;
            memcpy(accum + m_audioAccumPos, tmpBuf, toCopy * sizeof(int16_t));
            m_audioAccumPos += toCopy;
        }
    } else {
        // 手动回退转换
        if (fmt == FF_AV_SAMPLE_FMT_FLTP) {
            for (int s = 0; s < nb_samples && m_audioAccumPos < accumMax; ++s) {
                for (int c2 = 0; c2 < ch; ++c2) {
                    float v = (af->data[c2]) ? ((float*)af->data[c2])[s] : 0.0f;
                    if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                    accum[m_audioAccumPos++] = (int16_t)(v * 32767.0f);
                }
            }
        } else if (fmt == FF_AV_SAMPLE_FMT_S16P) {
            for (int s = 0; s < nb_samples && m_audioAccumPos < accumMax; ++s) {
                for (int c2 = 0; c2 < ch; ++c2) {
                    accum[m_audioAccumPos++] = (af->data[c2]) ? ((int16_t*)af->data[c2])[s] : 0;
                }
            }
        } else if (fmt == FF_AV_SAMPLE_FMT_S16) {
            int toCopy = nb_samples * ch;
            if (m_audioAccumPos + toCopy > accumMax) toCopy = accumMax - m_audioAccumPos;
            memcpy(accum + m_audioAccumPos, af->data[0], toCopy * 2);
            m_audioAccumPos += toCopy;
        } else if (fmt == FF_AV_SAMPLE_FMT_FLT) {
            float* src = (float*)af->data[0];
            int total = nb_samples * ch;
            for (int s = 0; s < total && m_audioAccumPos < accumMax; ++s) {
                float v = src[s];
                if (v > 1.0f) v = 1.0f; if (v < -1.0f) v = -1.0f;
                accum[m_audioAccumPos++] = (int16_t)(v * 32767.0f);
            }
        }
    }

    // 累积满一个缓冲区后提交 waveOut
    if (m_audioAccumPos >= accumMax) {
        int wi = m_audioWriteIdx % VIDEO_AUDIO_BUFS;
        if (m_waveHdrs[wi].dwFlags & WHDR_PREPARED) {
            for (int y = 0; y < 5 && !(m_waveHdrs[wi].dwFlags & WHDR_DONE); ++y)
                ::Sleep(0);
            waveOutUnprepareHeader(m_hWaveOut, &m_waveHdrs[wi], sizeof(WAVEHDR));
        }
        memcpy(m_audioBufs[wi], accum, accumMax * 2);
        m_waveHdrs[wi].lpData = (LPSTR)m_audioBufs[wi];
        m_waveHdrs[wi].dwBufferLength = (DWORD)(accumMax * 2);
        m_waveHdrs[wi].dwFlags = 0;
        waveOutPrepareHeader(m_hWaveOut, &m_waveHdrs[wi], sizeof(WAVEHDR));
        waveOutWrite(m_hWaveOut, &m_waveHdrs[wi], sizeof(WAVEHDR));
        m_audioWriteIdx++;
        m_audioAccumPos = 0;
    }
#endif // _WIN32
}

// ==================== Update ====================

void VideoBackendFFmpeg::Update() {
    if (!m_playing || m_finished || !g_ff.loaded) return;

    double nowSec = PlatformWindow::GetTime();
    double elapsed = nowSec - m_lastTimeSec;
    bool needVideoFrame = (elapsed >= m_frameDelay);

    // 非视频帧周期: 检查是否需要补充音频
    if (!needVideoFrame) {
#ifdef _WIN32
        if (m_hWaveOut) {
            int pending = 0;
            for (int i = 0; i < VIDEO_AUDIO_BUFS; ++i) {
                if ((m_waveHdrs[i].dwFlags & WHDR_PREPARED) &&
                    !(m_waveHdrs[i].dwFlags & WHDR_DONE))
                    pending++;
            }
            if (pending >= 4) return;
        } else {
            return;
        }
#else
        return;
#endif
    }

    int maxPackets = needVideoFrame ? 60 : 8;
    bool gotVideoFrame = false;

    for (int p = 0; p < maxPackets; ++p) {
        int readRc = g_ff.av_read_frame(m_formatCtx, m_packet);
        if (readRc < 0) {
            m_finished = true; m_playing = false;
            break;
        }

        AVPacketSimple* pkt = (AVPacketSimple*)m_packet;

        // 音频包处理
#ifdef _WIN32
        if (pkt->stream_index == m_audioStreamIdx && m_audioReady) {
            g_ff.avcodec_send_packet(m_audioCodecCtx, m_packet);
            g_ff.av_packet_unref(m_packet);
            while (g_ff.avcodec_receive_frame(m_audioCodecCtx, m_audioFrame) == 0) {
                ProcessAudioPacket(m_audioFrame);
            }
            continue;
        }
#else
        if (pkt->stream_index == m_audioStreamIdx) {
            g_ff.av_packet_unref(m_packet);
            continue;
        }
#endif

        // 视频包处理
        if (pkt->stream_index == m_videoStreamIdx) {
            g_ff.avcodec_send_packet(m_codecCtx, m_packet);
            g_ff.av_packet_unref(m_packet);
            if (g_ff.avcodec_receive_frame(m_codecCtx, m_frame) == 0) {
                if (needVideoFrame && !gotVideoFrame) {
                    gotVideoFrame = true;
                    m_lastTimeSec = nowSec;
                }
            }
            if (gotVideoFrame) break;
            continue;
        }

        g_ff.av_packet_unref(m_packet);
    }

    if (!gotVideoFrame) return;

    // YUV→RGBA 并上传纹理
    ConvertFrame();
    m_render->UpdateTexture(m_texId, 0, 0, m_width, m_height, 4, m_rgbaBuf);
}

// ==================== Draw ====================

void VideoBackendFFmpeg::Draw(float x, float y, float w, float h) {
    if (!m_texId || !m_render) return;
    m_render->PushMatrix();
    m_render->Translate(x, y, 0);
    m_render->SetColor(1, 1, 1, 1);
    m_render->BindTexture(m_texId);
    RenderVertex verts[4] = {
        {0, 0, 0,  0, 0,  1, 1, 1, 1},
        {w, 0, 0,  1, 0,  1, 1, 1, 1},
        {w, h, 0,  1, 1,  1, 1, 1, 1},
        {0, h, 0,  0, 1,  1, 1, 1, 1},
    };
    m_render->DrawArrays(DrawMode::Quads, verts, 4);
    m_render->UnbindTexture();
    m_render->PopMatrix();
}

// ==================== 工厂函数 ====================

VideoBackend* CreateVideoBackend() {
    return new VideoBackendFFmpeg();
}

#endif // !__EMSCRIPTEN__ && !__ANDROID__ && !CHOCO_PLATFORM_IOS
