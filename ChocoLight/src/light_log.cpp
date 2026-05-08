/**
 * @file light_log.cpp
 * @brief Light.Log module - SDL3 internal log control + emit
 *
 * Lua API (14 fns):
 *
 *  Priority configuration:
 *    Light.Log.GetPriority(category)          -> priority
 *    Light.Log.SetPriority(category, priority)
 *    Light.Log.SetPriorities(priority)        -- apply to all categories
 *    Light.Log.ResetPriorities()
 *    Light.Log.SetPriorityPrefix(priority, prefix) -> ok, err
 *
 *  Emit:
 *    Light.Log.Log(msg)                       -- APPLICATION + INFO
 *    Light.Log.LogMessage(category, priority, msg)
 *    Light.Log.LogTrace(category, msg)
 *    Light.Log.LogVerbose(category, msg)
 *    Light.Log.LogDebug(category, msg)
 *    Light.Log.LogInfo(category, msg)
 *    Light.Log.LogWarn(category, msg)
 *    Light.Log.LogError(category, msg)
 *    Light.Log.LogCritical(category, msg)
 *
 *  Constants:
 *    PRIORITY_INVALID/TRACE/VERBOSE/DEBUG/INFO/WARN/ERROR/CRITICAL  (0..7)
 *    CATEGORY_APPLICATION/ERROR/ASSERT/SYSTEM/AUDIO/VIDEO/RENDER/
 *             INPUT/TEST/GPU/CUSTOM                                  (0..9, 19)
 *
 * No SDL_Init dependency: SDL_log operates on a process-wide priority
 * table and a default stderr (or platform-specific) sink, all safe
 * pre-Init / post-Quit.
 *
 * Not bound (intentional):
 *   SDL_SetLogOutputFunction / SDL_GetLogOutputFunction
 *   SDL_GetDefaultLogOutputFunction
 *     - The output callback fires synchronously from any thread that
 *       emits a log; safe Lua dispatch needs the Tray-style poll
 *       trampoline. Defer until a real user script needs it.
 *   SDL_LogMessageV (va_list variant)
 *     - Lua-side string formatting is the natural API; we accept a
 *       pre-formatted string from Lua and pass it as the literal "%s"
 *       argument, so format-string injection in user input is impossible.
 *
 * All emit functions pass the user message via a printf "%s" template,
 * which means embedded `%` characters in user content cannot reach the
 * variadic format machinery. This is a hardening choice, not a quirk.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// Helper: bounded priority validation. SDL itself accepts any int but
// silently ignores out-of-range values; we surface a clean Lua error so
// scripts catch typos early.
static SDL_LogPriority CheckPriority(lua_State* L, int idx) {
    int p = (int)luaL_checkinteger(L, idx);
    if (p < SDL_LOG_PRIORITY_INVALID || p >= SDL_LOG_PRIORITY_COUNT) {
        luaL_error(L, "priority %d out of range [0..%d]",
                   p, (int)SDL_LOG_PRIORITY_COUNT - 1);
    }
    return (SDL_LogPriority)p;
}

// Categories are open-ended (CUSTOM and beyond), so we just check sign.
static int CheckCategory(lua_State* L, int idx) {
    int c = (int)luaL_checkinteger(L, idx);
    if (c < 0) {
        luaL_error(L, "category %d must be >= 0", c);
    }
    return c;
}

// ===========================================================
// Priority configuration
// ===========================================================
static int l_Log_GetPriority(lua_State* L) {
    int category = CheckCategory(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_GetLogPriority(category));
    return 1;
}

static int l_Log_SetPriority(lua_State* L) {
    int category = CheckCategory(L, 1);
    SDL_LogPriority priority = CheckPriority(L, 2);
    SDL_SetLogPriority(category, priority);
    return 0;
}

static int l_Log_SetPriorities(lua_State* L) {
    SDL_LogPriority priority = CheckPriority(L, 1);
    SDL_SetLogPriorities(priority);
    return 0;
}

static int l_Log_ResetPriorities(lua_State* /*L*/) {
    SDL_ResetLogPriorities();
    return 0;
}

static int l_Log_SetPriorityPrefix(lua_State* L) {
    SDL_LogPriority priority = CheckPriority(L, 1);
    const char* prefix = nullptr;
    if (!lua_isnoneornil(L, 2)) {
        prefix = luaL_checkstring(L, 2);
    }
    if (!SDL_SetLogPriorityPrefix(priority, prefix)) {
        lua_pushboolean(L, 0);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_SetLogPriorityPrefix failed");
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ===========================================================
// Emit helpers - all forward through "%s" so user-supplied messages
// cannot inject printf format directives.
// ===========================================================
static int l_Log_Log(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    SDL_Log("%s", msg);
    return 0;
}

static int l_Log_LogMessage(lua_State* L) {
    int category = CheckCategory(L, 1);
    SDL_LogPriority priority = CheckPriority(L, 2);
    const char* msg = luaL_checkstring(L, 3);
    SDL_LogMessage(category, priority, "%s", msg);
    return 0;
}

#define LIGHT_LOG_LEVEL_FN(NAME, SDLFN)                                 \
    static int l_Log_##NAME(lua_State* L) {                             \
        int category = CheckCategory(L, 1);                             \
        const char* msg = luaL_checkstring(L, 2);                       \
        SDLFN(category, "%s", msg);                                     \
        return 0;                                                       \
    }

LIGHT_LOG_LEVEL_FN(LogTrace,    SDL_LogTrace)
LIGHT_LOG_LEVEL_FN(LogVerbose,  SDL_LogVerbose)
LIGHT_LOG_LEVEL_FN(LogDebug,    SDL_LogDebug)
LIGHT_LOG_LEVEL_FN(LogInfo,     SDL_LogInfo)
LIGHT_LOG_LEVEL_FN(LogWarn,     SDL_LogWarn)
LIGHT_LOG_LEVEL_FN(LogError,    SDL_LogError)
LIGHT_LOG_LEVEL_FN(LogCritical, SDL_LogCritical)

#undef LIGHT_LOG_LEVEL_FN

// ===========================================================
// luaopen_Light_Log
// ===========================================================
static const luaL_Reg kLogReg[] = {
    { "GetPriority",        l_Log_GetPriority        },
    { "SetPriority",        l_Log_SetPriority        },
    { "SetPriorities",      l_Log_SetPriorities      },
    { "ResetPriorities",    l_Log_ResetPriorities    },
    { "SetPriorityPrefix",  l_Log_SetPriorityPrefix  },
    { "Log",                l_Log_Log                },
    { "LogMessage",         l_Log_LogMessage         },
    { "LogTrace",           l_Log_LogTrace           },
    { "LogVerbose",         l_Log_LogVerbose         },
    { "LogDebug",           l_Log_LogDebug           },
    { "LogInfo",            l_Log_LogInfo            },
    { "LogWarn",            l_Log_LogWarn            },
    { "LogError",           l_Log_LogError           },
    { "LogCritical",        l_Log_LogCritical        },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Log(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kLogReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Priorities (8)
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_INVALID);
    lua_setfield(L, -2, "PRIORITY_INVALID");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_TRACE);
    lua_setfield(L, -2, "PRIORITY_TRACE");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_VERBOSE);
    lua_setfield(L, -2, "PRIORITY_VERBOSE");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_DEBUG);
    lua_setfield(L, -2, "PRIORITY_DEBUG");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_INFO);
    lua_setfield(L, -2, "PRIORITY_INFO");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_WARN);
    lua_setfield(L, -2, "PRIORITY_WARN");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_ERROR);
    lua_setfield(L, -2, "PRIORITY_ERROR");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_PRIORITY_CRITICAL);
    lua_setfield(L, -2, "PRIORITY_CRITICAL");

    // Categories (11 stable + custom)
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_APPLICATION);
    lua_setfield(L, -2, "CATEGORY_APPLICATION");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_ERROR);
    lua_setfield(L, -2, "CATEGORY_ERROR");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_ASSERT);
    lua_setfield(L, -2, "CATEGORY_ASSERT");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_SYSTEM);
    lua_setfield(L, -2, "CATEGORY_SYSTEM");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_AUDIO);
    lua_setfield(L, -2, "CATEGORY_AUDIO");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_VIDEO);
    lua_setfield(L, -2, "CATEGORY_VIDEO");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_RENDER);
    lua_setfield(L, -2, "CATEGORY_RENDER");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_INPUT);
    lua_setfield(L, -2, "CATEGORY_INPUT");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_TEST);
    lua_setfield(L, -2, "CATEGORY_TEST");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_GPU);
    lua_setfield(L, -2, "CATEGORY_GPU");
    lua_pushinteger(L, (lua_Integer)SDL_LOG_CATEGORY_CUSTOM);
    lua_setfield(L, -2, "CATEGORY_CUSTOM");

    return 1;
}
