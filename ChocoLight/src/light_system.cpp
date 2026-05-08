/**
 * @file light_system.cpp
 * @brief Light.System 模块 - 平台/版本/CPU 信息 (基于 SDL_platform + SDL_version + SDL_cpuinfo)
 *
 * Lua API:
 *   Light.System.GetPlatform()         -> "Windows" / "Linux" / "macOS" / "iOS" / "Android" / "Emscripten" / ...
 *   Light.System.GetSDLVersion()       -> { major=3, minor=2, patch=30 }
 *   Light.System.GetSDLRevision()      -> "3.2.30+abc123" (commit hash 或 tag)
 *
 *   Light.System.GetSystemRAM()        -> int (MB; 0 表示未知)
 *   Light.System.GetLogicalCPUCores()  -> int (>=1)
 *   Light.System.GetCPUCacheLineSize() -> int (通常 64)
 *   Light.System.GetSIMDAlignment()    -> int (SIMD 对齐字节)
 *
 *   Light.System.GetCPUFeatures()      -> { mmx=bool, sse=bool, sse2=bool, sse3=bool, sse41=bool,
 *                                           sse42=bool, avx=bool, avx2=bool, avx512f=bool,
 *                                           neon=bool, altivec=bool, arm_simd=bool,
 *                                           lsx=bool, lasx=bool }
 *
 * 用途: 运行时自检, 条件启用 SIMD 加速路径, 日志/崩溃上报平台信息.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.System.GetPlatform ====================

static int l_System_GetPlatform(lua_State* L) {
    lua_pushstring(L, SDL_GetPlatform());
    return 1;
}

// ==================== Light.System.GetSDLVersion / Revision ====================

static int l_System_GetSDLVersion(lua_State* L) {
    // SDL_GetVersion 返回打包 int: major*1000000 + minor*1000 + patch
    int v = SDL_GetVersion();
    lua_createtable(L, 0, 3);
    lua_pushinteger(L, SDL_VERSIONNUM_MAJOR(v)); lua_setfield(L, -2, "major");
    lua_pushinteger(L, SDL_VERSIONNUM_MINOR(v)); lua_setfield(L, -2, "minor");
    lua_pushinteger(L, SDL_VERSIONNUM_MICRO(v)); lua_setfield(L, -2, "patch");
    return 1;
}

static int l_System_GetSDLRevision(lua_State* L) {
    const char* r = SDL_GetRevision();
    lua_pushstring(L, r ? r : "");
    return 1;
}

// ==================== Light.System.Get* scalars ====================

static int l_System_GetSystemRAM(lua_State* L) {
    lua_pushinteger(L, SDL_GetSystemRAM());
    return 1;
}

static int l_System_GetLogicalCPUCores(lua_State* L) {
    lua_pushinteger(L, SDL_GetNumLogicalCPUCores());
    return 1;
}

static int l_System_GetCPUCacheLineSize(lua_State* L) {
    lua_pushinteger(L, SDL_GetCPUCacheLineSize());
    return 1;
}

static int l_System_GetSIMDAlignment(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SDL_GetSIMDAlignment());
    return 1;
}

// ==================== Light.System.GetCPUFeatures ====================

static int l_System_GetCPUFeatures(lua_State* L) {
    lua_createtable(L, 0, 14);
#define SETBOOL(k, v) do { lua_pushboolean(L, (v) ? 1 : 0); lua_setfield(L, -2, k); } while (0)
    SETBOOL("mmx",      SDL_HasMMX());
    SETBOOL("sse",      SDL_HasSSE());
    SETBOOL("sse2",     SDL_HasSSE2());
    SETBOOL("sse3",     SDL_HasSSE3());
    SETBOOL("sse41",    SDL_HasSSE41());
    SETBOOL("sse42",    SDL_HasSSE42());
    SETBOOL("avx",      SDL_HasAVX());
    SETBOOL("avx2",     SDL_HasAVX2());
    SETBOOL("avx512f",  SDL_HasAVX512F());
    SETBOOL("neon",     SDL_HasNEON());
    SETBOOL("altivec",  SDL_HasAltiVec());
    SETBOOL("arm_simd", SDL_HasARMSIMD());
    SETBOOL("lsx",      SDL_HasLSX());
    SETBOOL("lasx",     SDL_HasLASX());
#undef SETBOOL
    return 1;
}

// ==================== luaopen_Light_System ====================

extern "C" LIGHT_API int luaopen_Light_System(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "GetPlatform",         l_System_GetPlatform         },
        { "GetSDLVersion",       l_System_GetSDLVersion       },
        { "GetSDLRevision",      l_System_GetSDLRevision      },
        { "GetSystemRAM",        l_System_GetSystemRAM        },
        { "GetLogicalCPUCores",  l_System_GetLogicalCPUCores  },
        { "GetCPUCacheLineSize", l_System_GetCPUCacheLineSize },
        { "GetSIMDAlignment",    l_System_GetSIMDAlignment    },
        { "GetCPUFeatures",      l_System_GetCPUFeatures      },
        { nullptr,               nullptr                      },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
