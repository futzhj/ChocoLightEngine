/**
 * @file light_audio.cpp
 * @brief Light.Audio module - low-latency audio device + AudioStream API (SDL_audio) - Phase AM
 *
 * Lua API (51 fns + 16 const):
 *
 *   [Drivers] (3 fns)
 *     GetNumAudioDrivers()                 -> int
 *     GetAudioDriver(index)                -> string, err
 *     GetCurrentAudioDriver()              -> string, err
 *
 *   [Device discovery / query] (8 fns)
 *     GetAudioPlaybackDevices()            -> array<id>
 *     GetAudioRecordingDevices()           -> array<id>
 *     GetAudioDeviceName(devid)            -> string, err
 *     GetAudioDeviceFormat(devid)          -> spec, sample_frames, err
 *     GetAudioDeviceChannelMap(devid)      -> array<int>, err
 *     IsAudioDevicePhysical(devid)         -> bool
 *     IsAudioDevicePlayback(devid)         -> bool
 *     GetAudioDeviceGain(devid)            -> float, err
 *
 *   [Device control] (6 fns)
 *     OpenAudioDevice(devid, spec_table?)  -> opened_devid, err
 *     CloseAudioDevice(devid)              -> ok
 *     PauseAudioDevice(devid)              -> ok, err
 *     ResumeAudioDevice(devid)             -> ok, err
 *     AudioDevicePaused(devid)             -> bool
 *     SetAudioDeviceGain(devid, gain)      -> ok, err
 *
 *   [Stream lifecycle] (2 fns)
 *     CreateAudioStream(src_spec, dst_spec) -> handle, err
 *     DestroyAudioStream(handle)           -> ok
 *
 *   [Stream config] (10 fns)
 *     GetAudioStreamFormat(handle)         -> src_spec, dst_spec, err
 *     SetAudioStreamFormat(handle, src, dst) -> ok, err
 *     GetAudioStreamFrequencyRatio(handle) -> ratio, err
 *     SetAudioStreamFrequencyRatio(handle, ratio) -> ok, err
 *     GetAudioStreamGain(handle)           -> gain, err
 *     SetAudioStreamGain(handle, gain)     -> ok, err
 *     GetAudioStreamInputChannelMap(handle) -> array<int>, err
 *     GetAudioStreamOutputChannelMap(handle) -> array<int>, err
 *     SetAudioStreamInputChannelMap(handle, array<int>?) -> ok, err
 *     SetAudioStreamOutputChannelMap(handle, array<int>?) -> ok, err
 *
 *   [Stream binding] (5 fns)
 *     BindAudioStream(devid, handle)       -> ok, err
 *     BindAudioStreams(devid, array<handle>) -> ok, err
 *     UnbindAudioStream(handle)            -> ok
 *     UnbindAudioStreams(array<handle>)    -> ok
 *     GetAudioStreamDevice(handle)         -> devid, err
 *
 *   [Stream IO] (6 fns)
 *     PutAudioStreamData(handle, string)   -> ok, err
 *     GetAudioStreamData(handle, max_bytes) -> string, err  (returns "" at EOF)
 *     GetAudioStreamAvailable(handle)      -> int_bytes
 *     GetAudioStreamQueued(handle)         -> int_bytes
 *     ClearAudioStream(handle)             -> ok, err
 *     FlushAudioStream(handle)             -> ok, err
 *
 *   [Stream lock] (2 fns)
 *     LockAudioStream(handle)              -> ok, err
 *     UnlockAudioStream(handle)            -> ok, err
 *
 *   [Stream device control] (3 fns)
 *     PauseAudioStreamDevice(handle)       -> ok, err
 *     ResumeAudioStreamDevice(handle)      -> ok, err
 *     AudioStreamDevicePaused(handle)      -> bool
 *
 *   [Simplified] (1 fn)
 *     OpenAudioDeviceStream(devid, spec_table) -> handle, err  (no callback variant)
 *
 *   [WAV] (1 fn)
 *     LoadWAV(path)                        -> spec, audio_data_string, err
 *
 *   [Utilities] (4 fns)
 *     MixAudio(dst_string, src_string, format, volume) -> mixed_string, err
 *     ConvertAudioSamples(src_spec, src_string, dst_spec) -> dst_string, err
 *     GetAudioFormatName(format)           -> string
 *     GetSilenceValueForFormat(format)     -> int
 *
 * Constants (16):
 *   AUDIO_UNKNOWN, AUDIO_U8, AUDIO_S8, AUDIO_S16LE, AUDIO_S16BE, AUDIO_S32LE,
 *   AUDIO_S32BE, AUDIO_F32LE, AUDIO_F32BE, AUDIO_S16, AUDIO_S32, AUDIO_F32 (host)
 *   AUDIO_DEVICE_DEFAULT_PLAYBACK = 0xFFFFFFFF
 *   AUDIO_DEVICE_DEFAULT_RECORDING = 0xFFFFFFFE
 *   AUDIO_MASK_BITSIZE, AUDIO_MASK_FLOAT, AUDIO_MASK_BIG_ENDIAN, AUDIO_MASK_SIGNED
 *
 * Out-of-scope: callback variants (SetAudioPostmixCallback / SetAudioStream*Callback),
 * LoadWAV_IO (needs IOStream wrapper), GetAudioStreamProperties (Properties wrapper).
 *
 * Lazy init: EnsureAudioSubsystem() does SDL_INIT_AUDIO; coexists with miniaudio
 * (light_audio_backend.cpp) - they don't share subsystem state.
 *
 * AudioStream handles are lightuserdata (SDL_AudioStream*); device IDs are
 * lua_Number (Uint32 fits in 53-bit double).
 */

#include "light.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Helpers
// ============================================================

namespace {
    bool g_audioSubsysInit = false;

    // SDL3 v3.2.30 上游 bug workaround:
    //   SDL_GetAudioStreamInputChannelMap / OutputChannelMap 在 stream 未设过 chmap 时,
    //   内部调 SDL_ChannelMapDup(NULL, channels) -> memcpy(buf, NULL, len) -> access violation。
    //   docstring: SDL_audio.c:174, SDL_audiocvt.c:674
    //   workaround: 跟踪每个 stream 是否曾 SetXxxChannelMap 成功; 未设 -> Get 直接返回空表
    //   注: ChocoLight 单线程, 无需 mutex
    std::set<SDL_AudioStream*> g_streams_with_in_chmap;
    std::set<SDL_AudioStream*> g_streams_with_out_chmap;

    bool EnsureAudioSubsystem() {
        if (g_audioSubsysInit) return true;
        if (SDL_WasInit(SDL_INIT_AUDIO) != 0) {
            g_audioSubsysInit = true;
            return true;
        }
        // SDL_InitSubSystem 对 audio 在 dummy driver 平台 (CI headless) 也应该成功。
        // 失败常见原因:整个 SDL 还没 SDL_Init 过 -> 必须先 SDL_Init(0) 再 SDL_InitSubSystem。
        if (SDL_WasInit(0) == 0) {
            if (!SDL_Init(SDL_INIT_AUDIO)) {
                CC::Log(CC::LOG_WARN, "Light.Audio: SDL_Init(AUDIO) failed: %s", SDL_GetError());
                return false;
            }
        } else {
            if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
                CC::Log(CC::LOG_WARN, "Light.Audio: SDL_InitSubSystem(AUDIO) failed: %s", SDL_GetError());
                return false;
            }
        }
        g_audioSubsysInit = true;
        return true;
    }

    SDL_AudioStream* CheckStream(lua_State* L, int idx) {
        if (lua_type(L, idx) != LUA_TLIGHTUSERDATA) return nullptr;
        return static_cast<SDL_AudioStream*>(lua_touserdata(L, idx));
    }

    SDL_AudioDeviceID CheckDevID(lua_State* L, int idx) {
        if (lua_type(L, idx) != LUA_TNUMBER) return 0;
        return static_cast<SDL_AudioDeviceID>(lua_tonumber(L, idx));
    }

    // 推送 SDL_AudioSpec -> Lua table {format=..., channels=..., freq=...}
    void PushSpec(lua_State* L, const SDL_AudioSpec& spec) {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)spec.format); lua_setfield(L, -2, "format");
        lua_pushinteger(L, spec.channels);            lua_setfield(L, -2, "channels");
        lua_pushinteger(L, spec.freq);                lua_setfield(L, -2, "freq");
    }

    // 从 Lua table 读 SDL_AudioSpec; 返回是否成功
    bool ReadSpec(lua_State* L, int idx, SDL_AudioSpec* out) {
        if (lua_type(L, idx) != LUA_TTABLE) return false;
        out->format = SDL_AUDIO_S16;  // 默认
        out->channels = 2;
        out->freq = 44100;

        lua_getfield(L, idx, "format");
        if (lua_isnumber(L, -1)) out->format = (SDL_AudioFormat)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "channels");
        if (lua_isnumber(L, -1)) out->channels = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, idx, "freq");
        if (lua_isnumber(L, -1)) out->freq = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        return true;
    }

    // 推送 int* array (channel map) 到 Lua table
    void PushIntArray(lua_State* L, const int* arr, int count) {
        lua_newtable(L);
        if (!arr || count <= 0) return;
        for (int i = 0; i < count; ++i) {
            lua_pushinteger(L, arr[i]);
            lua_rawseti(L, -2, i + 1);
        }
    }

    // 从 Lua table 读 int array; 返回长度, *out_buf 由调用者 free
    int ReadIntArray(lua_State* L, int idx, std::vector<int>& buf) {
        buf.clear();
        if (lua_type(L, idx) != LUA_TTABLE) return 0;
        int n = (int)lua_objlen(L, idx);
        buf.reserve(n);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, idx, i);
            buf.push_back((int)lua_tointeger(L, -1));
            lua_pop(L, 1);
        }
        return (int)buf.size();
    }

    // 复刻 SDL3 内部 SDL_ChannelMapIsDefault: identity mapping {0,1,...,n-1}
    // SDL3 在 SetXxxChannelMap 时会把 default mapping 静默降为 NULL,
    // 我们 wrapper 必须做同样的判断才能保持 g_streams_with_*_chmap 状态一致
    bool IsDefaultChannelMap(const int* map, int n) {
        if (!map || n <= 0) return true;
        for (int i = 0; i < n; ++i) {
            if (map[i] != i) return false;
        }
        return true;
    }
}

// ============================================================
// Drivers (3 fns)
// ============================================================

static int l_Audio_GetNumAudioDrivers(lua_State* L) {
    lua_pushinteger(L, SDL_GetNumAudioDrivers());
    return 1;
}

static int l_Audio_GetAudioDriver(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L); lua_pushstring(L, "index must be a number"); return 2;
    }
    int idx = (int)lua_tointeger(L, 1);
    const char* name = SDL_GetAudioDriver(idx);
    if (!name) { lua_pushnil(L); lua_pushstring(L, "invalid driver index"); return 2; }
    lua_pushstring(L, name); lua_pushnil(L); return 2;
}

static int l_Audio_GetCurrentAudioDriver(lua_State* L) {
    const char* name = SDL_GetCurrentAudioDriver();
    if (!name) { lua_pushnil(L); lua_pushstring(L, "no audio driver initialized"); return 2; }
    lua_pushstring(L, name); lua_pushnil(L); return 2;
}

// ============================================================
// Device discovery / query (8 fns)
// ============================================================

static int PushDeviceArray(lua_State* L, SDL_AudioDeviceID* (*fn)(int*)) {
    if (!EnsureAudioSubsystem()) { lua_newtable(L); return 1; }
    int count = 0;
    SDL_AudioDeviceID* ids = fn(&count);
    lua_newtable(L);
    if (!ids || count <= 0) {
        if (ids) SDL_free(ids);
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}

static int l_Audio_GetAudioPlaybackDevices(lua_State* L) {
    return PushDeviceArray(L, SDL_GetAudioPlaybackDevices);
}

static int l_Audio_GetAudioRecordingDevices(lua_State* L) {
    return PushDeviceArray(L, SDL_GetAudioRecordingDevices);
}

static int l_Audio_GetAudioDeviceName(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!EnsureAudioSubsystem()) { lua_pushnil(L); lua_pushstring(L, "audio subsystem unavailable"); return 2; }
    const char* name = SDL_GetAudioDeviceName(id);
    if (!name) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "invalid device id");
        return 2;
    }
    lua_pushstring(L, name); lua_pushnil(L); return 2;
}

static int l_Audio_GetAudioDeviceFormat(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 3; }
    if (!EnsureAudioSubsystem()) {
        lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, "audio subsystem unavailable"); return 3;
    }
    SDL_AudioSpec spec;
    int sample_frames = 0;
    if (!SDL_GetAudioDeviceFormat(id, &spec, &sample_frames)) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 3;
    }
    PushSpec(L, spec);
    lua_pushinteger(L, sample_frames);
    lua_pushnil(L);
    return 3;
}

static int l_Audio_GetAudioDeviceChannelMap(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!EnsureAudioSubsystem()) { lua_pushnil(L); lua_pushstring(L, "audio subsystem unavailable"); return 2; }
    int count = 0;
    int* map = SDL_GetAudioDeviceChannelMap(id, &count);
    if (!map) { lua_newtable(L); lua_pushnil(L); return 2; }  // null = default mapping
    PushIntArray(L, map, count);
    SDL_free(map);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_IsAudioDevicePhysical(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_IsAudioDevicePhysical(id) ? 1 : 0);
    return 1;
}

static int l_Audio_IsAudioDevicePlayback(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_IsAudioDevicePlayback(id) ? 1 : 0);
    return 1;
}

static int l_Audio_GetAudioDeviceGain(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 2; }
    float g = SDL_GetAudioDeviceGain(id);
    if (g < 0.0f) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnumber(L, (lua_Number)g); lua_pushnil(L); return 2;
}

// ============================================================
// Device control (6 fns)
// ============================================================

static int l_Audio_OpenAudioDevice(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!EnsureAudioSubsystem()) { lua_pushnil(L); lua_pushstring(L, "audio subsystem unavailable"); return 2; }

    SDL_AudioSpec spec;
    SDL_AudioSpec* spec_ptr = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE) {
        if (ReadSpec(L, 2, &spec)) spec_ptr = &spec;
    }

    SDL_AudioDeviceID opened = SDL_OpenAudioDevice(id, spec_ptr);
    if (opened == 0) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushnumber(L, (lua_Number)opened);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_CloseAudioDevice(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "devid must be a number"); return 2; }
    SDL_CloseAudioDevice(id);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_Audio_PauseAudioDevice(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!SDL_PauseAudioDevice(id)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_ResumeAudioDevice(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!SDL_ResumeAudioDevice(id)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_AudioDevicePaused(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_AudioDevicePaused(id) ? 1 : 0);
    return 1;
}

static int l_Audio_SetAudioDeviceGain(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "devid must be a number"); return 2; }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        lua_pushboolean(L, 0); lua_pushstring(L, "gain must be a number"); return 2;
    }
    float gain = (float)lua_tonumber(L, 2);
    if (!SDL_SetAudioDeviceGain(id, gain)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// Stream lifecycle (2 fns)
// ============================================================

static int l_Audio_CreateAudioStream(lua_State* L) {
    // SDL3 内部某些 stream 操作依赖 audio subsystem 初始化的全局 state,
    // 所以 stream 创建路径也需要 EnsureAudioSubsystem (否则部分平台会 crash)
    if (!EnsureAudioSubsystem()) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "audio subsystem unavailable");
        return 2;
    }
    SDL_AudioSpec src, dst;
    SDL_AudioSpec *src_p = nullptr, *dst_p = nullptr;
    if (lua_type(L, 1) == LUA_TTABLE && ReadSpec(L, 1, &src)) src_p = &src;
    if (lua_type(L, 2) == LUA_TTABLE && ReadSpec(L, 2, &dst)) dst_p = &dst;
    SDL_AudioStream* s = SDL_CreateAudioStream(src_p, dst_p);
    if (!s) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushlightuserdata(L, s);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_DestroyAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    // 清理 chmap workaround state, 避免 stale pointer
    g_streams_with_in_chmap.erase(s);
    g_streams_with_out_chmap.erase(s);
    SDL_DestroyAudioStream(s);
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// Stream config (10 fns)
// ============================================================

static int l_Audio_GetAudioStreamFormat(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 3; }
    SDL_AudioSpec src, dst;
    if (!SDL_GetAudioStreamFormat(s, &src, &dst)) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 3;
    }
    PushSpec(L, src);
    PushSpec(L, dst);
    lua_pushnil(L);
    return 3;
}

static int l_Audio_SetAudioStreamFormat(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    SDL_AudioSpec src, dst;
    SDL_AudioSpec *src_p = nullptr, *dst_p = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE && ReadSpec(L, 2, &src)) src_p = &src;
    if (lua_type(L, 3) == LUA_TTABLE && ReadSpec(L, 3, &dst)) dst_p = &dst;
    if (!SDL_SetAudioStreamFormat(s, src_p, dst_p)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_GetAudioStreamFrequencyRatio(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    float ratio = SDL_GetAudioStreamFrequencyRatio(s);
    if (ratio <= 0.0f) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnumber(L, (lua_Number)ratio); lua_pushnil(L); return 2;
}

static int l_Audio_SetAudioStreamFrequencyRatio(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        lua_pushboolean(L, 0); lua_pushstring(L, "ratio must be a number"); return 2;
    }
    float r = (float)lua_tonumber(L, 2);
    if (!SDL_SetAudioStreamFrequencyRatio(s, r)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_GetAudioStreamGain(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    float g = SDL_GetAudioStreamGain(s);
    if (g < 0.0f) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnumber(L, (lua_Number)g); lua_pushnil(L); return 2;
}

static int l_Audio_SetAudioStreamGain(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        lua_pushboolean(L, 0); lua_pushstring(L, "gain must be a number"); return 2;
    }
    float g = (float)lua_tonumber(L, 2);
    if (!SDL_SetAudioStreamGain(s, g)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_GetAudioStreamInputChannelMap(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    // SDL3 v3.2.30 bug workaround: stream 未设 in chmap 时 SDL 内部 NULL deref
    if (g_streams_with_in_chmap.find(s) == g_streams_with_in_chmap.end()) {
        lua_newtable(L); lua_pushnil(L); return 2;  // 默认无 mapping
    }
    int count = 0;
    int* map = SDL_GetAudioStreamInputChannelMap(s, &count);
    if (!map) { lua_newtable(L); lua_pushnil(L); return 2; }
    PushIntArray(L, map, count);
    SDL_free(map);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_GetAudioStreamOutputChannelMap(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    // SDL3 v3.2.30 bug workaround: stream 未设 out chmap 时 SDL 内部 NULL deref
    if (g_streams_with_out_chmap.find(s) == g_streams_with_out_chmap.end()) {
        lua_newtable(L); lua_pushnil(L); return 2;  // 默认无 mapping
    }
    int count = 0;
    int* map = SDL_GetAudioStreamOutputChannelMap(s, &count);
    if (!map) { lua_newtable(L); lua_pushnil(L); return 2; }
    PushIntArray(L, map, count);
    SDL_free(map);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_SetAudioStreamInputChannelMap(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    std::vector<int> buf;
    int count = ReadIntArray(L, 2, buf);
    int* arr = (count > 0) ? buf.data() : nullptr;
    if (!SDL_SetAudioStreamInputChannelMap(s, arr, count)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    // 跟踪 chmap 状态: nil/空/default identity 都会被 SDL3 降为 NULL src_chmap,
    // 只有 non-default mapping 才会真正设置 (避免后续 Get 触发 NULL deref bug)
    if (count > 0 && !IsDefaultChannelMap(arr, count)) {
        g_streams_with_in_chmap.insert(s);
    } else {
        g_streams_with_in_chmap.erase(s);
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_SetAudioStreamOutputChannelMap(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    std::vector<int> buf;
    int count = ReadIntArray(L, 2, buf);
    int* arr = (count > 0) ? buf.data() : nullptr;
    if (!SDL_SetAudioStreamOutputChannelMap(s, arr, count)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    // 与 SDL3 ChannelMapIsDefault 逻辑一致: default mapping 被降为 NULL
    if (count > 0 && !IsDefaultChannelMap(arr, count)) {
        g_streams_with_out_chmap.insert(s);
    } else {
        g_streams_with_out_chmap.erase(s);
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// Stream binding (5 fns)
// ============================================================

static int l_Audio_BindAudioStream(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    SDL_AudioStream* s = CheckStream(L, 2);
    if (id == 0 || !s) {
        lua_pushboolean(L, 0); lua_pushstring(L, "invalid devid or stream handle"); return 2;
    }
    if (!SDL_BindAudioStream(id, s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_BindAudioStreams(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0 || lua_type(L, 2) != LUA_TTABLE) {
        lua_pushboolean(L, 0); lua_pushstring(L, "invalid devid or streams array"); return 2;
    }
    int n = (int)lua_objlen(L, 2);
    if (n <= 0) { lua_pushboolean(L, 1); lua_pushnil(L); return 2; }  // no-op
    std::vector<SDL_AudioStream*> arr;
    arr.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 2, i);
        SDL_AudioStream* s = (lua_type(L, -1) == LUA_TLIGHTUSERDATA)
            ? static_cast<SDL_AudioStream*>(lua_touserdata(L, -1)) : nullptr;
        lua_pop(L, 1);
        if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "array contains invalid stream"); return 2; }
        arr.push_back(s);
    }
    if (!SDL_BindAudioStreams(id, arr.data(), (int)arr.size())) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_UnbindAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    SDL_UnbindAudioStream(s);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_Audio_UnbindAudioStreams(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        lua_pushboolean(L, 0); lua_pushstring(L, "expected streams array"); return 2;
    }
    int n = (int)lua_objlen(L, 1);
    if (n <= 0) { lua_pushboolean(L, 1); return 1; }
    std::vector<SDL_AudioStream*> arr;
    arr.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, 1, i);
        SDL_AudioStream* s = (lua_type(L, -1) == LUA_TLIGHTUSERDATA)
            ? static_cast<SDL_AudioStream*>(lua_touserdata(L, -1)) : nullptr;
        lua_pop(L, 1);
        if (s) arr.push_back(s);
    }
    if (!arr.empty()) {
        SDL_UnbindAudioStreams(arr.data(), (int)arr.size());
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_Audio_GetAudioStreamDevice(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    SDL_AudioDeviceID id = SDL_GetAudioStreamDevice(s);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "stream not bound"); return 2; }
    lua_pushnumber(L, (lua_Number)id);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// Stream IO (6 fns)
// ============================================================

static int l_Audio_PutAudioStreamData(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    size_t len = 0;
    const char* data = lua_tolstring(L, 2, &len);
    if (!data || len == 0) {
        // 空数据合法 (no-op)
        lua_pushboolean(L, 1); lua_pushnil(L); return 2;
    }
    if (!SDL_PutAudioStreamData(s, data, (int)len)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_GetAudioStreamData(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushnil(L); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (lua_type(L, 2) != LUA_TNUMBER) {
        lua_pushnil(L); lua_pushstring(L, "max_bytes must be a number"); return 2;
    }
    int max_bytes = (int)lua_tointeger(L, 2);
    if (max_bytes < 0) max_bytes = 0;
    if (max_bytes > 16 * 1024 * 1024) max_bytes = 16 * 1024 * 1024;  // 16 MB cap
    std::vector<char> buf(max_bytes > 0 ? (size_t)max_bytes : 1);
    int got = SDL_GetAudioStreamData(s, buf.data(), max_bytes);
    if (got < 0) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushlstring(L, buf.data(), (size_t)got);
    lua_pushnil(L);
    return 2;
}

static int l_Audio_GetAudioStreamAvailable(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushinteger(L, -1); return 1; }
    lua_pushinteger(L, SDL_GetAudioStreamAvailable(s));
    return 1;
}

static int l_Audio_GetAudioStreamQueued(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushinteger(L, -1); return 1; }
    lua_pushinteger(L, SDL_GetAudioStreamQueued(s));
    return 1;
}

static int l_Audio_ClearAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_ClearAudioStream(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_FlushAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_FlushAudioStream(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// Stream lock (2 fns)
// ============================================================

static int l_Audio_LockAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_LockAudioStream(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_UnlockAudioStream(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_UnlockAudioStream(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// Stream device control (3 fns)
// ============================================================

static int l_Audio_PauseAudioStreamDevice(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_PauseAudioStreamDevice(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_ResumeAudioStreamDevice(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid stream handle"); return 2; }
    if (!SDL_ResumeAudioStreamDevice(s)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Audio_AudioStreamDevicePaused(lua_State* L) {
    SDL_AudioStream* s = CheckStream(L, 1);
    if (!s) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_AudioStreamDevicePaused(s) ? 1 : 0);
    return 1;
}

// ============================================================
// Simplified (1 fn)
// ============================================================

static int l_Audio_OpenAudioDeviceStream(lua_State* L) {
    SDL_AudioDeviceID id = CheckDevID(L, 1);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, "devid must be a number"); return 2; }
    if (!EnsureAudioSubsystem()) { lua_pushnil(L); lua_pushstring(L, "audio subsystem unavailable"); return 2; }
    SDL_AudioSpec spec;
    SDL_AudioSpec* spec_ptr = nullptr;
    if (lua_type(L, 2) == LUA_TTABLE && ReadSpec(L, 2, &spec)) spec_ptr = &spec;
    // No-callback variant: pass NULL callback
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(id, spec_ptr, nullptr, nullptr);
    if (!s) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushlightuserdata(L, s);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// WAV (1 fn)
// ============================================================

static int l_Audio_LoadWAV(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, "path must be a string"); return 3;
    }
    const char* path = lua_tostring(L, 1);
    SDL_AudioSpec spec;
    Uint8* audio_buf = nullptr;
    Uint32 audio_len = 0;
    if (!SDL_LoadWAV(path, &spec, &audio_buf, &audio_len)) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 3;
    }
    PushSpec(L, spec);
    lua_pushlstring(L, reinterpret_cast<const char*>(audio_buf), (size_t)audio_len);
    lua_pushnil(L);
    SDL_free(audio_buf);
    return 3;
}

// ============================================================
// Utilities (4 fns)
// ============================================================

static int l_Audio_MixAudio(lua_State* L) {
    if (lua_type(L, 1) != LUA_TSTRING || lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L); lua_pushstring(L, "dst and src must be strings"); return 2;
    }
    if (lua_type(L, 3) != LUA_TNUMBER || lua_type(L, 4) != LUA_TNUMBER) {
        lua_pushnil(L); lua_pushstring(L, "format and volume must be numbers"); return 2;
    }
    size_t dst_len = 0, src_len = 0;
    const char* dst = lua_tolstring(L, 1, &dst_len);
    const char* src = lua_tolstring(L, 2, &src_len);
    SDL_AudioFormat fmt = (SDL_AudioFormat)lua_tointeger(L, 3);
    float vol = (float)lua_tonumber(L, 4);
    size_t mix_len = std::min(dst_len, src_len);
    if (mix_len == 0) {
        lua_pushlstring(L, dst, dst_len); lua_pushnil(L); return 2;
    }
    std::vector<Uint8> buf(dst_len);
    SDL_memcpy(buf.data(), dst, dst_len);
    if (!SDL_MixAudio(buf.data(), (const Uint8*)src, fmt, (Uint32)mix_len, vol)) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(buf.data()), buf.size());
    lua_pushnil(L);
    return 2;
}

static int l_Audio_ConvertAudioSamples(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE || lua_type(L, 3) != LUA_TTABLE) {
        lua_pushnil(L); lua_pushstring(L, "src_spec and dst_spec must be tables"); return 2;
    }
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L); lua_pushstring(L, "src data must be a string"); return 2;
    }
    SDL_AudioSpec src_spec, dst_spec;
    if (!ReadSpec(L, 1, &src_spec) || !ReadSpec(L, 3, &dst_spec)) {
        lua_pushnil(L); lua_pushstring(L, "invalid spec table"); return 2;
    }
    size_t src_len = 0;
    const char* src = lua_tolstring(L, 2, &src_len);
    Uint8* dst_data = nullptr;
    int dst_len = 0;
    if (!SDL_ConvertAudioSamples(&src_spec, (const Uint8*)src, (int)src_len,
                                 &dst_spec, &dst_data, &dst_len)) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(dst_data), (size_t)dst_len);
    lua_pushnil(L);
    SDL_free(dst_data);
    return 2;
}

static int l_Audio_GetAudioFormatName(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushstring(L, "AUDIO_UNKNOWN"); return 1;
    }
    SDL_AudioFormat fmt = (SDL_AudioFormat)lua_tointeger(L, 1);
    const char* name = SDL_GetAudioFormatName(fmt);
    lua_pushstring(L, name ? name : "AUDIO_UNKNOWN");
    return 1;
}

static int l_Audio_GetSilenceValueForFormat(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) { lua_pushinteger(L, 0); return 1; }
    SDL_AudioFormat fmt = (SDL_AudioFormat)lua_tointeger(L, 1);
    lua_pushinteger(L, SDL_GetSilenceValueForFormat(fmt));
    return 1;
}

// ============================================================
// luaopen_Light_Audio
// ============================================================

static const luaL_Reg kAudioReg[] = {
    // Drivers
    { "GetNumAudioDrivers",              l_Audio_GetNumAudioDrivers              },
    { "GetAudioDriver",                  l_Audio_GetAudioDriver                  },
    { "GetCurrentAudioDriver",           l_Audio_GetCurrentAudioDriver           },
    // Device discovery / query
    { "GetAudioPlaybackDevices",         l_Audio_GetAudioPlaybackDevices         },
    { "GetAudioRecordingDevices",        l_Audio_GetAudioRecordingDevices        },
    { "GetAudioDeviceName",              l_Audio_GetAudioDeviceName              },
    { "GetAudioDeviceFormat",            l_Audio_GetAudioDeviceFormat            },
    { "GetAudioDeviceChannelMap",        l_Audio_GetAudioDeviceChannelMap        },
    { "IsAudioDevicePhysical",           l_Audio_IsAudioDevicePhysical           },
    { "IsAudioDevicePlayback",           l_Audio_IsAudioDevicePlayback           },
    { "GetAudioDeviceGain",              l_Audio_GetAudioDeviceGain              },
    // Device control
    { "OpenAudioDevice",                 l_Audio_OpenAudioDevice                 },
    { "CloseAudioDevice",                l_Audio_CloseAudioDevice                },
    { "PauseAudioDevice",                l_Audio_PauseAudioDevice                },
    { "ResumeAudioDevice",               l_Audio_ResumeAudioDevice               },
    { "AudioDevicePaused",               l_Audio_AudioDevicePaused               },
    { "SetAudioDeviceGain",              l_Audio_SetAudioDeviceGain              },
    // Stream lifecycle
    { "CreateAudioStream",               l_Audio_CreateAudioStream               },
    { "DestroyAudioStream",              l_Audio_DestroyAudioStream              },
    // Stream config
    { "GetAudioStreamFormat",            l_Audio_GetAudioStreamFormat            },
    { "SetAudioStreamFormat",            l_Audio_SetAudioStreamFormat            },
    { "GetAudioStreamFrequencyRatio",    l_Audio_GetAudioStreamFrequencyRatio    },
    { "SetAudioStreamFrequencyRatio",    l_Audio_SetAudioStreamFrequencyRatio    },
    { "GetAudioStreamGain",              l_Audio_GetAudioStreamGain              },
    { "SetAudioStreamGain",              l_Audio_SetAudioStreamGain              },
    { "GetAudioStreamInputChannelMap",   l_Audio_GetAudioStreamInputChannelMap   },
    { "GetAudioStreamOutputChannelMap",  l_Audio_GetAudioStreamOutputChannelMap  },
    { "SetAudioStreamInputChannelMap",   l_Audio_SetAudioStreamInputChannelMap   },
    { "SetAudioStreamOutputChannelMap",  l_Audio_SetAudioStreamOutputChannelMap  },
    // Stream binding
    { "BindAudioStream",                 l_Audio_BindAudioStream                 },
    { "BindAudioStreams",                l_Audio_BindAudioStreams                },
    { "UnbindAudioStream",               l_Audio_UnbindAudioStream               },
    { "UnbindAudioStreams",              l_Audio_UnbindAudioStreams              },
    { "GetAudioStreamDevice",            l_Audio_GetAudioStreamDevice            },
    // Stream IO
    { "PutAudioStreamData",              l_Audio_PutAudioStreamData              },
    { "GetAudioStreamData",              l_Audio_GetAudioStreamData              },
    { "GetAudioStreamAvailable",         l_Audio_GetAudioStreamAvailable         },
    { "GetAudioStreamQueued",            l_Audio_GetAudioStreamQueued            },
    { "ClearAudioStream",                l_Audio_ClearAudioStream                },
    { "FlushAudioStream",                l_Audio_FlushAudioStream                },
    // Stream lock
    { "LockAudioStream",                 l_Audio_LockAudioStream                 },
    { "UnlockAudioStream",               l_Audio_UnlockAudioStream               },
    // Stream device control
    { "PauseAudioStreamDevice",          l_Audio_PauseAudioStreamDevice          },
    { "ResumeAudioStreamDevice",         l_Audio_ResumeAudioStreamDevice         },
    { "AudioStreamDevicePaused",         l_Audio_AudioStreamDevicePaused         },
    // Simplified
    { "OpenAudioDeviceStream",           l_Audio_OpenAudioDeviceStream           },
    // WAV
    { "LoadWAV",                         l_Audio_LoadWAV                         },
    // Utilities
    { "MixAudio",                        l_Audio_MixAudio                        },
    { "ConvertAudioSamples",             l_Audio_ConvertAudioSamples             },
    { "GetAudioFormatName",              l_Audio_GetAudioFormatName              },
    { "GetSilenceValueForFormat",        l_Audio_GetSilenceValueForFormat        },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Audio(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kAudioReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // ===== Constants =====
    // SDL_AudioFormat enum
    lua_pushinteger(L, SDL_AUDIO_UNKNOWN); lua_setfield(L, -2, "AUDIO_UNKNOWN");
    lua_pushinteger(L, SDL_AUDIO_U8);      lua_setfield(L, -2, "AUDIO_U8");
    lua_pushinteger(L, SDL_AUDIO_S8);      lua_setfield(L, -2, "AUDIO_S8");
    lua_pushinteger(L, SDL_AUDIO_S16LE);   lua_setfield(L, -2, "AUDIO_S16LE");
    lua_pushinteger(L, SDL_AUDIO_S16BE);   lua_setfield(L, -2, "AUDIO_S16BE");
    lua_pushinteger(L, SDL_AUDIO_S32LE);   lua_setfield(L, -2, "AUDIO_S32LE");
    lua_pushinteger(L, SDL_AUDIO_S32BE);   lua_setfield(L, -2, "AUDIO_S32BE");
    lua_pushinteger(L, SDL_AUDIO_F32LE);   lua_setfield(L, -2, "AUDIO_F32LE");
    lua_pushinteger(L, SDL_AUDIO_F32BE);   lua_setfield(L, -2, "AUDIO_F32BE");
    lua_pushinteger(L, SDL_AUDIO_S16);     lua_setfield(L, -2, "AUDIO_S16");
    lua_pushinteger(L, SDL_AUDIO_S32);     lua_setfield(L, -2, "AUDIO_S32");
    lua_pushinteger(L, SDL_AUDIO_F32);     lua_setfield(L, -2, "AUDIO_F32");

    // Device default values (Uint32, fits in lua_Number)
    lua_pushnumber(L, (lua_Number)SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
    lua_setfield(L, -2, "AUDIO_DEVICE_DEFAULT_PLAYBACK");
    lua_pushnumber(L, (lua_Number)SDL_AUDIO_DEVICE_DEFAULT_RECORDING);
    lua_setfield(L, -2, "AUDIO_DEVICE_DEFAULT_RECORDING");

    // Format masks
    lua_pushinteger(L, (lua_Integer)SDL_AUDIO_MASK_BITSIZE);     lua_setfield(L, -2, "AUDIO_MASK_BITSIZE");
    lua_pushinteger(L, (lua_Integer)SDL_AUDIO_MASK_FLOAT);       lua_setfield(L, -2, "AUDIO_MASK_FLOAT");
    lua_pushinteger(L, (lua_Integer)SDL_AUDIO_MASK_BIG_ENDIAN);  lua_setfield(L, -2, "AUDIO_MASK_BIG_ENDIAN");
    lua_pushinteger(L, (lua_Integer)SDL_AUDIO_MASK_SIGNED);      lua_setfield(L, -2, "AUDIO_MASK_SIGNED");

    return 1;
}
