/**
 * @file light_time.cpp
 * @brief Light.Time module - SDL3 timer + realtime clock + date/time
 *
 * Lua API:
 *
 *  Tick / performance counters (SDL_timer.h):
 *    Light.Time.GetTicks()                    -> ms (number)
 *    Light.Time.GetTicksNS()                  -> ns (number, may lose
 *                                                precision after ~104 days)
 *    Light.Time.GetPerformanceCounter()       -> hi-res counter (number)
 *    Light.Time.GetPerformanceFrequency()     -> counts per second (number)
 *
 *  Sleep helpers (use sparingly on CI; smoke uses 1ms):
 *    Light.Time.Delay(ms)                     -> (void)
 *    Light.Time.DelayNS(ns)                   -> (void)
 *    Light.Time.DelayPrecise(ns)              -> (void)
 *
 *  Realtime clock (SDL_time.h):
 *    Light.Time.GetCurrentTime()              -> ns_since_epoch | nil, err
 *    Light.Time.GetDateTimeLocalePreferences()-> {date_format, time_format}
 *    Light.Time.TimeToDateTime(ticks, local?) -> {year, month, day, hour,
 *                                                  minute, second,
 *                                                  nanosecond, day_of_week,
 *                                                  utc_offset} | nil, err
 *    Light.Time.DateTimeToTime(dt)            -> ticks | nil, err
 *
 *  Calendar helpers (cheap stateless math):
 *    Light.Time.GetDaysInMonth(year, month)   -> int
 *    Light.Time.GetDayOfYear(y, m, d)         -> int
 *    Light.Time.GetDayOfWeek(y, m, d)         -> int (0=Sunday)
 *
 * Constants:
 *    DATE_FORMAT_YYYYMMDD = 0
 *    DATE_FORMAT_DDMMYYYY = 1
 *    DATE_FORMAT_MMDDYYYY = 2
 *    TIME_FORMAT_24HR     = 0
 *    TIME_FORMAT_12HR     = 1
 *
 * Not bound (intentional):
 *    SDL_AddTimer / SDL_AddTimerNS / SDL_RemoveTimer
 *      - timer callback fires on a dedicated SDL thread; safe Lua dispatch
 *        needs the Tray-style poll trampoline. Defer to a later phase.
 *    SDL_TimeToWindows / SDL_TimeFromWindows
 *      - Windows-specific FILETIME interop; out of cross-platform scope.
 *
 * No SDL_Init dependency: SDL_GetTicks / SDL_GetCurrentTime / calendar
 * helpers are all process-wide and safe pre-Init.
 *
 * Numeric range: lua_Number is double (53-bit precision). Uint64 ticks /
 * SDL_Time exceed this range only after ~285 years from epoch, so storing
 * ns_since_epoch as a double is safe for any plausible real-world clock.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ===========================================================
// Tick / performance counters
// ===========================================================
static int l_Time_GetTicks(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetTicks());
    return 1;
}
static int l_Time_GetTicksNS(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetTicksNS());
    return 1;
}
static int l_Time_GetPerformanceCounter(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetPerformanceCounter());
    return 1;
}
static int l_Time_GetPerformanceFrequency(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SDL_GetPerformanceFrequency());
    return 1;
}

// ===========================================================
// Sleep helpers
// ===========================================================
static int l_Time_Delay(lua_State* L) {
    lua_Number ms = luaL_checknumber(L, 1);
    if (ms < 0) ms = 0;
    SDL_Delay((Uint32)ms);
    return 0;
}
static int l_Time_DelayNS(lua_State* L) {
    lua_Number ns = luaL_checknumber(L, 1);
    if (ns < 0) ns = 0;
    SDL_DelayNS((Uint64)ns);
    return 0;
}
static int l_Time_DelayPrecise(lua_State* L) {
    lua_Number ns = luaL_checknumber(L, 1);
    if (ns < 0) ns = 0;
    SDL_DelayPrecise((Uint64)ns);
    return 0;
}

// ===========================================================
// Realtime clock
// ===========================================================
static int l_Time_GetCurrentTime(lua_State* L) {
    SDL_Time t = 0;
    if (!SDL_GetCurrentTime(&t)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetCurrentTime failed");
        return 2;
    }
    // SDL_Time is Sint64 ns since Unix epoch
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

static int l_Time_GetDateTimeLocalePreferences(lua_State* L) {
    SDL_DateFormat df = SDL_DATE_FORMAT_YYYYMMDD;
    SDL_TimeFormat tf = SDL_TIME_FORMAT_24HR;
    if (!SDL_GetDateTimeLocalePreferences(&df, &tf)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_GetDateTimeLocalePreferences failed");
        return 2;
    }
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)df);
    lua_setfield(L, -2, "date_format");
    lua_pushinteger(L, (lua_Integer)tf);
    lua_setfield(L, -2, "time_format");
    return 1;
}

// Push SDL_DateTime as Lua table.
static void PushDateTime(lua_State* L, const SDL_DateTime& dt) {
    lua_newtable(L);
    lua_pushinteger(L, dt.year);        lua_setfield(L, -2, "year");
    lua_pushinteger(L, dt.month);       lua_setfield(L, -2, "month");
    lua_pushinteger(L, dt.day);         lua_setfield(L, -2, "day");
    lua_pushinteger(L, dt.hour);        lua_setfield(L, -2, "hour");
    lua_pushinteger(L, dt.minute);      lua_setfield(L, -2, "minute");
    lua_pushinteger(L, dt.second);      lua_setfield(L, -2, "second");
    lua_pushinteger(L, dt.nanosecond);  lua_setfield(L, -2, "nanosecond");
    lua_pushinteger(L, dt.day_of_week); lua_setfield(L, -2, "day_of_week");
    lua_pushinteger(L, dt.utc_offset);  lua_setfield(L, -2, "utc_offset");
}

// Read SDL_DateTime from Lua table at idx, with sane defaults.
// Returns false + leaves err on stack if a required field is missing/bad.
static bool ReadDateTime(lua_State* L, int idx, SDL_DateTime& out, const char*& err) {
    err = nullptr;
    if (lua_type(L, idx) != LUA_TTABLE) {
        err = "dt must be a table";
        return false;
    }

    auto getInt = [&](const char* k, int def, bool required) -> int {
        lua_getfield(L, idx, k);
        int v = def;
        int t = lua_type(L, -1);
        if (t == LUA_TNUMBER) {
            v = (int)lua_tointeger(L, -1);
        } else if (t != LUA_TNIL) {
            err = k;  // signal type error via field name
        } else if (required && t == LUA_TNIL) {
            err = k;
        }
        lua_pop(L, 1);
        return v;
    };

    out.year       = getInt("year",        1970, true);
    if (err) return false;
    out.month      = getInt("month",       1,    true);
    if (err) return false;
    out.day        = getInt("day",         1,    true);
    if (err) return false;
    out.hour       = getInt("hour",        0,    false);
    if (err) return false;
    out.minute     = getInt("minute",      0,    false);
    if (err) return false;
    out.second     = getInt("second",      0,    false);
    if (err) return false;
    out.nanosecond = getInt("nanosecond",  0,    false);
    if (err) return false;
    out.day_of_week= getInt("day_of_week", 0,    false);
    if (err) return false;
    out.utc_offset = getInt("utc_offset",  0,    false);
    if (err) return false;
    return true;
}

static int l_Time_TimeToDateTime(lua_State* L) {
    SDL_Time ticks = (SDL_Time)luaL_checknumber(L, 1);
    bool localTime = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        localTime = lua_toboolean(L, 2) != 0;
    }
    SDL_DateTime dt;
    if (!SDL_TimeToDateTime(ticks, &dt, localTime)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_TimeToDateTime failed");
        return 2;
    }
    PushDateTime(L, dt);
    return 1;
}

static int l_Time_DateTimeToTime(lua_State* L) {
    SDL_DateTime dt;
    const char* err = nullptr;
    if (!ReadDateTime(L, 1, dt, err)) {
        lua_pushnil(L);
        char msg[128];
        SDL_snprintf(msg, sizeof(msg), "invalid dt field: %s", err ? err : "?");
        lua_pushstring(L, msg);
        return 2;
    }
    SDL_Time t = 0;
    if (!SDL_DateTimeToTime(&dt, &t)) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_DateTimeToTime failed");
        return 2;
    }
    lua_pushnumber(L, (lua_Number)t);
    return 1;
}

// ===========================================================
// Calendar helpers
// ===========================================================
static int l_Time_GetDaysInMonth(lua_State* L) {
    int year = (int)luaL_checkinteger(L, 1);
    int month = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, SDL_GetDaysInMonth(year, month));
    return 1;
}
static int l_Time_GetDayOfYear(lua_State* L) {
    int y = (int)luaL_checkinteger(L, 1);
    int m = (int)luaL_checkinteger(L, 2);
    int d = (int)luaL_checkinteger(L, 3);
    lua_pushinteger(L, SDL_GetDayOfYear(y, m, d));
    return 1;
}
static int l_Time_GetDayOfWeek(lua_State* L) {
    int y = (int)luaL_checkinteger(L, 1);
    int m = (int)luaL_checkinteger(L, 2);
    int d = (int)luaL_checkinteger(L, 3);
    lua_pushinteger(L, SDL_GetDayOfWeek(y, m, d));
    return 1;
}

// ===========================================================
// luaopen_Light_Time
// ===========================================================
static const luaL_Reg kTimeReg[] = {
    { "GetTicks",                    l_Time_GetTicks                    },
    { "GetTicksNS",                  l_Time_GetTicksNS                  },
    { "GetPerformanceCounter",       l_Time_GetPerformanceCounter       },
    { "GetPerformanceFrequency",     l_Time_GetPerformanceFrequency     },
    { "Delay",                       l_Time_Delay                       },
    { "DelayNS",                     l_Time_DelayNS                     },
    { "DelayPrecise",                l_Time_DelayPrecise                },
    { "GetCurrentTime",              l_Time_GetCurrentTime              },
    { "GetDateTimeLocalePreferences",l_Time_GetDateTimeLocalePreferences},
    { "TimeToDateTime",              l_Time_TimeToDateTime              },
    { "DateTimeToTime",              l_Time_DateTimeToTime              },
    { "GetDaysInMonth",              l_Time_GetDaysInMonth              },
    { "GetDayOfYear",                l_Time_GetDayOfYear                },
    { "GetDayOfWeek",                l_Time_GetDayOfWeek                },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Time(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kTimeReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_YYYYMMDD);
    lua_setfield(L, -2, "DATE_FORMAT_YYYYMMDD");
    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_DDMMYYYY);
    lua_setfield(L, -2, "DATE_FORMAT_DDMMYYYY");
    lua_pushinteger(L, (lua_Integer)SDL_DATE_FORMAT_MMDDYYYY);
    lua_setfield(L, -2, "DATE_FORMAT_MMDDYYYY");

    lua_pushinteger(L, (lua_Integer)SDL_TIME_FORMAT_24HR);
    lua_setfield(L, -2, "TIME_FORMAT_24HR");
    lua_pushinteger(L, (lua_Integer)SDL_TIME_FORMAT_12HR);
    lua_setfield(L, -2, "TIME_FORMAT_12HR");

    return 1;
}
