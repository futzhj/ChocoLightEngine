/**
 * @file light_mouse.cpp
 * @brief Light.Mouse module - SDL_mouse.h (polling subset)
 *
 * Polling-style mouse query API. Complements:
 *   - Light.Cursor   - cursor creation/show/hide/global warp
 *   - Light.Input    - event-driven mouse buttons / position state
 *
 * This module focuses on direct, on-demand SDL queries for cases where
 * polling is preferable to listening to events (UI hover, tooltip timer,
 * pre-frame mouse sample).
 *
 * Lua API (7 fns):
 *
 *  Device queries:
 *    HasMouse()                    -> bool
 *    GetMice()                     -> {instance_id, ...}
 *    GetMouseNameForID(id)         -> string | nil
 *
 *  State (window-local / global / relative):
 *    GetMouseState()               -> buttons, x, y     (window-relative)
 *    GetGlobalMouseState()         -> buttons, x, y     (desktop coords)
 *    GetRelativeMouseState()       -> buttons, dx, dy   (since last call)
 *
 *  Capture:
 *    CaptureMouse(enabled)         -> bool
 *
 * Constants exposed on the module table:
 *    BUTTON_LEFT / BUTTON_MIDDLE / BUTTON_RIGHT / BUTTON_X1 / BUTTON_X2
 *    BUTTON_MASK_LEFT / BUTTON_MASK_MIDDLE / BUTTON_MASK_RIGHT /
 *    BUTTON_MASK_X1 / BUTTON_MASK_X2
 *
 * NOT bound (Window-coupled, deferred until shared Window type lands):
 *    GetMouseFocus, WarpMouseInWindow,
 *    SetWindowRelativeMouseMode, GetWindowRelativeMouseMode
 *
 * NOT bound (covered by Light.Cursor):
 *    Cursor lifecycle (Create*, Set, Destroy, Get, GetDefault),
 *    Show/HideCursor, CursorVisible, WarpMouseGlobal.
 *
 * Lazy init: SDL_INIT_VIDEO is required by mouse polling on most
 * platforms. We init lazily on first call so a script that requires
 * Light.Mouse before Light.Window does not crash.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Lazy SDL_INIT_VIDEO
// ============================================================
static bool g_mouseSubsysInited = false;
static bool EnsureVideoSubsystem() {
    if (g_mouseSubsysInited) return true;
    if (SDL_WasInit(SDL_INIT_VIDEO) != 0) {
        g_mouseSubsysInited = true;
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) return false;
    g_mouseSubsysInited = true;
    return true;
}

// ============================================================
// Device queries
// ============================================================
static int l_M_HasMouse(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasMouse() ? 1 : 0);
    return 1;
}
static int l_M_GetMice(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_createtable(L, 0, 0); return 1; }
    int count = 0;
    SDL_MouseID* ids = SDL_GetMice(&count);
    if (!ids) { lua_createtable(L, 0, 0); return 1; }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}
static int l_M_GetMouseNameForID(lua_State* L) {
    if (!EnsureVideoSubsystem()) { lua_pushnil(L); return 1; }
    SDL_MouseID id = (SDL_MouseID)luaL_checknumber(L, 1);
    const char* n = SDL_GetMouseNameForID(id);
    if (!n) { lua_pushnil(L); return 1; }
    lua_pushstring(L, n);
    return 1;
}

// ============================================================
// State queries
// ============================================================
static int l_M_GetMouseState(lua_State* L) {
    if (!EnsureVideoSubsystem()) {
        lua_pushinteger(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    float x = 0, y = 0;
    SDL_MouseButtonFlags b = SDL_GetMouseState(&x, &y);
    lua_pushinteger(L, (lua_Integer)b);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 3;
}
static int l_M_GetGlobalMouseState(lua_State* L) {
    if (!EnsureVideoSubsystem()) {
        lua_pushinteger(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    float x = 0, y = 0;
    SDL_MouseButtonFlags b = SDL_GetGlobalMouseState(&x, &y);
    lua_pushinteger(L, (lua_Integer)b);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 3;
}
static int l_M_GetRelativeMouseState(lua_State* L) {
    if (!EnsureVideoSubsystem()) {
        lua_pushinteger(L, 0); lua_pushnumber(L, 0); lua_pushnumber(L, 0);
        return 3;
    }
    float dx = 0, dy = 0;
    SDL_MouseButtonFlags b = SDL_GetRelativeMouseState(&dx, &dy);
    lua_pushinteger(L, (lua_Integer)b);
    lua_pushnumber(L, (lua_Number)dx);
    lua_pushnumber(L, (lua_Number)dy);
    return 3;
}

// ============================================================
// Capture
// ============================================================
static int l_M_CaptureMouse(lua_State* L) {
    if (!EnsureVideoSubsystem()) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_INIT_VIDEO failed");
        return 2;
    }
    bool en = lua_toboolean(L, 1) ? true : false;
    if (!SDL_CaptureMouse(en)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_CaptureMouse failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// luaopen
// ============================================================
static const luaL_Reg kReg[] = {
    { "HasMouse",                l_M_HasMouse                },
    { "GetMice",                 l_M_GetMice                 },
    { "GetMouseNameForID",       l_M_GetMouseNameForID       },
    { "GetMouseState",           l_M_GetMouseState           },
    { "GetGlobalMouseState",     l_M_GetGlobalMouseState     },
    { "GetRelativeMouseState",   l_M_GetRelativeMouseState   },
    { "CaptureMouse",            l_M_CaptureMouse            },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Mouse(lua_State* L) {
    lua_newtable(L);
    luaL_register(L, nullptr, kReg);

    // Button index (1..5) - same as SDL3 macros
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_LEFT);   lua_setfield(L, -2, "BUTTON_LEFT");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_MIDDLE); lua_setfield(L, -2, "BUTTON_MIDDLE");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_RIGHT);  lua_setfield(L, -2, "BUTTON_RIGHT");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_X1);     lua_setfield(L, -2, "BUTTON_X1");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_X2);     lua_setfield(L, -2, "BUTTON_X2");

    // Button bitmask values used by GetMouseState() return value.
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_LMASK);  lua_setfield(L, -2, "BUTTON_MASK_LEFT");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_MMASK);  lua_setfield(L, -2, "BUTTON_MASK_MIDDLE");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_RMASK);  lua_setfield(L, -2, "BUTTON_MASK_RIGHT");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_X1MASK); lua_setfield(L, -2, "BUTTON_MASK_X1");
    lua_pushinteger(L, (lua_Integer)SDL_BUTTON_X2MASK); lua_setfield(L, -2, "BUTTON_MASK_X2");

    return 1;
}
