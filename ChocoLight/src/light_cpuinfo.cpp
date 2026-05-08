/**
 * @file light_cpuinfo.cpp
 * @brief Light.CPUInfo module - SDL3 CPU/RAM/SIMD feature detection
 *
 * Lua API (18 fns):
 *
 *  Hardware counts:
 *    Light.CPUInfo.GetNumLogicalCPUCores()    -> int
 *    Light.CPUInfo.GetCPUCacheLineSize()      -> int (bytes)
 *    Light.CPUInfo.GetSystemRAM()             -> int (MB)
 *    Light.CPUInfo.GetSIMDAlignment()         -> int (bytes)
 *
 *  PowerPC SIMD:
 *    Light.CPUInfo.HasAltiVec()               -> bool
 *
 *  x86 SIMD:
 *    Light.CPUInfo.HasMMX()                   -> bool
 *    Light.CPUInfo.HasSSE()                   -> bool
 *    Light.CPUInfo.HasSSE2()                  -> bool
 *    Light.CPUInfo.HasSSE3()                  -> bool
 *    Light.CPUInfo.HasSSE41()                 -> bool
 *    Light.CPUInfo.HasSSE42()                 -> bool
 *    Light.CPUInfo.HasAVX()                   -> bool
 *    Light.CPUInfo.HasAVX2()                  -> bool
 *    Light.CPUInfo.HasAVX512F()               -> bool
 *
 *  ARM SIMD:
 *    Light.CPUInfo.HasARMSIMD()               -> bool
 *    Light.CPUInfo.HasNEON()                  -> bool
 *
 *  LoongArch SIMD:
 *    Light.CPUInfo.HasLSX()                   -> bool
 *    Light.CPUInfo.HasLASX()                  -> bool
 *
 * Constants:
 *    CACHELINE_SIZE = 128  (worst-case for buffer alignment, per SDL header)
 *
 * No SDL_Init dependency. All queries are pure CPU dispatch / cpuid feature
 * detection probed lazily on first call.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ---------- helpers: simple int / bool wrappers ----------
#define LIGHT_CPU_INT_FN(name, sdlcall)                          \
    static int l_CPUInfo_##name(lua_State* L) {                  \
        lua_pushinteger(L, (lua_Integer)sdlcall());              \
        return 1;                                                \
    }

#define LIGHT_CPU_BOOL_FN(name, sdlcall)                         \
    static int l_CPUInfo_##name(lua_State* L) {                  \
        lua_pushboolean(L, sdlcall() ? 1 : 0);                   \
        return 1;                                                \
    }

LIGHT_CPU_INT_FN (GetNumLogicalCPUCores, SDL_GetNumLogicalCPUCores)
LIGHT_CPU_INT_FN (GetCPUCacheLineSize,   SDL_GetCPUCacheLineSize)
LIGHT_CPU_INT_FN (GetSystemRAM,          SDL_GetSystemRAM)
LIGHT_CPU_INT_FN (GetSIMDAlignment,      SDL_GetSIMDAlignment)

LIGHT_CPU_BOOL_FN(HasAltiVec,            SDL_HasAltiVec)
LIGHT_CPU_BOOL_FN(HasMMX,                SDL_HasMMX)
LIGHT_CPU_BOOL_FN(HasSSE,                SDL_HasSSE)
LIGHT_CPU_BOOL_FN(HasSSE2,               SDL_HasSSE2)
LIGHT_CPU_BOOL_FN(HasSSE3,               SDL_HasSSE3)
LIGHT_CPU_BOOL_FN(HasSSE41,              SDL_HasSSE41)
LIGHT_CPU_BOOL_FN(HasSSE42,              SDL_HasSSE42)
LIGHT_CPU_BOOL_FN(HasAVX,                SDL_HasAVX)
LIGHT_CPU_BOOL_FN(HasAVX2,               SDL_HasAVX2)
LIGHT_CPU_BOOL_FN(HasAVX512F,            SDL_HasAVX512F)
LIGHT_CPU_BOOL_FN(HasARMSIMD,            SDL_HasARMSIMD)
LIGHT_CPU_BOOL_FN(HasNEON,               SDL_HasNEON)
LIGHT_CPU_BOOL_FN(HasLSX,                SDL_HasLSX)
LIGHT_CPU_BOOL_FN(HasLASX,               SDL_HasLASX)

#undef LIGHT_CPU_INT_FN
#undef LIGHT_CPU_BOOL_FN

static const luaL_Reg kCPUInfoReg[] = {
    { "GetNumLogicalCPUCores", l_CPUInfo_GetNumLogicalCPUCores },
    { "GetCPUCacheLineSize",   l_CPUInfo_GetCPUCacheLineSize   },
    { "GetSystemRAM",          l_CPUInfo_GetSystemRAM          },
    { "GetSIMDAlignment",      l_CPUInfo_GetSIMDAlignment      },
    { "HasAltiVec",            l_CPUInfo_HasAltiVec            },
    { "HasMMX",                l_CPUInfo_HasMMX                },
    { "HasSSE",                l_CPUInfo_HasSSE                },
    { "HasSSE2",               l_CPUInfo_HasSSE2               },
    { "HasSSE3",               l_CPUInfo_HasSSE3               },
    { "HasSSE41",              l_CPUInfo_HasSSE41              },
    { "HasSSE42",              l_CPUInfo_HasSSE42              },
    { "HasAVX",                l_CPUInfo_HasAVX                },
    { "HasAVX2",               l_CPUInfo_HasAVX2               },
    { "HasAVX512F",            l_CPUInfo_HasAVX512F            },
    { "HasARMSIMD",            l_CPUInfo_HasARMSIMD            },
    { "HasNEON",               l_CPUInfo_HasNEON               },
    { "HasLSX",                l_CPUInfo_HasLSX                },
    { "HasLASX",               l_CPUInfo_HasLASX               },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_CPUInfo(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kCPUInfoReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    lua_pushinteger(L, (lua_Integer)SDL_CACHELINE_SIZE);
    lua_setfield(L, -2, "CACHELINE_SIZE");
    return 1;
}
