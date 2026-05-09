/**
 * @file light_audio_backend.cpp
 * @brief AudioBackend miniaudio 实现
 *
 * Phase AT: 扩展 3D 空间化 + SoundGroup + Effect (滤波器/Echo) + Listener
 */

#include "light_audio_backend.h"
#include "light.h"
#include "miniaudio.h"
#include <cstring>
#include <cstdlib>

// ==================== 内部状态 ====================

static ma_engine  g_engine;
static bool       g_engineInit = false;

// Effect 类型 tag (用于 uninit dispatch)
enum class EffectKind : int {
    None,
    LowPass,
    HighPass,
    BandPass,
    Notch,
    Peak,
    Echo,
};

// Effect 句柄: 各 effect 节点之一 + 类型 tag
struct EffectHandle {
    EffectKind kind;
    bool       enabled;
    union {
        ma_lpf_node    lpf;
        ma_hpf_node    hpf;
        ma_bpf_node    bpf;
        ma_notch_node  notch;
        ma_peak_node   peak;
        ma_delay_node  delay;
    } node;
};

// SoundGroup 句柄
struct GroupHandle {
    ma_sound_group group;
    GroupHandle*   parent;  // 父 group, 仅供循环检测; 实际 routing 由 ma_node attach 处理
};

// 音频句柄: 封装 ma_sound + 可选 ma_audio_buffer + 当前 routing 状态
struct AudioHandle {
    ma_sound         sound;
    ma_audio_buffer* pcmBuffer;  // 仅 LoadPCM 时非空
    bool             valid;

    // Phase AT: 当前 routing target (用于 effect/group 切换)
    GroupHandle*  currentGroup;   // nullptr -> master endpoint
    EffectHandle* currentEffect;  // nullptr -> 无 effect
};

// ==================== 前向声明 ====================

namespace {
    // 把 sound 重新连接到当前 group/effect 链
    void RewireSoundOutput(AudioHandle* h);

    // 取 group 输入节点 (用于 sound/effect attach 目标)
    ma_node* GroupInputNode(GroupHandle* g);

    // 取 group 当前父输出节点 (用于 group 自身 reroute)
    ma_node* GroupOutputTarget(GroupHandle* g);

    // 取 effect 节点 (用于 attach 目标)
    ma_node* EffectNode(EffectHandle* e);
}

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

// ==================== Phase AT: Sound 扩展 ====================

void SetLooping(AudioHandle* h, bool looping) {
    if (!h || !h->valid) return;
    ma_sound_set_looping(&h->sound, looping ? MA_TRUE : MA_FALSE);
}

bool GetLooping(AudioHandle* h) {
    if (!h || !h->valid) return false;
    return ma_sound_is_looping(&h->sound) != 0;
}

void SetPitch(AudioHandle* h, float pitch) {
    if (!h || !h->valid) return;
    if (pitch < 0.01f) pitch = 0.01f;  // 防止 0 / 负值
    ma_sound_set_pitch(&h->sound, pitch);
}

float GetPitch(AudioHandle* h) {
    if (!h || !h->valid) return 1.0f;
    return ma_sound_get_pitch(&h->sound);
}

void SetPan(AudioHandle* h, float pan) {
    if (!h || !h->valid) return;
    if (pan < -1.0f) pan = -1.0f;
    if (pan >  1.0f) pan =  1.0f;
    ma_sound_set_pan(&h->sound, pan);
}

float GetPan(AudioHandle* h) {
    if (!h || !h->valid) return 0.0f;
    return ma_sound_get_pan(&h->sound);
}

void SetSpatializationEnabled(AudioHandle* h, bool enabled) {
    if (!h || !h->valid) return;
    ma_sound_set_spatialization_enabled(&h->sound, enabled ? MA_TRUE : MA_FALSE);
}

bool GetSpatializationEnabled(AudioHandle* h) {
    if (!h || !h->valid) return false;
    return ma_sound_is_spatialization_enabled(&h->sound) != 0;
}

void SetPosition(AudioHandle* h, float x, float y, float z) {
    if (!h || !h->valid) return;
    ma_sound_set_position(&h->sound, x, y, z);
}

void GetPosition(AudioHandle* h, float* x, float* y, float* z) {
    if (!h || !h->valid) {
        if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
        return;
    }
    ma_vec3f p = ma_sound_get_position(&h->sound);
    if (x) *x = p.x; if (y) *y = p.y; if (z) *z = p.z;
}

void SetVelocity(AudioHandle* h, float x, float y, float z) {
    if (!h || !h->valid) return;
    ma_sound_set_velocity(&h->sound, x, y, z);
}

void SetAttenuationModel(AudioHandle* h, int model) {
    if (!h || !h->valid) return;
    ma_attenuation_model m;
    switch (model) {
        case 0: m = ma_attenuation_model_none;        break;
        case 1: m = ma_attenuation_model_inverse;     break;
        case 2: m = ma_attenuation_model_linear;      break;
        case 3: m = ma_attenuation_model_exponential; break;
        default: m = ma_attenuation_model_inverse;    break;
    }
    ma_sound_set_attenuation_model(&h->sound, m);
}

int GetAttenuationModel(AudioHandle* h) {
    if (!h || !h->valid) return 1;
    ma_attenuation_model m = ma_sound_get_attenuation_model(&h->sound);
    switch (m) {
        case ma_attenuation_model_none:        return 0;
        case ma_attenuation_model_inverse:     return 1;
        case ma_attenuation_model_linear:      return 2;
        case ma_attenuation_model_exponential: return 3;
        default:                               return 1;
    }
}

void  SetMinDistance(AudioHandle* h, float d) { if (h && h->valid) ma_sound_set_min_distance(&h->sound, d); }
float GetMinDistance(AudioHandle* h)         { return (h && h->valid) ? ma_sound_get_min_distance(&h->sound) : 1.0f; }
void  SetMaxDistance(AudioHandle* h, float d) { if (h && h->valid) ma_sound_set_max_distance(&h->sound, d); }
float GetMaxDistance(AudioHandle* h)         { return (h && h->valid) ? ma_sound_get_max_distance(&h->sound) : 0.0f; }
void  SetRolloff    (AudioHandle* h, float r) { if (h && h->valid) ma_sound_set_rolloff(&h->sound, r); }
float GetRolloff    (AudioHandle* h)         { return (h && h->valid) ? ma_sound_get_rolloff(&h->sound) : 1.0f; }

// ==================== Phase AT: Routing helpers (匿名命名空间内实现) ====================

} // namespace AudioBackend (临时关闭以放 anonymous helpers)

namespace {
    ma_node* GroupInputNode(GroupHandle* g) {
        // ma_sound_group 本质是 ma_sound; 它作为 node 时, input bus 是其自身
        return (ma_node*)&g->group;
    }

    ma_node* GroupOutputTarget(GroupHandle* g) {
        // group 自己 output 到: 父 group 输入 / engine endpoint
        if (g->parent) return GroupInputNode(g->parent);
        return ma_engine_get_endpoint(&g_engine);
    }

    ma_node* EffectNode(EffectHandle* e) {
        if (!e) return nullptr;
        switch (e->kind) {
            case EffectKind::LowPass:  return (ma_node*)&e->node.lpf;
            case EffectKind::HighPass: return (ma_node*)&e->node.hpf;
            case EffectKind::BandPass: return (ma_node*)&e->node.bpf;
            case EffectKind::Notch:    return (ma_node*)&e->node.notch;
            case EffectKind::Peak:     return (ma_node*)&e->node.peak;
            case EffectKind::Echo:     return (ma_node*)&e->node.delay;
            default:                   return nullptr;
        }
    }

    // 根据 sound 的 currentGroup 取目标 ma_node
    ma_node* SoundFinalTarget(AudioHandle* h) {
        if (h->currentGroup) return GroupInputNode(h->currentGroup);
        return ma_engine_get_endpoint(&g_engine);
    }

    void RewireSoundOutput(AudioHandle* h) {
        if (!h || !h->valid) return;
        ma_node* finalTarget = SoundFinalTarget(h);

        if (h->currentEffect) {
            ma_node* enode = EffectNode(h->currentEffect);
            if (enode) {
                // sound -> effect -> finalTarget
                ma_node_attach_output_bus(&h->sound.engineNode, 0, enode, 0);
                ma_node_attach_output_bus(enode, 0, finalTarget, 0);
                return;
            }
        }
        // 无 effect: sound 直接 -> finalTarget
        ma_node_attach_output_bus(&h->sound.engineNode, 0, finalTarget, 0);
    }
}

namespace AudioBackend {

// ==================== Phase AT: SoundGroup ====================

GroupHandle* CreateGroup(GroupHandle* parent) {
    if (!g_engineInit) return nullptr;

    GroupHandle* g = new GroupHandle();
    memset(g, 0, sizeof(GroupHandle));
    g->parent = parent;

    ma_sound_group* parentMa = parent ? &parent->group : nullptr;
    ma_result r = ma_sound_group_init(&g_engine, 0, parentMa, &g->group);
    if (r != MA_SUCCESS) {
        CC::Log(CC::LOG_ERROR, "AudioBackend: ma_sound_group_init failed (%d)", r);
        delete g;
        return nullptr;
    }
    return g;
}

void FreeGroup(GroupHandle* g) {
    if (!g) return;
    ma_sound_group_uninit(&g->group);
    delete g;
}

void  SetGroupVolume(GroupHandle* g, float v) { if (g) ma_sound_group_set_volume(&g->group, v); }
float GetGroupVolume(GroupHandle* g)         { return g ? ma_sound_group_get_volume(&g->group) : 0.0f; }
void  SetGroupPitch (GroupHandle* g, float p) { if (g) ma_sound_group_set_pitch(&g->group, p < 0.01f ? 0.01f : p); }
float GetGroupPitch (GroupHandle* g)         { return g ? ma_sound_group_get_pitch(&g->group) : 1.0f; }

void GroupPause(GroupHandle* g)  { if (g) ma_sound_group_stop(&g->group); }
void GroupResume(GroupHandle* g) { if (g) ma_sound_group_start(&g->group); }
void GroupStop(GroupHandle* g) {
    if (!g) return;
    ma_sound_group_stop(&g->group);
    // group 没有直接 seek 接口, 这里仅 stop (子 sound seek 由用户负责)
}

void SetSoundGroup(AudioHandle* h, GroupHandle* g) {
    if (!h || !h->valid) return;
    h->currentGroup = g;
    RewireSoundOutput(h);
}

bool SetGroupParent(GroupHandle* g, GroupHandle* parent) {
    if (!g) return false;
    // 循环检测: 如果 parent 链上能到达 g, 拒绝 (会形成环)
    GroupHandle* p = parent;
    while (p) {
        if (p == g) return false;  // 环! 拒绝
        p = p->parent;
    }
    g->parent = parent;
    // group 自身也是 ma_node, 重定向到新父
    ma_node_attach_output_bus(GroupInputNode(g), 0, GroupOutputTarget(g), 0);
    return true;
}

// ==================== Phase AT: Effect ====================

EffectHandle* CreateLowPass(float cutoffHz, int order) {
    if (!g_engineInit) return nullptr;
    if (order < 2)  order = 2;
    if (order > 8)  order = 8;
    if (order & 1)  order++;  // 强制偶数
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_lpf_node_config cfg = ma_lpf_node_config_init(ch, sr, cutoffHz, (ma_uint32)order);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::LowPass;
    e->enabled = true;
    if (ma_lpf_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.lpf) != MA_SUCCESS) {
        delete e;
        return nullptr;
    }
    // 默认 attach 到 master, 之后 SetSoundEffect 会 rewire
    ma_node_attach_output_bus(&e->node.lpf, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

EffectHandle* CreateHighPass(float cutoffHz, int order) {
    if (!g_engineInit) return nullptr;
    if (order < 2) order = 2;
    if (order > 8) order = 8;
    if (order & 1) order++;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_hpf_node_config cfg = ma_hpf_node_config_init(ch, sr, cutoffHz, (ma_uint32)order);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::HighPass;
    e->enabled = true;
    if (ma_hpf_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.hpf) != MA_SUCCESS) {
        delete e; return nullptr;
    }
    ma_node_attach_output_bus(&e->node.hpf, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

EffectHandle* CreateBandPass(float cutoffHz, int order) {
    if (!g_engineInit) return nullptr;
    if (order < 2) order = 2;
    if (order > 8) order = 8;
    if (order & 1) order++;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_bpf_node_config cfg = ma_bpf_node_config_init(ch, sr, cutoffHz, (ma_uint32)order);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::BandPass;
    e->enabled = true;
    if (ma_bpf_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.bpf) != MA_SUCCESS) {
        delete e; return nullptr;
    }
    ma_node_attach_output_bus(&e->node.bpf, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

EffectHandle* CreateNotch(float cutoffHz, float q) {
    if (!g_engineInit) return nullptr;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_notch_node_config cfg = ma_notch_node_config_init(ch, sr, q, cutoffHz);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::Notch;
    e->enabled = true;
    if (ma_notch_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.notch) != MA_SUCCESS) {
        delete e; return nullptr;
    }
    ma_node_attach_output_bus(&e->node.notch, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

EffectHandle* CreatePeak(float cutoffHz, float gainDB, float q) {
    if (!g_engineInit) return nullptr;
    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_peak_node_config cfg = ma_peak_node_config_init(ch, sr, gainDB, q, cutoffHz);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::Peak;
    e->enabled = true;
    if (ma_peak_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.peak) != MA_SUCCESS) {
        delete e; return nullptr;
    }
    ma_node_attach_output_bus(&e->node.peak, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

EffectHandle* CreateEcho(int delayMs, float decay, float wet, float dry) {
    if (!g_engineInit) return nullptr;
    if (delayMs < 1) delayMs = 1;
    if (decay < 0.0f) decay = 0.0f;
    if (decay > 0.99f) decay = 0.99f;
    if (wet < 0.0f) wet = 0.0f;
    if (dry < 0.0f) dry = 0.0f;

    ma_uint32 ch = ma_engine_get_channels(&g_engine);
    ma_uint32 sr = ma_engine_get_sample_rate(&g_engine);
    ma_uint32 delayFrames = (ma_uint32)((double)delayMs * sr / 1000.0);
    ma_delay_node_config cfg = ma_delay_node_config_init(ch, sr, delayFrames, decay);

    EffectHandle* e = new EffectHandle();
    memset(e, 0, sizeof(EffectHandle));
    e->kind = EffectKind::Echo;
    e->enabled = true;
    if (ma_delay_node_init(ma_engine_get_node_graph(&g_engine), &cfg, nullptr, &e->node.delay) != MA_SUCCESS) {
        delete e; return nullptr;
    }
    ma_delay_node_set_wet(&e->node.delay, wet);
    ma_delay_node_set_dry(&e->node.delay, dry);
    ma_node_attach_output_bus(&e->node.delay, 0, ma_engine_get_endpoint(&g_engine), 0);
    return e;
}

void FreeEffect(EffectHandle* e) {
    if (!e) return;
    switch (e->kind) {
        case EffectKind::LowPass:  ma_lpf_node_uninit(&e->node.lpf,   nullptr); break;
        case EffectKind::HighPass: ma_hpf_node_uninit(&e->node.hpf,   nullptr); break;
        case EffectKind::BandPass: ma_bpf_node_uninit(&e->node.bpf,   nullptr); break;
        case EffectKind::Notch:    ma_notch_node_uninit(&e->node.notch, nullptr); break;
        case EffectKind::Peak:     ma_peak_node_uninit(&e->node.peak,  nullptr); break;
        case EffectKind::Echo:     ma_delay_node_uninit(&e->node.delay, nullptr); break;
        default: break;
    }
    delete e;
}

void SetEffectEnabled(EffectHandle* e, bool enabled) {
    if (!e) return;
    e->enabled = enabled;
    ma_node* n = EffectNode(e);
    if (n) {
        ma_node_set_state(n, enabled ? ma_node_state_started : ma_node_state_stopped);
    }
}

bool GetEffectEnabled(EffectHandle* e) {
    return e ? e->enabled : false;
}

void SetSoundEffect(AudioHandle* h, EffectHandle* e) {
    if (!h || !h->valid) return;
    h->currentEffect = e;
    RewireSoundOutput(h);
}

// ==================== Phase AT: Listener ====================

void SetListenerPosition(int idx, float x, float y, float z) {
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_engine_listener_set_position(&g_engine, (ma_uint32)idx, x, y, z);
}

void GetListenerPosition(int idx, float* x, float* y, float* z) {
    if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_vec3f p = ma_engine_listener_get_position(&g_engine, (ma_uint32)idx);
    if (x) *x = p.x; if (y) *y = p.y; if (z) *z = p.z;
}

void SetListenerDirection(int idx, float x, float y, float z) {
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_engine_listener_set_direction(&g_engine, (ma_uint32)idx, x, y, z);
}

void GetListenerDirection(int idx, float* x, float* y, float* z) {
    if (x) *x = 0; if (y) *y = 0; if (z) *z = -1;
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_vec3f d = ma_engine_listener_get_direction(&g_engine, (ma_uint32)idx);
    if (x) *x = d.x; if (y) *y = d.y; if (z) *z = d.z;
}

void SetListenerWorldUp(int idx, float x, float y, float z) {
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_engine_listener_set_world_up(&g_engine, (ma_uint32)idx, x, y, z);
}

void GetListenerWorldUp(int idx, float* x, float* y, float* z) {
    if (x) *x = 0; if (y) *y = 1; if (z) *z = 0;
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_vec3f u = ma_engine_listener_get_world_up(&g_engine, (ma_uint32)idx);
    if (x) *x = u.x; if (y) *y = u.y; if (z) *z = u.z;
}

void SetListenerVelocity(int idx, float x, float y, float z) {
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_engine_listener_set_velocity(&g_engine, (ma_uint32)idx, x, y, z);
}

void GetListenerVelocity(int idx, float* x, float* y, float* z) {
    if (x) *x = 0; if (y) *y = 0; if (z) *z = 0;
    if (!g_engineInit) return;
    if (idx < 0) idx = 0;
    ma_vec3f v = ma_engine_listener_get_velocity(&g_engine, (ma_uint32)idx);
    if (x) *x = v.x; if (y) *y = v.y; if (z) *z = v.z;
}

int GetListenerCount() {
    if (!g_engineInit) return 0;
    return (int)ma_engine_get_listener_count(&g_engine);
}

// ==================== Phase AT: Master / Engine ====================

void SetGlobalVolume(float v) {
    if (!g_engineInit) return;
    if (v < 0.0f) v = 0.0f;
    ma_engine_set_volume(&g_engine, v);
}

float GetGlobalVolume() {
    if (!g_engineInit) return 1.0f;
    return ma_engine_get_volume(&g_engine);
}

} // namespace AudioBackend
