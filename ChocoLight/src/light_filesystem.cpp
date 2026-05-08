/**
 * @file light_filesystem.cpp
 * @brief Light.Filesystem module - SDL3 abstract filesystem operations
 *
 * Lua API (10 fns):
 *
 *  Path discovery:
 *    Light.Filesystem.GetBasePath()                     -> string|nil, err
 *    Light.Filesystem.GetPrefPath(org, app)             -> string|nil, err
 *    Light.Filesystem.GetUserFolder(folder)             -> string|nil, err
 *    Light.Filesystem.GetCurrentDirectory()             -> string|nil, err
 *
 *  Mutation:
 *    Light.Filesystem.CreateDirectory(path)             -> ok, err
 *    Light.Filesystem.RemovePath(path)                  -> ok, err
 *    Light.Filesystem.RenamePath(old, new)              -> ok, err
 *    Light.Filesystem.CopyFile(src, dst)                -> ok, err
 *
 *  Inspection:
 *    Light.Filesystem.GetPathInfo(path)                 -> info|nil, err
 *      info = { type, size, create_time, modify_time, access_time }
 *    Light.Filesystem.GlobDirectory(path, [pattern], [flags]) -> array|nil, err
 *      flags: bitwise OR of GLOB_*. pattern can be nil = no filter.
 *
 * Constants (16):
 *    FOLDER_HOME / _DESKTOP / _DOCUMENTS / _DOWNLOADS / _MUSIC / _PICTURES /
 *      _PUBLICSHARE / _SAVEDGAMES / _SCREENSHOTS / _TEMPLATES / _VIDEOS  (11)
 *    PATHTYPE_NONE / _FILE / _DIRECTORY / _OTHER                          (4)
 *    GLOB_CASEINSENSITIVE                                                  (1)
 *
 * Notes:
 *  - SDL_EnumerateDirectory is not bound; GlobDirectory(path, "*") is the
 *    natural Lua replacement (returns a flat string array, no callback
 *    plumbing needed).
 *  - GetPrefPath / GetCurrentDirectory / GlobDirectory return SDL-allocated
 *    memory which we copy into Lua strings then SDL_free.
 *  - GetBasePath / GetUserFolder return internal SDL static strings (not
 *    freed).
 *  - All mutation paths surface SDL_GetError() as the err string when bool
 *    returns false.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Path discovery
// ============================================================
static int l_FS_GetBasePath(lua_State* L) {
    const char* p = SDL_GetBasePath();
    if (!p) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetBasePath failed");
        return 2;
    }
    lua_pushstring(L, p);
    return 1;
}

static int l_FS_GetPrefPath(lua_State* L) {
    const char* org = luaL_checkstring(L, 1);
    const char* app = luaL_checkstring(L, 2);
    char* p = SDL_GetPrefPath(org, app);
    if (!p) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetPrefPath failed");
        return 2;
    }
    lua_pushstring(L, p);
    SDL_free(p);
    return 1;
}

static int l_FS_GetUserFolder(lua_State* L) {
    int folder = (int)luaL_checkinteger(L, 1);
    if (folder < 0 || folder >= SDL_FOLDER_COUNT) {
        lua_pushnil(L);
        lua_pushfstring(L, "folder %d out of range [0..%d]",
                        folder, (int)SDL_FOLDER_COUNT - 1);
        return 2;
    }
    const char* p = SDL_GetUserFolder((SDL_Folder)folder);
    if (!p) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetUserFolder failed");
        return 2;
    }
    lua_pushstring(L, p);
    return 1;
}

static int l_FS_GetCurrentDirectory(lua_State* L) {
    char* p = SDL_GetCurrentDirectory();
    if (!p) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetCurrentDirectory failed");
        return 2;
    }
    lua_pushstring(L, p);
    SDL_free(p);
    return 1;
}

// ============================================================
// Mutation
// ============================================================
static int l_FS_CreateDirectory(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!SDL_CreateDirectory(path)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CreateDirectory failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_FS_RemovePath(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!SDL_RemovePath(path)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_RemovePath failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_FS_RenamePath(lua_State* L) {
    const char* oldp = luaL_checkstring(L, 1);
    const char* newp = luaL_checkstring(L, 2);
    if (!SDL_RenamePath(oldp, newp)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_RenamePath failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_FS_CopyFile(lua_State* L) {
    const char* src = luaL_checkstring(L, 1);
    const char* dst = luaL_checkstring(L, 2);
    if (!SDL_CopyFile(src, dst)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CopyFile failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// Inspection
// ============================================================
static int l_FS_GetPathInfo(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetPathInfo failed");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)info.type);          lua_setfield(L, -2, "type");
    lua_pushnumber(L,  (lua_Number) info.size);          lua_setfield(L, -2, "size");
    // SDL_Time is signed 64-bit nanoseconds since Unix epoch. Lua double
    // (53 mantissa bits) loses ~15 ns precision around year 2270 - far
    // beyond practical use, so passing as number is safe.
    lua_pushnumber(L, (lua_Number)info.create_time);     lua_setfield(L, -2, "create_time");
    lua_pushnumber(L, (lua_Number)info.modify_time);     lua_setfield(L, -2, "modify_time");
    lua_pushnumber(L, (lua_Number)info.access_time);     lua_setfield(L, -2, "access_time");
    return 1;
}

static int l_FS_GlobDirectory(lua_State* L) {
    const char* path    = luaL_checkstring(L, 1);
    const char* pattern = lua_isnoneornil(L, 2) ? nullptr : luaL_checkstring(L, 2);
    SDL_GlobFlags flags = (SDL_GlobFlags)luaL_optinteger(L, 3, 0);

    int count = 0;
    char** items = SDL_GlobDirectory(path, pattern, flags, &count);
    if (!items) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GlobDirectory failed");
        return 2;
    }
    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        lua_pushstring(L, items[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(items);
    return 1;
}

// ============================================================
// luaopen_Light_Filesystem
// ============================================================
static const luaL_Reg kFSReg[] = {
    { "GetBasePath",         l_FS_GetBasePath         },
    { "GetPrefPath",         l_FS_GetPrefPath         },
    { "GetUserFolder",       l_FS_GetUserFolder       },
    { "GetCurrentDirectory", l_FS_GetCurrentDirectory },
    { "CreateDirectory",     l_FS_CreateDirectory     },
    { "RemovePath",          l_FS_RemovePath          },
    { "RenamePath",          l_FS_RenamePath          },
    { "CopyFile",            l_FS_CopyFile            },
    { "GetPathInfo",         l_FS_GetPathInfo         },
    { "GlobDirectory",       l_FS_GlobDirectory       },
    { nullptr, nullptr },
};

#define LIGHT_PUSH_INT_CONST(NAME, VALUE)        \
    lua_pushinteger(L, (lua_Integer)(VALUE));    \
    lua_setfield(L, -2, NAME)

extern "C" LIGHT_API int luaopen_Light_Filesystem(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kFSReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Folders (11)
    LIGHT_PUSH_INT_CONST("FOLDER_HOME",        SDL_FOLDER_HOME);
    LIGHT_PUSH_INT_CONST("FOLDER_DESKTOP",     SDL_FOLDER_DESKTOP);
    LIGHT_PUSH_INT_CONST("FOLDER_DOCUMENTS",   SDL_FOLDER_DOCUMENTS);
    LIGHT_PUSH_INT_CONST("FOLDER_DOWNLOADS",   SDL_FOLDER_DOWNLOADS);
    LIGHT_PUSH_INT_CONST("FOLDER_MUSIC",       SDL_FOLDER_MUSIC);
    LIGHT_PUSH_INT_CONST("FOLDER_PICTURES",    SDL_FOLDER_PICTURES);
    LIGHT_PUSH_INT_CONST("FOLDER_PUBLICSHARE", SDL_FOLDER_PUBLICSHARE);
    LIGHT_PUSH_INT_CONST("FOLDER_SAVEDGAMES",  SDL_FOLDER_SAVEDGAMES);
    LIGHT_PUSH_INT_CONST("FOLDER_SCREENSHOTS", SDL_FOLDER_SCREENSHOTS);
    LIGHT_PUSH_INT_CONST("FOLDER_TEMPLATES",   SDL_FOLDER_TEMPLATES);
    LIGHT_PUSH_INT_CONST("FOLDER_VIDEOS",      SDL_FOLDER_VIDEOS);

    // Path types (4)
    LIGHT_PUSH_INT_CONST("PATHTYPE_NONE",      SDL_PATHTYPE_NONE);
    LIGHT_PUSH_INT_CONST("PATHTYPE_FILE",      SDL_PATHTYPE_FILE);
    LIGHT_PUSH_INT_CONST("PATHTYPE_DIRECTORY", SDL_PATHTYPE_DIRECTORY);
    LIGHT_PUSH_INT_CONST("PATHTYPE_OTHER",     SDL_PATHTYPE_OTHER);

    // Glob flags (1)
    LIGHT_PUSH_INT_CONST("GLOB_CASEINSENSITIVE", SDL_GLOB_CASEINSENSITIVE);

    return 1;
}

#undef LIGHT_PUSH_INT_CONST
