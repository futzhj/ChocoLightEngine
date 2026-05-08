/**
 * @file light_version.cpp
 * @brief Light.Version module - SDL3 version & revision strings
 *
 * Lua API (4 fns):
 *
 *    Light.Version.GetVersion()         -> int      (encoded: M*1e6 + m*1e3 + p)
 *    Light.Version.GetRevision()        -> string   (git revision short hash)
 *    Light.Version.AsTuple([version])   -> M, m, p  (decode encoded version int)
 *    Light.Version.AtLeast(M, m, p)     -> bool     (linked SDL >= MAJOR.MINOR.PATCH)
 *
 * Constants:
 *    HEADER_MAJOR / HEADER_MINOR / HEADER_MICRO   (compile-time SDL header version)
 *    HEADER_VERSION                                 (encoded compile-time version)
 *
 * Notes:
 *  - GetVersion / GetRevision return the version of the SDL **runtime**
 *    actually linked into the process. The HEADER_* constants are the
 *    compile-time SDL **header** version. They will normally agree but
 *    may diverge if a system replaces the loaded SDL3 .dll/.so/.dylib.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int l_Version_GetVersion(lua_State* L) {
    int v = SDL_GetVersion();
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

static int l_Version_GetRevision(lua_State* L) {
    const char* r = SDL_GetRevision();
    lua_pushstring(L, r ? r : "");
    return 1;
}

// Decode an encoded version int into (major, minor, patch). Defaults to
// the current runtime version if no arg supplied.
static int l_Version_AsTuple(lua_State* L) {
    int v;
    if (lua_isnoneornil(L, 1)) {
        v = SDL_GetVersion();
    } else {
        v = (int)luaL_checkinteger(L, 1);
    }
    int major = SDL_VERSIONNUM_MAJOR(v);
    int minor = SDL_VERSIONNUM_MINOR(v);
    int patch = SDL_VERSIONNUM_MICRO(v);
    lua_pushinteger(L, major);
    lua_pushinteger(L, minor);
    lua_pushinteger(L, patch);
    return 3;
}

static int l_Version_AtLeast(lua_State* L) {
    int X = (int)luaL_checkinteger(L, 1);
    int Y = (int)luaL_checkinteger(L, 2);
    int Z = (int)luaL_checkinteger(L, 3);
    int linked = SDL_GetVersion();
    int target = SDL_VERSIONNUM(X, Y, Z);
    lua_pushboolean(L, linked >= target ? 1 : 0);
    return 1;
}

static const luaL_Reg kVersionReg[] = {
    { "GetVersion",  l_Version_GetVersion  },
    { "GetRevision", l_Version_GetRevision },
    { "AsTuple",     l_Version_AsTuple     },
    { "AtLeast",     l_Version_AtLeast     },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Version(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kVersionReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_pushinteger(L, (lua_Integer)SDL_MAJOR_VERSION);
    lua_setfield(L, -2, "HEADER_MAJOR");
    lua_pushinteger(L, (lua_Integer)SDL_MINOR_VERSION);
    lua_setfield(L, -2, "HEADER_MINOR");
    lua_pushinteger(L, (lua_Integer)SDL_MICRO_VERSION);
    lua_setfield(L, -2, "HEADER_MICRO");
    lua_pushinteger(L, (lua_Integer)SDL_VERSION);
    lua_setfield(L, -2, "HEADER_VERSION");
    return 1;
}
