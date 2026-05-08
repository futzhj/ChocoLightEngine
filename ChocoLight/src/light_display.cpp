/**
 * @file light_display.cpp
 * @brief Light.Display 模块 - 显示器枚举/几何/分辨率查询 (基于 SDL_video display 部分)
 *
 * Lua API:
 *   Light.Display.GetAll()                 -> { id1, id2, ... }, err
 *   Light.Display.GetPrimary()             -> id, err
 *   Light.Display.GetForPoint(x, y)        -> id, err
 *   Light.Display.GetName(id)              -> name_string, err
 *   Light.Display.GetBounds(id)            -> {x,y,w,h}, err           (含任务栏的全部区域)
 *   Light.Display.GetUsableBounds(id)      -> {x,y,w,h}, err           (扣除任务栏/Dock 的可用区)
 *   Light.Display.GetContentScale(id)      -> scale_float, err         (HiDPI 缩放, 1.0=100%)
 *   Light.Display.GetCurrentMode(id)       -> {w,h,format,refresh,pixel_density}, err
 *
 * 平台覆盖: Win/Mac/Linux/Android/iOS 完整, Web 仅返回单一窗口尺寸
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 内部: 把 SDL_Rect 推到栈顶作为表 {x,y,w,h}
static void PushRect(lua_State* L, const SDL_Rect& r) {
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, r.x); lua_setfield(L, -2, "x");
    lua_pushinteger(L, r.y); lua_setfield(L, -2, "y");
    lua_pushinteger(L, r.w); lua_setfield(L, -2, "w");
    lua_pushinteger(L, r.h); lua_setfield(L, -2, "h");
}

// ==================== Light.Display.GetAll ====================

static int l_Display_GetAll(lua_State* L) {
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (!ids) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushinteger(L, (lua_Integer)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetPrimary ====================

static int l_Display_GetPrimary(lua_State* L) {
    SDL_DisplayID id = SDL_GetPrimaryDisplay();
    if (id == 0) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetForPoint ====================

static int l_Display_GetForPoint(lua_State* L) {
    SDL_Point p;
    p.x = (int)luaL_checkinteger(L, 1);
    p.y = (int)luaL_checkinteger(L, 2);
    SDL_DisplayID id = SDL_GetDisplayForPoint(&p);
    if (id == 0) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetName ====================

static int l_Display_GetName(lua_State* L) {
    SDL_DisplayID id = (SDL_DisplayID)luaL_checkinteger(L, 1);
    const char* name = SDL_GetDisplayName(id);
    if (!name) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetBounds ====================

static int l_Display_GetBounds(lua_State* L) {
    SDL_DisplayID id = (SDL_DisplayID)luaL_checkinteger(L, 1);
    SDL_Rect r;
    if (!SDL_GetDisplayBounds(id, &r)) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    PushRect(L, r);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetUsableBounds ====================

static int l_Display_GetUsableBounds(lua_State* L) {
    SDL_DisplayID id = (SDL_DisplayID)luaL_checkinteger(L, 1);
    SDL_Rect r;
    if (!SDL_GetDisplayUsableBounds(id, &r)) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    PushRect(L, r);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetContentScale ====================

static int l_Display_GetContentScale(lua_State* L) {
    SDL_DisplayID id = (SDL_DisplayID)luaL_checkinteger(L, 1);
    float scale = SDL_GetDisplayContentScale(id);
    if (scale <= 0.0f) {
        // 失败时 SDL 返回 0.0, 同时设置 GetError
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnumber(L, (lua_Number)scale);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Display.GetCurrentMode ====================

static int l_Display_GetCurrentMode(lua_State* L) {
    SDL_DisplayID id = (SDL_DisplayID)luaL_checkinteger(L, 1);
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(id);
    if (!mode) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, mode->w);                 lua_setfield(L, -2, "w");
    lua_pushinteger(L, mode->h);                 lua_setfield(L, -2, "h");
    lua_pushinteger(L, (lua_Integer)mode->format); lua_setfield(L, -2, "format");
    lua_pushnumber(L,  (lua_Number)mode->refresh_rate); lua_setfield(L, -2, "refresh_rate");
    lua_pushnumber(L,  (lua_Number)mode->pixel_density); lua_setfield(L, -2, "pixel_density");
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Display ====================

extern "C" LIGHT_API int luaopen_Light_Display(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "GetAll",            l_Display_GetAll            },
        { "GetPrimary",        l_Display_GetPrimary        },
        { "GetForPoint",       l_Display_GetForPoint       },
        { "GetName",           l_Display_GetName           },
        { "GetBounds",         l_Display_GetBounds         },
        { "GetUsableBounds",   l_Display_GetUsableBounds   },
        { "GetContentScale",   l_Display_GetContentScale   },
        { "GetCurrentMode",    l_Display_GetCurrentMode    },
        { nullptr,             nullptr                     },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
