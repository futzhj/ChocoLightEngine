/**
 * @file light_rect.cpp
 * @brief Light.Rect module - SDL_rect.h (rectangle / point math, no init)
 *
 * Convention: rectangles and points are passed as flat scalars, never
 * userdata. This keeps the API allocation-free and trivially Lua-friendly.
 *
 *   Rect  is 4 args: x, y, w, h
 *   Point is 2 args: x, y
 *
 * Lua API (17 fns):
 *
 *  Integer (SDL_Rect / SDL_Point):
 *    PointInRect(px, py,  rx, ry, rw, rh)                 -> bool
 *    RectEmpty(x, y, w, h)                                -> bool
 *    RectsEqual(ax,ay,aw,ah, bx,by,bw,bh)                 -> bool
 *    HasRectIntersection(ax,ay,aw,ah, bx,by,bw,bh)        -> bool
 *    GetRectIntersection(ax,ay,aw,ah, bx,by,bw,bh)        -> rx,ry,rw,rh | nil
 *    GetRectUnion(ax,ay,aw,ah, bx,by,bw,bh)               -> rx,ry,rw,rh | nil,err
 *    GetRectEnclosingPoints({x1,y1,x2,y2,...}, [cx,cy,cw,ch])
 *                                                         -> rx,ry,rw,rh | nil
 *    GetRectAndLineIntersection(rx,ry,rw,rh, x1,y1,x2,y2) -> nx1,ny1,nx2,ny2 | nil
 *
 *  Float (SDL_FRect / SDL_FPoint):
 *    PointInRectFloat(px, py,  rx, ry, rw, rh)            -> bool
 *    RectEmptyFloat(x, y, w, h)                           -> bool
 *    RectsEqualFloat(...)                                 -> bool
 *    RectsEqualEpsilon(ax,ay,aw,ah, bx,by,bw,bh, eps)     -> bool
 *    HasRectIntersectionFloat(...)                        -> bool
 *    GetRectIntersectionFloat(...)                        -> rx,ry,rw,rh | nil
 *    GetRectUnionFloat(...)                               -> rx,ry,rw,rh | nil,err
 *    GetRectEnclosingPointsFloat({...}, [c4])             -> rx,ry,rw,rh | nil
 *    GetRectAndLineIntersectionFloat(rx,ry,rw,rh, x1,y1,x2,y2)
 *                                                         -> nx1,ny1,nx2,ny2 | nil
 *
 * Skipped:
 *  - SDL_RectToFRect: trivial in Lua (numbers are double); not exposed.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <vector>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// helpers - integer
// ============================================================
static SDL_Rect CheckRect(lua_State* L, int idx) {
    SDL_Rect r;
    r.x = (int)luaL_checkinteger(L, idx);
    r.y = (int)luaL_checkinteger(L, idx + 1);
    r.w = (int)luaL_checkinteger(L, idx + 2);
    r.h = (int)luaL_checkinteger(L, idx + 3);
    return r;
}

static SDL_Point CheckPoint(lua_State* L, int idx) {
    SDL_Point p;
    p.x = (int)luaL_checkinteger(L, idx);
    p.y = (int)luaL_checkinteger(L, idx + 1);
    return p;
}

static int PushRect(lua_State* L, const SDL_Rect& r) {
    lua_pushinteger(L, r.x);
    lua_pushinteger(L, r.y);
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 4;
}

// ============================================================
// helpers - float
// ============================================================
static SDL_FRect CheckFRect(lua_State* L, int idx) {
    SDL_FRect r;
    r.x = (float)luaL_checknumber(L, idx);
    r.y = (float)luaL_checknumber(L, idx + 1);
    r.w = (float)luaL_checknumber(L, idx + 2);
    r.h = (float)luaL_checknumber(L, idx + 3);
    return r;
}

static SDL_FPoint CheckFPoint(lua_State* L, int idx) {
    SDL_FPoint p;
    p.x = (float)luaL_checknumber(L, idx);
    p.y = (float)luaL_checknumber(L, idx + 1);
    return p;
}

static int PushFRect(lua_State* L, const SDL_FRect& r) {
    lua_pushnumber(L, (lua_Number)r.x);
    lua_pushnumber(L, (lua_Number)r.y);
    lua_pushnumber(L, (lua_Number)r.w);
    lua_pushnumber(L, (lua_Number)r.h);
    return 4;
}

// ============================================================
// integer API
// ============================================================
static int l_Rect_PointInRect(lua_State* L) {
    SDL_Point p = CheckPoint(L, 1);
    SDL_Rect r = CheckRect(L, 3);
    lua_pushboolean(L, SDL_PointInRect(&p, &r) ? 1 : 0);
    return 1;
}

static int l_Rect_RectEmpty(lua_State* L) {
    SDL_Rect r = CheckRect(L, 1);
    lua_pushboolean(L, SDL_RectEmpty(&r) ? 1 : 0);
    return 1;
}

static int l_Rect_RectsEqual(lua_State* L) {
    SDL_Rect a = CheckRect(L, 1);
    SDL_Rect b = CheckRect(L, 5);
    lua_pushboolean(L, SDL_RectsEqual(&a, &b) ? 1 : 0);
    return 1;
}

static int l_Rect_HasRectIntersection(lua_State* L) {
    SDL_Rect a = CheckRect(L, 1);
    SDL_Rect b = CheckRect(L, 5);
    lua_pushboolean(L, SDL_HasRectIntersection(&a, &b) ? 1 : 0);
    return 1;
}

static int l_Rect_GetRectIntersection(lua_State* L) {
    SDL_Rect a = CheckRect(L, 1);
    SDL_Rect b = CheckRect(L, 5);
    SDL_Rect out;
    if (!SDL_GetRectIntersection(&a, &b, &out)) {
        lua_pushnil(L);
        return 1;
    }
    return PushRect(L, out);
}

static int l_Rect_GetRectUnion(lua_State* L) {
    SDL_Rect a = CheckRect(L, 1);
    SDL_Rect b = CheckRect(L, 5);
    SDL_Rect out;
    if (!SDL_GetRectUnion(&a, &b, &out)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetRectUnion failed");
        return 2;
    }
    return PushRect(L, out);
}

// {x1,y1, x2,y2, ...} flat array; optional clip at args 2..5
static int l_Rect_GetRectEnclosingPoints(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int len = (int)lua_objlen(L, 1);
    if (len < 2 || (len % 2) != 0) {
        return luaL_error(L, "arg 1: points table must have even length >= 2, got %d", len);
    }
    int count = len / 2;
    std::vector<SDL_Point> pts((size_t)count);
    for (int i = 0; i < count; ++i) {
        lua_rawgeti(L, 1, i * 2 + 1);
        pts[(size_t)i].x = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, 1, i * 2 + 2);
        pts[(size_t)i].y = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    SDL_Rect clip;
    SDL_Rect* clip_ptr = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        clip = CheckRect(L, 2);
        clip_ptr = &clip;
    }

    SDL_Rect out;
    if (!SDL_GetRectEnclosingPoints(pts.data(), count, clip_ptr, &out)) {
        lua_pushnil(L);
        return 1;
    }
    return PushRect(L, out);
}

static int l_Rect_GetRectAndLineIntersection(lua_State* L) {
    SDL_Rect r = CheckRect(L, 1);
    int x1 = (int)luaL_checkinteger(L, 5);
    int y1 = (int)luaL_checkinteger(L, 6);
    int x2 = (int)luaL_checkinteger(L, 7);
    int y2 = (int)luaL_checkinteger(L, 8);
    if (!SDL_GetRectAndLineIntersection(&r, &x1, &y1, &x2, &y2)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, x1);
    lua_pushinteger(L, y1);
    lua_pushinteger(L, x2);
    lua_pushinteger(L, y2);
    return 4;
}

// ============================================================
// float API
// ============================================================
static int l_Rect_PointInRectFloat(lua_State* L) {
    SDL_FPoint p = CheckFPoint(L, 1);
    SDL_FRect r = CheckFRect(L, 3);
    lua_pushboolean(L, SDL_PointInRectFloat(&p, &r) ? 1 : 0);
    return 1;
}

static int l_Rect_RectEmptyFloat(lua_State* L) {
    SDL_FRect r = CheckFRect(L, 1);
    lua_pushboolean(L, SDL_RectEmptyFloat(&r) ? 1 : 0);
    return 1;
}

static int l_Rect_RectsEqualFloat(lua_State* L) {
    SDL_FRect a = CheckFRect(L, 1);
    SDL_FRect b = CheckFRect(L, 5);
    lua_pushboolean(L, SDL_RectsEqualFloat(&a, &b) ? 1 : 0);
    return 1;
}

static int l_Rect_RectsEqualEpsilon(lua_State* L) {
    SDL_FRect a = CheckFRect(L, 1);
    SDL_FRect b = CheckFRect(L, 5);
    float eps = (float)luaL_checknumber(L, 9);
    lua_pushboolean(L, SDL_RectsEqualEpsilon(&a, &b, eps) ? 1 : 0);
    return 1;
}

static int l_Rect_HasRectIntersectionFloat(lua_State* L) {
    SDL_FRect a = CheckFRect(L, 1);
    SDL_FRect b = CheckFRect(L, 5);
    lua_pushboolean(L, SDL_HasRectIntersectionFloat(&a, &b) ? 1 : 0);
    return 1;
}

static int l_Rect_GetRectIntersectionFloat(lua_State* L) {
    SDL_FRect a = CheckFRect(L, 1);
    SDL_FRect b = CheckFRect(L, 5);
    SDL_FRect out;
    if (!SDL_GetRectIntersectionFloat(&a, &b, &out)) {
        lua_pushnil(L);
        return 1;
    }
    return PushFRect(L, out);
}

static int l_Rect_GetRectUnionFloat(lua_State* L) {
    SDL_FRect a = CheckFRect(L, 1);
    SDL_FRect b = CheckFRect(L, 5);
    SDL_FRect out;
    if (!SDL_GetRectUnionFloat(&a, &b, &out)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetRectUnionFloat failed");
        return 2;
    }
    return PushFRect(L, out);
}

static int l_Rect_GetRectEnclosingPointsFloat(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int len = (int)lua_objlen(L, 1);
    if (len < 2 || (len % 2) != 0) {
        return luaL_error(L, "arg 1: points table must have even length >= 2, got %d", len);
    }
    int count = len / 2;
    std::vector<SDL_FPoint> pts((size_t)count);
    for (int i = 0; i < count; ++i) {
        lua_rawgeti(L, 1, i * 2 + 1);
        pts[(size_t)i].x = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_rawgeti(L, 1, i * 2 + 2);
        pts[(size_t)i].y = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }

    SDL_FRect clip;
    SDL_FRect* clip_ptr = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        clip = CheckFRect(L, 2);
        clip_ptr = &clip;
    }

    SDL_FRect out;
    if (!SDL_GetRectEnclosingPointsFloat(pts.data(), count, clip_ptr, &out)) {
        lua_pushnil(L);
        return 1;
    }
    return PushFRect(L, out);
}

static int l_Rect_GetRectAndLineIntersectionFloat(lua_State* L) {
    SDL_FRect r = CheckFRect(L, 1);
    float x1 = (float)luaL_checknumber(L, 5);
    float y1 = (float)luaL_checknumber(L, 6);
    float x2 = (float)luaL_checknumber(L, 7);
    float y2 = (float)luaL_checknumber(L, 8);
    if (!SDL_GetRectAndLineIntersectionFloat(&r, &x1, &y1, &x2, &y2)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)x1);
    lua_pushnumber(L, (lua_Number)y1);
    lua_pushnumber(L, (lua_Number)x2);
    lua_pushnumber(L, (lua_Number)y2);
    return 4;
}

// ============================================================
// luaopen
// ============================================================
extern "C" LIGHT_API int luaopen_Light_Rect(lua_State* L) {
    static const luaL_Reg fns[] = {
        // integer
        { "PointInRect",                    l_Rect_PointInRect                    },
        { "RectEmpty",                      l_Rect_RectEmpty                      },
        { "RectsEqual",                     l_Rect_RectsEqual                     },
        { "HasRectIntersection",            l_Rect_HasRectIntersection            },
        { "GetRectIntersection",            l_Rect_GetRectIntersection            },
        { "GetRectUnion",                   l_Rect_GetRectUnion                   },
        { "GetRectEnclosingPoints",         l_Rect_GetRectEnclosingPoints         },
        { "GetRectAndLineIntersection",     l_Rect_GetRectAndLineIntersection     },
        // float
        { "PointInRectFloat",               l_Rect_PointInRectFloat               },
        { "RectEmptyFloat",                 l_Rect_RectEmptyFloat                 },
        { "RectsEqualFloat",                l_Rect_RectsEqualFloat                },
        { "RectsEqualEpsilon",              l_Rect_RectsEqualEpsilon              },
        { "HasRectIntersectionFloat",       l_Rect_HasRectIntersectionFloat       },
        { "GetRectIntersectionFloat",       l_Rect_GetRectIntersectionFloat       },
        { "GetRectUnionFloat",              l_Rect_GetRectUnionFloat              },
        { "GetRectEnclosingPointsFloat",    l_Rect_GetRectEnclosingPointsFloat    },
        { "GetRectAndLineIntersectionFloat",l_Rect_GetRectAndLineIntersectionFloat},
        { nullptr, nullptr },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
