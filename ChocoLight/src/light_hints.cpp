/**
 * @file light_hints.cpp
 * @brief Light.Hints module - SDL3 runtime configuration hints
 *
 * Lua API (6 fns):
 *   Light.Hints.Get(name)                     -> string | nil
 *   Light.Hints.GetBoolean(name, default)     -> boolean
 *   Light.Hints.Set(name, value)              -> ok, err
 *   Light.Hints.SetWithPriority(name, val, p) -> ok, err
 *   Light.Hints.Reset(name)                   -> ok, err
 *   Light.Hints.ResetAll()                    -> (void)
 *
 * Constants (Light.Hints.PRIORITY_*):
 *   DEFAULT  = 0   (only set if no previous SetHint set it)
 *   NORMAL   = 1   (override DEFAULT, not OVERRIDE)
 *   OVERRIDE = 2   (always override)
 *
 * Priority argument accepts either the number (0/1/2) or the string
 * name ("default"/"normal"/"override"). Invalid value -> (false, err).
 *
 * No SDL_Init dependency: SDL_SetHint / SDL_GetHint operate on a
 * process-wide string table and are safe to call before SDL_Init or
 * after SDL_Quit. This makes Light.Hints usable from bootstrap scripts
 * to configure SDL behaviour before any subsystem is initialized
 * (e.g. set SDL_HINT_VIDEO_DRIVER = "dummy" before Light.UI.Window).
 *
 * Not bound (intentional):
 *   SDL_AddHintCallback / SDL_RemoveHintCallback
 *     - callbacks fire synchronously from any thread that calls
 *       SDL_SetHint; wiring into Lua safely requires the Tray-style
 *       poll-dispatch trampoline. Defer to a later phase if a real
 *       user script actually needs hint-change notifications.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static bool ResolvePriority(lua_State* L, int idx, SDL_HintPriority& out) {
    int t = lua_type(L, idx);
    if (t == LUA_TNUMBER) {
        int n = (int)lua_tointeger(L, idx);
        if (n < SDL_HINT_DEFAULT || n > SDL_HINT_OVERRIDE) return false;
        out = (SDL_HintPriority)n;
        return true;
    }
    if (t == LUA_TSTRING) {
        const char* s = lua_tostring(L, idx);
        if (!s) return false;
        if (!SDL_strcasecmp(s, "default"))  { out = SDL_HINT_DEFAULT;  return true; }
        if (!SDL_strcasecmp(s, "normal"))   { out = SDL_HINT_NORMAL;   return true; }
        if (!SDL_strcasecmp(s, "override")) { out = SDL_HINT_OVERRIDE; return true; }
        return false;
    }
    return false;
}

// ===========================================================
// Light.Hints.Get(name) -> string | nil
// ===========================================================
static int l_Hints_Get(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* v = SDL_GetHint(name);
    if (v == nullptr) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, v);
    }
    return 1;
}

// ===========================================================
// Light.Hints.GetBoolean(name, default) -> boolean
// ===========================================================
static int l_Hints_GetBoolean(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool def = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        def = lua_toboolean(L, 2) != 0;
    }
    bool v = SDL_GetHintBoolean(name, def);
    lua_pushboolean(L, v ? 1 : 0);
    return 1;
}

// ===========================================================
// Light.Hints.Set(name, value) -> ok, err
// value nil -> empty string (valid SDL_SetHint behaviour).
// ===========================================================
static int l_Hints_Set(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* value = "";
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) == LUA_TBOOLEAN) {
            value = lua_toboolean(L, 2) ? "1" : "0";
        } else {
            value = luaL_checkstring(L, 2);
        }
    }
    bool ok = SDL_SetHint(name, value);
    if (!ok) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SetHint failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ===========================================================
// Light.Hints.SetWithPriority(name, value, priority) -> ok, err
// ===========================================================
static int l_Hints_SetWithPriority(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const char* value = "";
    if (!lua_isnoneornil(L, 2)) {
        if (lua_type(L, 2) == LUA_TBOOLEAN) {
            value = lua_toboolean(L, 2) ? "1" : "0";
        } else {
            value = luaL_checkstring(L, 2);
        }
    }
    SDL_HintPriority prio = SDL_HINT_NORMAL;
    if (!ResolvePriority(L, 3, prio)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "priority must be 0..2 or 'default'/'normal'/'override'");
        return 2;
    }
    bool ok = SDL_SetHintWithPriority(name, value, prio);
    if (!ok) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SetHintWithPriority failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ===========================================================
// Light.Hints.Reset(name) -> ok, err
// ===========================================================
static int l_Hints_Reset(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    bool ok = SDL_ResetHint(name);
    if (!ok) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_ResetHint failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ===========================================================
// Light.Hints.ResetAll() -> (void)
// ===========================================================
static int l_Hints_ResetAll(lua_State* L) {
    (void)L;
    SDL_ResetHints();
    return 0;
}

// ===========================================================
// luaopen_Light_Hints
// ===========================================================
static const luaL_Reg kHintsReg[] = {
    { "Get",             l_Hints_Get             },
    { "GetBoolean",      l_Hints_GetBoolean      },
    { "Set",             l_Hints_Set             },
    { "SetWithPriority", l_Hints_SetWithPriority },
    { "Reset",           l_Hints_Reset           },
    { "ResetAll",        l_Hints_ResetAll        },
    { nullptr,           nullptr                 },
};

extern "C" LIGHT_API int luaopen_Light_Hints(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kHintsReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Priority constants
    lua_pushinteger(L, (lua_Integer)SDL_HINT_DEFAULT);
    lua_setfield(L, -2, "PRIORITY_DEFAULT");
    lua_pushinteger(L, (lua_Integer)SDL_HINT_NORMAL);
    lua_setfield(L, -2, "PRIORITY_NORMAL");
    lua_pushinteger(L, (lua_Integer)SDL_HINT_OVERRIDE);
    lua_setfield(L, -2, "PRIORITY_OVERRIDE");

    return 1;
}
