/**
 * @file light_loadso.cpp
 * @brief Light.Loadso module - SDL3 shared library / DLL loader
 *
 * Lua API (3 fns):
 *
 *    Light.Loadso.LoadObject(sofile)              -> handle (lightuserdata) | nil, err
 *    Light.Loadso.LoadFunction(handle, name)      -> fn_ptr (lightuserdata) | nil, err
 *    Light.Loadso.UnloadObject(handle)            -> nil
 *      (nil-safe: UnloadObject(nil) is a no-op)
 *
 * Notes:
 *  - `handle` is `SDL_SharedObject*` wrapped as `lightuserdata`. User must
 *    call UnloadObject explicitly; there is no GC finalizer.
 *  - `fn_ptr` is `SDL_FunctionPointer` wrapped as `lightuserdata`. It
 *    cannot be called directly from Lua - the intended consumers are
 *    other native bindings (e.g. GL extension loaders or plugin ABI
 *    probes) that receive the pointer as opaque.
 *  - LoadFunction on a missing symbol returns nil + SDL error, not an
 *    exception. This lets scripts probe a DLL's ABI gracefully.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static SDL_SharedObject* CheckHandle(lua_State* L, int idx) {
    if (!lua_islightuserdata(L, idx)) {
        luaL_error(L, "arg %d: expected shared-object handle (lightuserdata), got %s",
                   idx, luaL_typename(L, idx));
    }
    return (SDL_SharedObject*)lua_touserdata(L, idx);
}

static int l_Loadso_LoadObject(lua_State* L) {
    const char* sofile = luaL_checkstring(L, 1);
    SDL_SharedObject* h = SDL_LoadObject(sofile);
    if (!h) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_LoadObject failed");
        return 2;
    }
    lua_pushlightuserdata(L, h);
    return 1;
}

static int l_Loadso_LoadFunction(lua_State* L) {
    SDL_SharedObject* h = CheckHandle(L, 1);
    const char* name = luaL_checkstring(L, 2);
    SDL_FunctionPointer fp = SDL_LoadFunction(h, name);
    if (!fp) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_LoadFunction failed");
        return 2;
    }
    // SDL_FunctionPointer is typedef'd to a function-pointer type; casting
    // through void* is the portable way to stash it in lightuserdata.
    lua_pushlightuserdata(L, (void*)fp);
    return 1;
}

static int l_Loadso_UnloadObject(lua_State* L) {
    if (lua_isnoneornil(L, 1)) return 0;
    SDL_SharedObject* h = CheckHandle(L, 1);
    SDL_UnloadObject(h);
    return 0;
}

static const luaL_Reg kLoadsoReg[] = {
    { "LoadObject",   l_Loadso_LoadObject   },
    { "LoadFunction", l_Loadso_LoadFunction },
    { "UnloadObject", l_Loadso_UnloadObject },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Loadso(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kLoadsoReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    return 1;
}
