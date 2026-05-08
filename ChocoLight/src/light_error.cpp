/**
 * @file light_error.cpp
 * @brief Light.Error module - SDL3 thread-local error string
 *
 * Lua API (6 fns):
 *
 *    Light.Error.Get()             -> string  (empty string if no error set)
 *    Light.Error.Set(message)      -> false   (always returns false, mirrors SDL convention)
 *    Light.Error.Clear()           -> true
 *    Light.Error.OutOfMemory()     -> false   (sets standardized OOM message)
 *    Light.Error.Unsupported()     -> false   (sets standardized "not supported" message)
 *    Light.Error.InvalidParam(name)-> false   (sets standardized invalid-param message)
 *
 * Notes:
 *  - SDL_SetError is variadic in C; we expose a plain 1-arg string version.
 *    Lua callers compose with string.format if they need formatting.
 *  - SDL maintains the error string per-OS-thread. Lua is single-threaded
 *    in this engine, so all calls share one error slot.
 *  - SDL_SetError, SDL_OutOfMemory, SDL_Unsupported, SDL_InvalidParamError
 *    all return `false` by SDL convention so a caller can `return SDL_SetError(...);`
 *    in a fn returning bool. We mirror this by pushing boolean false.
 *  - SDL_ClearError returns true.
 *
 * No SDL_Init dependency.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int l_Error_Get(lua_State* L) {
    const char* e = SDL_GetError();
    lua_pushstring(L, e ? e : "");
    return 1;
}

static int l_Error_Set(lua_State* L) {
    const char* msg = luaL_checkstring(L, 1);
    // Pass through "%s" so SDL doesn't interpret % chars in user message
    // as printf format specifiers.
    bool rc = SDL_SetError("%s", msg);
    lua_pushboolean(L, rc ? 1 : 0);
    return 1;
}

static int l_Error_Clear(lua_State* L) {
    bool rc = SDL_ClearError();
    lua_pushboolean(L, rc ? 1 : 0);
    return 1;
}

static int l_Error_OutOfMemory(lua_State* L) {
    bool rc = SDL_OutOfMemory();
    lua_pushboolean(L, rc ? 1 : 0);
    return 1;
}

static int l_Error_Unsupported(lua_State* L) {
    // SDL_Unsupported is a macro -> SDL_SetError("That operation is not supported")
    bool rc = SDL_Unsupported();
    lua_pushboolean(L, rc ? 1 : 0);
    return 1;
}

static int l_Error_InvalidParam(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    // SDL_InvalidParamError is a macro -> SDL_SetError("Parameter '%s' is invalid", name)
    bool rc = SDL_InvalidParamError(name);
    lua_pushboolean(L, rc ? 1 : 0);
    return 1;
}

static const luaL_Reg kErrorReg[] = {
    { "Get",          l_Error_Get          },
    { "Set",          l_Error_Set          },
    { "Clear",        l_Error_Clear        },
    { "OutOfMemory",  l_Error_OutOfMemory  },
    { "Unsupported",  l_Error_Unsupported  },
    { "InvalidParam", l_Error_InvalidParam },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Error(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kErrorReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    return 1;
}
