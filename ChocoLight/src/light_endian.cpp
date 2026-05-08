/**
 * @file light_endian.cpp
 * @brief Light.Endian module - SDL3 byte-swap + bit utilities
 *
 * Lua API (14 fns):
 *
 *  Unconditional byte swaps:
 *    Light.Endian.Swap16(x)        -> u16
 *    Light.Endian.Swap32(x)        -> u32
 *    Light.Endian.Swap64(x)        -> u64 (lossy >2^53)
 *    Light.Endian.SwapFloat(x)     -> float (round-trips bit pattern)
 *
 *  Little-endian forms - swap iff host is big-endian:
 *    Light.Endian.Swap16LE(x)      -> u16
 *    Light.Endian.Swap32LE(x)      -> u32
 *    Light.Endian.Swap64LE(x)      -> u64
 *    Light.Endian.SwapFloatLE(x)   -> float
 *
 *  Big-endian forms - swap iff host is little-endian:
 *    Light.Endian.Swap16BE(x)      -> u16
 *    Light.Endian.Swap32BE(x)      -> u32
 *    Light.Endian.Swap64BE(x)      -> u64
 *    Light.Endian.SwapFloatBE(x)   -> float
 *
 *  Bit utilities (SDL_bits.h):
 *    Light.Endian.MostSignificantBitIndex32(x) -> int (0..31, -1 if x=0)
 *    Light.Endian.HasExactlyOneBitSet32(x)     -> boolean
 *
 *  Constants:
 *    Light.Endian.LIL_ENDIAN       = 1234
 *    Light.Endian.BIG_ENDIAN       = 4321
 *    Light.Endian.BYTE_ORDER       = SDL_BYTEORDER          (host)
 *    Light.Endian.FLOAT_WORD_ORDER = SDL_FLOATWORDORDER     (host)
 *    Light.Endian.IS_LITTLE_ENDIAN = boolean convenience
 *
 * No SDL_Init dependency - all functions are header-only inlines / macros.
 *
 * Numeric conversions:
 *  - lua_Number is double (53-bit mantissa). u32 fits exactly. u64 values
 *    above 2^53 may lose low bits when round-tripped through Lua. For
 *    binary protocol parsing where 64-bit values are common, prefer
 *    splitting into hi/lo 32-bit halves on the Lua side before passing
 *    through Swap64.
 *  - Negative inputs are masked into the unsigned domain before swapping.
 *  - SwapFloat preserves bit pattern (NaN payloads survive).
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Lua passes numbers as double. Mask down to the requested width so that
// negative inputs (e.g. -1 representing 0xFFFFFFFF) round-trip cleanly.
static inline Uint16 read_u16(lua_State* L, int idx) {
    lua_Number n = luaL_checknumber(L, idx);
    int64_t i = (int64_t)n;
    return (Uint16)(i & 0xFFFF);
}
static inline Uint32 read_u32(lua_State* L, int idx) {
    lua_Number n = luaL_checknumber(L, idx);
    int64_t i = (int64_t)n;
    return (Uint32)(i & 0xFFFFFFFFu);
}
static inline Uint64 read_u64(lua_State* L, int idx) {
    lua_Number n = luaL_checknumber(L, idx);
    return (Uint64)(int64_t)n;
}
static inline float read_f32(lua_State* L, int idx) {
    return (float)luaL_checknumber(L, idx);
}

// ===========================================================
// Swap*
// ===========================================================
static int l_Endian_Swap16(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap16(read_u16(L, 1))); return 1;
}
static int l_Endian_Swap32(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap32(read_u32(L, 1))); return 1;
}
static int l_Endian_Swap64(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap64(read_u64(L, 1))); return 1;
}
static int l_Endian_SwapFloat(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_SwapFloat(read_f32(L, 1))); return 1;
}

// ===========================================================
// Swap*LE
// ===========================================================
static int l_Endian_Swap16LE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap16LE(read_u16(L, 1))); return 1;
}
static int l_Endian_Swap32LE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap32LE(read_u32(L, 1))); return 1;
}
static int l_Endian_Swap64LE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap64LE(read_u64(L, 1))); return 1;
}
static int l_Endian_SwapFloatLE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_SwapFloatLE(read_f32(L, 1))); return 1;
}

// ===========================================================
// Swap*BE
// ===========================================================
static int l_Endian_Swap16BE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap16BE(read_u16(L, 1))); return 1;
}
static int l_Endian_Swap32BE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap32BE(read_u32(L, 1))); return 1;
}
static int l_Endian_Swap64BE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_Swap64BE(read_u64(L, 1))); return 1;
}
static int l_Endian_SwapFloatBE(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_SwapFloatBE(read_f32(L, 1))); return 1;
}

// ===========================================================
// Bit utilities
// ===========================================================
static int l_Endian_MostSignificantBitIndex32(lua_State* L) {
    Uint32 x = read_u32(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_MostSignificantBitIndex32(x));
    return 1;
}
static int l_Endian_HasExactlyOneBitSet32(lua_State* L) {
    Uint32 x = read_u32(L, 1);
    lua_pushboolean(L, SDL_HasExactlyOneBitSet32(x) ? 1 : 0);
    return 1;
}

// ===========================================================
// luaopen_Light_Endian
// ===========================================================
static const luaL_Reg kEndianReg[] = {
    { "Swap16",                     l_Endian_Swap16                     },
    { "Swap32",                     l_Endian_Swap32                     },
    { "Swap64",                     l_Endian_Swap64                     },
    { "SwapFloat",                  l_Endian_SwapFloat                  },
    { "Swap16LE",                   l_Endian_Swap16LE                   },
    { "Swap32LE",                   l_Endian_Swap32LE                   },
    { "Swap64LE",                   l_Endian_Swap64LE                   },
    { "SwapFloatLE",                l_Endian_SwapFloatLE                },
    { "Swap16BE",                   l_Endian_Swap16BE                   },
    { "Swap32BE",                   l_Endian_Swap32BE                   },
    { "Swap64BE",                   l_Endian_Swap64BE                   },
    { "SwapFloatBE",                l_Endian_SwapFloatBE                },
    { "MostSignificantBitIndex32",  l_Endian_MostSignificantBitIndex32  },
    { "HasExactlyOneBitSet32",      l_Endian_HasExactlyOneBitSet32      },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Endian(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kEndianReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    lua_pushinteger(L, (lua_Integer)SDL_LIL_ENDIAN);
    lua_setfield(L, -2, "LIL_ENDIAN");
    lua_pushinteger(L, (lua_Integer)SDL_BIG_ENDIAN);
    lua_setfield(L, -2, "BIG_ENDIAN");
    lua_pushinteger(L, (lua_Integer)SDL_BYTEORDER);
    lua_setfield(L, -2, "BYTE_ORDER");
    lua_pushinteger(L, (lua_Integer)SDL_FLOATWORDORDER);
    lua_setfield(L, -2, "FLOAT_WORD_ORDER");
    lua_pushboolean(L, SDL_BYTEORDER == SDL_LIL_ENDIAN ? 1 : 0);
    lua_setfield(L, -2, "IS_LITTLE_ENDIAN");

    return 1;
}
