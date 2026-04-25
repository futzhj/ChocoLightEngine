/**
 * @file light_audio_backend.cpp
 * @brief AudioBackend miniaudio 实现
 */

#include "light_audio_backend.h"
#include "light.h"
#include "miniaudio.h"
#include <cstring>

// ==================== 内部状态 ====================

static ma_engine  g_engine;
static bool       g_engineInit = false;

// 音频句柄: 封装 ma_sound + 可选 ma_audio_buffer
struct AudioHandle {
    ma_sound         sound;
    ma_audio_buffer* pcmBuffer;  // 仅 LoadPCM 时非空
    bool             valid;
};

// ==================== AudioBackend 实现 ====================

namespace AudioBackend {

bool Init() {
    if (g_engineInit) return true;
    ma_engine_config cfg = ma_engine_config_init();
    ma_result r = ma_engine_init(&cfg, &g_engine);
    if (r != MA_SUCCESS) {
        CC::Log(CC::LOG_ERROR, "AudioBackend: ma_engine_init failed (%d)", r);
        return false;
    }
    g_engineInit = true;
    CC::Log(CC::LOG_INFO, "AudioBackend: miniaudio engine initialized");
    return true;
}

void Shutdown() {
    if (!g_engineInit) return;
    ma_engine_uninit(&g_engine);
    g_engineInit = false;
    CC::Log(CC::LOG_INFO, "AudioBackend: shutdown");
}

AudioHandle* LoadFile(const char* path) {
    if (!g_engineInit) return nullptr;

    AudioHandle* h = new AudioHandle();
    memset(h, 0, sizeof(AudioHandle));

    ma_result r = ma_sound_init_from_file(&g_engine, path, 0, nullptr, nullptr, &h->sound);
    if (r != MA_SUCCESS) {
        delete h;
        return nullptr;  // 格式不支持或文件不存在
    }
    h->valid = true;
    return h;
}

AudioHandle* LoadPCM(const void* data, uint64_t frameCount,
                     int format, int channels, int sampleRate) {
    if (!g_engineInit || !data || frameCount == 0) return nullptr;

    // 映射 format 到 ma_format
    ma_format maFmt;
    switch (format) {
        case 1: maFmt = ma_format_u8;  break;
        case 2: maFmt = ma_format_s16; break;
        case 3: maFmt = ma_format_s32; break;
        case 5: maFmt = ma_format_f32; break;
        default: maFmt = ma_format_s16; break;
    }

    // 创建 audio buffer (持有数据副本)
    ma_audio_buffer_config bufCfg = ma_audio_buffer_config_init(
        maFmt, (ma_uint32)channels, frameCount, data, nullptr);

    ma_audio_buffer* buf = new ma_audio_buffer();
    ma_result r = ma_audio_buffer_init(&bufCfg, buf);
    if (r != MA_SUCCESS) {
        delete buf;
        CC::Log(CC::LOG_ERROR, "AudioBackend: ma_audio_buffer_init failed (%d)", r);
        return nullptr;
    }

    // 从 buffer 创建 sound
    AudioHandle* h = new AudioHandle();
    memset(h, 0, sizeof(AudioHandle));
    h->pcmBuffer = buf;

    r = ma_sound_init_from_data_source(&g_engine, buf, 0, nullptr, &h->sound);
    if (r != MA_SUCCESS) {
        ma_audio_buffer_uninit(buf);
        delete buf;
        delete h;
        CC::Log(CC::LOG_ERROR, "AudioBackend: ma_sound_init_from_data_source failed (%d)", r);
        return nullptr;
    }
    h->valid = true;
    return h;
}

void Play(AudioHandle* h) {
    if (!h || !h->valid) return;
    ma_sound_start(&h->sound);
}

void Pause(AudioHandle* h) {
    if (!h || !h->valid) return;
    ma_sound_stop(&h->sound);
}

void Stop(AudioHandle* h) {
    if (!h || !h->valid) return;
    ma_sound_stop(&h->sound);
    ma_sound_seek_to_pcm_frame(&h->sound, 0);
}

void SetVolume(AudioHandle* h, float vol) {
    if (!h || !h->valid) return;
    ma_sound_set_volume(&h->sound, vol);
}

float GetVolume(AudioHandle* h) {
    if (!h || !h->valid) return 0.0f;
    return ma_sound_get_volume(&h->sound);
}

bool IsPlaying(AudioHandle* h) {
    if (!h || !h->valid) return false;
    return ma_sound_is_playing(&h->sound) != 0;
}

void Free(AudioHandle* h) {
    if (!h) return;
    if (h->valid) {
        ma_sound_uninit(&h->sound);
    }
    if (h->pcmBuffer) {
        ma_audio_buffer_uninit(h->pcmBuffer);
        delete h->pcmBuffer;
    }
    delete h;
}

} // namespace AudioBackend
