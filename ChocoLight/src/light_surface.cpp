/**
 * @file light_surface.cpp
 * @brief Light.Surface module - SDL_surface.h
 *
 * Pure software pixel surfaces. No SDL_Init required - all operations are
 * in-memory. This is the foundation that future image loaders (PNG/JPG via
 * SDL_image) and screenshot helpers will produce/consume.
 *
 * Convention:
 *   - Rectangles passed as 4 ints (x, y, w, h) or nil for "whole surface".
 *   - Colors passed as 4 floats 0..1 for ClearSurface, 4 Uint8 0..255 for
 *     ColorMod/MapRGBA/Read/WritePixel, packed Uint32 for FillRect.
 *   - All getters that return a single bool/int return that value directly,
 *     with nil + err string only on Get*-style helpers when the underlying
 *     surface produced an SDL error.
 *
 * Lua API (33 fns):
 *
 *  Construction / lifetime:
 *    CreateSurface(w, h, format)            -> ud | nil, err
 *    DestroySurface(s)
 *    DuplicateSurface(s)                    -> ud | nil, err
 *    ConvertSurface(s, format)              -> ud | nil, err
 *    ScaleSurface(s, w, h, scaleMode)       -> ud | nil, err
 *    LoadBMP(path)                          -> ud | nil, err
 *    SaveBMP(s, path)                       -> bool, err
 *
 *  Inspect (cheap, never fails):
 *    GetWidth(s)  -> int
 *    GetHeight(s) -> int
 *    GetFormat(s) -> int (SDL_PixelFormat)
 *    GetPitch(s)  -> int
 *    GetSize(s)   -> w, h
 *    GetSurfaceProperties(s) -> propid
 *
 *  Lock / pixel access:
 *    LockSurface(s)                         -> bool
 *    UnlockSurface(s)
 *    ReadSurfacePixel(s, x, y)              -> r, g, b, a (Uint8)
 *    WriteSurfacePixel(s, x, y, r, g, b, a) -> bool
 *
 *  Color/alpha mod, blend, clip, RLE, color key, flip:
 *    SetSurfaceColorMod(s, r, g, b)         -> bool
 *    GetSurfaceColorMod(s)                  -> r, g, b
 *    SetSurfaceAlphaMod(s, a)               -> bool
 *    GetSurfaceAlphaMod(s)                  -> a
 *    SetSurfaceBlendMode(s, mode)           -> bool
 *    GetSurfaceBlendMode(s)                 -> mode
 *    SetSurfaceColorKey(s, enabled, key)    -> bool
 *    GetSurfaceColorKey(s)                  -> key | nil
 *    SurfaceHasColorKey(s)                  -> bool
 *    SetSurfaceClipRect(s, x, y, w, h | nil) -> bool
 *    GetSurfaceClipRect(s)                  -> x, y, w, h
 *    SetSurfaceRLE(s, enabled)              -> bool
 *    SurfaceHasRLE(s)                       -> bool
 *    FlipSurface(s, mode)                   -> bool
 *
 *  Fill / clear / blit:
 *    ClearSurface(s, r, g, b, a)            -> bool          (floats 0..1)
 *    FillSurfaceRect(s, x, y, w, h | nil, color_u32) -> bool
 *    BlitSurface(src, src_rect|nil, dst, dst_rect|nil) -> bool
 *    BlitSurfaceScaled(src, src_rect|nil, dst, dst_rect|nil, scaleMode) -> bool
 *
 *  Pixel format helpers:
 *    MapSurfaceRGB(s, r, g, b)              -> uint32
 *    MapSurfaceRGBA(s, r, g, b, a)          -> uint32
 *
 * Constants:
 *    FLIP_NONE / FLIP_HORIZONTAL / FLIP_VERTICAL
 *    SCALEMODE_NEAREST / SCALEMODE_LINEAR / SCALEMODE_PIXELART
 *
 * NOT bound:
 *    LoadBMP_IO / SaveBMP_IO        - cross-module IOStream coupling
 *    CreateSurfaceFrom              - external buffer lifetime risk
 *    AlternateImages 4 fns          - HiDPI plate API, niche
 *    Palette 3 fns                  - separate Palette type, defer
 *    ConvertPixels / ConvertPixelsAndColorspace / ConvertSurfaceAndColorspace
 *                                   - raw buffer manipulation, complex
 *    PremultiplyAlpha (raw buffer)  - raw buffer
 *    BlitSurfaceUnchecked variants  - low-level, prefer the safe ones
 *    BlitSurfaceTiled / TiledWithScale / 9Grid - niche advanced blits
 *    Read/WriteSurfacePixelFloat    - covered by integer variants
 *
 * Lifetime:
 *    Each LSurface userdata stores SDL_Surface* and a "destroyed" flag.
 *    __gc auto-calls SDL_DestroySurface; explicit DestroySurface is also
 *    supported and is idempotent.
 */
#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7.1 — 类型安全 helpers + magic

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#define MT_SURFACE "Light.Surface.Surface"

/// Phase G.1.7.1: 首字段 magic 防止 type-confusion (双保险: metatable 名 + magic)
struct LSurface {
    uint32_t     magic;  // 必须 = LT_MAGIC_SURFACE (首字段)
    SDL_Surface* p;
};

// ============================================================
// helpers
// ============================================================
/// Phase G.1.7.1: magic 双保险
static LSurface* CheckHandle(lua_State* L, int idx) {
    auto* h = (LSurface*)luaL_checkudata(L, idx, MT_SURFACE);
    if (h && h->magic != LT::LT_MAGIC_SURFACE) {
        luaL_error(L, "Light.Surface: type confusion at arg #%d (magic mismatch)", idx);
    }
    return h;
}
static SDL_Surface* CheckLive(lua_State* L, int idx) {
    LSurface* h = CheckHandle(L, idx);
    if (!h->p) luaL_error(L, "surface has been destroyed");
    return h->p;
}
static int PushSdlError(lua_State* L) {
    lua_pushnil(L);
    const char* e = SDL_GetError();
    lua_pushstring(L, (e && *e) ? e : "SDL error");
    return 2;
}
static int NewHandle(lua_State* L, SDL_Surface* s) {
    if (!s) return PushSdlError(L);
    LSurface* h = (LSurface*)lua_newuserdata(L, sizeof(LSurface));
    h->magic = LT::LT_MAGIC_SURFACE;  // Phase G.1.7.1 — type tag
    h->p = s;
    luaL_getmetatable(L, MT_SURFACE);
    lua_setmetatable(L, -2);
    return 1;
}
// Try to read a 4-int rect starting at idx. We only treat the slot as a
// rect when args[idx..idx+3] are ALL numbers. This avoids ambiguity where
// FillSurfaceRect(s, color) passes a single number that would otherwise be
// mistaken for rect.x and crash on the missing rect.y. lua_isnumber on an
// out-of-range argument is documented as returning 0 (i.e., false), so it
// is safe to peek past the end of the call.
static bool TryReadRect(lua_State* L, int idx, SDL_Rect& r, int& next_idx) {
    if (!lua_isnumber(L, idx)     || !lua_isnumber(L, idx + 1) ||
        !lua_isnumber(L, idx + 2) || !lua_isnumber(L, idx + 3)) {
        next_idx = idx;
        return false;
    }
    r.x = (int)luaL_checkinteger(L, idx);
    r.y = (int)luaL_checkinteger(L, idx + 1);
    r.w = (int)luaL_checkinteger(L, idx + 2);
    r.h = (int)luaL_checkinteger(L, idx + 3);
    next_idx = idx + 4;
    return true;
}

// ============================================================
// Construction / lifetime
// ============================================================
static int l_S_CreateSurface(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    int fmt = (int)luaL_checkinteger(L, 3);
    return NewHandle(L, SDL_CreateSurface(w, h, (SDL_PixelFormat)fmt));
}
static int l_S_DestroySurface(lua_State* L) {
    LSurface* h = CheckHandle(L, 1);
    if (h->p) {
        SDL_DestroySurface(h->p);
        h->p = nullptr;
        h->magic = LT::LT_MAGIC_DEAD;  // Phase G.1.7.1 — 释放后不可再访问
    }
    return 0;
}
static int l_S_DuplicateSurface(lua_State* L) {
    return NewHandle(L, SDL_DuplicateSurface(CheckLive(L, 1)));
}
static int l_S_ConvertSurface(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    int fmt = (int)luaL_checkinteger(L, 2);
    return NewHandle(L, SDL_ConvertSurface(s, (SDL_PixelFormat)fmt));
}
static int l_S_ScaleSurface(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    int sm = (int)luaL_checkinteger(L, 4);
    return NewHandle(L, SDL_ScaleSurface(s, w, h, (SDL_ScaleMode)sm));
}
static int l_S_LoadBMP(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    return NewHandle(L, SDL_LoadBMP(path));
}
static int l_S_SaveBMP(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    const char* path = luaL_checkstring(L, 2);
    if (!SDL_SaveBMP(s, path)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SaveBMP failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_S_Gc(lua_State* L) {
    LSurface* h = (LSurface*)lua_touserdata(L, 1);
    if (h && h->p) { SDL_DestroySurface(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// Inspect
// ============================================================
static int l_S_GetWidth(lua_State* L) { lua_pushinteger(L, CheckLive(L, 1)->w); return 1; }
static int l_S_GetHeight(lua_State* L) { lua_pushinteger(L, CheckLive(L, 1)->h); return 1; }
static int l_S_GetFormat(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)CheckLive(L, 1)->format);
    return 1;
}
static int l_S_GetPitch(lua_State* L) { lua_pushinteger(L, CheckLive(L, 1)->pitch); return 1; }
static int l_S_GetSize(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    lua_pushinteger(L, s->w);
    lua_pushinteger(L, s->h);
    return 2;
}
static int l_S_GetSurfaceProperties(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetSurfaceProperties(CheckLive(L, 1)));
    return 1;
}

// ============================================================
// Lock / pixel
// ============================================================
static int l_S_LockSurface(lua_State* L) {
    lua_pushboolean(L, SDL_LockSurface(CheckLive(L, 1)) ? 1 : 0);
    return 1;
}
static int l_S_UnlockSurface(lua_State* L) {
    SDL_UnlockSurface(CheckLive(L, 1));
    return 0;
}
static int l_S_ReadSurfacePixel(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    Uint8 r, g, b, a;
    if (!SDL_ReadSurfacePixel(s, x, y, &r, &g, &b, &a)) return PushSdlError(L);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    lua_pushinteger(L, a);
    return 4;
}
static int l_S_WriteSurfacePixel(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    Uint8 r = (Uint8)luaL_checkinteger(L, 4);
    Uint8 g = (Uint8)luaL_checkinteger(L, 5);
    Uint8 b = (Uint8)luaL_checkinteger(L, 6);
    Uint8 a = (Uint8)luaL_checkinteger(L, 7);
    lua_pushboolean(L, SDL_WriteSurfacePixel(s, x, y, r, g, b, a) ? 1 : 0);
    return 1;
}

// ============================================================
// Mods / blend / clip / rle / colorkey / flip
// ============================================================
static int l_S_SetSurfaceColorMod(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 r = (Uint8)luaL_checkinteger(L, 2);
    Uint8 g = (Uint8)luaL_checkinteger(L, 3);
    Uint8 b = (Uint8)luaL_checkinteger(L, 4);
    lua_pushboolean(L, SDL_SetSurfaceColorMod(s, r, g, b) ? 1 : 0);
    return 1;
}
static int l_S_GetSurfaceColorMod(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 r, g, b;
    if (!SDL_GetSurfaceColorMod(s, &r, &g, &b)) return PushSdlError(L);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}
static int l_S_SetSurfaceAlphaMod(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 a = (Uint8)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_SetSurfaceAlphaMod(s, a) ? 1 : 0);
    return 1;
}
static int l_S_GetSurfaceAlphaMod(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 a;
    if (!SDL_GetSurfaceAlphaMod(s, &a)) return PushSdlError(L);
    lua_pushinteger(L, a);
    return 1;
}
static int l_S_SetSurfaceBlendMode(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    SDL_BlendMode m = (SDL_BlendMode)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_SetSurfaceBlendMode(s, m) ? 1 : 0);
    return 1;
}
static int l_S_GetSurfaceBlendMode(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    SDL_BlendMode m = SDL_BLENDMODE_NONE;
    if (!SDL_GetSurfaceBlendMode(s, &m)) return PushSdlError(L);
    lua_pushinteger(L, (lua_Integer)m);
    return 1;
}
static int l_S_SetSurfaceColorKey(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    bool en = lua_toboolean(L, 2) ? true : false;
    Uint32 key = (Uint32)luaL_checknumber(L, 3);
    lua_pushboolean(L, SDL_SetSurfaceColorKey(s, en, key) ? 1 : 0);
    return 1;
}
static int l_S_GetSurfaceColorKey(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint32 key;
    if (!SDL_GetSurfaceColorKey(s, &key)) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)key);
    return 1;
}
static int l_S_SurfaceHasColorKey(lua_State* L) {
    lua_pushboolean(L, SDL_SurfaceHasColorKey(CheckLive(L, 1)) ? 1 : 0);
    return 1;
}
static int l_S_SetSurfaceClipRect(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    SDL_Rect r;
    int next;
    bool has = TryReadRect(L, 2, r, next);
    lua_pushboolean(L, SDL_SetSurfaceClipRect(s, has ? &r : nullptr) ? 1 : 0);
    return 1;
}
static int l_S_GetSurfaceClipRect(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    SDL_Rect r;
    if (!SDL_GetSurfaceClipRect(s, &r)) return PushSdlError(L);
    lua_pushinteger(L, r.x);
    lua_pushinteger(L, r.y);
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 4;
}
static int l_S_SetSurfaceRLE(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    bool en = lua_toboolean(L, 2) ? true : false;
    lua_pushboolean(L, SDL_SetSurfaceRLE(s, en) ? 1 : 0);
    return 1;
}
static int l_S_SurfaceHasRLE(lua_State* L) {
    lua_pushboolean(L, SDL_SurfaceHasRLE(CheckLive(L, 1)) ? 1 : 0);
    return 1;
}
static int l_S_FlipSurface(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    SDL_FlipMode m = (SDL_FlipMode)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_FlipSurface(s, m) ? 1 : 0);
    return 1;
}

// ============================================================
// Fill / clear / blit
// ============================================================
static int l_S_ClearSurface(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    float r = (float)luaL_checknumber(L, 2);
    float g = (float)luaL_checknumber(L, 3);
    float b = (float)luaL_checknumber(L, 4);
    float a = (float)luaL_checknumber(L, 5);
    lua_pushboolean(L, SDL_ClearSurface(s, r, g, b, a) ? 1 : 0);
    return 1;
}
static int l_S_FillSurfaceRect(lua_State* L) {
    // FillSurfaceRect(s, color)               - whole surface
    // FillSurfaceRect(s, x, y, w, h, color)   - region
    SDL_Surface* s = CheckLive(L, 1);
    SDL_Rect r;
    int next;
    bool has = TryReadRect(L, 2, r, next);
    int color_idx = has ? next : 2;
    Uint32 color = (Uint32)luaL_checknumber(L, color_idx);
    lua_pushboolean(L, SDL_FillSurfaceRect(s, has ? &r : nullptr, color) ? 1 : 0);
    return 1;
}
// Variadic Blit signature accepted forms:
//   BlitSurface(src, dst)
//   BlitSurface(src, sx,sy,sw,sh, dst)
//   BlitSurface(src, dst, dx,dy,dw,dh)
//   BlitSurface(src, sx,sy,sw,sh, dst, dx,dy,dw,dh)
static int l_S_BlitSurface(lua_State* L) {
    SDL_Surface* src = CheckLive(L, 1);
    SDL_Rect sr;
    int next_after_src;
    bool hsr = TryReadRect(L, 2, sr, next_after_src);
    int dst_idx = hsr ? next_after_src : 2;
    SDL_Surface* dst = CheckLive(L, dst_idx);
    SDL_Rect dr;
    int next_after_dst;
    bool hdr = TryReadRect(L, dst_idx + 1, dr, next_after_dst);
    lua_pushboolean(L, SDL_BlitSurface(src, hsr ? &sr : nullptr,
                                       dst, hdr ? &dr : nullptr) ? 1 : 0);
    return 1;
}
// BlitSurfaceScaled: same variadic forms as BlitSurface, with the scale
// mode as the LAST argument.
static int l_S_BlitSurfaceScaled(lua_State* L) {
    SDL_Surface* src = CheckLive(L, 1);
    SDL_Rect sr;
    int next_after_src;
    bool hsr = TryReadRect(L, 2, sr, next_after_src);
    int dst_idx = hsr ? next_after_src : 2;
    SDL_Surface* dst = CheckLive(L, dst_idx);
    SDL_Rect dr;
    int next_after_dst;
    bool hdr = TryReadRect(L, dst_idx + 1, dr, next_after_dst);
    int sm_idx = hdr ? next_after_dst : (dst_idx + 1);
    SDL_ScaleMode sm = (SDL_ScaleMode)luaL_checkinteger(L, sm_idx);
    lua_pushboolean(L, SDL_BlitSurfaceScaled(src, hsr ? &sr : nullptr,
                                              dst, hdr ? &dr : nullptr, sm) ? 1 : 0);
    return 1;
}

// ============================================================
// Pixel format helpers
// ============================================================
static int l_S_MapSurfaceRGB(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 r = (Uint8)luaL_checkinteger(L, 2);
    Uint8 g = (Uint8)luaL_checkinteger(L, 3);
    Uint8 b = (Uint8)luaL_checkinteger(L, 4);
    lua_pushnumber(L, (lua_Number)SDL_MapSurfaceRGB(s, r, g, b));
    return 1;
}
static int l_S_MapSurfaceRGBA(lua_State* L) {
    SDL_Surface* s = CheckLive(L, 1);
    Uint8 r = (Uint8)luaL_checkinteger(L, 2);
    Uint8 g = (Uint8)luaL_checkinteger(L, 3);
    Uint8 b = (Uint8)luaL_checkinteger(L, 4);
    Uint8 a = (Uint8)luaL_checkinteger(L, 5);
    lua_pushnumber(L, (lua_Number)SDL_MapSurfaceRGBA(s, r, g, b, a));
    return 1;
}

// ============================================================
// luaopen
// ============================================================
static void RegisterMetatable(lua_State* L) {
    luaL_newmetatable(L, MT_SURFACE);
    lua_pushcfunction(L, l_S_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

static void SetIntField(lua_State* L, const char* name, lua_Integer v) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, name);
}

extern "C" LIGHT_API int luaopen_Light_Surface(lua_State* L) {
    RegisterMetatable(L);

    static const luaL_Reg fns[] = {
        // Construction / lifetime
        { "CreateSurface",          l_S_CreateSurface          },
        { "DestroySurface",         l_S_DestroySurface         },
        { "DuplicateSurface",       l_S_DuplicateSurface       },
        { "ConvertSurface",         l_S_ConvertSurface         },
        { "ScaleSurface",           l_S_ScaleSurface           },
        { "LoadBMP",                l_S_LoadBMP                },
        { "SaveBMP",                l_S_SaveBMP                },
        // Inspect
        { "GetWidth",               l_S_GetWidth               },
        { "GetHeight",              l_S_GetHeight              },
        { "GetFormat",              l_S_GetFormat              },
        { "GetPitch",               l_S_GetPitch               },
        { "GetSize",                l_S_GetSize                },
        { "GetSurfaceProperties",   l_S_GetSurfaceProperties   },
        // Lock / pixel
        { "LockSurface",            l_S_LockSurface            },
        { "UnlockSurface",          l_S_UnlockSurface          },
        { "ReadSurfacePixel",       l_S_ReadSurfacePixel       },
        { "WriteSurfacePixel",      l_S_WriteSurfacePixel      },
        // Mods / blend / clip / rle / colorkey / flip
        { "SetSurfaceColorMod",     l_S_SetSurfaceColorMod     },
        { "GetSurfaceColorMod",     l_S_GetSurfaceColorMod     },
        { "SetSurfaceAlphaMod",     l_S_SetSurfaceAlphaMod     },
        { "GetSurfaceAlphaMod",     l_S_GetSurfaceAlphaMod     },
        { "SetSurfaceBlendMode",    l_S_SetSurfaceBlendMode    },
        { "GetSurfaceBlendMode",    l_S_GetSurfaceBlendMode    },
        { "SetSurfaceColorKey",     l_S_SetSurfaceColorKey     },
        { "GetSurfaceColorKey",     l_S_GetSurfaceColorKey     },
        { "SurfaceHasColorKey",     l_S_SurfaceHasColorKey     },
        { "SetSurfaceClipRect",     l_S_SetSurfaceClipRect     },
        { "GetSurfaceClipRect",     l_S_GetSurfaceClipRect     },
        { "SetSurfaceRLE",          l_S_SetSurfaceRLE          },
        { "SurfaceHasRLE",          l_S_SurfaceHasRLE          },
        { "FlipSurface",            l_S_FlipSurface            },
        // Fill / clear / blit
        { "ClearSurface",           l_S_ClearSurface           },
        { "FillSurfaceRect",        l_S_FillSurfaceRect        },
        { "BlitSurface",            l_S_BlitSurface            },
        { "BlitSurfaceScaled",      l_S_BlitSurfaceScaled      },
        // Format helpers
        { "MapSurfaceRGB",          l_S_MapSurfaceRGB          },
        { "MapSurfaceRGBA",         l_S_MapSurfaceRGBA         },
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);

    // Constants
    SetIntField(L, "FLIP_NONE",          (lua_Integer)SDL_FLIP_NONE);
    SetIntField(L, "FLIP_HORIZONTAL",    (lua_Integer)SDL_FLIP_HORIZONTAL);
    SetIntField(L, "FLIP_VERTICAL",      (lua_Integer)SDL_FLIP_VERTICAL);
    SetIntField(L, "SCALEMODE_NEAREST",  (lua_Integer)SDL_SCALEMODE_NEAREST);
    SetIntField(L, "SCALEMODE_LINEAR",   (lua_Integer)SDL_SCALEMODE_LINEAR);
#ifdef SDL_SCALEMODE_PIXELART
    SetIntField(L, "SCALEMODE_PIXELART", (lua_Integer)SDL_SCALEMODE_PIXELART);
#endif
    return 1;
}
