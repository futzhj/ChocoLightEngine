/**
 * @file light_audio_sound.cpp
 * @brief Light.Audio.Sound — 高层 sound 资源 (3D 空间化 + group/effect 路由)
 *
 * Phase AT.1 — 暴露 miniaudio engine 的 sound 能力到 Lua, 含 3D 位置/距离衰减/
 *              立体声 panning/音调/loop, 并集成 SoundGroup (Mixer) + Effect (滤波器/Echo).
 *
 * Lua API (25+ 方法):
 *   Light.Audio.Sound.Load(path) -> Sound|nil [, err]
 *   Light.Audio.Sound.LoadPCM(data, format, channels, sampleRate) -> Sound|nil [, err]
 *
 *   sound:Play() / Pause() / Stop() / IsPlaying() -> bool
 *   sound:SetVolume(v) / GetVolume() -> v
 *   sound:SetLooping(b) / GetLooping() -> bool
 *   sound:SetPitch(p) / GetPitch() -> p
 *   sound:SetPan(pan) / GetPan() -> pan
 *
 *   sound:Set3DEnabled(bool) / Get3DEnabled() -> bool
 *   sound:SetPosition(x,y,z) / GetPosition() -> x,y,z
 *   sound:SetVelocity(x,y,z)
 *   sound:SetAttenuationModel("none"|"inverse"|"linear"|"exp") / GetAttenuationModel() -> string
 *   sound:SetMinDistance(d) / GetMinDistance() -> d
 *   sound:SetMaxDistance(d) / GetMaxDistance() -> d
 *   sound:SetRolloff(r) / GetRolloff() -> r
 *
 *   sound:SetGroup(group | nil)
 *   sound:SetEffect(effect | nil)
 *
 *   sound:Delete() / __gc / __tostring
 *
 * 内部:
 *   - userdata 持 AudioHandle* + groupRef/effectRef (Lua registry ref)
 *   - Sound 持有 group/effect 的 Lua ref 防止 GC, __gc 时释放 ref + AudioBackend::Free
 *   - Volume/Pitch/Pan/Position 等直接转发 AudioBackend
 */

#include "light.h"
#include "light_audio_backend.h"
#include "asset_loader.h"

#include <cstdint>
#include <cstring>
#include <utility>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 元表名
static const char* SOUND_MT = "Light.Audio.Sound";
static const char* GROUP_MT = "Light.Audio.SoundGroup";    // 跨模块共享
static const char* EFFECT_MT = "Light.Audio.Effect";       // 跨模块共享

// Sound userdata
struct SoundUserdata {
    AudioHandle* h;
    int groupRef;    // LUA_NOREF / LUA_REFNIL / registry index
    int effectRef;
};

// GroupUserdata 与 EffectUserdata 在 light_audio_group.cpp / light_audio_effect.cpp 定义,
// 但其首字段必须是对应 handle pointer, 这里通过 luaL_testudata 安全访问.
// 我们仅依赖 first field = handle pointer 的内存布局假设
struct GroupUserdata_Shared {
    GroupHandle* h;
    int parentRef;
    int effectRef;
};
struct EffectUserdata_Shared {
    EffectHandle* h;
};

static SoundUserdata* CheckSound(lua_State* L, int idx) {
    return (SoundUserdata*)luaL_checkudata(L, idx, SOUND_MT);
}

// ==================== Sound.Load(path) ====================

static int l_Sound_Load(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!AudioBackend::Init()) {
        lua_pushnil(L);
        lua_pushstring(L, "AudioBackend init failed (no audio device?)");
        return 2;
    }
    AudioHandle* h = AudioBackend::LoadFile(path);
    if (!h) {
        lua_pushnil(L);
        lua_pushfstring(L, "failed to load audio file: %s", path);
        return 2;
    }
    SoundUserdata* ud = (SoundUserdata*)lua_newuserdata(L, sizeof(SoundUserdata));
    ud->h = h;
    ud->groupRef = LUA_NOREF;
    ud->effectRef = LUA_NOREF;
    luaL_getmetatable(L, SOUND_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// Phase G.1.5 — 改为 int 返 push 数量 (Sound: 默认 1)
static int SoundPushResult_(void* L_, AssetLoader::FutureState* state) {
    lua_State* L = (lua_State*)L_;
    if (!L) return 0;
    if (!state || state->status.load() != (int)AssetLoader::FutureStatus::Ready || !state->resSoundHandle) {
        lua_pushnil(L);
        return 1;
    }
    SoundUserdata* ud = (SoundUserdata*)lua_newuserdata(L, sizeof(SoundUserdata));
    ud->h = (AudioHandle*)state->resSoundHandle;
    ud->groupRef = LUA_NOREF;
    ud->effectRef = LUA_NOREF;
    state->resSoundHandle = nullptr;
    luaL_getmetatable(L, SOUND_MT);
    lua_setmetatable(L, -2);
    return 1;
}

static void SoundAsyncDispatcher_(void* L_, AssetLoader::FutureState* state, int cbLuaRef) {
    lua_State* L = (lua_State*)L_;
    if (!L || !state || cbLuaRef < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbLuaRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (state->status.load() == (int)AssetLoader::FutureStatus::Ready) {
        SoundPushResult_(L, state);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, state->errorMsg.empty() ? "unknown error" : state->errorMsg.c_str());
    }
    if (lua_pcall(L, 2, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Sound LoadAsync cb error: %s", err ? err : "(none)");
        lua_pop(L, 1);
    }
}

static int l_Sound_LoadAsync(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int cbRef = -1;
    if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    auto state = AssetLoader::LoadSoundAsync(path);
    AssetLoader::RegisterResultPusher(state, SoundPushResult_);
    if (cbRef >= 0) {
        AssetLoader::RegisterCallback(state, SoundAsyncDispatcher_, L, cbRef);
        if (state->status.load() != (int)AssetLoader::FutureStatus::Pending) {
            SoundAsyncDispatcher_(L, state.get(), cbRef);
            luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
            state->dispatcher = nullptr;
            state->cbLuaRef = -1;
            state->cbLuaState = nullptr;
        }
    }
    return AssetLoader::PushAsyncFuture(L, std::move(state));
}

// ==================== Sound.LoadPCM(data, fmt, ch, rate) ====================

static int l_Sound_LoadPCM(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    const char* fmtStr = luaL_checkstring(L, 2);
    int channels = (int)luaL_checkinteger(L, 3);
    int sampleRate = (int)luaL_checkinteger(L, 4);
    if (!AudioBackend::Init()) {
        lua_pushnil(L);
        lua_pushstring(L, "AudioBackend init failed");
        return 2;
    }
    int fmt = 2;  // s16 default
    if (strcmp(fmtStr, "u8") == 0) fmt = 1;
    else if (strcmp(fmtStr, "s16") == 0) fmt = 2;
    else if (strcmp(fmtStr, "s32") == 0) fmt = 3;
    else if (strcmp(fmtStr, "f32") == 0) fmt = 5;
    else {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown format '%s' (expected u8/s16/s32/f32)", fmtStr);
        return 2;
    }
    int bytesPerSample = (fmt == 1) ? 1 : (fmt == 2 ? 2 : 4);
    int frameSize = bytesPerSample * channels;
    if (frameSize <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "channels must be > 0");
        return 2;
    }
    uint64_t frameCount = len / (size_t)frameSize;
    if (frameCount == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "data too short for given format");
        return 2;
    }
    AudioHandle* h = AudioBackend::LoadPCM(data, frameCount, fmt, channels, sampleRate);
    if (!h) {
        lua_pushnil(L);
        lua_pushstring(L, "LoadPCM failed");
        return 2;
    }
    SoundUserdata* ud = (SoundUserdata*)lua_newuserdata(L, sizeof(SoundUserdata));
    ud->h = h;
    ud->groupRef = LUA_NOREF;
    ud->effectRef = LUA_NOREF;
    luaL_getmetatable(L, SOUND_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== 播放控制 ====================

static int l_Sound_Play(lua_State* L)      { auto* ud = CheckSound(L, 1); AudioBackend::Play(ud->h); return 0; }
static int l_Sound_Pause(lua_State* L)     { auto* ud = CheckSound(L, 1); AudioBackend::Pause(ud->h); return 0; }
static int l_Sound_Stop(lua_State* L)      { auto* ud = CheckSound(L, 1); AudioBackend::Stop(ud->h); return 0; }
static int l_Sound_IsPlaying(lua_State* L) { auto* ud = CheckSound(L, 1); lua_pushboolean(L, AudioBackend::IsPlaying(ud->h) ? 1 : 0); return 1; }

static int l_Sound_SetVolume(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetVolume(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetVolume(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetVolume(ud->h));
    return 1;
}

static int l_Sound_SetLooping(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetLooping(ud->h, lua_toboolean(L, 2) != 0);
    return 0;
}
static int l_Sound_GetLooping(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushboolean(L, AudioBackend::GetLooping(ud->h) ? 1 : 0);
    return 1;
}

static int l_Sound_SetPitch(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetPitch(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetPitch(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetPitch(ud->h));
    return 1;
}

static int l_Sound_SetPan(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetPan(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetPan(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetPan(ud->h));
    return 1;
}

// ==================== 3D 空间化 ====================

static int l_Sound_Set3DEnabled(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetSpatializationEnabled(ud->h, lua_toboolean(L, 2) != 0);
    return 0;
}
static int l_Sound_Get3DEnabled(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushboolean(L, AudioBackend::GetSpatializationEnabled(ud->h) ? 1 : 0);
    return 1;
}

static int l_Sound_SetPosition(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    AudioBackend::SetPosition(ud->h, x, y, z);
    return 0;
}
static int l_Sound_GetPosition(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    float x, y, z;
    AudioBackend::GetPosition(ud->h, &x, &y, &z);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    lua_pushnumber(L, z);
    return 3;
}

static int l_Sound_SetVelocity(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    AudioBackend::SetVelocity(ud->h, x, y, z);
    return 0;
}

// 字符串 <-> int 衰减模型
static int ParseAttenuation(const char* s) {
    if (!s) return 1;
    if (strcmp(s, "none") == 0) return 0;
    if (strcmp(s, "inverse") == 0) return 1;
    if (strcmp(s, "linear") == 0) return 2;
    if (strcmp(s, "exp") == 0) return 3;
    return 1;
}
static const char* AttenuationName(int m) {
    switch (m) {
        case 0: return "none";
        case 1: return "inverse";
        case 2: return "linear";
        case 3: return "exp";
        default: return "inverse";
    }
}

static int l_Sound_SetAttenuationModel(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetAttenuationModel(ud->h, ParseAttenuation(luaL_checkstring(L, 2)));
    return 0;
}
static int l_Sound_GetAttenuationModel(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushstring(L, AttenuationName(AudioBackend::GetAttenuationModel(ud->h)));
    return 1;
}

static int l_Sound_SetMinDistance(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetMinDistance(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetMinDistance(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetMinDistance(ud->h));
    return 1;
}
static int l_Sound_SetMaxDistance(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetMaxDistance(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetMaxDistance(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetMaxDistance(ud->h));
    return 1;
}
static int l_Sound_SetRolloff(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    AudioBackend::SetRolloff(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Sound_GetRolloff(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushnumber(L, AudioBackend::GetRolloff(ud->h));
    return 1;
}

// ==================== Group / Effect 关联 ====================

static int l_Sound_SetGroup(lua_State* L) {
    auto* ud = CheckSound(L, 1);

    // 释放旧 ref
    if (ud->groupRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->groupRef);
        ud->groupRef = LUA_NOREF;
    }

    GroupHandle* gh = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        // 校验类型
        void* gud = luaL_checkudata(L, 2, GROUP_MT);
        gh = ((GroupUserdata_Shared*)gud)->h;
        // 持有 ref 防 GC
        lua_pushvalue(L, 2);
        ud->groupRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    AudioBackend::SetSoundGroup(ud->h, gh);
    return 0;
}

static int l_Sound_SetEffect(lua_State* L) {
    auto* ud = CheckSound(L, 1);

    if (ud->effectRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->effectRef);
        ud->effectRef = LUA_NOREF;
    }

    EffectHandle* eh = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        void* eud = luaL_checkudata(L, 2, EFFECT_MT);
        eh = ((EffectUserdata_Shared*)eud)->h;
        lua_pushvalue(L, 2);
        ud->effectRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    AudioBackend::SetSoundEffect(ud->h, eh);
    return 0;
}

// ==================== Lifecycle ====================

static int l_Sound_Delete(lua_State* L) {
    auto* ud = (SoundUserdata*)luaL_checkudata(L, 1, SOUND_MT);
    if (ud->groupRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->groupRef);
        ud->groupRef = LUA_NOREF;
    }
    if (ud->effectRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->effectRef);
        ud->effectRef = LUA_NOREF;
    }
    if (ud->h) {
        AudioBackend::Free(ud->h);
        ud->h = nullptr;
    }
    return 0;
}

static int l_Sound_Tostring(lua_State* L) {
    auto* ud = CheckSound(L, 1);
    lua_pushfstring(L, "Light.Audio.Sound(%p, vol=%.2f, playing=%s)",
                    ud->h,
                    AudioBackend::GetVolume(ud->h),
                    AudioBackend::IsPlaying(ud->h) ? "true" : "false");
    return 1;
}

// ==================== Module registration ====================

static const luaL_Reg kSoundMethods[] = {
    { "Play",                 l_Sound_Play },
    { "Pause",                l_Sound_Pause },
    { "Stop",                 l_Sound_Stop },
    { "IsPlaying",            l_Sound_IsPlaying },
    { "SetVolume",            l_Sound_SetVolume },
    { "GetVolume",            l_Sound_GetVolume },
    { "SetLooping",           l_Sound_SetLooping },
    { "GetLooping",           l_Sound_GetLooping },
    { "SetPitch",             l_Sound_SetPitch },
    { "GetPitch",             l_Sound_GetPitch },
    { "SetPan",               l_Sound_SetPan },
    { "GetPan",               l_Sound_GetPan },
    { "Set3DEnabled",         l_Sound_Set3DEnabled },
    { "Get3DEnabled",         l_Sound_Get3DEnabled },
    { "SetPosition",          l_Sound_SetPosition },
    { "GetPosition",          l_Sound_GetPosition },
    { "SetVelocity",          l_Sound_SetVelocity },
    { "SetAttenuationModel",  l_Sound_SetAttenuationModel },
    { "GetAttenuationModel",  l_Sound_GetAttenuationModel },
    { "SetMinDistance",       l_Sound_SetMinDistance },
    { "GetMinDistance",       l_Sound_GetMinDistance },
    { "SetMaxDistance",       l_Sound_SetMaxDistance },
    { "GetMaxDistance",       l_Sound_GetMaxDistance },
    { "SetRolloff",           l_Sound_SetRolloff },
    { "GetRolloff",           l_Sound_GetRolloff },
    { "SetGroup",             l_Sound_SetGroup },
    { "SetEffect",            l_Sound_SetEffect },
    { "Delete",               l_Sound_Delete },
    { "__gc",                 l_Sound_Delete },
    { "__tostring",           l_Sound_Tostring },
    { nullptr, nullptr }
};

static const luaL_Reg kSoundFns[] = {
    { "Load",     l_Sound_Load },
    { "LoadAsync", l_Sound_LoadAsync },
    { "LoadPCM",  l_Sound_LoadPCM },
    { nullptr, nullptr }
};

extern "C" LIGHT_API int luaopen_Light_Audio_Sound(lua_State* L) {
    // 注册元表
    if (luaL_newmetatable(L, SOUND_MT)) {
        luaL_setfuncs(L, kSoundMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    // Light.Audio.Sound 模块表
    lua_newtable(L);
    luaL_setfuncs(L, kSoundFns, 0);
    return 1;
}
