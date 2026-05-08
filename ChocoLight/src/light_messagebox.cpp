/**
 * @file light_messagebox.cpp
 * @brief Light.MessageBox 模块 - 系统模态对话框 (基于 SDL_ShowMessageBox)
 *
 * Lua API:
 *   Light.MessageBox.ShowSimple(level, title, message) -> ok, err
 *     - level: "info" / "warning" / "error" (大小写不敏感; 默认 info)
 *
 *   Light.MessageBox.Show({
 *       level   = "info"|"warning"|"error",  -- 可选, 默认 info
 *       title   = "...",                     -- 必填
 *       message = "...",                     -- 必填
 *       buttons = { "OK", "Cancel", ... },   -- 必填, 至少 1 个
 *       default_button = 1,                  -- 可选, 1-based 索引
 *       cancel_button  = 2,                  -- 可选, 1-based 索引
 *   }) -> button_index (1-based) | nil, err
 *
 * 注意: Show / ShowSimple 是阻塞调用, 主循环会被冻结直到用户关闭对话框.
 *       Web/Android 平台行为与桌面不同 (浏览器 alert / 原生 AlertDialog).
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <vector>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 字符串 -> SDL flag, 大小写不敏感.
static Uint32 ParseLevel(const char* s) {
    if (!s) return SDL_MESSAGEBOX_INFORMATION;
    if (SDL_strcasecmp(s, "warning") == 0) return SDL_MESSAGEBOX_WARNING;
    if (SDL_strcasecmp(s, "error")   == 0) return SDL_MESSAGEBOX_ERROR;
    return SDL_MESSAGEBOX_INFORMATION;
}

// ==================== Light.MessageBox.ShowSimple ====================

static int l_MB_ShowSimple(lua_State* L) {
    const char* level   = luaL_optstring(L, 1, "info");
    const char* title   = luaL_checkstring(L, 2);
    const char* message = luaL_checkstring(L, 3);

    Uint32 flags = ParseLevel(level);
    bool ok = SDL_ShowSimpleMessageBox(flags, title, message, nullptr);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnil(L);
    return 2;
}

// ==================== Light.MessageBox.Show ====================

static int l_MB_Show(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    // 解析 level
    lua_getfield(L, 1, "level");
    Uint32 flags = ParseLevel(lua_isstring(L, -1) ? lua_tostring(L, -1) : nullptr);
    lua_pop(L, 1);

    // title / message
    lua_getfield(L, 1, "title");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "title is required");
        return 2;
    }
    std::string title = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 1, "message");
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "message is required");
        return 2;
    }
    std::string message = lua_tostring(L, -1);
    lua_pop(L, 1);

    // buttons (字符串数组)
    lua_getfield(L, 1, "buttons");
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "buttons must be a table of strings");
        return 2;
    }
    int n = (int)lua_objlen(L, -1);
    if (n <= 0 || n > 32) {  // 防御: 上限 32 个按钮
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, "buttons count out of range (1..32)");
        return 2;
    }

    // 持有按钮文本字符串生命周期
    std::vector<std::string> btn_texts;
    btn_texts.reserve(n);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 2);
            lua_pushnil(L);
            lua_pushfstring(L, "buttons[%d] is not a string", i);
            return 2;
        }
        btn_texts.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);  // pop buttons

    // default_button / cancel_button (1-based)
    lua_getfield(L, 1, "default_button");
    int default_idx = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : -1;
    lua_pop(L, 1);
    lua_getfield(L, 1, "cancel_button");
    int cancel_idx  = lua_isnumber(L, -1) ? (int)lua_tointeger(L, -1) : -1;
    lua_pop(L, 1);

    // 装配 SDL_MessageBoxButtonData
    std::vector<SDL_MessageBoxButtonData> buttons(n);
    for (int i = 0; i < n; ++i) {
        buttons[i].flags = 0;
        if ((i + 1) == default_idx) buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT;
        if ((i + 1) == cancel_idx)  buttons[i].flags |= SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT;
        buttons[i].buttonID = i + 1;  // 直接用 1-based 作 ID, Lua 端无需再换算
        buttons[i].text     = btn_texts[i].c_str();
    }

    SDL_MessageBoxData data = {};
    data.flags       = flags;
    data.window      = nullptr;
    data.title       = title.c_str();
    data.message     = message.c_str();
    data.numbuttons  = n;
    data.buttons     = buttons.data();
    data.colorScheme = nullptr;

    int hit_id = -1;
    bool ok = SDL_ShowMessageBox(&data, &hit_id);
    if (!ok) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    if (hit_id <= 0) {
        // 用户用 ESC 关闭却没设 cancel_button 时, SDL 可能返回 -1
        lua_pushnil(L);
        lua_pushstring(L, "no button selected");
        return 2;
    }
    lua_pushinteger(L, hit_id);
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_MessageBox ====================

extern "C" LIGHT_API int luaopen_Light_MessageBox(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "ShowSimple", l_MB_ShowSimple },
        { "Show",       l_MB_Show       },
        { nullptr,      nullptr         },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
