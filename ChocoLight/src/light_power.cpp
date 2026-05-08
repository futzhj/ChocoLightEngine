/**
 * @file light_power.cpp
 * @brief Light.Power 模块 - 电源/电池信息查询 (基于 SDL_GetPowerInfo)
 *
 * Lua API:
 *   Light.Power.Info() -> state, seconds, percent
 *     - state:    "unknown" | "on_battery" | "no_battery" | "charging" | "charged"
 *     - seconds:  剩余使用时间秒数, -1 表示未知或无电池
 *     - percent:  0-100 表示电量百分比, -1 表示未知或无电池
 *
 * 典型用法:
 *   local state, sec, pct = Light.Power.Info()
 *   if state == "on_battery" and pct < 20 then
 *       -- 弹出低电量警告
 *   end
 *
 * 平台覆盖: Win/Mac/Linux/iOS/Android (Web 通常返回 unknown)
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.Power.Info ====================

static const char* PowerStateToString(SDL_PowerState s) {
    switch (s) {
        case SDL_POWERSTATE_ON_BATTERY: return "on_battery";
        case SDL_POWERSTATE_NO_BATTERY: return "no_battery";
        case SDL_POWERSTATE_CHARGING:   return "charging";
        case SDL_POWERSTATE_CHARGED:    return "charged";
        case SDL_POWERSTATE_UNKNOWN:
        case SDL_POWERSTATE_ERROR:
        default:                        return "unknown";
    }
}

static int l_Power_Info(lua_State* L) {
    int seconds = -1;
    int percent = -1;
    SDL_PowerState state = SDL_GetPowerInfo(&seconds, &percent);

    lua_pushstring(L, PowerStateToString(state));
    lua_pushinteger(L, seconds);
    lua_pushinteger(L, percent);
    return 3;
}

// ==================== luaopen_Light_Power ====================

extern "C" LIGHT_API int luaopen_Light_Power(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Info",  l_Power_Info },
        { nullptr, nullptr      },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
