/**
 * @file light_url.cpp
 * @brief Light.URL 模块 - 用系统默认应用打开 URL/文件 (基于 SDL_OpenURL)
 *
 * Lua API:
 *   Light.URL.Open(url) -> ok, err
 *     - http:// / https:// : 浏览器打开
 *     - file://path / 本地路径 : 系统文件管理器或关联应用打开
 *     - mailto: / tel: 等 scheme : 系统对应 handler
 *
 * 平台覆盖: Win(ShellExecute) / Mac(LSOpenCFURLRef) / Linux(xdg-open)
 *           Web/iOS/Android 行为按系统约定
 *
 * 安全: SDL3 不做 URL 校验, 直接交给 OS; 调用方需校验来自不可信源的 URL.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.URL.Open ====================

static int l_URL_Open(lua_State* L) {
    const char* url = luaL_checkstring(L, 1);
    bool ok = SDL_OpenURL(url);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_URL ====================

extern "C" LIGHT_API int luaopen_Light_URL(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Open",  l_URL_Open },
        { nullptr, nullptr    },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
