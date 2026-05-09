/**
 * @file light_event.cpp
 * @brief Light.Event module — SDL3 SDL_events.h subset binding (Phase AR)
 *
 * Lua API:
 *   Light.Event.HasEvent(type) -> bool
 *   Light.Event.HasEvents(min_type, max_type) -> bool
 *   Light.Event.FlushEvent(type) -> ()
 *   Light.Event.FlushEvents(min_type, max_type) -> ()
 *   Light.Event.Push(type, [code], [data1], [data2]) -> bool
 *   Light.Event.Pump() -> ()
 *   Light.Event.SetEnabled(type, enabled) -> ()
 *   Light.Event.IsEnabled(type) -> bool
 *   Light.Event.Register(num) -> int   -- starting user event ID, 0 on overflow
 *
 * Not bound (intentional):
 *   - PeepEvents: complex buffer ops, low Lua API value
 *   - SetEventFilter / GetEventFilter / AddEventWatch / RemoveEventWatch:
 *     persistent Lua callbacks, lifetime/leak risk; defer to a future phase
 *   - PollEvent / WaitEvent / WaitEventTimeout: engine consumes these in
 *     PlatformWindow main loop, exposing them would create races
 *   - GetWindowFromEvent: requires Window userdata, not currently exposed
 *   - FilterEvents: needs callback, same reason as SetEventFilter
 *
 * Note: Push() does not pre-validate event types beyond SDL's range checks.
 * Callers should typically use Register() to obtain user event IDs.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ===========================================================
// Event queue queries
// ===========================================================
static int l_Event_HasEvent(lua_State* L) {
    Uint32 type = (Uint32)luaL_checkinteger(L, 1);
    lua_pushboolean(L, SDL_HasEvent(type) ? 1 : 0);
    return 1;
}

static int l_Event_HasEvents(lua_State* L) {
    Uint32 minType = (Uint32)luaL_checkinteger(L, 1);
    Uint32 maxType = (Uint32)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_HasEvents(minType, maxType) ? 1 : 0);
    return 1;
}

static int l_Event_FlushEvent(lua_State* L) {
    Uint32 type = (Uint32)luaL_checkinteger(L, 1);
    SDL_FlushEvent(type);
    return 0;
}

static int l_Event_FlushEvents(lua_State* L) {
    Uint32 minType = (Uint32)luaL_checkinteger(L, 1);
    Uint32 maxType = (Uint32)luaL_checkinteger(L, 2);
    SDL_FlushEvents(minType, maxType);
    return 0;
}

// ===========================================================
// Event push (user-defined events)
// ===========================================================
// Lua: Push(type, [code], [data1], [data2]) -> bool
// data1/data2 are integers stored in the user event's data1/data2 fields
// (SDL stores them as void*, so we cast int -> intptr_t -> void*).
static int l_Event_Push(lua_State* L) {
    Uint32 type = (Uint32)luaL_checkinteger(L, 1);
    Sint32 code = (Sint32)luaL_optinteger(L, 2, 0);

    // 用 lua_Integer (可能是 ptrdiff_t 等) 作为 data1/data2 的中转值
    lua_Integer d1 = luaL_optinteger(L, 3, 0);
    lua_Integer d2 = luaL_optinteger(L, 4, 0);

    SDL_Event ev;
    SDL_zero(ev);
    ev.user.type      = type;
    ev.user.timestamp = SDL_GetTicksNS();
    ev.user.windowID  = 0;
    ev.user.code      = code;
    ev.user.data1     = (void*)(intptr_t)d1;
    ev.user.data2     = (void*)(intptr_t)d2;

    bool ok = SDL_PushEvent(&ev);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

// ===========================================================
// Event queue control
// ===========================================================
static int l_Event_Pump(lua_State* L) {
    (void)L;
    SDL_PumpEvents();
    return 0;
}

static int l_Event_SetEnabled(lua_State* L) {
    Uint32 type   = (Uint32)luaL_checkinteger(L, 1);
    bool enabled  = lua_toboolean(L, 2) != 0;
    SDL_SetEventEnabled(type, enabled);
    return 0;
}

static int l_Event_IsEnabled(lua_State* L) {
    Uint32 type = (Uint32)luaL_checkinteger(L, 1);
    lua_pushboolean(L, SDL_EventEnabled(type) ? 1 : 0);
    return 1;
}

static int l_Event_Register(lua_State* L) {
    int num = (int)luaL_optinteger(L, 1, 1);
    if (num <= 0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    Uint32 start = SDL_RegisterEvents(num);
    // SDL 在事件 ID 池耗尽时返回 (Uint32)-1; 我们映射为 0 (Lua 风格的"失败")
    if (start == (Uint32)-1) {
        lua_pushinteger(L, 0);
    } else {
        lua_pushinteger(L, (lua_Integer)start);
    }
    return 1;
}

// ===========================================================
// Module registration
// ===========================================================
static const luaL_Reg kEventReg[] = {
    { "HasEvent",     l_Event_HasEvent     },
    { "HasEvents",    l_Event_HasEvents    },
    { "FlushEvent",   l_Event_FlushEvent   },
    { "FlushEvents",  l_Event_FlushEvents  },
    { "Push",         l_Event_Push         },
    { "Pump",         l_Event_Pump         },
    { "SetEnabled",   l_Event_SetEnabled   },
    { "IsEnabled",    l_Event_IsEnabled    },
    { "Register",     l_Event_Register     },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Event(lua_State* L) {
    LT::RegisterModule(L, "Event", kEventReg);

    // 暴露常用 SDL 事件类型常量, 方便 Lua 端无需硬编码数字
    // (与 SDL_EventType 枚举一致)
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_FIRST);                lua_setfield(L, -2, "FIRST");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_QUIT);                 lua_setfield(L, -2, "QUIT");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_TERMINATING);          lua_setfield(L, -2, "TERMINATING");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_LOW_MEMORY);           lua_setfield(L, -2, "LOW_MEMORY");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_WILL_ENTER_BACKGROUND);lua_setfield(L, -2, "WILL_ENTER_BACKGROUND");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_DID_ENTER_BACKGROUND); lua_setfield(L, -2, "DID_ENTER_BACKGROUND");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_WILL_ENTER_FOREGROUND);lua_setfield(L, -2, "WILL_ENTER_FOREGROUND");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_DID_ENTER_FOREGROUND); lua_setfield(L, -2, "DID_ENTER_FOREGROUND");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_LOCALE_CHANGED);       lua_setfield(L, -2, "LOCALE_CHANGED");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_KEY_DOWN);             lua_setfield(L, -2, "KEY_DOWN");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_KEY_UP);               lua_setfield(L, -2, "KEY_UP");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_TEXT_INPUT);           lua_setfield(L, -2, "TEXT_INPUT");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_TEXT_EDITING);         lua_setfield(L, -2, "TEXT_EDITING");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_MOUSE_MOTION);         lua_setfield(L, -2, "MOUSE_MOTION");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_MOUSE_BUTTON_DOWN);    lua_setfield(L, -2, "MOUSE_BUTTON_DOWN");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_MOUSE_BUTTON_UP);      lua_setfield(L, -2, "MOUSE_BUTTON_UP");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_MOUSE_WHEEL);          lua_setfield(L, -2, "MOUSE_WHEEL");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_PROXIMITY_IN);     lua_setfield(L, -2, "PEN_PROXIMITY_IN");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_PROXIMITY_OUT);    lua_setfield(L, -2, "PEN_PROXIMITY_OUT");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_DOWN);             lua_setfield(L, -2, "PEN_DOWN");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_UP);               lua_setfield(L, -2, "PEN_UP");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_BUTTON_DOWN);      lua_setfield(L, -2, "PEN_BUTTON_DOWN");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_BUTTON_UP);        lua_setfield(L, -2, "PEN_BUTTON_UP");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_MOTION);           lua_setfield(L, -2, "PEN_MOTION");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_PEN_AXIS);             lua_setfield(L, -2, "PEN_AXIS");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_USER);                 lua_setfield(L, -2, "USER");
    lua_pushinteger(L, (lua_Integer)SDL_EVENT_LAST);                 lua_setfield(L, -2, "LAST");
    return 1;
}
