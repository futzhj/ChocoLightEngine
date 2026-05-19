#include "light.h"
#include "light_utils_core.h"

#include <SDL3/SDL.h>

extern "C" {
#include "tiny_hash.h"
}

#include <cstdint>
#include <string>

namespace {

static const char* ModeName(SDL_PathType type) {
    switch (type) {
        case SDL_PATHTYPE_FILE: return "file";
        case SDL_PATHTYPE_DIRECTORY: return "directory";
        case SDL_PATHTYPE_OTHER: return "other";
        default: return "none";
    }
}

static void PushAttributes(lua_State* L, const SDL_PathInfo& info) {
    lua_newtable(L);
    lua_pushstring(L, ModeName(info.type)); lua_setfield(L, -2, "mode");
    lua_pushnumber(L, (lua_Number)info.size); lua_setfield(L, -2, "size");
    lua_pushnumber(L, (lua_Number)info.modify_time); lua_setfield(L, -2, "modification");
    lua_pushnumber(L, (lua_Number)info.access_time); lua_setfield(L, -2, "access");
    lua_pushnumber(L, (lua_Number)info.modify_time); lua_setfield(L, -2, "change");
}

static int l_lfs_attributes(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const char* key = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        key = luaL_checkstring(L, 2);
    }
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(path, &info)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetPathInfo failed");
        return 2;
    }
    PushAttributes(L, info);
    if (key) {
        lua_getfield(L, -1, key);
        return 1;
    }
    return 1;
}

static int l_lfs_dir_iter(lua_State* L) {
    lua_getfield(L, lua_upvalueindex(2), "index");
    int index = (int)lua_tointeger(L, -1) + 1;
    lua_pop(L, 1);
    lua_pushinteger(L, index);
    lua_setfield(L, lua_upvalueindex(2), "index");
    lua_rawgeti(L, lua_upvalueindex(1), index);
    return 1;
}

static int l_lfs_dir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int count = 0;
    char** items = SDL_GlobDirectory(path, nullptr, 0, &count);
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
    lua_newtable(L);
    lua_pushinteger(L, 0);
    lua_setfield(L, -2, "index");
    lua_pushcclosure(L, l_lfs_dir_iter, 2);
    return 1;
}

static int l_lfs_mkdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!SDL_CreateDirectory(path)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CreateDirectory failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_lfs_rmdir(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!SDL_RemovePath(path)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_RemovePath failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_lfs_currentdir(lua_State* L) {
    char* path = SDL_GetCurrentDirectory();
    if (!path) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetCurrentDirectory failed");
        return 2;
    }
    lua_pushstring(L, path);
    SDL_free(path);
    return 1;
}

static int l_lfs_chdir(lua_State* L) {
    luaL_checkstring(L, 1);
    lua_pushnil(L);
    lua_pushstring(L, "chdir: not supported");
    return 2;
}

static int l_md5_sum(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[16];
    md5(data, len, digest);
    lua_pushlstring(L, reinterpret_cast<const char*>(digest), 16);
    return 1;
}

static int l_md5_sumhexa(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[16];
    md5(data, len, digest);
    std::string hex = LT::Utils::HexEncode(digest, 16, false);
    lua_pushlstring(L, hex.data(), hex.size());
    return 1;
}

static int l_sha1_sum(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[20];
    LT::Utils::SHA1(reinterpret_cast<const uint8_t*>(data), len, digest);
    lua_pushlstring(L, reinterpret_cast<const char*>(digest), 20);
    return 1;
}

static int l_sha1_sumhexa(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint8_t digest[20];
    LT::Utils::SHA1(reinterpret_cast<const uint8_t*>(data), len, digest);
    std::string hex = LT::Utils::HexEncode(digest, 20, false);
    lua_pushlstring(L, hex.data(), hex.size());
    return 1;
}

}

extern "C" LIGHT_API int luaopen_lfs(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"attributes", l_lfs_attributes},
        {"dir",        l_lfs_dir},
        {"mkdir",      l_lfs_mkdir},
        {"rmdir",      l_lfs_rmdir},
        {"currentdir", l_lfs_currentdir},
        {"chdir",      l_lfs_chdir},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}

extern "C" LIGHT_API int luaopen_md5(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"sum",     l_md5_sum},
        {"sumhexa", l_md5_sumhexa},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}

extern "C" LIGHT_API int luaopen_sha1(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"sum",     l_sha1_sum},
        {"sumhexa", l_sha1_sumhexa},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}
