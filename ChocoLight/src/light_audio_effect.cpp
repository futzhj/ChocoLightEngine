/**
 * @file light_audio_effect.cpp
 * @brief Light.Audio.Effect — 滤波器 + Echo 效果节点
 *
 * Phase AT.3 — miniaudio 内置 6 种 effect node 暴露到 Lua, 用于 sound 输出处理.
 *
 * Lua API:
 *   Light.Audio.Effect.NewLowPass(cutoffHz, [order=4]) -> Effect|nil [, err]
 *   Light.Audio.Effect.NewHighPass(cutoffHz, [order=4]) -> Effect|nil [, err]
 *   Light.Audio.Effect.NewBandPass(cutoffHz, [order=4]) -> Effect|nil [, err]
 *   Light.Audio.Effect.NewNotch(cutoffHz, [q=0.7]) -> Effect|nil [, err]
 *   Light.Audio.Effect.NewPeak(cutoffHz, gainDB, [q=0.7]) -> Effect|nil [, err]
 *   Light.Audio.Effect.NewEcho({delay_ms=250, decay=0.5, wet=0.5, dry=0.5}) -> Effect|nil
 *
 *   effect:SetEnabled(b) / GetEnabled() -> bool
 *   effect:Delete() / __gc / __tostring
 *
 * 限制:
 *   - 每个 effect 实例只能给一个 sound 用 (内部输出 bus 共享)
 *   - reverb 不实施 (miniaudio 无原生 reverb_node, 用 Echo 替代最常见用例)
 */

#include "light.h"
#include "light_audio_backend.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static const char* EFFECT_MT = "Light.Audio.Effect";

struct EffectUserdata {
    EffectHandle* h;
};

static EffectUserdata* CheckEffect(lua_State* L, int idx) {
    return (EffectUserdata*)luaL_checkudata(L, idx, EFFECT_MT);
}

static int PushEffectUserdata(lua_State* L, EffectHandle* h, const char* errMsg) {
    if (!h) {
        lua_pushnil(L);
        lua_pushstring(L, errMsg);
        return 2;
    }
    EffectUserdata* ud = (EffectUserdata*)lua_newuserdata(L, sizeof(EffectUserdata));
    ud->h = h;
    luaL_getmetatable(L, EFFECT_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== Factory ====================

static int l_Effect_NewLowPass(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    float cutoff = (float)luaL_checknumber(L, 1);
    int order = (int)luaL_optinteger(L, 2, 4);
    return PushEffectUserdata(L, AudioBackend::CreateLowPass(cutoff, order), "CreateLowPass failed");
}

static int l_Effect_NewHighPass(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    float cutoff = (float)luaL_checknumber(L, 1);
    int order = (int)luaL_optinteger(L, 2, 4);
    return PushEffectUserdata(L, AudioBackend::CreateHighPass(cutoff, order), "CreateHighPass failed");
}

static int l_Effect_NewBandPass(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    float cutoff = (float)luaL_checknumber(L, 1);
    int order = (int)luaL_optinteger(L, 2, 4);
    return PushEffectUserdata(L, AudioBackend::CreateBandPass(cutoff, order), "CreateBandPass failed");
}

static int l_Effect_NewNotch(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    float cutoff = (float)luaL_checknumber(L, 1);
    float q = (float)luaL_optnumber(L, 2, 0.7);
    return PushEffectUserdata(L, AudioBackend::CreateNotch(cutoff, q), "CreateNotch failed");
}

static int l_Effect_NewPeak(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    float cutoff = (float)luaL_checknumber(L, 1);
    float gainDB = (float)luaL_checknumber(L, 2);
    float q = (float)luaL_optnumber(L, 3, 0.7);
    return PushEffectUserdata(L, AudioBackend::CreatePeak(cutoff, gainDB, q), "CreatePeak failed");
}

static int l_Effect_NewEcho(lua_State* L) {
    if (!AudioBackend::Init()) { lua_pushnil(L); lua_pushstring(L, "AudioBackend init failed"); return 2; }
    luaL_checktype(L, 1, LUA_TTABLE);

    int delay_ms = 250;
    float decay = 0.5f, wet = 0.5f, dry = 0.5f;

    lua_getfield(L, 1, "delay_ms");
    if (lua_isnumber(L, -1)) delay_ms = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "decay");
    if (lua_isnumber(L, -1)) decay = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "wet");
    if (lua_isnumber(L, -1)) wet = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "dry");
    if (lua_isnumber(L, -1)) dry = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    return PushEffectUserdata(L, AudioBackend::CreateEcho(delay_ms, decay, wet, dry), "CreateEcho failed");
}

// ==================== Methods ====================

static int l_Effect_SetEnabled(lua_State* L) {
    auto* ud = CheckEffect(L, 1);
    AudioBackend::SetEffectEnabled(ud->h, lua_toboolean(L, 2) != 0);
    return 0;
}

static int l_Effect_GetEnabled(lua_State* L) {
    auto* ud = CheckEffect(L, 1);
    lua_pushboolean(L, AudioBackend::GetEffectEnabled(ud->h) ? 1 : 0);
    return 1;
}

static int l_Effect_Delete(lua_State* L) {
    auto* ud = (EffectUserdata*)luaL_checkudata(L, 1, EFFECT_MT);
    if (ud->h) {
        AudioBackend::FreeEffect(ud->h);
        ud->h = nullptr;
    }
    return 0;
}

static int l_Effect_Tostring(lua_State* L) {
    auto* ud = CheckEffect(L, 1);
    lua_pushfstring(L, "Light.Audio.Effect(%p, enabled=%s)",
                    ud->h,
                    AudioBackend::GetEffectEnabled(ud->h) ? "true" : "false");
    return 1;
}

// ==================== Module registration ====================

static const luaL_Reg kEffectMethods[] = {
    { "SetEnabled", l_Effect_SetEnabled },
    { "GetEnabled", l_Effect_GetEnabled },
    { "Delete",     l_Effect_Delete },
    { "__gc",       l_Effect_Delete },
    { "__tostring", l_Effect_Tostring },
    { nullptr, nullptr }
};

static const luaL_Reg kEffectFns[] = {
    { "NewLowPass",  l_Effect_NewLowPass },
    { "NewHighPass", l_Effect_NewHighPass },
    { "NewBandPass", l_Effect_NewBandPass },
    { "NewNotch",    l_Effect_NewNotch },
    { "NewPeak",     l_Effect_NewPeak },
    { "NewEcho",     l_Effect_NewEcho },
    { nullptr, nullptr }
};

extern "C" LIGHT_API int luaopen_Light_Audio_Effect(lua_State* L) {
    if (luaL_newmetatable(L, EFFECT_MT)) {
        luaL_setfuncs(L, kEffectMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, kEffectFns, 0);
    return 1;
}
