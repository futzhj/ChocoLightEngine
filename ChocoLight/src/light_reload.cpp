/**
 * @file light_reload.cpp
 * @brief Phase G.0 — Light.Reload 模块: Lua 脚本语义级热重载
 *
 * 工作原理:
 *   - Module(name): 清 package.loaded[name] + require(name), 返新模块
 *   - File(path):   luaL_loadfile + pcall, 返 chunk 返回值
 *   - Preserve(key, factory): 状态保留 (Lua registry, 按 key 索引)
 *   - WatchModule(name): 集成 Light.HotReload, 自动 reload module
 *   - RestartScript(path?): 请 lumen-master 主循环退出后重新 dofile 入口脚本
 *   - SetErrorHandler / GetLastError / Stats / ResetState / Clear
 *
 * 错误恢复:
 *   - reload 失败时, package.loaded 保持老版本 (Lua 内置 require 行为, 失败不缓存)
 *   - 失败信息记录到 s_lastError*, 调用 SetErrorHandler 注册的 Lua callback
 *   - GetLastError 查询最近一次失败
 *
 * 跨平台:
 *   - 文件 mtime 走现有 Light.HotReload (sys/stat 已跨平台)
 *   - RestartScript 走 lumen-master 暴露的 lumen_RequestRestart (Windows-only, mobile/web 桩)
 */

#include "light.h"
#include <cstring>
#include <cstdint>
#include <ctime>

// Phase G.0 — lumen-master 暴露的 RestartScript 接口
//   light.exe 内符号 (LUMEN_EXPORT). Light.dll 通过运行时 GetProcAddress 反查,
//   避免静态链接依赖 (Light.dll 由 light.exe 用 LoadLibrary 加载).
//   mobile/web 平台无 light.exe 容器, 函数指针保持 nullptr 即可 (调用静默 no-op).
#if defined(_WIN32) && !defined(__EMSCRIPTEN__) && !defined(__ANDROID__)
#include <windows.h>
typedef void (*lumen_RequestRestart_fn)(const char*);
typedef bool (*lumen_IsRestartPending_fn)(void);

static lumen_RequestRestart_fn   p_lumen_RequestRestart   = nullptr;
static lumen_IsRestartPending_fn p_lumen_IsRestartPending = nullptr;
static bool s_lumenSymbolsResolved = false;

// 懒解析: 第一次调用时查 light.exe 的导出符号
static void ResolveLumenSymbols() {
    if (s_lumenSymbolsResolved) return;
    s_lumenSymbolsResolved = true;
    HMODULE hExe = GetModuleHandleA(nullptr);   // light.exe (current process main module)
    if (!hExe) return;
    p_lumen_RequestRestart   = (lumen_RequestRestart_fn)  GetProcAddress(hExe, "lumen_RequestRestart");
    p_lumen_IsRestartPending = (lumen_IsRestartPending_fn)GetProcAddress(hExe, "lumen_IsRestartPending");
}

static inline void lumen_RequestRestart(const char* path) {
    ResolveLumenSymbols();
    if (p_lumen_RequestRestart) p_lumen_RequestRestart(path);
}
static inline bool lumen_IsRestartPending() {
    ResolveLumenSymbols();
    return p_lumen_IsRestartPending ? p_lumen_IsRestartPending() : false;
}
#else
// 非 Windows / mobile / web: 桩
static inline void lumen_RequestRestart(const char*) {}
static inline bool lumen_IsRestartPending() { return false; }
#endif

namespace {

constexpr int RELOAD_MAX_STATES   = 256;   // Preserve key 上限
constexpr int RELOAD_MAX_WATCHES  = 128;   // WatchModule 上限

// 状态保留项 — 按 key 索引, value 是 Lua registry ref
struct PreservedState {
    bool used;
    char key[128];
    int  stateRef;       // luaL_ref to Lua table; LUA_NOREF=空
};

// 自动 reload 项 — 关联 HotReload watch
struct WatchedModule {
    bool used;
    int  hrWatchId;      // Light.HotReload.Watch 返的 id
    char name[128];      // module name (e.g. "game.player")
    char path[512];      // 解析后的文件路径 (诊断用)
};

// 全局状态 (POD)
static PreservedState s_preserved[RELOAD_MAX_STATES];
static WatchedModule  s_watched[RELOAD_MAX_WATCHES];

static int      s_errorHandlerRef = LUA_NOREF;
static char     s_lastErrorPath[512] = {0};
static char     s_lastErrorMsg[1024] = {0};
static int64_t  s_lastErrorTime = 0;
static bool     s_hasLastError  = false;

static int64_t s_modulesReloaded = 0;
static int64_t s_filesReloaded   = 0;
static int64_t s_errorsTotal     = 0;

// 查找 helper (小步循环, 不抽过度)
static int FindPreservedByKey(const char* key) {
    for (int i = 0; i < RELOAD_MAX_STATES; ++i) {
        if (s_preserved[i].used && std::strcmp(s_preserved[i].key, key) == 0)
            return i;
    }
    return -1;
}

static int FindFreePreservedSlot() {
    for (int i = 0; i < RELOAD_MAX_STATES; ++i) {
        if (!s_preserved[i].used) return i;
    }
    return -1;
}

static int FindFreeWatchedSlot() {
    for (int i = 0; i < RELOAD_MAX_WATCHES; ++i) {
        if (!s_watched[i].used) return i;
    }
    return -1;
}

// 记录错误 + log + 调 Lua error handler
//   path: 出错的模块名 / 文件路径
//   err:  错误消息 (Lua error string)
static void RecordError(lua_State* L, const char* path, const char* err) {
    std::strncpy(s_lastErrorPath, path ? path : "", sizeof(s_lastErrorPath) - 1);
    s_lastErrorPath[sizeof(s_lastErrorPath) - 1] = '\0';
    std::strncpy(s_lastErrorMsg, err ? err : "unknown", sizeof(s_lastErrorMsg) - 1);
    s_lastErrorMsg[sizeof(s_lastErrorMsg) - 1] = '\0';
    s_lastErrorTime = (int64_t)std::time(nullptr);
    s_hasLastError = true;
    ++s_errorsTotal;
    CC::Log(CC::LOG_WARN, "Reload error: %s -- %s", s_lastErrorPath, s_lastErrorMsg);

    // 触发 Lua error handler (如已注册)
    if (s_errorHandlerRef != LUA_NOREF && L) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
        if (lua_isfunction(L, -1)) {
            lua_pushstring(L, s_lastErrorPath);
            lua_pushstring(L, s_lastErrorMsg);
            if (lua_pcall(L, 2, 0, 0) != 0) {
                const char* he = lua_tostring(L, -1);
                CC::Log(CC::LOG_WARN, "Reload error handler error: %s", he ? he : "?");
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);   // 非 function (ref 失效), 丢弃
        }
    }
}

} // namespace

// ============================================================
// Lua 绑定
// ============================================================

// Reload.Module(name) -> module | nil, err
//   清 package.loaded[name] + require(name)
//   成功返新模块, 失败返 nil + err string
static int l_Reload_Module(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // 1) 清 package.loaded[name]
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushliteral(L, "package not a table");
        return 2;
    }
    lua_getfield(L, -1, "loaded");
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    lua_pop(L, 2);   // pop loaded + package

    // 2) require(name)
    lua_getglobal(L, "require");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushliteral(L, "require not a function");
        return 2;
    }
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 1, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        RecordError(L, name, err);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "require failed");
        return 2;
    }
    ++s_modulesReloaded;
    return 1;   // 返新模块
}

// Reload.File(path) -> any... | nil, err
//   luaL_loadfile + pcall, 返 chunk 全部返回值
//   失败返 nil + err string
static int l_Reload_File(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    const int top = lua_gettop(L);

    if (luaL_loadfile(L, path) != 0) {
        const char* err = lua_tostring(L, -1);
        RecordError(L, path, err);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "load failed");
        return 2;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        RecordError(L, path, err);
        lua_pop(L, 1);
        lua_pushnil(L);
        lua_pushstring(L, err ? err : "exec failed");
        return 2;
    }
    ++s_filesReloaded;
    return lua_gettop(L) - top;   // chunk 所有返回值
}

// Reload.Preserve(key, factory) -> any
//   第一次: 调 factory() 创建 state, 存到 registry
//   之后:   返同一 state (factory 不再调)
//   factory 错误 → luaL_error 抛
static int l_Reload_Preserve(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    // 1) 查已存在
    int slot = FindPreservedByKey(key);
    if (slot >= 0 && s_preserved[slot].stateRef != LUA_NOREF) {
        lua_rawgeti(L, LUA_REGISTRYINDEX, s_preserved[slot].stateRef);
        return 1;
    }

    // 2) 第一次: 调 factory()
    lua_pushvalue(L, 2);
    if (lua_pcall(L, 0, 1, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        return luaL_error(L, "Preserve('%s') factory error: %s", key, err ? err : "?");
    }
    // 3) 保存到 registry (dup 一份留栈顶返回, ref 内部 pop 一份)
    lua_pushvalue(L, -1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);

    int newSlot = (slot >= 0) ? slot : FindFreePreservedSlot();
    if (newSlot < 0) {
        luaL_unref(L, LUA_REGISTRYINDEX, ref);
        return luaL_error(L, "Preserve: max states (%d) reached", RELOAD_MAX_STATES);
    }
    s_preserved[newSlot].used = true;
    std::strncpy(s_preserved[newSlot].key, key, sizeof(s_preserved[newSlot].key) - 1);
    s_preserved[newSlot].key[sizeof(s_preserved[newSlot].key) - 1] = '\0';
    s_preserved[newSlot].stateRef = ref;
    return 1;   // 返刚创建的 state (栈顶)
}

// Reload.ResetState(key) -> bool
//   删除 key 对应的 preserved state, 下次 Preserve 重新调 factory
static int l_Reload_ResetState(lua_State* L) {
    const char* key = luaL_checkstring(L, 1);
    int slot = FindPreservedByKey(key);
    if (slot < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (s_preserved[slot].stateRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_preserved[slot].stateRef);
    }
    s_preserved[slot].used = false;
    s_preserved[slot].stateRef = LUA_NOREF;
    s_preserved[slot].key[0] = '\0';
    lua_pushboolean(L, 1);
    return 1;
}

// WatchModule 内部 callback (用 closure 捕 module name, HotReload mtime 变时触发)
static int WatchModuleCallback(lua_State* L) {
    // upvalue[1] = module name
    const char* name = lua_tostring(L, lua_upvalueindex(1));
    if (!name) return 0;

    // 调 Light.Reload.Module(name)
    lua_getglobal(L, "Light");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    lua_getfield(L, -1, "Reload");
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return 0; }
    lua_getfield(L, -1, "Module");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 3); return 0; }
    lua_pushstring(L, name);
    if (lua_pcall(L, 1, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "WatchModule auto-reload error: %s -- %s", name, err ? err : "?");
        lua_pop(L, 1);
    }
    lua_pop(L, 2);   // pop Reload + Light
    return 0;
}

// Reload.WatchModule(name) -> bool
//   集成 Light.HotReload: 解析 name → path, 注册 watcher + 内部 callback
//   找不到模块 / 注册失败 → 返 false
static int l_Reload_WatchModule(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);

    // 1) 解析 path: package.searchpath(name, package.path)
    lua_getglobal(L, "package");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); lua_pushboolean(L, 0); return 1; }
    lua_getfield(L, -1, "searchpath");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); lua_pushboolean(L, 0); return 1; }
    lua_pushstring(L, name);
    lua_getfield(L, -3, "path");
    if (lua_pcall(L, 2, 1, 0) != 0) {
        lua_pop(L, 2);   // pop err + package
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!lua_isstring(L, -1)) {
        lua_pop(L, 2);   // pop result + package
        lua_pushboolean(L, 0);
        return 1;
    }
    const char* path = lua_tostring(L, -1);
    char pathCopy[512];
    std::strncpy(pathCopy, path, sizeof(pathCopy) - 1);
    pathCopy[sizeof(pathCopy) - 1] = '\0';
    lua_pop(L, 2);   // pop path + package

    // 2) 找空闲 slot
    int slot = FindFreeWatchedSlot();
    if (slot < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 3) 调 Light.HotReload.Watch(path, callback_closure)
    lua_getglobal(L, "Light");
    lua_getfield(L, -1, "HotReload");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 2);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_getfield(L, -1, "Watch");
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 3);
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushstring(L, pathCopy);
    lua_pushstring(L, name);
    lua_pushcclosure(L, WatchModuleCallback, 1);
    if (lua_pcall(L, 2, 1, 0) != 0) {
        lua_pop(L, 3);   // pop err + HotReload + Light
        lua_pushboolean(L, 0);
        return 1;
    }
    if (!lua_isnumber(L, -1)) {
        lua_pop(L, 3);
        lua_pushboolean(L, 0);
        return 1;
    }
    const int hrId = (int)lua_tointeger(L, -1);
    lua_pop(L, 3);   // pop id + HotReload + Light

    s_watched[slot].used = true;
    s_watched[slot].hrWatchId = hrId;
    std::strncpy(s_watched[slot].name, name, sizeof(s_watched[slot].name) - 1);
    s_watched[slot].name[sizeof(s_watched[slot].name) - 1] = '\0';
    std::strncpy(s_watched[slot].path, pathCopy, sizeof(s_watched[slot].path) - 1);
    s_watched[slot].path[sizeof(s_watched[slot].path) - 1] = '\0';

    lua_pushboolean(L, 1);
    return 1;
}

// Reload.UnwatchModule(name) -> bool
//   反向操作: 找到对应 slot + Light.HotReload.Unwatch(id) + 释放 slot
static int l_Reload_UnwatchModule(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    for (int i = 0; i < RELOAD_MAX_WATCHES; ++i) {
        if (!s_watched[i].used) continue;
        if (std::strcmp(s_watched[i].name, name) != 0) continue;
        // 调 Light.HotReload.Unwatch(id)
        lua_getglobal(L, "Light");
        lua_getfield(L, -1, "HotReload");
        lua_getfield(L, -1, "Unwatch");
        lua_pushinteger(L, s_watched[i].hrWatchId);
        if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
        lua_pop(L, 2);   // pop HotReload + Light

        s_watched[i].used = false;
        s_watched[i].hrWatchId = 0;
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushboolean(L, 0);
    return 1;
}

// Reload.SetErrorHandler(fn|nil)
//   注册全局 reload 失败 hook; 传 nil/none 清除
static int l_Reload_SetErrorHandler(lua_State* L) {
    const int t = lua_type(L, 1);
    if (t != LUA_TFUNCTION && t != LUA_TNIL && t != LUA_TNONE) {
        return luaL_error(L, "SetErrorHandler: expected function or nil");
    }
    if (s_errorHandlerRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_errorHandlerRef);
        s_errorHandlerRef = LUA_NOREF;
    }
    if (t == LUA_TFUNCTION) {
        lua_pushvalue(L, 1);
        s_errorHandlerRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return 0;
}

// Reload.GetLastError() -> {path, msg, time} | nil
static int l_Reload_GetLastError(lua_State* L) {
    if (!s_hasLastError) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, 0, 3);
    lua_pushstring(L, s_lastErrorPath);
    lua_setfield(L, -2, "path");
    lua_pushstring(L, s_lastErrorMsg);
    lua_setfield(L, -2, "msg");
    lua_pushinteger(L, (lua_Integer)s_lastErrorTime);
    lua_setfield(L, -2, "time");
    return 1;
}

// Reload.Stats() -> {modules_reloaded, files_reloaded, errors, preserved_count, watched_count}
static int l_Reload_Stats(lua_State* L) {
    int pcount = 0, wcount = 0;
    for (int i = 0; i < RELOAD_MAX_STATES; ++i)  if (s_preserved[i].used) ++pcount;
    for (int i = 0; i < RELOAD_MAX_WATCHES; ++i) if (s_watched[i].used)   ++wcount;
    lua_createtable(L, 0, 5);
    lua_pushinteger(L, (lua_Integer)s_modulesReloaded); lua_setfield(L, -2, "modules_reloaded");
    lua_pushinteger(L, (lua_Integer)s_filesReloaded);   lua_setfield(L, -2, "files_reloaded");
    lua_pushinteger(L, (lua_Integer)s_errorsTotal);     lua_setfield(L, -2, "errors");
    lua_pushinteger(L, (lua_Integer)pcount);            lua_setfield(L, -2, "preserved_count");
    lua_pushinteger(L, (lua_Integer)wcount);            lua_setfield(L, -2, "watched_count");
    return 1;
}

// Reload.Clear()
//   清所有 preserved state + 取消所有 watch + 清错误信息 (不重置 stats 计数器)
static int l_Reload_Clear(lua_State* L) {
    for (int i = 0; i < RELOAD_MAX_STATES; ++i) {
        if (s_preserved[i].used && s_preserved[i].stateRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_preserved[i].stateRef);
        }
        s_preserved[i].used = false;
        s_preserved[i].stateRef = LUA_NOREF;
        s_preserved[i].key[0] = '\0';
    }
    for (int i = 0; i < RELOAD_MAX_WATCHES; ++i) {
        if (s_watched[i].used) {
            lua_getglobal(L, "Light");
            lua_getfield(L, -1, "HotReload");
            lua_getfield(L, -1, "Unwatch");
            lua_pushinteger(L, s_watched[i].hrWatchId);
            if (lua_pcall(L, 1, 0, 0) != 0) lua_pop(L, 1);
            lua_pop(L, 2);
        }
        s_watched[i].used = false;
        s_watched[i].hrWatchId = 0;
    }
    s_hasLastError = false;
    s_lastErrorPath[0] = '\0';
    s_lastErrorMsg[0]  = '\0';
    return 0;
}

// Reload.RestartScript(path?) -> bool
//   请 lumen-master 主循环退出后重新 dofile 入口脚本
//   path 缺省 = arg[0] (启动脚本)
//   桌面平台生效; mobile/web 桩返 false
//   用户必须在调用后让 UI.Loop() 退出 (例 Game:Close()), 否则 restart 不触发
static int l_Reload_RestartScript(lua_State* L) {
    // 参数语义区分:
    //   - 显式传字符串: 必须非空, 空串直接拒绝 (避免误触发 lumen 重启)
    //   - 未传 / nil: fallback 到 arg[0] (启动脚本)
    //   - 其它类型: 报错
    const int t1 = lua_type(L, 1);
    const char* path = nullptr;
    char pathBuf[1024] = {0};

    if (t1 == LUA_TSTRING) {
        path = lua_tostring(L, 1);
        if (!path || !*path) {
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "RestartScript: explicit empty path rejected");
            return 2;
        }
    } else if (t1 == LUA_TNONE || t1 == LUA_TNIL) {
        // fallback: arg[0]
        lua_getglobal(L, "arg");
        if (lua_istable(L, -1)) {
            lua_rawgeti(L, -1, 0);
            if (lua_isstring(L, -1)) {
                std::strncpy(pathBuf, lua_tostring(L, -1), sizeof(pathBuf) - 1);
                pathBuf[sizeof(pathBuf) - 1] = '\0';
                path = pathBuf;
            }
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
        if (!path || !*path) {
            lua_pushboolean(L, 0);
            lua_pushliteral(L, "RestartScript: arg[0] not set, no default target");
            return 2;
        }
    } else {
        return luaL_error(L, "RestartScript: expected string or nil, got %s", lua_typename(L, t1));
    }
    lumen_RequestRestart(path);
    lua_pushboolean(L, 1);
    return 1;
}

// Reload.IsRestartPending() -> bool
//   查询 lumen 是否已收到 restart 请求 (用户主循环可据此 break)
static int l_Reload_IsRestartPending(lua_State* L) {
    lua_pushboolean(L, lumen_IsRestartPending() ? 1 : 0);
    return 1;
}

// ============================================================
// luaopen_Light_Reload
// ============================================================

int luaopen_Light_Reload(lua_State* L) {
    static const luaL_Reg reload_funcs[] = {
        {"Module",            l_Reload_Module},
        {"File",              l_Reload_File},
        {"Preserve",          l_Reload_Preserve},
        {"ResetState",        l_Reload_ResetState},
        {"WatchModule",       l_Reload_WatchModule},
        {"UnwatchModule",     l_Reload_UnwatchModule},
        {"SetErrorHandler",   l_Reload_SetErrorHandler},
        {"GetLastError",      l_Reload_GetLastError},
        {"Stats",             l_Reload_Stats},
        {"Clear",             l_Reload_Clear},
        {"RestartScript",     l_Reload_RestartScript},
        {"IsRestartPending",  l_Reload_IsRestartPending},
        {nullptr, nullptr}
    };
    LT::RegisterModule(L, "Reload", reload_funcs);
    return 1;
}
