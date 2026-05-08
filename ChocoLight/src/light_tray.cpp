/**
 * @file light_tray.cpp
 * @brief Light.Tray 模块 - 系统托盘图标 + 上下文菜单 (基于 SDL_tray)
 *
 * 平台覆盖: Windows / macOS / Linux (GNOME/KDE/Unity 等). Web/Android/iOS 上 SDL3 走 dummy 实现, 一切返回 nil.
 *
 * 本期不暴露 Lua callback (避免 trampoline 复杂度);
 * 改用 polling 模式: 内部为每个 entry 自动注册 click-count 累加 callback,
 * Lua 端调用 Light.Tray.WasClicked(entry) 拿到 (并清零) 累计次数.
 *
 * Lua API (17 fns):
 *   Light.Tray.Create(tooltip)          -> tray_handle, err            (icon 不支持; 后续 Phase 加)
 *   Light.Tray.Destroy(tray)            -> ok, err
 *   Light.Tray.SetTooltip(tray, str)    -> ok, err
 *
 *   Light.Tray.GetMenu(tray)            -> menu_handle, err            (首次调用会创建 menu)
 *   Light.Tray.AddButton(menu, label)   -> entry_handle, err
 *   Light.Tray.AddCheckbox(menu, label, [checked]) -> entry_handle, err
 *   Light.Tray.AddSeparator(menu)       -> entry_handle, err
 *   Light.Tray.AddSubmenu(menu, label)  -> entry_handle, submenu_handle, err
 *
 *   Light.Tray.SetEntryLabel(entry, str)   -> ok, err
 *   Light.Tray.GetEntryLabel(entry)        -> str, err
 *   Light.Tray.SetEntryEnabled(entry, b)   -> ok, err
 *   Light.Tray.GetEntryEnabled(entry)      -> bool
 *     注意: SDL3 v3.2.30 Windows 后端 SDL_GetTrayEntryEnabled 存在上游 bug
 *           `((mii.fState & MFS_ENABLED) != 0)` -- MFS_ENABLED 值为 0, 表达式恒为 false.
 *           因此 Windows 上 GetEntryEnabled 实际无法正确返回状态. 其他平台不受影响.
 *           本模块不做 workaround, 保持与 SDL3 一致.
 *   Light.Tray.SetEntryChecked(entry, b)   -> ok, err
 *   Light.Tray.GetEntryChecked(entry)      -> bool
 *   Light.Tray.RemoveEntry(entry)          -> ok, err
 *
 *   Light.Tray.WasClicked(entry)        -> int_count_then_reset
 *   Light.Tray.Update()                 -> 每帧调用; 平台 event loop pump
 *
 * 生命周期:
 *   Light.Tray.Destroy(tray) 会调用 SDL_DestroyTray, SDL3 内部级联销毁 menu/entry.
 *   销毁后所有 entry/menu lightuserdata 失效, Lua 端不应再使用.
 *   click counts map 在 Destroy 时不主动清理 (避免遍历内部 entry tree),
 *   而是在 Update 前判定 entry 是否已死. 简化版本: 直接 leak 几条 int (可忽略).
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <unordered_map>
#include <mutex>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// 全局 click counter (map: entry -> int).
// 用 std::mutex 保护 (callback 在主线程派发, 但 Update 也在主线程,
// 严格说不需要锁; 加锁是为了未来扩展并 Mac/Linux 不同 backend 的安全).
// ============================================================
static std::unordered_map<SDL_TrayEntry*, int> g_click_counts;
static std::mutex g_click_mutex;

static void SDLCALL TrayClickTrampoline(void* /*userdata*/, SDL_TrayEntry* entry) {
    if (!entry) return;
    std::lock_guard<std::mutex> lk(g_click_mutex);
    g_click_counts[entry] += 1;
}

static SDL_Tray*      CheckTray(lua_State* L, int idx)  { return lua_islightuserdata(L, idx) ? (SDL_Tray*)lua_touserdata(L, idx)      : nullptr; }
static SDL_TrayMenu*  CheckMenu(lua_State* L, int idx)  { return lua_islightuserdata(L, idx) ? (SDL_TrayMenu*)lua_touserdata(L, idx)  : nullptr; }
static SDL_TrayEntry* CheckEntry(lua_State* L, int idx) { return lua_islightuserdata(L, idx) ? (SDL_TrayEntry*)lua_touserdata(L, idx) : nullptr; }

// ==================== Tray Create / Destroy / SetTooltip ====================

static int l_Tray_Create(lua_State* L) {
    const char* tooltip = luaL_optstring(L, 1, nullptr);
    SDL_Tray* tray = SDL_CreateTray(/*icon*/ nullptr, tooltip);
    if (!tray) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, tray);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_Destroy(lua_State* L) {
    SDL_Tray* tray = CheckTray(L, 1);
    if (!tray) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid tray handle"); return 2; }
    SDL_DestroyTray(tray);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_SetTooltip(lua_State* L) {
    SDL_Tray* tray = CheckTray(L, 1);
    if (!tray) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid tray handle"); return 2; }
    const char* s = luaL_checkstring(L, 2);
    SDL_SetTrayTooltip(tray, s);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Menu ====================

static int l_Tray_GetMenu(lua_State* L) {
    SDL_Tray* tray = CheckTray(L, 1);
    if (!tray) { lua_pushnil(L); lua_pushstring(L, "invalid tray handle"); return 2; }
    SDL_TrayMenu* menu = SDL_GetTrayMenu(tray);
    if (!menu) {
        // 首次调用需创建
        menu = SDL_CreateTrayMenu(tray);
        if (!menu) {
            lua_pushnil(L);
            lua_pushstring(L, SDL_GetError());
            return 2;
        }
    }
    lua_pushlightuserdata(L, menu);
    lua_pushnil(L);
    return 2;
}

// 内部辅助: insert entry 并自动 attach click trampoline (separator 不需要)
static SDL_TrayEntry* InsertAndHook(SDL_TrayMenu* menu, const char* label,
                                    SDL_TrayEntryFlags flags, bool hook_click) {
    SDL_TrayEntry* e = SDL_InsertTrayEntryAt(menu, /*pos=-1 means append*/ -1, label, flags);
    if (e && hook_click) {
        SDL_SetTrayEntryCallback(e, TrayClickTrampoline, nullptr);
    }
    return e;
}

static int l_Tray_AddButton(lua_State* L) {
    SDL_TrayMenu* menu = CheckMenu(L, 1);
    if (!menu) { lua_pushnil(L); lua_pushstring(L, "invalid menu handle"); return 2; }
    const char* label = luaL_checkstring(L, 2);
    SDL_TrayEntry* e = InsertAndHook(menu, label, SDL_TRAYENTRY_BUTTON, /*hook*/ true);
    if (!e) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushlightuserdata(L, e);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_AddCheckbox(lua_State* L) {
    SDL_TrayMenu* menu = CheckMenu(L, 1);
    if (!menu) { lua_pushnil(L); lua_pushstring(L, "invalid menu handle"); return 2; }
    const char* label = luaL_checkstring(L, 2);
    bool checked = lua_toboolean(L, 3) != 0;
    SDL_TrayEntryFlags flags = SDL_TRAYENTRY_CHECKBOX;
    if (checked) flags |= SDL_TRAYENTRY_CHECKED;
    SDL_TrayEntry* e = InsertAndHook(menu, label, flags, /*hook*/ true);
    if (!e) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushlightuserdata(L, e);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_AddSeparator(lua_State* L) {
    SDL_TrayMenu* menu = CheckMenu(L, 1);
    if (!menu) { lua_pushnil(L); lua_pushstring(L, "invalid menu handle"); return 2; }
    // SDL3 用 label=NULL + flags=0 表示 separator (按文档)
    SDL_TrayEntry* e = SDL_InsertTrayEntryAt(menu, -1, nullptr, 0);
    if (!e) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushlightuserdata(L, e);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_AddSubmenu(lua_State* L) {
    SDL_TrayMenu* menu = CheckMenu(L, 1);
    if (!menu) { lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, "invalid menu handle"); return 3; }
    const char* label = luaL_checkstring(L, 2);
    SDL_TrayEntry* e = SDL_InsertTrayEntryAt(menu, -1, label, SDL_TRAYENTRY_SUBMENU);
    if (!e) { lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 3; }
    SDL_TrayMenu* sub = SDL_CreateTraySubmenu(e);
    if (!sub) { lua_pushnil(L); lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 3; }
    lua_pushlightuserdata(L, e);
    lua_pushlightuserdata(L, sub);
    lua_pushnil(L);
    return 3;
}

// ==================== Entry property setters / getters ====================

static int l_Tray_SetEntryLabel(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid entry handle"); return 2; }
    const char* s = luaL_checkstring(L, 2);
    SDL_SetTrayEntryLabel(e, s);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_GetEntryLabel(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushnil(L); lua_pushstring(L, "invalid entry handle"); return 2; }
    const char* s = SDL_GetTrayEntryLabel(e);
    lua_pushstring(L, s ? s : "");
    lua_pushnil(L);
    return 2;
}

static int l_Tray_SetEntryEnabled(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid entry handle"); return 2; }
    bool b = lua_toboolean(L, 2) != 0;
    SDL_SetTrayEntryEnabled(e, b);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_GetEntryEnabled(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GetTrayEntryEnabled(e) ? 1 : 0);
    return 1;
}

static int l_Tray_SetEntryChecked(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid entry handle"); return 2; }
    bool b = lua_toboolean(L, 2) != 0;
    SDL_SetTrayEntryChecked(e, b);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

static int l_Tray_GetEntryChecked(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GetTrayEntryChecked(e) ? 1 : 0);
    return 1;
}

static int l_Tray_RemoveEntry(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid entry handle"); return 2; }
    {
        std::lock_guard<std::mutex> lk(g_click_mutex);
        g_click_counts.erase(e);
    }
    SDL_RemoveTrayEntry(e);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Polling ====================

// 取出累计点击次数并清零. 无 entry 或未点击返回 0.
static int l_Tray_WasClicked(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    int count = 0;
    if (e) {
        std::lock_guard<std::mutex> lk(g_click_mutex);
        auto it = g_click_counts.find(e);
        if (it != g_click_counts.end()) {
            count = it->second;
            it->second = 0;
        }
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_Tray_Update(lua_State* /*L*/) {
    SDL_UpdateTrays();
    return 0;
}

// ==================== luaopen_Light_Tray ====================

extern "C" LIGHT_API int luaopen_Light_Tray(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Create",           l_Tray_Create           },
        { "Destroy",          l_Tray_Destroy          },
        { "SetTooltip",       l_Tray_SetTooltip       },
        { "GetMenu",          l_Tray_GetMenu          },
        { "AddButton",        l_Tray_AddButton        },
        { "AddCheckbox",      l_Tray_AddCheckbox      },
        { "AddSeparator",     l_Tray_AddSeparator     },
        { "AddSubmenu",       l_Tray_AddSubmenu       },
        { "SetEntryLabel",    l_Tray_SetEntryLabel    },
        { "GetEntryLabel",    l_Tray_GetEntryLabel    },
        { "SetEntryEnabled",  l_Tray_SetEntryEnabled  },
        { "GetEntryEnabled",  l_Tray_GetEntryEnabled  },
        { "SetEntryChecked",  l_Tray_SetEntryChecked  },
        { "GetEntryChecked",  l_Tray_GetEntryChecked  },
        { "RemoveEntry",      l_Tray_RemoveEntry      },
        { "WasClicked",       l_Tray_WasClicked       },
        { "Update",           l_Tray_Update           },
        { nullptr,            nullptr                 },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
