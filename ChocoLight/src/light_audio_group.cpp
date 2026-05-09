/**
 * @file light_audio_group.cpp
 * @brief Light.Audio.SoundGroup — Mixer 通道组 (支持嵌套)
 *
 * Phase AT.2 — 多通道 sound 控制 (背景音乐 / 音效 / 对话 分组管理).
 *
 * Lua API:
 *   Light.Audio.SoundGroup.New([parent_group | nil]) -> SoundGroup|nil [, err]
 *
 *   group:SetVolume(v) / GetVolume() -> v
 *   group:SetPitch(p) / GetPitch() -> p
 *   group:Pause() / Resume() / Stop()
 *   group:SetParent(group | nil) -> bool       -- 含循环检测
 *   group:SetEffect(effect | nil)
 *   group:Delete() / __gc / __tostring
 *
 * 内部:
 *   - userdata: GroupHandle* + parentRef (防父 GC) + effectRef
 *   - SetParent 检测循环 (在 backend 内部), 失败返回 false
 */

#include "light.h"
#include "light_audio_backend.h"

#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static const char* GROUP_MT = "Light.Audio.SoundGroup";
static const char* EFFECT_MT = "Light.Audio.Effect";

struct GroupUserdata {
    GroupHandle* h;
    int parentRef;
    int effectRef;
};

struct EffectUserdata_Shared {
    EffectHandle* h;
};

static GroupUserdata* CheckGroup(lua_State* L, int idx) {
    return (GroupUserdata*)luaL_checkudata(L, idx, GROUP_MT);
}

// ==================== SoundGroup.New([parent]) ====================

static int l_Group_New(lua_State* L) {
    if (!AudioBackend::Init()) {
        lua_pushnil(L);
        lua_pushstring(L, "AudioBackend init failed");
        return 2;
    }

    GroupHandle* parent = nullptr;
    int parentRef = LUA_NOREF;
    if (!lua_isnoneornil(L, 1)) {
        void* pud = luaL_checkudata(L, 1, GROUP_MT);
        parent = ((GroupUserdata*)pud)->h;
        lua_pushvalue(L, 1);
        parentRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    GroupHandle* h = AudioBackend::CreateGroup(parent);
    if (!h) {
        if (parentRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, parentRef);
        lua_pushnil(L);
        lua_pushstring(L, "CreateGroup failed");
        return 2;
    }
    GroupUserdata* ud = (GroupUserdata*)lua_newuserdata(L, sizeof(GroupUserdata));
    ud->h = h;
    ud->parentRef = parentRef;
    ud->effectRef = LUA_NOREF;
    luaL_getmetatable(L, GROUP_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== Volume / Pitch ====================

static int l_Group_SetVolume(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    AudioBackend::SetGroupVolume(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Group_GetVolume(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    lua_pushnumber(L, AudioBackend::GetGroupVolume(ud->h));
    return 1;
}
static int l_Group_SetPitch(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    AudioBackend::SetGroupPitch(ud->h, (float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Group_GetPitch(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    lua_pushnumber(L, AudioBackend::GetGroupPitch(ud->h));
    return 1;
}

// ==================== 播放控制 ====================

static int l_Group_Pause(lua_State* L)  { auto* ud = CheckGroup(L, 1); AudioBackend::GroupPause(ud->h); return 0; }
static int l_Group_Resume(lua_State* L) { auto* ud = CheckGroup(L, 1); AudioBackend::GroupResume(ud->h); return 0; }
static int l_Group_Stop(lua_State* L)   { auto* ud = CheckGroup(L, 1); AudioBackend::GroupStop(ud->h); return 0; }

// ==================== 嵌套 ====================

static int l_Group_SetParent(lua_State* L) {
    auto* ud = CheckGroup(L, 1);

    GroupHandle* parent = nullptr;
    int newParentRef = LUA_NOREF;
    if (!lua_isnoneornil(L, 2)) {
        void* pud = luaL_checkudata(L, 2, GROUP_MT);
        parent = ((GroupUserdata*)pud)->h;
        lua_pushvalue(L, 2);
        newParentRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    if (!AudioBackend::SetGroupParent(ud->h, parent)) {
        // 循环检测失败
        if (newParentRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, newParentRef);
        lua_pushboolean(L, 0);
        lua_pushstring(L, "cycle detected (cannot set parent)");
        return 2;
    }

    // 成功: 释放旧 ref, 替换为新 ref
    if (ud->parentRef != LUA_NOREF) luaL_unref(L, LUA_REGISTRYINDEX, ud->parentRef);
    ud->parentRef = newParentRef;

    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Group:SetEffect ====================
// (group 级 effect 暂不实施 — 仅在 backend SetSoundEffect 路径上做; group SetEffect
//  需要在 RewireSoundOutput 中考虑 group 输出经 effect, 实施成本高, 此版只支持 sound 级 effect)
//
// 这里保留接口但实际不接 backend, 避免 API 残缺:
static int l_Group_SetEffect(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    if (ud->effectRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->effectRef);
        ud->effectRef = LUA_NOREF;
    }
    if (!lua_isnoneornil(L, 2)) {
        // 类型校验, 但暂不实施 group 级 effect routing
        (void)luaL_checkudata(L, 2, EFFECT_MT);
        lua_pushvalue(L, 2);
        ud->effectRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    // TODO Phase AT.x: implement group-level effect routing in backend
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Lifecycle ====================

static int l_Group_Delete(lua_State* L) {
    auto* ud = (GroupUserdata*)luaL_checkudata(L, 1, GROUP_MT);
    if (ud->parentRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->parentRef);
        ud->parentRef = LUA_NOREF;
    }
    if (ud->effectRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, ud->effectRef);
        ud->effectRef = LUA_NOREF;
    }
    if (ud->h) {
        AudioBackend::FreeGroup(ud->h);
        ud->h = nullptr;
    }
    return 0;
}

static int l_Group_Tostring(lua_State* L) {
    auto* ud = CheckGroup(L, 1);
    lua_pushfstring(L, "Light.Audio.SoundGroup(%p, vol=%.2f)",
                    ud->h,
                    AudioBackend::GetGroupVolume(ud->h));
    return 1;
}

// ==================== Module registration ====================

static const luaL_Reg kGroupMethods[] = {
    { "SetVolume", l_Group_SetVolume },
    { "GetVolume", l_Group_GetVolume },
    { "SetPitch",  l_Group_SetPitch },
    { "GetPitch",  l_Group_GetPitch },
    { "Pause",     l_Group_Pause },
    { "Resume",    l_Group_Resume },
    { "Stop",      l_Group_Stop },
    { "SetParent", l_Group_SetParent },
    { "SetEffect", l_Group_SetEffect },
    { "Delete",    l_Group_Delete },
    { "__gc",      l_Group_Delete },
    { "__tostring", l_Group_Tostring },
    { nullptr, nullptr }
};

static const luaL_Reg kGroupFns[] = {
    { "New", l_Group_New },
    { nullptr, nullptr }
};

extern "C" LIGHT_API int luaopen_Light_Audio_SoundGroup(lua_State* L) {
    if (luaL_newmetatable(L, GROUP_MT)) {
        luaL_setfuncs(L, kGroupMethods, 0);
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, kGroupFns, 0);
    return 1;
}
