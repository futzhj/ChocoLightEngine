/**
 * @file light_misc.cpp
 * @brief Light.Misc module - SDL_misc.h
 *
 * Lua API (1 fn):
 *   Light.Misc.OpenURL(url) -> ok, err
 *
 * Notes:
 *  - SDL_OpenURL hands the URL to the OS shell handler. On desktop this
 *    typically launches the default browser; on mobile / web it follows
 *    the platform convention.
 *  - The function does NOT validate the URL string format - any
 *    "scheme:..." string is forwarded as-is.
 *  - On headless CI runners SDL_OpenURL may still succeed (the shell call
 *    only requires a working OS handler registration), so smoke does not
 *    assert success.
 *  - This module deliberately does NOT auto-init any SDL subsystem.
 *    SDL_OpenURL is documented as not requiring SDL_Init().
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

static int l_Misc_OpenURL(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    bool ok = SDL_OpenURL(url);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) {
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "SDL_OpenURL failed");
        return 2;
    }
    return 1;
}

extern "C" LIGHT_API int luaopen_Light_Misc(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "OpenURL", l_Misc_OpenURL },
        { nullptr,   nullptr        },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
