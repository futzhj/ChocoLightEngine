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
 * Lua API (18 fns):
 *   Light.Tray.Create(tooltip)              -> tray_handle, err
 *   Light.Tray.Destroy(tray)                -> ok, err
 *   Light.Tray.SetTooltip(tray, str)        -> ok, err
 *   Light.Tray.SetIconFromFile(tray, path)  -> ok, err   (Phase I.2: PNG/JPG -> SDL_Surface 桥接)
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
 *   Phase I.3 — Lua callback (与 WasClicked 轮询模型共存):
 *     Light.Tray.SetClickCallback(entry, fn|nil)  -> ok, err
 *       注册主线程 Lua 回调。fn 签名: function(count_int).
 *       传 nil 取消。
 *     Light.Tray.PollCallbacks()                  -> dispatched_count
 *       主线程调用 (通常在 Update 后). 扫描所有已注册回调的 entry,
 *       count>0 则 pcall(fn, count) 并清零 counter. pcall 异常仅警告, 不中断派发.
 *
 * 跨线程安全设计:
 *   SDL_tray native callback 可能在 OS message 线程触发 (特别是 Windows),
 *   在那个上下文直接调 lua_pcall 危险 (lua_State 非线程安全). 本模块的
 *   native trampoline 只同步 ++count (用 mutex), 所有 lua_pcall 都在
 *   PollCallbacks 主线程调用中执行.
 *
 * 生命周期:
 *   Light.Tray.Destroy(tray) 会调用 SDL_DestroyTray, SDL3 内部级联销毁 menu/entry.
 *   销毁后所有 entry/menu lightuserdata 失效, Lua 端不应再使用.
 *   click counts 与 callback ref 在 RemoveEntry 时单条清理; 在 Destroy 前
 *   推荐先 SetClickCallback(entry, nil) 取消所有回调 (避免 Lua ref 泄漏).
 *   最坏情况: 进程生命期内 leak 几个 int + 几个 Lua function ref, 可接受.
 */
#include "light.h"

#include <SDL3/SDL.h>
#include "stb_image.h"

#include <unordered_map>
#include <mutex>
#include <vector>
#include <utility>
#include <cstring>

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

// Phase I.3: 另一张 map 存 Lua callback ref
// 锁顺序约定: 永远先 g_cb_mutex 再 g_click_mutex (实际上本文件不嵌套加锁,仅为安全).
struct CbSlot {
    int        lua_ref;  // luaL_ref 在 LUA_REGISTRYINDEX
    lua_State* L;        // 注册时的 lua_State (主线程)
};
static std::unordered_map<SDL_TrayEntry*, CbSlot> g_callbacks;
static std::mutex g_cb_mutex;

static void SDLCALL TrayClickTrampoline(void* /*userdata*/, SDL_TrayEntry* entry) {
    if (!entry) return;
    // 只计数, 不调 lua. lua_pcall 推迟到 PollCallbacks (主线程).
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

// ==================== Light.Tray.SetIconFromFile (Phase I.2) ====================
//
// stb_image 加载 PNG/JPG/BMP 为 RGBA pixels -> SDL_CreateSurface(RGBA32) ->
// 逐行 memcpy (应对 surface->pitch 可能的对齐填充) -> SDL_SetTrayIcon ->
// 立即 SDL_DestroySurface (SDL3 各平台后端都会复制为 native icon, surface 不需 retain).
//
// 平台限制: dummy backend (Web/Android/iOS) 上 SDL_SetTrayIcon 是 noop, 返回 ok.
static int l_Tray_SetIconFromFile(lua_State* L) {
    SDL_Tray* tray = CheckTray(L, 1);
    if (!tray) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid tray handle"); return 2; }
    const char* path = luaL_checkstring(L, 2);

    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load(path, &w, &h, &ch, /*force RGBA*/ 4);
    if (!pixels || w <= 0 || h <= 0) {
        lua_pushboolean(L, 0);
        lua_pushfstring(L, "failed to load image '%s': %s", path,
                        stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return 2;
    }

    // 创建 SDL_Surface (不拾手 stb 内存, 避免 ownership 混乱)
    SDL_Surface* surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (!surface) {
        stbi_image_free(pixels);
        lua_pushboolean(L, 0);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }

    // 逐行复制 (surface->pitch 可能 != w*4, SDL3 内部可能对齐)
    const int row_bytes = w * 4;
    unsigned char* dst = (unsigned char*)surface->pixels;
    for (int y = 0; y < h; ++y) {
        std::memcpy(dst + y * surface->pitch, pixels + y * row_bytes, (size_t)row_bytes);
    }
    stbi_image_free(pixels);

    SDL_SetTrayIcon(tray, surface);
    SDL_DestroySurface(surface);  // SDL3 已复制为 native icon

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

    // 清理该 entry 对应的 Lua callback ref (Phase I.3)
    int old_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        auto it = g_callbacks.find(e);
        if (it != g_callbacks.end()) {
            old_ref = it->second.lua_ref;
            g_callbacks.erase(it);
        }
    }
    if (old_ref != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, old_ref);
    }

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

// ==================== Phase I.3: Lua Callback ====================

// SetClickCallback(entry, function|nil) -> ok, err
static int l_Tray_SetClickCallback(lua_State* L) {
    SDL_TrayEntry* e = CheckEntry(L, 1);
    if (!e) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid entry handle");
        return 2;
    }
    int arg2_type = lua_type(L, 2);
    if (arg2_type != LUA_TFUNCTION && arg2_type != LUA_TNIL) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "callback must be a function or nil");
        return 2;
    }

    // 取老 ref (该 entry 以前的注册)
    int old_ref = LUA_NOREF;
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        auto it = g_callbacks.find(e);
        if (it != g_callbacks.end()) {
            old_ref = it->second.lua_ref;
        }
    }

    if (arg2_type == LUA_TNIL) {
        // 取消注册
        {
            std::lock_guard<std::mutex> lk(g_cb_mutex);
            g_callbacks.erase(e);
        }
        if (old_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, old_ref);
        }
    } else {
        // 同一 entry 重复注册 -> 先 unref 老的
        if (old_ref != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, old_ref);
        }
        // 压一份 function 副本 (luaL_ref 会 pop) -> ref
        lua_pushvalue(L, 2);
        int new_ref = luaL_ref(L, LUA_REGISTRYINDEX);
        {
            std::lock_guard<std::mutex> lk(g_cb_mutex);
            g_callbacks[e] = CbSlot{ new_ref, L };
        }
    }

    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// PollCallbacks() -> dispatched_count
// 主线程调用: 打包 native click counter 交给 Lua callback 处理.
static int l_Tray_PollCallbacks(lua_State* L) {
    // 1. snapshot callback map
    std::vector<std::pair<SDL_TrayEntry*, CbSlot>> snapshot;
    {
        std::lock_guard<std::mutex> lk(g_cb_mutex);
        snapshot.reserve(g_callbacks.size());
        for (auto& kv : g_callbacks) {
            snapshot.push_back(kv);
        }
    }

    int dispatched = 0;
    for (auto& kv : snapshot) {
        SDL_TrayEntry* e = kv.first;
        const CbSlot& slot = kv.second;

        // 2. 拿并清零 click count
        int count = 0;
        {
            std::lock_guard<std::mutex> lk(g_click_mutex);
            auto it = g_click_counts.find(e);
            if (it != g_click_counts.end() && it->second > 0) {
                count = it->second;
                it->second = 0;
            }
        }
        if (count <= 0) continue;

        // 3. pcall 注册时的 fn(count) (在注册时的 lua_State 上)
        lua_State* CL = slot.L ? slot.L : L;
        lua_rawgeti(CL, LUA_REGISTRYINDEX, slot.lua_ref);
        lua_pushinteger(CL, count);
        if (lua_pcall(CL, 1, 0, 0) != 0) {
            const char* err = lua_tostring(CL, -1);
            SDL_Log("[Light.Tray] callback error: %s", err ? err : "(no message)");
            lua_pop(CL, 1);
        }
        ++dispatched;
    }

    lua_pushinteger(L, dispatched);
    return 1;
}

// ==================== luaopen_Light_Tray ====================

extern "C" LIGHT_API int luaopen_Light_Tray(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Create",           l_Tray_Create           },
        { "Destroy",          l_Tray_Destroy          },
        { "SetTooltip",       l_Tray_SetTooltip       },
        { "SetIconFromFile",  l_Tray_SetIconFromFile  },
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
        { "SetClickCallback", l_Tray_SetClickCallback },
        { "PollCallbacks",    l_Tray_PollCallbacks    },
        { nullptr,            nullptr                 },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
