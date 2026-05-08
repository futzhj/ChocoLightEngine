/**
 * @file light_haptic.cpp
 * @brief Light.Haptic 模块 - 触觉反馈 (基于 SDL_haptic)
 *
 * Lua API (20 fn):
 *   设备发现:
 *     Light.Haptic.Init()                              -> ok, err
 *     Light.Haptic.Quit()                              -> nil
 *     Light.Haptic.GetHaptics()                        -> array<{id, name}>, err
 *     Light.Haptic.IsMouseHaptic()                     -> bool
 *
 *   设备打开/关闭:
 *     Light.Haptic.Open(id)                            -> dev, err
 *     Light.Haptic.OpenFromMouse()                     -> dev, err
 *     Light.Haptic.Close(dev)                          -> ok, err
 *
 *   设备查询:
 *     Light.Haptic.GetID(dev)                          -> id, err
 *     Light.Haptic.GetName(dev)                        -> name, err
 *     Light.Haptic.GetFeatures(dev)                    -> bitmask, err
 *     Light.Haptic.GetMaxEffects(dev)                  -> n, err
 *     Light.Haptic.GetMaxEffectsPlaying(dev)           -> n, err
 *     Light.Haptic.GetNumAxes(dev)                     -> n, err
 *
 *   简易 rumble (不暴露复杂 SDL_HapticEffect 结构):
 *     Light.Haptic.RumbleSupported(dev)                -> bool
 *     Light.Haptic.InitRumble(dev)                     -> ok, err
 *     Light.Haptic.PlayRumble(dev, strength, length)   -> ok, err  (strength=0..1)
 *     Light.Haptic.StopRumble(dev)                     -> ok, err
 *
 *   全局控制:
 *     Light.Haptic.Pause(dev)                          -> ok, err
 *     Light.Haptic.Resume(dev)                         -> ok, err
 *     Light.Haptic.SetGain(dev, gain)                  -> ok, err  (0..100)
 *     Light.Haptic.SetAutocenter(dev, percent)         -> ok, err  (0..100)
 *     Light.Haptic.StopAll(dev)                        -> ok, err
 *
 * 设计要点:
 * - dev 句柄用 lightuserdata, Close 后失效, 不缓存内部状态.
 * - SDL3 PlayHapticRumble strength 期望 0..1 float; Lua 也用 0..1.
 * - 不暴露 SDL_HapticEffect 结构体 (摇杆方向 / 周期 / 包络 等),
 *   90% 用例只需简易 rumble; 复杂效果留待 Phase K.2 (可选).
 *
 * 平台覆盖:
 *   Windows DirectInput / XInput, Linux evdev, macOS IOKit.
 *   Android / iOS / Web 通常不支持独立 haptic 子系统 (但 gamepad rumble 走另一路径).
 */
#include "light.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_haptic.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// 内部辅助
// ============================================================

static SDL_Haptic* CheckHaptic(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Haptic*)lua_touserdata(L, idx);
    }
    return nullptr;
}

// ============================================================
// Init / Quit
// ============================================================

static int l_Haptic_Init(lua_State* L) {
    if (!SDL_InitSubSystem(SDL_INIT_HAPTIC)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_Quit(lua_State* L) {
    SDL_QuitSubSystem(SDL_INIT_HAPTIC);
    lua_pushnil(L);
    return 1;
}

// ============================================================
// 设备发现
// ============================================================

static int l_Haptic_GetHaptics(lua_State* L) {
    int count = 0;
    SDL_HapticID* ids = SDL_GetHaptics(&count);
    if (!ids && count != 0) {
        // count==0 是合法 (无设备), 仅 ids==NULL && count>0 才算错
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)ids[i]);
        lua_setfield(L, -2, "id");
        const char* name = SDL_GetHapticNameForID(ids[i]);
        lua_pushstring(L, name ? name : "");
        lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, i + 1);
    }
    if (ids) SDL_free(ids);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_IsMouseHaptic(lua_State* L) {
    lua_pushboolean(L, SDL_IsMouseHaptic() ? 1 : 0);
    return 1;
}

// ============================================================
// 设备打开/关闭
// ============================================================

static int l_Haptic_Open(lua_State* L) {
    SDL_HapticID id = (SDL_HapticID)luaL_checkinteger(L, 1);
    SDL_Haptic* dev = SDL_OpenHaptic(id);
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_OpenFromMouse(lua_State* L) {
    SDL_Haptic* dev = SDL_OpenHapticFromMouse();
    if (!dev) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, dev);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_Close(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid haptic handle");
        return 2;
    }
    SDL_CloseHaptic(dev);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// 设备查询
// ============================================================

static int l_Haptic_GetID(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    SDL_HapticID id = SDL_GetHapticID(dev);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetName(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    const char* name = SDL_GetHapticName(dev);
    if (!name) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetFeatures(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    Uint32 features = SDL_GetHapticFeatures(dev);
    lua_pushinteger(L, (lua_Integer)features);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetMaxEffects(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetMaxHapticEffects(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetMaxEffectsPlaying(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetMaxHapticEffectsPlaying(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_GetNumAxes(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushnil(L); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int n = SDL_GetNumHapticAxes(dev);
    if (n < 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, n);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// 简易 rumble
// ============================================================

static int l_Haptic_RumbleSupported(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HapticRumbleSupported(dev) ? 1 : 0);
    return 1;
}

static int l_Haptic_InitRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_InitHapticRumble(dev)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_PlayRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    lua_Number strength = luaL_checknumber(L, 2);  // 0..1
    lua_Integer length  = luaL_checkinteger(L, 3); // ms

    // 范围保护 (SDL3 内部不强校验, 这里收紧)
    if (strength < 0.0) strength = 0.0;
    if (strength > 1.0) strength = 1.0;
    if (length < 0) length = 0;

    if (!SDL_PlayHapticRumble(dev, (float)strength, (Uint32)length)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Haptic_StopRumble(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_StopHapticRumble(dev)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// 全局控制
// ============================================================

static int l_Haptic_Pause(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_PauseHaptic(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_Resume(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_ResumeHaptic(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_SetGain(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int gain = (int)luaL_checkinteger(L, 2);
    // 范围 0..100, SDL3 内部 clamp 但不报错; 这里直接传
    if (gain < 0) gain = 0;
    if (gain > 100) gain = 100;
    if (!SDL_SetHapticGain(dev, gain)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_SetAutocenter(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    int pct = (int)luaL_checkinteger(L, 2);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    if (!SDL_SetHapticAutocenter(dev, pct)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

static int l_Haptic_StopAll(lua_State* L) {
    SDL_Haptic* dev = CheckHaptic(L, 1);
    if (!dev) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid haptic handle"); return 2; }
    if (!SDL_StopHapticEffects(dev)) {
        lua_pushboolean(L, 0); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushboolean(L, 1); lua_pushnil(L); return 2;
}

// ============================================================
// 注册
// ============================================================

static const luaL_Reg kHapticReg[] = {
    {"Init",                    l_Haptic_Init},
    {"Quit",                    l_Haptic_Quit},
    {"GetHaptics",              l_Haptic_GetHaptics},
    {"IsMouseHaptic",           l_Haptic_IsMouseHaptic},
    {"Open",                    l_Haptic_Open},
    {"OpenFromMouse",           l_Haptic_OpenFromMouse},
    {"Close",                   l_Haptic_Close},
    {"GetID",                   l_Haptic_GetID},
    {"GetName",                 l_Haptic_GetName},
    {"GetFeatures",             l_Haptic_GetFeatures},
    {"GetMaxEffects",           l_Haptic_GetMaxEffects},
    {"GetMaxEffectsPlaying",    l_Haptic_GetMaxEffectsPlaying},
    {"GetNumAxes",              l_Haptic_GetNumAxes},
    {"RumbleSupported",         l_Haptic_RumbleSupported},
    {"InitRumble",              l_Haptic_InitRumble},
    {"PlayRumble",              l_Haptic_PlayRumble},
    {"StopRumble",              l_Haptic_StopRumble},
    {"Pause",                   l_Haptic_Pause},
    {"Resume",                  l_Haptic_Resume},
    {"SetGain",                 l_Haptic_SetGain},
    {"SetAutocenter",           l_Haptic_SetAutocenter},
    {"StopAll",                 l_Haptic_StopAll},
    {nullptr, nullptr}
};

extern "C" LIGHT_API int luaopen_Light_Haptic(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kHapticReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    return 1;
}
