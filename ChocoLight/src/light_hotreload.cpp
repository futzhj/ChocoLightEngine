/**
 * @file light_hotreload.cpp
 * @brief Light.HotReload 模块 — 资源热重载 (基于 stat mtime 的轮询)
 *
 * 工作原理:
 *   - 每个监视项记录路径 + 上次 mtime + Lua 回调引用
 *   - Check(dt) 累计时间到 interval 后, 扫描所有项的 mtime
 *   - mtime 变化则触发回调, 回调负责重新加载资源
 *
 * 跨平台:
 *   使用 <sys/stat.h> 的 stat() 函数, Windows/Linux/macOS/Android/iOS 都可用
 *
 * Lua API:
 *   local id = Light.HotReload.Watch(path, callback)
 *     callback(path) — 文件变更时调用
 *   Light.HotReload.Unwatch(id)
 *   Light.HotReload.SetInterval(seconds)  -- 检查间隔, 默认 0.5
 *   Light.HotReload.Check(dt)             -- 主循环每帧调用
 *   Light.HotReload.CheckNow()            -- 立即检查一次
 *   Light.HotReload.List()                -- 返回 { {id, path, mtime}, ... }
 *   Light.HotReload.Clear()               -- 清空所有监视
 */

#include "light.h"
#include <sys/stat.h>
#include <cstring>
#include <cstdint>

#ifdef _WIN32
  // Windows MSVC: stat -> _stat64, 用 _stati64 处理大文件
  #define HR_STAT _stat64
  typedef struct _stat64 hr_stat_t;
#else
  #define HR_STAT stat
  typedef struct stat hr_stat_t;
#endif

namespace {

// 监视项最大数量 (固定数组避免动态分配)
constexpr int HR_MAX_WATCHES = 256;

struct WatchItem {
    bool       used;
    int        id;          // 唯一 ID
    char       path[512];   // 监视路径
    int64_t    mtime;       // 上次 mtime (秒)
    int        cbRef;       // Lua callback 引用 (LUA_REGISTRYINDEX)
};

// 全局状态 (POD, 避免 Android .so 全局构造问题)
static WatchItem s_watches[HR_MAX_WATCHES];
static int       s_nextId    = 1;
static double    s_interval  = 0.5;  // 检查间隔 (秒)
static double    s_elapsed   = 0.0;  // 累计时间

// 获取文件 mtime, 失败返回 -1
static int64_t GetFileMtime(const char* path) {
    hr_stat_t st;
    if (HR_STAT(path, &st) != 0) return -1;
    return (int64_t)st.st_mtime;
}

// 查找空闲槽位, 失败返回 -1
static int FindFreeSlot() {
    for (int i = 0; i < HR_MAX_WATCHES; i++) {
        if (!s_watches[i].used) return i;
    }
    return -1;
}

// 按 ID 查找槽位, 失败返回 -1
static int FindSlotById(int id) {
    for (int i = 0; i < HR_MAX_WATCHES; i++) {
        if (s_watches[i].used && s_watches[i].id == id) return i;
    }
    return -1;
}

// 触发单个监视项的回调
static void TriggerCallback(lua_State* L, WatchItem& item) {
    if (item.cbRef == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, item.cbRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushstring(L, item.path);
    if (lua_pcall(L, 1, 0, 0)) {
        const char* err = lua_tostring(L, -1);
        if (err) CC::Log(CC::LOG_ERROR, "HotReload callback error: %s", err);
        lua_pop(L, 1);
    }
}

// 扫描全部监视项, 触发 mtime 变更的回调
static int ScanAllWatches(lua_State* L) {
    int triggered = 0;
    for (int i = 0; i < HR_MAX_WATCHES; i++) {
        if (!s_watches[i].used) continue;
        int64_t cur = GetFileMtime(s_watches[i].path);
        if (cur > 0 && cur != s_watches[i].mtime) {
            s_watches[i].mtime = cur;
            TriggerCallback(L, s_watches[i]);
            triggered++;
        }
    }
    return triggered;
}

} // namespace

// ==================== Lua 绑定 ====================

// HotReload.Watch(path, callback) -> id (失败返回 nil)
static int l_HotReload_Watch(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    int slot = FindFreeSlot();
    if (slot < 0) {
        CC::Log(CC::LOG_WARN, "HotReload: max watches (%d) reached", HR_MAX_WATCHES);
        lua_pushnil(L);
        return 1;
    }

    int64_t mtime = GetFileMtime(path);
    if (mtime < 0) {
        CC::Log(CC::LOG_WARN, "HotReload: file not found: %s", path);
        lua_pushnil(L);
        return 1;
    }

    // 引用 Lua 回调
    lua_pushvalue(L, 2);
    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);

    WatchItem& item = s_watches[slot];
    item.used  = true;
    item.id    = s_nextId++;
    std::strncpy(item.path, path, sizeof(item.path) - 1);
    item.path[sizeof(item.path) - 1] = '\0';
    item.mtime = mtime;
    item.cbRef = cbRef;

    lua_pushinteger(L, item.id);
    return 1;
}

// HotReload.Unwatch(id) -> bool
static int l_HotReload_Unwatch(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    int slot = FindSlotById(id);
    if (slot < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (s_watches[slot].cbRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_watches[slot].cbRef);
    }
    s_watches[slot].used  = false;
    s_watches[slot].cbRef = LUA_NOREF;
    lua_pushboolean(L, 1);
    return 1;
}

// HotReload.SetInterval(seconds)
static int l_HotReload_SetInterval(lua_State* L) {
    double sec = luaL_checknumber(L, 1);
    if (sec < 0.0) sec = 0.0;
    s_interval = sec;
    return 0;
}

// HotReload.Check(dt) -> 触发的回调数量
static int l_HotReload_Check(lua_State* L) {
    double dt = luaL_optnumber(L, 1, 0.0);
    s_elapsed += dt;
    if (s_elapsed < s_interval) {
        lua_pushinteger(L, 0);
        return 1;
    }
    s_elapsed = 0.0;
    int n = ScanAllWatches(L);
    lua_pushinteger(L, n);
    return 1;
}

// HotReload.CheckNow() -> 触发的回调数量
static int l_HotReload_CheckNow(lua_State* L) {
    s_elapsed = 0.0;
    int n = ScanAllWatches(L);
    lua_pushinteger(L, n);
    return 1;
}

// HotReload.List() -> { {id=N, path=S, mtime=N}, ... }
static int l_HotReload_List(lua_State* L) {
    lua_newtable(L);
    int idx = 1;
    for (int i = 0; i < HR_MAX_WATCHES; i++) {
        if (!s_watches[i].used) continue;
        lua_newtable(L);
        lua_pushinteger(L, s_watches[i].id);
        lua_setfield(L, -2, "id");
        lua_pushstring(L, s_watches[i].path);
        lua_setfield(L, -2, "path");
        lua_pushinteger(L, (lua_Integer)s_watches[i].mtime);
        lua_setfield(L, -2, "mtime");
        lua_rawseti(L, -2, idx++);
    }
    return 1;
}

// HotReload.Clear()
static int l_HotReload_Clear(lua_State* L) {
    for (int i = 0; i < HR_MAX_WATCHES; i++) {
        if (s_watches[i].used && s_watches[i].cbRef != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, s_watches[i].cbRef);
        }
        s_watches[i].used  = false;
        s_watches[i].cbRef = LUA_NOREF;
    }
    s_elapsed = 0.0;
    return 0;
}

// ==================== luaopen_Light_HotReload ====================

int luaopen_Light_HotReload(lua_State* L) {
    static const luaL_Reg hotreload_funcs[] = {
        {"Watch",       l_HotReload_Watch},
        {"Unwatch",     l_HotReload_Unwatch},
        {"SetInterval", l_HotReload_SetInterval},
        {"Check",       l_HotReload_Check},
        {"CheckNow",    l_HotReload_CheckNow},
        {"List",        l_HotReload_List},
        {"Clear",       l_HotReload_Clear},
        {nullptr, nullptr}
    };
    LT::RegisterModule(L, "HotReload", hotreload_funcs);
    return 1;
}
