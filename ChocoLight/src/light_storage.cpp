/**
 * @file light_storage.cpp
 * @brief Phase C3: Light.Storage 模块 - 跨平台只读/可写存储 (基于 SDL_Storage)
 *
 * Lua API:
 *   Light.Storage.OpenUser(org, app) -> ok, err
 *     打开/重新打开 User Storage (玩家存档区), 阻塞等待 ready (最多 1 秒)
 *   Light.Storage.CloseUser()
 *
 *   Light.Storage.Title.Read(path) -> data, err     (只读, 游戏资源)
 *   Light.Storage.Title.Exists(path) -> bool
 *   Light.Storage.Title.Size(path) -> bytes, err
 *
 *   Light.Storage.User.Read(path) -> data, err
 *   Light.Storage.User.Write(path, data) -> ok, err
 *   Light.Storage.User.Exists(path) -> bool
 *   Light.Storage.User.Delete(path) -> ok, err
 *   Light.Storage.User.Size(path) -> bytes, err
 *
 * 设计要点:
 *   - Title storage 在 luaopen 时延迟打开 (首次访问)
 *   - User storage 必须用户显式 OpenUser, 否则 User.* 返回错误
 *   - 平台差异 (Win=AppData, Linux=XDG_DATA_HOME, Mac=ApplicationSupport, iOS/Android=app sandbox)
 *     由 SDL3 自动处理
 */
#include "light.h"

#include <SDL3/SDL.h>
#include <cstdint>
#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

SDL_Storage* g_titleStorage = nullptr;
SDL_Storage* g_userStorage  = nullptr;

// 阻塞等待 storage ready (典型耗时 < 50ms)
bool WaitReady(SDL_Storage* st, Uint32 timeoutMs = 1000) {
    if (!st) return false;
    Uint64 start = SDL_GetTicks();
    while (!SDL_StorageReady(st)) {
        if (SDL_GetTicks() - start > timeoutMs) return false;
        SDL_Delay(10);
    }
    return true;
}

SDL_Storage* EnsureTitle() {
    if (g_titleStorage) return g_titleStorage;
    g_titleStorage = SDL_OpenTitleStorage(nullptr, 0);
    if (!g_titleStorage) {
        CC::Log(CC::LOG_WARN, "Light.Storage: SDL_OpenTitleStorage failed: %s", SDL_GetError());
        return nullptr;
    }
    if (!WaitReady(g_titleStorage)) {
        CC::Log(CC::LOG_WARN, "Light.Storage: Title storage not ready within timeout");
    }
    return g_titleStorage;
}

// ==================== 通用读 ====================

int ReadStorage(lua_State* L, SDL_Storage* st, const char* path) {
    if (!st || !SDL_StorageReady(st)) {
        lua_pushnil(L);
        lua_pushstring(L, "storage not ready");
        return 2;
    }
    Uint64 size = 0;
    if (!SDL_GetStorageFileSize(st, path, &size)) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    if (size > (Uint64)0x7FFFFFFF) {
        // > 2GB, Lua 字符串不安全
        lua_pushnil(L);
        lua_pushstring(L, "file too large");
        return 2;
    }

    std::vector<uint8_t> buf((size_t)size);
    if (!SDL_ReadStorageFile(st, path, buf.data(), (Uint64)buf.size())) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(buf.data()), buf.size());
    lua_pushnil(L);
    return 2;
}

int ExistsStorage(lua_State* L, SDL_Storage* st, const char* path) {
    if (!st || !SDL_StorageReady(st)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    SDL_PathInfo info;
    bool ok = SDL_GetStoragePathInfo(st, path, &info);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

int SizeStorage(lua_State* L, SDL_Storage* st, const char* path) {
    if (!st || !SDL_StorageReady(st)) {
        lua_pushnil(L);
        lua_pushstring(L, "storage not ready");
        return 2;
    }
    Uint64 size = 0;
    if (!SDL_GetStorageFileSize(st, path, &size)) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)size);
    lua_pushnil(L);
    return 2;
}

} // namespace

// ==================== Light.Storage.OpenUser ====================

static int l_Storage_OpenUser(lua_State* L) {
    const char* org = luaL_checkstring(L, 1);
    const char* app = luaL_checkstring(L, 2);

    if (g_userStorage) {
        SDL_CloseStorage(g_userStorage);
        g_userStorage = nullptr;
    }

    g_userStorage = SDL_OpenUserStorage(org, app, 0);
    if (!g_userStorage) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    if (!WaitReady(g_userStorage)) {
        SDL_CloseStorage(g_userStorage);
        g_userStorage = nullptr;
        lua_pushboolean(L, 0);
        lua_pushstring(L, "user storage not ready within 1s");
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Storage_CloseUser(lua_State*) {
    if (g_userStorage) {
        SDL_CloseStorage(g_userStorage);
        g_userStorage = nullptr;
    }
    return 0;
}

// ==================== Light.Storage.Title.* ====================

static int l_Title_Read(lua_State* L)   { return ReadStorage  (L, EnsureTitle(), luaL_checkstring(L, 1)); }
static int l_Title_Exists(lua_State* L) { return ExistsStorage(L, EnsureTitle(), luaL_checkstring(L, 1)); }
static int l_Title_Size(lua_State* L)   { return SizeStorage  (L, EnsureTitle(), luaL_checkstring(L, 1)); }

// ==================== Light.Storage.User.* ====================

static int l_User_Read(lua_State* L)   { return ReadStorage  (L, g_userStorage, luaL_checkstring(L, 1)); }
static int l_User_Exists(lua_State* L) { return ExistsStorage(L, g_userStorage, luaL_checkstring(L, 1)); }
static int l_User_Size(lua_State* L)   { return SizeStorage  (L, g_userStorage, luaL_checkstring(L, 1)); }

static int l_User_Write(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    size_t len = 0;
    const char* data = luaL_checklstring(L, 2, &len);

    if (!g_userStorage || !SDL_StorageReady(g_userStorage)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "user storage not ready (call Light.Storage.OpenUser first)");
        return 2;
    }
    if (!SDL_WriteStorageFile(g_userStorage, path, data, (Uint64)len)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_User_Delete(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!g_userStorage || !SDL_StorageReady(g_userStorage)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "user storage not ready");
        return 2;
    }
    if (!SDL_RemoveStoragePath(g_userStorage, path)) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Storage ====================

static const luaL_Reg kRoot[] = {
    { "OpenUser",  l_Storage_OpenUser  },
    { "CloseUser", l_Storage_CloseUser },
    { nullptr,     nullptr             },
};

static const luaL_Reg kTitle[] = {
    { "Read",   l_Title_Read   },
    { "Exists", l_Title_Exists },
    { "Size",   l_Title_Size   },
    { nullptr,  nullptr        },
};

static const luaL_Reg kUser[] = {
    { "Read",   l_User_Read   },
    { "Write",  l_User_Write  },
    { "Exists", l_User_Exists },
    { "Delete", l_User_Delete },
    { "Size",   l_User_Size   },
    { nullptr,  nullptr       },
};

extern "C" LIGHT_API int luaopen_Light_Storage(lua_State* L) {
    lua_newtable(L);                           // root table
    luaL_register(L, nullptr, kRoot);

    // Title 子表
    lua_newtable(L);
    luaL_register(L, nullptr, kTitle);
    lua_setfield(L, -2, "Title");

    // User 子表
    lua_newtable(L);
    luaL_register(L, nullptr, kUser);
    lua_setfield(L, -2, "User");

    return 1;
}

// ==================== 关闭清理 ====================

extern "C" void Light_Storage_Shutdown() {
    if (g_userStorage) {
        SDL_CloseStorage(g_userStorage);
        g_userStorage = nullptr;
    }
    if (g_titleStorage) {
        SDL_CloseStorage(g_titleStorage);
        g_titleStorage = nullptr;
    }
}
