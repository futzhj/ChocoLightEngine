/**
 * @file light_clipboard.cpp
 * @brief Light.Clipboard 模块 - 系统剪贴板读写 (基于 SDL_clipboard)
 *
 * Lua API:
 *   Light.Clipboard.SetText(text)        -> ok, err
 *   Light.Clipboard.GetText()            -> text_string, err   (空时返回 "")
 *   Light.Clipboard.HasText()            -> bool                (无 err 输出)
 *
 * 平台覆盖: Win/Mac/Linux/Web/Android/iOS 均原生支持
 *           (Web 走浏览器 navigator.clipboard, 可能受用户手势限制)
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.Clipboard.SetText ====================

static int l_Clipboard_SetText(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    bool ok = SDL_SetClipboardText(text);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Clipboard.GetText ====================

static int l_Clipboard_GetText(lua_State* L) {
    // SDL3: 总返回 malloc 字符串 (空剪贴板返回空字符串而非 NULL); 调用方 free
    char* text = SDL_GetClipboardText();
    if (!text) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushstring(L, text);
    SDL_free(text);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Clipboard.HasText ====================

static int l_Clipboard_HasText(lua_State* L) {
    bool has = SDL_HasClipboardText();
    lua_pushboolean(L, has);
    return 1;
}

// ==================== luaopen_Light_Clipboard ====================

extern "C" LIGHT_API int luaopen_Light_Clipboard(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "SetText", l_Clipboard_SetText },
        { "GetText", l_Clipboard_GetText },
        { "HasText", l_Clipboard_HasText },
        { nullptr,   nullptr             },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
