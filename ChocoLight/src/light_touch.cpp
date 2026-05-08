/**
 * @file light_touch.cpp
 * @brief Light.Touch module - multi-touch input query (SDL_touch)
 *
 * Lua API (4 fns):
 *   Light.Touch.GetDevices()           -> array<id>
 *   Light.Touch.GetDeviceName(id)      -> string, err
 *   Light.Touch.GetDeviceType(id)      -> 'direct' | 'indirect_abs' | 'indirect_rel' | 'invalid'
 *   Light.Touch.GetFingers(id)         -> array<{id, x, y, pressure}>
 *
 * Constants:
 *   Light.Touch.MOUSE_ID  -- (Uint32)-1; filter mouse-synthesized touches
 *   Light.Touch.TOUCH_ID  -- (Uint64)-1; filter touch-synthesized mouse events
 *
 * Note: SDL3 docs - "On some platforms SDL first sees the touch device if it
 * was actually used. Therefore the returned list might be empty, although
 * devices are available."
 *
 * Polling model. Real finger down/motion/up events flow through SDL event
 * loop; this module exposes only the active-fingers snapshot.
 *
 * Thread-safety: SDL_GetTouchDevices/Fingers must be called from main thread.
 *
 * Lazy-init guard: SDL_GetTouchDevices crashes on Windows when SDL is not
 * initialized. Light.UI.Window initializes SDL_INIT_VIDEO|EVENTS|GAMEPAD on
 * window creation, but a script that requires Light.Touch without creating
 * a window (e.g. CI smoke) hits the uninitialized path. EnsureTouchSubsystem
 * matches the pattern used by Light.Sensor / Light.Haptic.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstdint>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Lazy init: SDL_INIT_EVENTS required by SDL_touch family on Windows.
static bool g_touchSubsysInit = false;
static bool EnsureTouchSubsystem() {
    if (g_touchSubsysInit) return true;
    if (SDL_WasInit(SDL_INIT_EVENTS) != 0) {
        g_touchSubsysInit = true;
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_EVENTS)) {
        return false;
    }
    g_touchSubsysInit = true;
    return true;
}

// SDL_TouchDeviceType -> string name
static const char* TouchTypeName(SDL_TouchDeviceType t) {
    switch (t) {
        case SDL_TOUCH_DEVICE_DIRECT:            return "direct";
        case SDL_TOUCH_DEVICE_INDIRECT_ABSOLUTE: return "indirect_abs";
        case SDL_TOUCH_DEVICE_INDIRECT_RELATIVE: return "indirect_rel";
        case SDL_TOUCH_DEVICE_INVALID:           return "invalid";
        default:                                  return "unknown";
    }
}

// ============================================================
// Light.Touch.GetDevices() -> array<id>
// ============================================================
static int l_Touch_GetDevices(lua_State* L) {
    if (!EnsureTouchSubsystem()) {
        lua_newtable(L);
        return 1;
    }
    int count = 0;
    SDL_TouchID* ids = SDL_GetTouchDevices(&count);

    lua_newtable(L);
    if (ids == nullptr || count <= 0) {
        if (ids) SDL_free(ids);
        return 1;
    }

    for (int i = 0; i < count; ++i) {
        // SDL_TouchID is Uint64; lua_Number is double (53 bit precision).
        // Real-world touch ids stay well below 2^53 across platforms.
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}

// ============================================================
// Light.Touch.GetDeviceName(id) -> string, err
// ============================================================
static int l_Touch_GetDeviceName(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureTouchSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "touch subsystem unavailable");
        return 2;
    }
    SDL_TouchID id = (SDL_TouchID)lua_tonumber(L, 1);
    const char* name = SDL_GetTouchDeviceName(id);
    if (!name) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "invalid touch device id");
        return 2;
    }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

// ============================================================
// Light.Touch.GetDeviceType(id) -> string
// ============================================================
static int l_Touch_GetDeviceType(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushstring(L, "invalid");
        return 1;
    }
    if (!EnsureTouchSubsystem()) {
        lua_pushstring(L, "invalid");
        return 1;
    }
    SDL_TouchID id = (SDL_TouchID)lua_tonumber(L, 1);
    SDL_TouchDeviceType t = SDL_GetTouchDeviceType(id);
    lua_pushstring(L, TouchTypeName(t));
    return 1;
}

// ============================================================
// Light.Touch.GetFingers(id) -> array<{id, x, y, pressure}>, err
// ============================================================
static int l_Touch_GetFingers(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureTouchSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "touch subsystem unavailable");
        return 2;
    }
    SDL_TouchID id = (SDL_TouchID)lua_tonumber(L, 1);

    int count = 0;
    SDL_Finger** fingers = SDL_GetTouchFingers(id, &count);

    if (fingers == nullptr) {
        const char* e = SDL_GetError();
        if (e && *e) {
            lua_pushnil(L);
            lua_pushstring(L, e);
            return 2;
        }
        lua_newtable(L);
        return 1;
    }

    lua_newtable(L);
    for (int i = 0; i < count; ++i) {
        SDL_Finger* f = fingers[i];
        if (!f) continue;

        lua_newtable(L);

        lua_pushnumber(L, (lua_Number)f->id);
        lua_setfield(L, -2, "id");

        lua_pushnumber(L, (lua_Number)f->x);
        lua_setfield(L, -2, "x");

        lua_pushnumber(L, (lua_Number)f->y);
        lua_setfield(L, -2, "y");

        lua_pushnumber(L, (lua_Number)f->pressure);
        lua_setfield(L, -2, "pressure");

        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(fingers);
    return 1;
}

// ============================================================
// luaopen_Light_Touch
// ============================================================
// Registration uses the manual lua_pushcfunction loop pattern from
// light_haptic.cpp (no luaL_register dependency).
static const luaL_Reg kTouchReg[] = {
    { "GetDevices",    l_Touch_GetDevices    },
    { "GetDeviceName", l_Touch_GetDeviceName },
    { "GetDeviceType", l_Touch_GetDeviceType },
    { "GetFingers",    l_Touch_GetFingers    },
    { nullptr,         nullptr               },
};

extern "C" LIGHT_API int luaopen_Light_Touch(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kTouchReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Constants: filter SDL synthesized events.
    // SDL_TOUCH_MOUSEID = (Uint32)-1; SDL_MOUSE_TOUCHID = (Uint64)-1.
    // Use plain double literals (lua_Number is double in Lua 5.1).
    lua_pushnumber(L, (lua_Number)4294967295.0);
    lua_setfield(L, -2, "MOUSE_ID");

    lua_pushnumber(L, -1.0);
    lua_setfield(L, -2, "TOUCH_ID");

    return 1;
}
