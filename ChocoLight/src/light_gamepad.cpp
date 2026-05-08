/**
 * @file light_gamepad.cpp
 * @brief Light.Gamepad 模块 - 游戏手柄抽象 (基于 SDL_gamepad)
 *
 * Lua API:
 *   Light.Gamepad.GetGamepads()                  -> { instance_id... }, err
 *   Light.Gamepad.Open(instance_id)              -> handle, err
 *   Light.Gamepad.Close(handle)                  -> ok, err
 *
 *   Light.Gamepad.GetID(handle)                  -> instance_id, err
 *   Light.Gamepad.GetName(handle)                -> name_string, err
 *   Light.Gamepad.GetType(handle)                -> type_string, err
 *     ("xbox360"|"xboxone"|"ps3"|"ps4"|"ps5"|"switchpro"|"unknown" ...)
 *
 *   Light.Gamepad.IsConnected(handle)            -> bool
 *   Light.Gamepad.GetConnectionState(handle)     -> "wired"|"wireless"|"invalid"|"unknown"
 *   Light.Gamepad.GetPowerInfo(handle)           -> state_string, percent_int_or_nil
 *     state: "unknown"|"on_battery"|"no_battery"|"charging"|"charged"|"error"
 *
 *   Light.Gamepad.HasButton(handle, btn_name)    -> bool
 *   Light.Gamepad.GetButton(handle, btn_name)    -> bool
 *     btn_name: "south"|"east"|"west"|"north"|"back"|"guide"|"start"|
 *               "leftstick"|"rightstick"|"leftshoulder"|"rightshoulder"|
 *               "dpup"|"dpdown"|"dpleft"|"dpright"|"misc1"|...|"touchpad"
 *     (实际通过 SDL_GetGamepadButtonFromString 校验, 名字以 SDL3 mapping 字符串为准)
 *
 *   Light.Gamepad.HasAxis(handle, axis_name)     -> bool
 *   Light.Gamepad.GetAxis(handle, axis_name)     -> int16  (-32768 .. 32767)
 *     axis_name: "leftx"|"lefty"|"rightx"|"righty"|"lefttrigger"|"righttrigger"
 *
 *   Light.Gamepad.Rumble(handle, low, high, duration_ms) -> ok, err
 *     low/high: 0..65535 (低频/高频电机强度)
 *     duration_ms: 0..2^32-1
 *
 * 平台覆盖: Win/Mac/Linux/Android (大部分通过 HID), Web (Gamepad API)
 *           iOS 部分支持
 *
 * 注意: SDL3 用 SDL_JoystickID 作为 instance_id (Uint32, lua_Integer 安全).
 *       handle 是 SDL_Gamepad*, 用 lightuserdata 暴露给 Lua, Close 后失效.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 内部: 校验 lightuserdata 转 SDL_Gamepad*; 返回 nullptr 时压栈 err
static SDL_Gamepad* CheckGamepad(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Gamepad*)lua_touserdata(L, idx);
    }
    return nullptr;
}

// 内部: SDL_PowerState -> 字符串
static const char* PowerStateToStr(SDL_PowerState s) {
    switch (s) {
        case SDL_POWERSTATE_ERROR:      return "error";
        case SDL_POWERSTATE_UNKNOWN:    return "unknown";
        case SDL_POWERSTATE_ON_BATTERY: return "on_battery";
        case SDL_POWERSTATE_NO_BATTERY: return "no_battery";
        case SDL_POWERSTATE_CHARGING:   return "charging";
        case SDL_POWERSTATE_CHARGED:    return "charged";
        default:                        return "unknown";
    }
}

// 内部: SDL_JoystickConnectionState -> 字符串
static const char* ConnStateToStr(SDL_JoystickConnectionState s) {
    switch (s) {
        case SDL_JOYSTICK_CONNECTION_INVALID:  return "invalid";
        case SDL_JOYSTICK_CONNECTION_UNKNOWN:  return "unknown";
        case SDL_JOYSTICK_CONNECTION_WIRED:    return "wired";
        case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
        default:                               return "unknown";
    }
}

// ==================== Light.Gamepad.GetGamepads ====================

static int l_Gamepad_GetGamepads(lua_State* L) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
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

// ==================== Light.Gamepad.Open / Close ====================

static int l_Gamepad_Open(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (!gp) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, gp);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_Close(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid gamepad handle");
        return 2;
    }
    SDL_CloseGamepad(gp);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Gamepad.GetID / GetName / GetType ====================

static int l_Gamepad_GetID(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_JoystickID id = SDL_GetGamepadID(gp);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetName(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    const char* name = SDL_GetGamepadName(gp);
    if (!name) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetType(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_GamepadType t = SDL_GetGamepadType(gp);
    const char* s = SDL_GetGamepadStringForType(t);
    lua_pushstring(L, s ? s : "unknown");
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Gamepad.IsConnected / GetConnectionState ====================

static int l_Gamepad_IsConnected(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadConnected(gp) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetConnectionState(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushstring(L, "invalid"); return 1; }
    lua_pushstring(L, ConnStateToStr(SDL_GetGamepadConnectionState(gp)));
    return 1;
}

// ==================== Light.Gamepad.GetPowerInfo ====================

static int l_Gamepad_GetPowerInfo(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushstring(L, "error"); lua_pushnil(L); return 2; }
    int percent = -1;
    SDL_PowerState s = SDL_GetGamepadPowerInfo(gp, &percent);
    lua_pushstring(L, PowerStateToStr(s));
    if (percent >= 0) {
        lua_pushinteger(L, percent);
    } else {
        lua_pushnil(L);  // 不支持电池查询时为 nil
    }
    return 2;
}

// ==================== Light.Gamepad.HasButton / GetButton ====================

static int l_Gamepad_HasButton(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadHasButton(gp, b) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetButton(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GetGamepadButton(gp, b) ? 1 : 0);
    return 1;
}

// ==================== Light.Gamepad.HasAxis / GetAxis ====================

static int l_Gamepad_HasAxis(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
    if (a == SDL_GAMEPAD_AXIS_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadHasAxis(gp, a) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetAxis(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
    if (a == SDL_GAMEPAD_AXIS_INVALID) { lua_pushinteger(L, 0); return 1; }
    Sint16 v = SDL_GetGamepadAxis(gp, a);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

// ==================== Light.Gamepad.Rumble ====================

static int l_Gamepad_Rumble(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid gamepad handle");
        return 2;
    }
    lua_Integer low_i  = luaL_checkinteger(L, 2);
    lua_Integer high_i = luaL_checkinteger(L, 3);
    lua_Integer dur_i  = luaL_checkinteger(L, 4);
    if (low_i  < 0 || low_i  > 65535) {
        lua_pushboolean(L, 0); lua_pushstring(L, "low must be 0..65535"); return 2;
    }
    if (high_i < 0 || high_i > 65535) {
        lua_pushboolean(L, 0); lua_pushstring(L, "high must be 0..65535"); return 2;
    }
    if (dur_i < 0) {
        lua_pushboolean(L, 0); lua_pushstring(L, "duration_ms must be >= 0"); return 2;
    }
    bool ok = SDL_RumbleGamepad(gp, (Uint16)low_i, (Uint16)high_i, (Uint32)dur_i);
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Gamepad ====================

extern "C" LIGHT_API int luaopen_Light_Gamepad(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "GetGamepads",         l_Gamepad_GetGamepads         },
        { "Open",                l_Gamepad_Open                },
        { "Close",               l_Gamepad_Close               },
        { "GetID",               l_Gamepad_GetID               },
        { "GetName",             l_Gamepad_GetName             },
        { "GetType",             l_Gamepad_GetType             },
        { "IsConnected",         l_Gamepad_IsConnected         },
        { "GetConnectionState",  l_Gamepad_GetConnectionState  },
        { "GetPowerInfo",        l_Gamepad_GetPowerInfo        },
        { "HasButton",           l_Gamepad_HasButton           },
        { "GetButton",           l_Gamepad_GetButton           },
        { "HasAxis",             l_Gamepad_HasAxis             },
        { "GetAxis",             l_Gamepad_GetAxis             },
        { "Rumble",              l_Gamepad_Rumble              },
        { nullptr,               nullptr                       },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
