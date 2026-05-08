/**
 * @file light_pixels.cpp
 * @brief Light.Pixels module - SDL3 pixel format + palette
 *
 * Lua API (12 fns):
 *
 *  Pixel format introspection:
 *    Light.Pixels.GetPixelFormatName(format)             -> string
 *    Light.Pixels.GetMasksForPixelFormat(format)         -> table | nil, err
 *    Light.Pixels.GetPixelFormatForMasks(bpp,R,G,B,A)    -> format
 *    Light.Pixels.GetPixelFormatDetails(format)          -> table | nil, err
 *
 *  Color <-> pixel:
 *    Light.Pixels.MapRGB(format, r, g, b, [palette])     -> u32
 *    Light.Pixels.MapRGBA(format, r, g, b, a, [palette]) -> u32
 *    Light.Pixels.GetRGB(pixel, format, [palette])       -> r, g, b
 *    Light.Pixels.GetRGBA(pixel, format, [palette])      -> r, g, b, a
 *
 *  Palette CRUD (lightuserdata handle, explicit Destroy):
 *    Light.Pixels.CreatePalette(ncolors)                 -> handle | nil, err
 *    Light.Pixels.SetPaletteColors(handle, colors, [first]) -> ok, err
 *    Light.Pixels.DestroyPalette(handle)
 *    Light.Pixels.PaletteSize(handle)                    -> ncolors
 *
 *  Constants:
 *    PIXELFORMAT_*    (29 most-used formats: indexed/packed/array/YUV)
 *    COLORSPACE_*     (8 stable colorspaces incl. defaults)
 *
 * Notes:
 *  - The MapRGB / GetRGB family takes a SDL_PixelFormat enum (not a
 *    SDL_PixelFormatDetails*). Internally we call SDL_GetPixelFormatDetails
 *    on every invocation. SDL caches details internally so this is cheap.
 *  - Palette is optional; only required for INDEX* formats and ignored
 *    for direct-color formats. Pass nil/none to omit.
 *  - SetPaletteColors accepts a table of color tables: {{r,g,b,a}, ...}
 *    or {{r=,g=,b=,a=}, ...}. Alpha defaults to 255 if omitted.
 *  - 8-bit channel inputs are clamped to [0,255] silently.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Helpers
// ============================================================
static SDL_PixelFormat CheckPixelFormat(lua_State* L, int idx) {
    return (SDL_PixelFormat)(lua_Integer)luaL_checkinteger(L, idx);
}

static Uint8 ClampU8(lua_Integer v) {
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (Uint8)v;
}

// Optional palette argument (nil/none -> nullptr).
// On error (wrong type), raise a Lua error.
static SDL_Palette* CheckOptPalette(lua_State* L, int idx) {
    if (lua_isnoneornil(L, idx)) return nullptr;
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Palette*)lua_touserdata(L, idx);
    }
    luaL_error(L, "palette must be a Light.Pixels palette handle or nil");
    return nullptr;
}

static SDL_Palette* CheckPalette(lua_State* L, int idx) {
    if (!lua_islightuserdata(L, idx)) {
        luaL_error(L, "palette must be a Light.Pixels palette handle");
        return nullptr;
    }
    SDL_Palette* p = (SDL_Palette*)lua_touserdata(L, idx);
    if (!p) {
        luaL_error(L, "palette handle is null");
        return nullptr;
    }
    return p;
}

// ============================================================
// Format introspection
// ============================================================
static int l_Pixels_GetPixelFormatName(lua_State* L) {
    SDL_PixelFormat fmt = CheckPixelFormat(L, 1);
    const char* name = SDL_GetPixelFormatName(fmt);
    lua_pushstring(L, name ? name : "");
    return 1;
}

static int l_Pixels_GetMasksForPixelFormat(lua_State* L) {
    SDL_PixelFormat fmt = CheckPixelFormat(L, 1);
    int bpp = 0;
    Uint32 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_GetMasksForPixelFormat(fmt, &bpp, &r, &g, &b, &a)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetMasksForPixelFormat failed");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, bpp);          lua_setfield(L, -2, "bpp");
    lua_pushnumber(L, (lua_Number)r); lua_setfield(L, -2, "Rmask");
    lua_pushnumber(L, (lua_Number)g); lua_setfield(L, -2, "Gmask");
    lua_pushnumber(L, (lua_Number)b); lua_setfield(L, -2, "Bmask");
    lua_pushnumber(L, (lua_Number)a); lua_setfield(L, -2, "Amask");
    return 1;
}

static int l_Pixels_GetPixelFormatForMasks(lua_State* L) {
    int bpp = (int)luaL_checkinteger(L, 1);
    Uint32 rm = (Uint32)(int64_t)luaL_checknumber(L, 2);
    Uint32 gm = (Uint32)(int64_t)luaL_checknumber(L, 3);
    Uint32 bm = (Uint32)(int64_t)luaL_checknumber(L, 4);
    Uint32 am = (Uint32)(int64_t)luaL_checknumber(L, 5);
    SDL_PixelFormat fmt = SDL_GetPixelFormatForMasks(bpp, rm, gm, bm, am);
    lua_pushinteger(L, (lua_Integer)fmt);
    return 1;
}

static int l_Pixels_GetPixelFormatDetails(lua_State* L) {
    SDL_PixelFormat fmt = CheckPixelFormat(L, 1);
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(fmt);
    if (!d) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetPixelFormatDetails failed");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)d->format);          lua_setfield(L, -2, "format");
    lua_pushinteger(L, d->bits_per_pixel);                lua_setfield(L, -2, "bits_per_pixel");
    lua_pushinteger(L, d->bytes_per_pixel);               lua_setfield(L, -2, "bytes_per_pixel");
    lua_pushnumber(L,  (lua_Number)d->Rmask);             lua_setfield(L, -2, "Rmask");
    lua_pushnumber(L,  (lua_Number)d->Gmask);             lua_setfield(L, -2, "Gmask");
    lua_pushnumber(L,  (lua_Number)d->Bmask);             lua_setfield(L, -2, "Bmask");
    lua_pushnumber(L,  (lua_Number)d->Amask);             lua_setfield(L, -2, "Amask");
    lua_pushinteger(L, d->Rbits);                         lua_setfield(L, -2, "Rbits");
    lua_pushinteger(L, d->Gbits);                         lua_setfield(L, -2, "Gbits");
    lua_pushinteger(L, d->Bbits);                         lua_setfield(L, -2, "Bbits");
    lua_pushinteger(L, d->Abits);                         lua_setfield(L, -2, "Abits");
    lua_pushinteger(L, d->Rshift);                        lua_setfield(L, -2, "Rshift");
    lua_pushinteger(L, d->Gshift);                        lua_setfield(L, -2, "Gshift");
    lua_pushinteger(L, d->Bshift);                        lua_setfield(L, -2, "Bshift");
    lua_pushinteger(L, d->Ashift);                        lua_setfield(L, -2, "Ashift");
    return 1;
}

// ============================================================
// Map / Get RGB[A]
// ============================================================
static int l_Pixels_MapRGB(lua_State* L) {
    SDL_PixelFormat fmt = CheckPixelFormat(L, 1);
    Uint8 r = ClampU8(luaL_checkinteger(L, 2));
    Uint8 g = ClampU8(luaL_checkinteger(L, 3));
    Uint8 b = ClampU8(luaL_checkinteger(L, 4));
    SDL_Palette* pal = CheckOptPalette(L, 5);
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(fmt);
    if (!d) {
        return luaL_error(L, "MapRGB: invalid pixel format %d", (int)fmt);
    }
    Uint32 px = SDL_MapRGB(d, pal, r, g, b);
    lua_pushnumber(L, (lua_Number)px);
    return 1;
}

static int l_Pixels_MapRGBA(lua_State* L) {
    SDL_PixelFormat fmt = CheckPixelFormat(L, 1);
    Uint8 r = ClampU8(luaL_checkinteger(L, 2));
    Uint8 g = ClampU8(luaL_checkinteger(L, 3));
    Uint8 b = ClampU8(luaL_checkinteger(L, 4));
    Uint8 a = ClampU8(luaL_checkinteger(L, 5));
    SDL_Palette* pal = CheckOptPalette(L, 6);
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(fmt);
    if (!d) {
        return luaL_error(L, "MapRGBA: invalid pixel format %d", (int)fmt);
    }
    Uint32 px = SDL_MapRGBA(d, pal, r, g, b, a);
    lua_pushnumber(L, (lua_Number)px);
    return 1;
}

static int l_Pixels_GetRGB(lua_State* L) {
    Uint32 pixel = (Uint32)(int64_t)luaL_checknumber(L, 1);
    SDL_PixelFormat fmt = CheckPixelFormat(L, 2);
    SDL_Palette* pal = CheckOptPalette(L, 3);
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(fmt);
    if (!d) {
        return luaL_error(L, "GetRGB: invalid pixel format %d", (int)fmt);
    }
    Uint8 r = 0, g = 0, b = 0;
    SDL_GetRGB(pixel, d, pal, &r, &g, &b);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    return 3;
}

static int l_Pixels_GetRGBA(lua_State* L) {
    Uint32 pixel = (Uint32)(int64_t)luaL_checknumber(L, 1);
    SDL_PixelFormat fmt = CheckPixelFormat(L, 2);
    SDL_Palette* pal = CheckOptPalette(L, 3);
    const SDL_PixelFormatDetails* d = SDL_GetPixelFormatDetails(fmt);
    if (!d) {
        return luaL_error(L, "GetRGBA: invalid pixel format %d", (int)fmt);
    }
    Uint8 r = 0, g = 0, b = 0, a = 0;
    SDL_GetRGBA(pixel, d, pal, &r, &g, &b, &a);
    lua_pushinteger(L, r);
    lua_pushinteger(L, g);
    lua_pushinteger(L, b);
    lua_pushinteger(L, a);
    return 4;
}

// ============================================================
// Palette
// ============================================================
static int l_Pixels_CreatePalette(lua_State* L) {
    int ncolors = (int)luaL_checkinteger(L, 1);
    if (ncolors <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "ncolors must be > 0");
        return 2;
    }
    SDL_Palette* p = SDL_CreatePalette(ncolors);
    if (!p) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CreatePalette failed");
        return 2;
    }
    lua_pushlightuserdata(L, p);
    return 1;
}

// Read color array: { {r,g,b[,a]}, ... } or { {r=,g=,b=[,a=]}, ... }
// Alpha defaults to 255.
// On any malformed entry, raises a lua error with the row index.
static void ReadColorArray(lua_State* L, int idx, std::vector<SDL_Color>& out) {
    luaL_checktype(L, idx, LUA_TTABLE);
    int n = (int)lua_objlen(L, idx);
    out.reserve((size_t)n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (!lua_istable(L, -1)) {
            luaL_error(L, "colors[%d] is not a table", i);
        }
        SDL_Color c{};
        // try named keys first; fall back to positional.
        lua_getfield(L, -1, "r");
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 1);
            c.r = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 2);
            c.g = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 3);
            c.b = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_rawgeti(L, -1, 4);
            c.a = lua_isnil(L, -1) ? 255 : ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
        } else {
            c.r = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "g");
            c.g = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "b");
            c.b = ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
            lua_getfield(L, -1, "a");
            c.a = lua_isnil(L, -1) ? 255 : ClampU8(luaL_checkinteger(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1); // pop row table
        out.push_back(c);
    }
}

static int l_Pixels_SetPaletteColors(lua_State* L) {
    SDL_Palette* p = CheckPalette(L, 1);
    std::vector<SDL_Color> colors;
    ReadColorArray(L, 2, colors);
    int firstcolor = (int)luaL_optinteger(L, 3, 0);
    if (!SDL_SetPaletteColors(p, colors.data(), firstcolor, (int)colors.size())) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SetPaletteColors failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_Pixels_DestroyPalette(lua_State* L) {
    if (lua_isnoneornil(L, 1)) return 0;
    SDL_Palette* p = CheckPalette(L, 1);
    SDL_DestroyPalette(p);
    return 0;
}

static int l_Pixels_PaletteSize(lua_State* L) {
    SDL_Palette* p = CheckPalette(L, 1);
    lua_pushinteger(L, (lua_Integer)p->ncolors);
    return 1;
}

// ============================================================
// luaopen_Light_Pixels
// ============================================================
static const luaL_Reg kPixelsReg[] = {
    { "GetPixelFormatName",         l_Pixels_GetPixelFormatName         },
    { "GetMasksForPixelFormat",     l_Pixels_GetMasksForPixelFormat     },
    { "GetPixelFormatForMasks",     l_Pixels_GetPixelFormatForMasks     },
    { "GetPixelFormatDetails",      l_Pixels_GetPixelFormatDetails      },
    { "MapRGB",                     l_Pixels_MapRGB                     },
    { "MapRGBA",                    l_Pixels_MapRGBA                    },
    { "GetRGB",                     l_Pixels_GetRGB                     },
    { "GetRGBA",                    l_Pixels_GetRGBA                    },
    { "CreatePalette",              l_Pixels_CreatePalette              },
    { "SetPaletteColors",           l_Pixels_SetPaletteColors           },
    { "DestroyPalette",             l_Pixels_DestroyPalette             },
    { "PaletteSize",                l_Pixels_PaletteSize                },
    { nullptr, nullptr },
};

#define LIGHT_PIXELS_PUSH_FORMAT(NAME)                              \
    lua_pushinteger(L, (lua_Integer)SDL_PIXELFORMAT_##NAME);        \
    lua_setfield(L, -2, "PIXELFORMAT_" #NAME)

#define LIGHT_PIXELS_PUSH_COLORSPACE(NAME)                          \
    lua_pushinteger(L, (lua_Integer)SDL_COLORSPACE_##NAME);         \
    lua_setfield(L, -2, "COLORSPACE_" #NAME)

extern "C" LIGHT_API int luaopen_Light_Pixels(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kPixelsReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Pixel formats - common 8/16/24/32-bit, 64/128-bit float, indexed, YUV
    LIGHT_PIXELS_PUSH_FORMAT(UNKNOWN);
    LIGHT_PIXELS_PUSH_FORMAT(INDEX1LSB);
    LIGHT_PIXELS_PUSH_FORMAT(INDEX1MSB);
    LIGHT_PIXELS_PUSH_FORMAT(INDEX4LSB);
    LIGHT_PIXELS_PUSH_FORMAT(INDEX4MSB);
    LIGHT_PIXELS_PUSH_FORMAT(INDEX8);
    LIGHT_PIXELS_PUSH_FORMAT(RGB332);
    LIGHT_PIXELS_PUSH_FORMAT(XRGB4444);
    LIGHT_PIXELS_PUSH_FORMAT(XBGR4444);
    LIGHT_PIXELS_PUSH_FORMAT(ARGB4444);
    LIGHT_PIXELS_PUSH_FORMAT(RGBA4444);
    LIGHT_PIXELS_PUSH_FORMAT(ABGR4444);
    LIGHT_PIXELS_PUSH_FORMAT(BGRA4444);
    LIGHT_PIXELS_PUSH_FORMAT(XRGB1555);
    LIGHT_PIXELS_PUSH_FORMAT(ARGB1555);
    LIGHT_PIXELS_PUSH_FORMAT(RGBA5551);
    LIGHT_PIXELS_PUSH_FORMAT(RGB565);
    LIGHT_PIXELS_PUSH_FORMAT(BGR565);
    LIGHT_PIXELS_PUSH_FORMAT(RGB24);
    LIGHT_PIXELS_PUSH_FORMAT(BGR24);
    LIGHT_PIXELS_PUSH_FORMAT(XRGB8888);
    LIGHT_PIXELS_PUSH_FORMAT(RGBX8888);
    LIGHT_PIXELS_PUSH_FORMAT(XBGR8888);
    LIGHT_PIXELS_PUSH_FORMAT(BGRX8888);
    LIGHT_PIXELS_PUSH_FORMAT(ARGB8888);
    LIGHT_PIXELS_PUSH_FORMAT(RGBA8888);
    LIGHT_PIXELS_PUSH_FORMAT(ABGR8888);
    LIGHT_PIXELS_PUSH_FORMAT(BGRA8888);
    LIGHT_PIXELS_PUSH_FORMAT(YV12);
    LIGHT_PIXELS_PUSH_FORMAT(IYUV);
    LIGHT_PIXELS_PUSH_FORMAT(YUY2);

    // Colorspaces
    LIGHT_PIXELS_PUSH_COLORSPACE(UNKNOWN);
    LIGHT_PIXELS_PUSH_COLORSPACE(SRGB);
    LIGHT_PIXELS_PUSH_COLORSPACE(SRGB_LINEAR);
    LIGHT_PIXELS_PUSH_COLORSPACE(HDR10);
    LIGHT_PIXELS_PUSH_COLORSPACE(JPEG);
    LIGHT_PIXELS_PUSH_COLORSPACE(BT601_LIMITED);
    LIGHT_PIXELS_PUSH_COLORSPACE(BT709_LIMITED);
    LIGHT_PIXELS_PUSH_COLORSPACE(BT2020_LIMITED);
    LIGHT_PIXELS_PUSH_COLORSPACE(RGB_DEFAULT);
    LIGHT_PIXELS_PUSH_COLORSPACE(YUV_DEFAULT);

    return 1;
}

#undef LIGHT_PIXELS_PUSH_FORMAT
#undef LIGHT_PIXELS_PUSH_COLORSPACE
