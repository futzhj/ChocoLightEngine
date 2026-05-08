/**
 * @file light_joystick.cpp
 * @brief Light.Joystick module - SDL_joystick.h
 *
 * Low-level joystick API. Light.Gamepad is the higher-level (mapped)
 * abstraction for "console-style" controllers; this module covers raw
 * joysticks (flight sticks, throttle/pedal, dance pads, fight sticks,
 * arcade panels) where Gamepad's normalized layout is wrong.
 *
 * Lua API (~36 fns):
 *
 *  Discovery (no handle):
 *    HasJoystick()                          -> bool
 *    GetJoysticks()                         -> {instance_id, ...}
 *    GetJoystickNameForID(id)               -> string | nil
 *    GetJoystickPathForID(id)               -> string | nil
 *    GetJoystickPlayerIndexForID(id)        -> int (-1 if unset)
 *    GetJoystickVendorForID(id)             -> uint16
 *    GetJoystickProductForID(id)            -> uint16
 *    GetJoystickProductVersionForID(id)     -> uint16
 *    GetJoystickTypeForID(id)               -> int (TYPE_* enum)
 *    GetJoystickGUIDForID(id)               -> string (33-char hex)
 *    SetJoystickEventsEnabled(b)
 *    JoystickEventsEnabled()                -> bool
 *    UpdateJoysticks()
 *
 *  Open / lookup / close:
 *    OpenJoystick(id)                       -> ud | nil, err
 *    GetJoystickFromID(id)                  -> ud | nil
 *    GetJoystickFromPlayerIndex(idx)        -> ud | nil
 *    CloseJoystick(j)
 *
 *  Per-handle metadata:
 *    GetJoystickName(j)                     -> string | nil
 *    GetJoystickPath(j)                     -> string | nil
 *    GetJoystickPlayerIndex(j)              -> int
 *    SetJoystickPlayerIndex(j, idx)         -> bool
 *    GetJoystickVendor(j)                   -> uint16
 *    GetJoystickProduct(j)                  -> uint16
 *    GetJoystickProductVersion(j)           -> uint16
 *    GetJoystickFirmwareVersion(j)          -> uint16
 *    GetJoystickSerial(j)                   -> string | nil
 *    GetJoystickType(j)                     -> int
 *    GetJoystickGUID(j)                     -> string (33-char hex)
 *    GetJoystickID(j)                       -> instance_id
 *    GetJoystickProperties(j)               -> propid
 *    JoystickConnected(j)                   -> bool
 *    GetJoystickConnectionState(j)          -> int (CONNECTION_* enum)
 *    GetJoystickPowerInfo(j)                -> state_int, percent_int
 *
 *  Capabilities (counts):
 *    GetNumJoystickAxes(j)                  -> int
 *    GetNumJoystickHats(j)                  -> int
 *    GetNumJoystickButtons(j)               -> int
 *
 *  Polling:
 *    GetJoystickAxis(j, axis)               -> Sint16
 *    GetJoystickHat(j, hat)                 -> Uint8 (HAT_* enum)
 *    GetJoystickButton(j, button)           -> bool
 *
 *  Effects:
 *    RumbleJoystick(j, low, high, ms)       -> bool, err
 *    RumbleJoystickTriggers(j, l, r, ms)    -> bool, err
 *    SetJoystickLED(j, r, g, b)             -> bool, err
 *
 * Constants:
 *    TYPE_UNKNOWN/GAMEPAD/WHEEL/ARCADE_STICK/FLIGHT_STICK/DANCE_PAD/
 *    GUITAR/DRUM_KIT/ARCADE_PAD/THROTTLE
 *    CONNECTION_INVALID/UNKNOWN/WIRED/WIRELESS
 *    HAT_CENTERED/UP/DOWN/LEFT/RIGHT/LEFTUP/LEFTDOWN/RIGHTUP/RIGHTDOWN
 *    AXIS_MAX = 32767, AXIS_MIN = -32768
 *
 * NOT bound:
 *    LockJoysticks / UnlockJoysticks        - thread sync, no Lua use
 *    VirtualJoystick (8 fns)                - VirtualJoystickDesc complex
 *    GetNumJoystickBalls / GetJoystickBall  - trackball, obsolete on modern HW
 *    GetJoystickAxisInitialState            - rare, prefer GetJoystickAxis
 *    GetJoystickGUIDInfo                    - bits readable from GUID string
 *    SendJoystickEffect                     - raw bytes, device-specific
 *
 * Lazy init:
 *    SDL_INIT_JOYSTICK is initialized on the first OpenJoystick / device
 *    listing call. CI without a real joystick will see HasJoystick=false
 *    and a 0-length GetJoysticks list.
 *
 * Lifetime:
 *    LJoystick userdata stores SDL_Joystick* and an "owned" flag.
 *    OpenJoystick produces an owned handle (auto __gc closes).
 *    GetJoystickFromID / GetJoystickFromPlayerIndex produce a borrowed
 *    handle (no auto-close); CloseJoystick on a borrowed handle is a
 *    no-op for safety.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#define MT_JOYSTICK "Light.Joystick.Joystick"

struct LJoystick {
    SDL_Joystick* p;
    bool owned;
};

// ============================================================
// Lazy SDL_INIT_JOYSTICK
// ============================================================
static bool g_joySubsysInited = false;
static bool EnsureJoySubsystem() {
    if (g_joySubsysInited) return true;
    if (SDL_WasInit(SDL_INIT_JOYSTICK) != 0) {
        g_joySubsysInited = true;
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_JOYSTICK)) return false;
    g_joySubsysInited = true;
    return true;
}

// ============================================================
// helpers
// ============================================================
static LJoystick* CheckHandle(lua_State* L, int idx) {
    return (LJoystick*)luaL_checkudata(L, idx, MT_JOYSTICK);
}
static SDL_Joystick* CheckLive(lua_State* L, int idx) {
    LJoystick* h = CheckHandle(L, idx);
    if (!h->p) luaL_error(L, "joystick handle has been closed");
    return h->p;
}
static int PushSdlError(lua_State* L) {
    lua_pushnil(L);
    const char* e = SDL_GetError();
    lua_pushstring(L, (e && *e) ? e : "SDL error");
    return 2;
}
static int PushBoolErr(lua_State* L, bool ok) {
    if (ok) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0);
    const char* e = SDL_GetError();
    lua_pushstring(L, (e && *e) ? e : "SDL error");
    return 2;
}
static int NewHandle(lua_State* L, SDL_Joystick* p, bool owned) {
    if (!p) return PushSdlError(L);
    LJoystick* h = (LJoystick*)lua_newuserdata(L, sizeof(LJoystick));
    h->p = p;
    h->owned = owned;
    luaL_getmetatable(L, MT_JOYSTICK);
    lua_setmetatable(L, -2);
    return 1;
}
// Push SDL_GUID as 33-char hex string (32 hex + NUL).
static void PushGuid(lua_State* L, SDL_GUID g) {
    char buf[33] = { 0 };
    SDL_GUIDToString(g, buf, sizeof(buf));
    lua_pushstring(L, buf);
}

// ============================================================
// Discovery
// ============================================================
static int l_J_HasJoystick(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_HasJoystick() ? 1 : 0);
    return 1;
}
static int l_J_GetJoysticks(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_createtable(L, 0, 0); return 1; }
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (!ids) { lua_createtable(L, 0, 0); return 1; }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}
#define J_DEF_ID_STR(NAME, SDLFN)                                       \
    static int l_J_##NAME(lua_State* L) {                               \
        if (!EnsureJoySubsystem()) { lua_pushnil(L); return 1; }        \
        SDL_JoystickID id = (SDL_JoystickID)luaL_checknumber(L, 1);     \
        const char* s = SDLFN(id);                                      \
        if (!s) { lua_pushnil(L); return 1; }                           \
        lua_pushstring(L, s);                                           \
        return 1;                                                       \
    }
J_DEF_ID_STR(GetJoystickNameForID, SDL_GetJoystickNameForID)
J_DEF_ID_STR(GetJoystickPathForID, SDL_GetJoystickPathForID)

#define J_DEF_ID_INT(NAME, SDLFN)                                       \
    static int l_J_##NAME(lua_State* L) {                               \
        if (!EnsureJoySubsystem()) { lua_pushinteger(L, 0); return 1; } \
        SDL_JoystickID id = (SDL_JoystickID)luaL_checknumber(L, 1);     \
        lua_pushinteger(L, (lua_Integer)SDLFN(id));                     \
        return 1;                                                       \
    }
J_DEF_ID_INT(GetJoystickPlayerIndexForID,    SDL_GetJoystickPlayerIndexForID)
J_DEF_ID_INT(GetJoystickVendorForID,         SDL_GetJoystickVendorForID)
J_DEF_ID_INT(GetJoystickProductForID,        SDL_GetJoystickProductForID)
J_DEF_ID_INT(GetJoystickProductVersionForID, SDL_GetJoystickProductVersionForID)
J_DEF_ID_INT(GetJoystickTypeForID,           SDL_GetJoystickTypeForID)

static int l_J_GetJoystickGUIDForID(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_pushnil(L); return 1; }
    SDL_JoystickID id = (SDL_JoystickID)luaL_checknumber(L, 1);
    PushGuid(L, SDL_GetJoystickGUIDForID(id));
    return 1;
}

static int l_J_SetJoystickEventsEnabled(lua_State* L) {
    if (!EnsureJoySubsystem()) return 0;
    SDL_SetJoystickEventsEnabled(lua_toboolean(L, 1) ? true : false);
    return 0;
}
static int l_J_JoystickEventsEnabled(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_JoystickEventsEnabled() ? 1 : 0);
    return 1;
}
static int l_J_UpdateJoysticks(lua_State* L) {
    if (!EnsureJoySubsystem()) return 0;
    SDL_UpdateJoysticks();
    return 0;
}

// ============================================================
// Open / lookup / close
// ============================================================
static int l_J_OpenJoystick(lua_State* L) {
    if (!EnsureJoySubsystem()) return PushSdlError(L);
    SDL_JoystickID id = (SDL_JoystickID)luaL_checknumber(L, 1);
    return NewHandle(L, SDL_OpenJoystick(id), /*owned=*/true);
}
static int l_J_GetJoystickFromID(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_pushnil(L); return 1; }
    SDL_JoystickID id = (SDL_JoystickID)luaL_checknumber(L, 1);
    SDL_Joystick* p = SDL_GetJoystickFromID(id);
    if (!p) { lua_pushnil(L); return 1; }
    return NewHandle(L, p, /*owned=*/false);
}
static int l_J_GetJoystickFromPlayerIndex(lua_State* L) {
    if (!EnsureJoySubsystem()) { lua_pushnil(L); return 1; }
    int idx = (int)luaL_checkinteger(L, 1);
    SDL_Joystick* p = SDL_GetJoystickFromPlayerIndex(idx);
    if (!p) { lua_pushnil(L); return 1; }
    return NewHandle(L, p, /*owned=*/false);
}
static int l_J_CloseJoystick(lua_State* L) {
    LJoystick* h = CheckHandle(L, 1);
    if (h->p && h->owned) { SDL_CloseJoystick(h->p); }
    h->p = nullptr;
    h->owned = false;
    return 0;
}
static int l_J_Gc(lua_State* L) {
    LJoystick* h = (LJoystick*)lua_touserdata(L, 1);
    if (h && h->p && h->owned) { SDL_CloseJoystick(h->p); h->p = nullptr; }
    return 0;
}

// ============================================================
// Per-handle metadata
// ============================================================
#define J_DEF_H_STR(NAME, SDLFN)                                        \
    static int l_J_##NAME(lua_State* L) {                               \
        SDL_Joystick* j = CheckLive(L, 1);                              \
        const char* s = SDLFN(j);                                       \
        if (!s) { lua_pushnil(L); return 1; }                           \
        lua_pushstring(L, s);                                           \
        return 1;                                                       \
    }
J_DEF_H_STR(GetJoystickName,   SDL_GetJoystickName)
J_DEF_H_STR(GetJoystickPath,   SDL_GetJoystickPath)
J_DEF_H_STR(GetJoystickSerial, SDL_GetJoystickSerial)

#define J_DEF_H_INT(NAME, SDLFN)                                        \
    static int l_J_##NAME(lua_State* L) {                               \
        SDL_Joystick* j = CheckLive(L, 1);                              \
        lua_pushinteger(L, (lua_Integer)SDLFN(j));                      \
        return 1;                                                       \
    }
J_DEF_H_INT(GetJoystickPlayerIndex,         SDL_GetJoystickPlayerIndex)
J_DEF_H_INT(GetJoystickVendor,              SDL_GetJoystickVendor)
J_DEF_H_INT(GetJoystickProduct,             SDL_GetJoystickProduct)
J_DEF_H_INT(GetJoystickProductVersion,      SDL_GetJoystickProductVersion)
J_DEF_H_INT(GetJoystickFirmwareVersion,     SDL_GetJoystickFirmwareVersion)
J_DEF_H_INT(GetJoystickType,                SDL_GetJoystickType)
J_DEF_H_INT(GetJoystickConnectionState,     SDL_GetJoystickConnectionState)
J_DEF_H_INT(GetNumJoystickAxes,             SDL_GetNumJoystickAxes)
J_DEF_H_INT(GetNumJoystickHats,             SDL_GetNumJoystickHats)
J_DEF_H_INT(GetNumJoystickButtons,          SDL_GetNumJoystickButtons)

static int l_J_SetJoystickPlayerIndex(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    int idx = (int)luaL_checkinteger(L, 2);
    return PushBoolErr(L, SDL_SetJoystickPlayerIndex(j, idx));
}
static int l_J_GetJoystickGUID(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    PushGuid(L, SDL_GetJoystickGUID(j));
    return 1;
}
static int l_J_GetJoystickID(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    lua_pushnumber(L, (lua_Number)SDL_GetJoystickID(j));
    return 1;
}
static int l_J_GetJoystickProperties(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    lua_pushnumber(L, (lua_Number)SDL_GetJoystickProperties(j));
    return 1;
}
static int l_J_JoystickConnected(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    lua_pushboolean(L, SDL_JoystickConnected(j) ? 1 : 0);
    return 1;
}
static int l_J_GetJoystickPowerInfo(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    int percent = -1;
    SDL_PowerState st = SDL_GetJoystickPowerInfo(j, &percent);
    lua_pushinteger(L, (lua_Integer)st);
    lua_pushinteger(L, percent);
    return 2;
}

// ============================================================
// Polling
// ============================================================
static int l_J_GetJoystickAxis(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    int axis = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, (lua_Integer)SDL_GetJoystickAxis(j, axis));
    return 1;
}
static int l_J_GetJoystickHat(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    int hat = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, (lua_Integer)SDL_GetJoystickHat(j, hat));
    return 1;
}
static int l_J_GetJoystickButton(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    int btn = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SDL_GetJoystickButton(j, btn) ? 1 : 0);
    return 1;
}

// ============================================================
// Effects
// ============================================================
static int l_J_RumbleJoystick(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    Uint16 lf = (Uint16)luaL_checkinteger(L, 2);
    Uint16 hf = (Uint16)luaL_checkinteger(L, 3);
    Uint32 ms = (Uint32)luaL_checkinteger(L, 4);
    return PushBoolErr(L, SDL_RumbleJoystick(j, lf, hf, ms));
}
static int l_J_RumbleJoystickTriggers(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    Uint16 l = (Uint16)luaL_checkinteger(L, 2);
    Uint16 r = (Uint16)luaL_checkinteger(L, 3);
    Uint32 ms = (Uint32)luaL_checkinteger(L, 4);
    return PushBoolErr(L, SDL_RumbleJoystickTriggers(j, l, r, ms));
}
static int l_J_SetJoystickLED(lua_State* L) {
    SDL_Joystick* j = CheckLive(L, 1);
    Uint8 r = (Uint8)luaL_checkinteger(L, 2);
    Uint8 g = (Uint8)luaL_checkinteger(L, 3);
    Uint8 b = (Uint8)luaL_checkinteger(L, 4);
    return PushBoolErr(L, SDL_SetJoystickLED(j, r, g, b));
}

// ============================================================
// luaopen
// ============================================================
static const luaL_Reg kReg[] = {
    // Discovery
    { "HasJoystick",                     l_J_HasJoystick                     },
    { "GetJoysticks",                    l_J_GetJoysticks                    },
    { "GetJoystickNameForID",            l_J_GetJoystickNameForID            },
    { "GetJoystickPathForID",            l_J_GetJoystickPathForID            },
    { "GetJoystickPlayerIndexForID",     l_J_GetJoystickPlayerIndexForID     },
    { "GetJoystickVendorForID",          l_J_GetJoystickVendorForID          },
    { "GetJoystickProductForID",         l_J_GetJoystickProductForID         },
    { "GetJoystickProductVersionForID",  l_J_GetJoystickProductVersionForID  },
    { "GetJoystickTypeForID",            l_J_GetJoystickTypeForID            },
    { "GetJoystickGUIDForID",            l_J_GetJoystickGUIDForID            },
    { "SetJoystickEventsEnabled",        l_J_SetJoystickEventsEnabled        },
    { "JoystickEventsEnabled",           l_J_JoystickEventsEnabled           },
    { "UpdateJoysticks",                 l_J_UpdateJoysticks                 },
    // Open / lookup / close
    { "OpenJoystick",                    l_J_OpenJoystick                    },
    { "GetJoystickFromID",               l_J_GetJoystickFromID               },
    { "GetJoystickFromPlayerIndex",      l_J_GetJoystickFromPlayerIndex      },
    { "CloseJoystick",                   l_J_CloseJoystick                   },
    // Per-handle metadata
    { "GetJoystickName",                 l_J_GetJoystickName                 },
    { "GetJoystickPath",                 l_J_GetJoystickPath                 },
    { "GetJoystickSerial",               l_J_GetJoystickSerial               },
    { "GetJoystickPlayerIndex",          l_J_GetJoystickPlayerIndex          },
    { "SetJoystickPlayerIndex",          l_J_SetJoystickPlayerIndex          },
    { "GetJoystickVendor",               l_J_GetJoystickVendor               },
    { "GetJoystickProduct",              l_J_GetJoystickProduct              },
    { "GetJoystickProductVersion",       l_J_GetJoystickProductVersion       },
    { "GetJoystickFirmwareVersion",      l_J_GetJoystickFirmwareVersion      },
    { "GetJoystickType",                 l_J_GetJoystickType                 },
    { "GetJoystickConnectionState",      l_J_GetJoystickConnectionState      },
    { "GetJoystickGUID",                 l_J_GetJoystickGUID                 },
    { "GetJoystickID",                   l_J_GetJoystickID                   },
    { "GetJoystickProperties",           l_J_GetJoystickProperties           },
    { "JoystickConnected",               l_J_JoystickConnected               },
    { "GetJoystickPowerInfo",            l_J_GetJoystickPowerInfo            },
    // Counts
    { "GetNumJoystickAxes",              l_J_GetNumJoystickAxes              },
    { "GetNumJoystickHats",              l_J_GetNumJoystickHats              },
    { "GetNumJoystickButtons",           l_J_GetNumJoystickButtons           },
    // Polling
    { "GetJoystickAxis",                 l_J_GetJoystickAxis                 },
    { "GetJoystickHat",                  l_J_GetJoystickHat                  },
    { "GetJoystickButton",               l_J_GetJoystickButton               },
    // Effects
    { "RumbleJoystick",                  l_J_RumbleJoystick                  },
    { "RumbleJoystickTriggers",          l_J_RumbleJoystickTriggers          },
    { "SetJoystickLED",                  l_J_SetJoystickLED                  },
    { nullptr, nullptr },
};

static void RegisterMetatable(lua_State* L) {
    luaL_newmetatable(L, MT_JOYSTICK);
    lua_pushcfunction(L, l_J_Gc);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
}

#define LJ_PUSH(name, val) do {                       \
    lua_pushinteger(L, (lua_Integer)(val));           \
    lua_setfield(L, -2, name);                        \
} while (0)

extern "C" LIGHT_API int luaopen_Light_Joystick(lua_State* L) {
    RegisterMetatable(L);

    lua_newtable(L);
    luaL_register(L, nullptr, kReg);

    // SDL_JoystickType
    LJ_PUSH("TYPE_UNKNOWN",      SDL_JOYSTICK_TYPE_UNKNOWN);
    LJ_PUSH("TYPE_GAMEPAD",      SDL_JOYSTICK_TYPE_GAMEPAD);
    LJ_PUSH("TYPE_WHEEL",        SDL_JOYSTICK_TYPE_WHEEL);
    LJ_PUSH("TYPE_ARCADE_STICK", SDL_JOYSTICK_TYPE_ARCADE_STICK);
    LJ_PUSH("TYPE_FLIGHT_STICK", SDL_JOYSTICK_TYPE_FLIGHT_STICK);
    LJ_PUSH("TYPE_DANCE_PAD",    SDL_JOYSTICK_TYPE_DANCE_PAD);
    LJ_PUSH("TYPE_GUITAR",       SDL_JOYSTICK_TYPE_GUITAR);
    LJ_PUSH("TYPE_DRUM_KIT",     SDL_JOYSTICK_TYPE_DRUM_KIT);
    LJ_PUSH("TYPE_ARCADE_PAD",   SDL_JOYSTICK_TYPE_ARCADE_PAD);
    LJ_PUSH("TYPE_THROTTLE",     SDL_JOYSTICK_TYPE_THROTTLE);

    // SDL_JoystickConnectionState
    LJ_PUSH("CONNECTION_INVALID",  SDL_JOYSTICK_CONNECTION_INVALID);
    LJ_PUSH("CONNECTION_UNKNOWN",  SDL_JOYSTICK_CONNECTION_UNKNOWN);
    LJ_PUSH("CONNECTION_WIRED",    SDL_JOYSTICK_CONNECTION_WIRED);
    LJ_PUSH("CONNECTION_WIRELESS", SDL_JOYSTICK_CONNECTION_WIRELESS);

    // Hat masks
    LJ_PUSH("HAT_CENTERED",  SDL_HAT_CENTERED);
    LJ_PUSH("HAT_UP",        SDL_HAT_UP);
    LJ_PUSH("HAT_RIGHT",     SDL_HAT_RIGHT);
    LJ_PUSH("HAT_DOWN",      SDL_HAT_DOWN);
    LJ_PUSH("HAT_LEFT",      SDL_HAT_LEFT);
    LJ_PUSH("HAT_RIGHTUP",   SDL_HAT_RIGHTUP);
    LJ_PUSH("HAT_RIGHTDOWN", SDL_HAT_RIGHTDOWN);
    LJ_PUSH("HAT_LEFTUP",    SDL_HAT_LEFTUP);
    LJ_PUSH("HAT_LEFTDOWN",  SDL_HAT_LEFTDOWN);

    // Axis range sentinels
    LJ_PUSH("AXIS_MAX",  SDL_JOYSTICK_AXIS_MAX);   //  32767
    LJ_PUSH("AXIS_MIN",  SDL_JOYSTICK_AXIS_MIN);   // -32768

    return 1;
}
