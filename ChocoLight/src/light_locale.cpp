/**
 * @file light_locale.cpp
 * @brief Light.Locale 模块 - 系统首选语言/区域查询 (基于 SDL_GetPreferredLocales)
 *
 * Lua API:
 *   Light.Locale.GetPreferred() -> { {language=str, country=str|nil}, ... }, err
 *     - 按用户偏好顺序返回, 第一个为最高优先级
 *     - language: ISO-639 小写 (如 "en", "zh", "ja")
 *     - country:  ISO-3166 大写 (如 "US", "CN", "JP"), 部分平台为 nil
 *     - 失败返回 nil + err 字符串
 *
 * 典型用法:
 *   local locales = Light.Locale.GetPreferred()
 *   if locales and locales[1] then
 *       local primary = locales[1]
 *       if primary.language == "zh" then ... end
 *   end
 *
 * 平台覆盖: Win/Mac/Linux/iOS/Android/Web 均有效
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.Locale.GetPreferred ====================

static int l_Locale_GetPreferred(lua_State* L) {
    int count = 0;
    SDL_Locale** locales = SDL_GetPreferredLocales(&count);
    if (!locales) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        if (!locales[i]) continue;

        // 每个 locale 是一个子表 {language=..., country=...}
        lua_createtable(L, 0, 2);

        lua_pushstring(L, locales[i]->language ? locales[i]->language : "");
        lua_setfield(L, -2, "language");

        if (locales[i]->country) {
            lua_pushstring(L, locales[i]->country);
            lua_setfield(L, -2, "country");
        }
        // country 为 NULL 时不设字段, 用户可用 locale.country 判断 nil

        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(locales);  // SDL3 约定: 单块内存, 只释放数组本身

    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Locale ====================

extern "C" LIGHT_API int luaopen_Light_Locale(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "GetPreferred", l_Locale_GetPreferred },
        { nullptr,        nullptr               },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
