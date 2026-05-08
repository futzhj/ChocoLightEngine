/**
 * @file light_cursor.cpp
 * @brief Light.Cursor 模块 - 鼠标光标控制 (基于 SDL_mouse 的 cursor 部分)
 *
 * Lua API:
 *   Light.Cursor.CreateSystem(name)        -> handle, err
 *     - name: "default"|"text"|"wait"|"crosshair"|"progress"|"pointer"|
 *             "move"|"not_allowed"|"ew_resize"|"ns_resize"|
 *             "nesw_resize"|"nwse_resize"|"n_resize"|"s_resize"|
 *             "e_resize"|"w_resize"|"ne_resize"|"nw_resize"|
 *             "se_resize"|"sw_resize"
 *
 *   Light.Cursor.Set(handle)               -> ok, err            (handle=nil 表示恢复默认)
 *   Light.Cursor.Destroy(handle)           -> ok, err
 *   Light.Cursor.Show()                    -> ok, err
 *   Light.Cursor.Hide()                    -> ok, err
 *   Light.Cursor.IsVisible()               -> bool
 *   Light.Cursor.WarpGlobal(x, y)          -> ok, err            (全局屏幕坐标)
 *
 * 平台覆盖: Win/Mac/Linux 完整, Web/Mobile 系统光标受限
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 名称 → SDL_SystemCursor 枚举映射 (大小写不敏感, 未知名返回 -1)
static int ParseSystemCursor(const char* s) {
    if (!s) return -1;
    struct Map { const char* name; int val; };
    static const Map kMap[] = {
        { "default",     SDL_SYSTEM_CURSOR_DEFAULT      },
        { "text",        SDL_SYSTEM_CURSOR_TEXT         },
        { "wait",        SDL_SYSTEM_CURSOR_WAIT         },
        { "crosshair",   SDL_SYSTEM_CURSOR_CROSSHAIR    },
        { "progress",    SDL_SYSTEM_CURSOR_PROGRESS     },
        { "pointer",     SDL_SYSTEM_CURSOR_POINTER      },
        { "move",        SDL_SYSTEM_CURSOR_MOVE         },
        { "not_allowed", SDL_SYSTEM_CURSOR_NOT_ALLOWED  },
        { "ew_resize",   SDL_SYSTEM_CURSOR_EW_RESIZE    },
        { "ns_resize",   SDL_SYSTEM_CURSOR_NS_RESIZE    },
        { "nesw_resize", SDL_SYSTEM_CURSOR_NESW_RESIZE  },
        { "nwse_resize", SDL_SYSTEM_CURSOR_NWSE_RESIZE  },
        { "n_resize",    SDL_SYSTEM_CURSOR_N_RESIZE     },
        { "s_resize",    SDL_SYSTEM_CURSOR_S_RESIZE     },
        { "e_resize",    SDL_SYSTEM_CURSOR_E_RESIZE     },
        { "w_resize",    SDL_SYSTEM_CURSOR_W_RESIZE     },
        { "ne_resize",   SDL_SYSTEM_CURSOR_NE_RESIZE    },
        { "nw_resize",   SDL_SYSTEM_CURSOR_NW_RESIZE    },
        { "se_resize",   SDL_SYSTEM_CURSOR_SE_RESIZE    },
        { "sw_resize",   SDL_SYSTEM_CURSOR_SW_RESIZE    },
    };
    for (const auto& m : kMap) {
        if (SDL_strcasecmp(s, m.name) == 0) return m.val;
    }
    return -1;
}

// ==================== Light.Cursor.CreateSystem ====================

static int l_Cursor_CreateSystem(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    int sc = ParseSystemCursor(name);
    if (sc < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "unknown system cursor name: %s", name);
        return 2;
    }
    SDL_Cursor* cur = SDL_CreateSystemCursor((SDL_SystemCursor)sc);
    if (!cur) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, cur);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Cursor.Set ====================

static int l_Cursor_Set(lua_State* L) {
    // 允许 handle=nil → 用 SDL_GetDefaultCursor 恢复默认
    SDL_Cursor* cur = nullptr;
    if (lua_isnoneornil(L, 1)) {
        cur = SDL_GetDefaultCursor();
    } else {
        cur = (SDL_Cursor*)lua_touserdata(L, 1);
    }
    if (!cur) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "no cursor available (no active video subsystem?)");
        return 2;
    }
    bool ok = SDL_SetCursor(cur);
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Cursor.Destroy ====================

static int l_Cursor_Destroy(lua_State* L) {
    SDL_Cursor* cur = (SDL_Cursor*)lua_touserdata(L, 1);
    if (!cur) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid cursor handle");
        return 2;
    }
    SDL_DestroyCursor(cur);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Cursor.Show / Hide / IsVisible ====================

static int l_Cursor_Show(lua_State* L) {
    bool ok = SDL_ShowCursor();
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Cursor_Hide(lua_State* L) {
    bool ok = SDL_HideCursor();
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Cursor_IsVisible(lua_State* L) {
    lua_pushboolean(L, SDL_CursorVisible() ? 1 : 0);
    return 1;
}

// ==================== Light.Cursor.WarpGlobal ====================

static int l_Cursor_WarpGlobal(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    bool ok = SDL_WarpMouseGlobal(x, y);
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Cursor ====================

extern "C" LIGHT_API int luaopen_Light_Cursor(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "CreateSystem", l_Cursor_CreateSystem },
        { "Set",          l_Cursor_Set          },
        { "Destroy",      l_Cursor_Destroy      },
        { "Show",         l_Cursor_Show         },
        { "Hide",         l_Cursor_Hide         },
        { "IsVisible",    l_Cursor_IsVisible    },
        { "WarpGlobal",   l_Cursor_WarpGlobal   },
        { nullptr,        nullptr               },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
