/**
 * @file light_io.cpp
 * @brief Phase C2: Light.IO 模块 - 异步文件加载 (基于 SDL_AsyncIO)
 *
 * Lua API:
 *   Light.IO.LoadAsync(path, callback)
 *     - path:     UTF-8 文件路径
 *     - callback: function(ok, data, err) end
 *                 ok=true 时 data 为加载的字符串 (二进制安全)
 *                 ok=false 时 err 为错误描述
 *   Light.IO.Poll()
 *     - 主循环每帧调用, 触发已完成请求的 callback
 *     - 返回本次 poll 触发的回调数量
 *
 * 实现策略:
 *   - 单例 SDL_AsyncIOQueue, 启动延迟创建
 *   - 每个请求附带 PendingLoad 结构 (callback Lua ref + path 复本)
 *   - Poll 非阻塞获取 outcome, 从 LUA_REGISTRYINDEX 取回回调
 *   - Web/Android 等平台自动适配 SDL3 实现
 */
#include "light.h"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {

// 全局单例 (lazy-init)
SDL_AsyncIOQueue* g_queue = nullptr;
lua_State*        g_callbackL = nullptr; // 持有 Lua 回调 ref 的 state

struct PendingLoad {
    int   callbackRef;     // LUA_REGISTRYINDEX 索引
    char* path;            // strdup'd, 仅日志用
};

bool EnsureQueue() {
    if (g_queue) return true;
    g_queue = SDL_CreateAsyncIOQueue();
    if (!g_queue) {
        CC::Log(CC::LOG_ERROR, "Light.IO: SDL_CreateAsyncIOQueue failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

void PushErrorCallback(lua_State* L, int callbackRef, const char* err) {
    if (!L || callbackRef == LUA_NOREF) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, callbackRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    lua_pushboolean(L, 0);
    lua_pushnil(L);
    lua_pushstring(L, err ? err : "unknown");
    if (lua_pcall(L, 3, 0, 0) != 0) {
        CC::Log(CC::LOG_WARN, "Light.IO callback error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    luaL_unref(L, LUA_REGISTRYINDEX, callbackRef);
}

} // namespace

// ==================== Light.IO.LoadAsync ====================

static int l_IO_LoadAsync(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);

    if (!EnsureQueue()) {
        // 立即同步失败回调
        lua_pushvalue(L, 2);
        int cb = luaL_ref(L, LUA_REGISTRYINDEX);
        PushErrorCallback(L, cb, "AsyncIOQueue init failed");
        lua_pushboolean(L, 0);
        return 1;
    }

    g_callbackL = L; // 记录 state 用于回调

    // 把 callback (栈顶第 2 个参数) 压栈再 ref, 避免破坏栈
    lua_pushvalue(L, 2);
    int cbRef = luaL_ref(L, LUA_REGISTRYINDEX);

    PendingLoad* pending = new PendingLoad{ cbRef, SDL_strdup(path) };

    // 提交异步加载: SDL3 自动从工作线程读文件, 完成后塞 outcome 到 queue
    if (!SDL_LoadFileAsync(path, g_queue, pending)) {
        const char* err = SDL_GetError();
        CC::Log(CC::LOG_WARN, "Light.IO.LoadAsync(%s) failed: %s", path, err);
        PushErrorCallback(L, cbRef, err);
        SDL_free(pending->path);
        delete pending;
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Light.IO.Poll ====================

static int l_IO_Poll(lua_State* L) {
    if (!g_queue) {
        lua_pushinteger(L, 0);
        return 1;
    }

    int triggered = 0;
    SDL_AsyncIOOutcome outcome;
    while (SDL_GetAsyncIOResult(g_queue, &outcome)) {
        auto* pending = static_cast<PendingLoad*>(outcome.userdata);
        if (!pending) continue;

        const bool ok = (outcome.result == SDL_ASYNCIO_COMPLETE);

        // 推回调
        lua_rawgeti(L, LUA_REGISTRYINDEX, pending->callbackRef);
        if (lua_isfunction(L, -1)) {
            lua_pushboolean(L, ok ? 1 : 0);
            if (ok && outcome.buffer) {
                lua_pushlstring(L, static_cast<const char*>(outcome.buffer),
                                static_cast<size_t>(outcome.bytes_transferred));
            } else {
                lua_pushnil(L);
            }
            lua_pushstring(L, ok ? ""
                                 : (outcome.result == SDL_ASYNCIO_CANCELED ? "canceled"
                                                                            : "load failed"));
            if (lua_pcall(L, 3, 0, 0) != 0) {
                CC::Log(CC::LOG_WARN, "Light.IO callback error: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
            triggered++;
        } else {
            lua_pop(L, 1);
        }

        luaL_unref(L, LUA_REGISTRYINDEX, pending->callbackRef);
        if (outcome.buffer) SDL_free(outcome.buffer);
        if (pending->path)   SDL_free(pending->path);
        delete pending;
    }

    lua_pushinteger(L, triggered);
    return 1;
}

// ==================== luaopen_Light_IO ====================

extern "C" LIGHT_API int luaopen_Light_IO(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "LoadAsync", l_IO_LoadAsync },
        { "Poll",      l_IO_Poll      },
        { nullptr,     nullptr        },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}

// ==================== 关闭清理 (供 platform shutdown 调用) ====================

extern "C" void Light_IO_Shutdown() {
    if (g_queue) {
        // 取消尚未完成的请求并释放
        SDL_AsyncIOOutcome outcome;
        while (SDL_GetAsyncIOResult(g_queue, &outcome)) {
            if (auto* p = static_cast<PendingLoad*>(outcome.userdata)) {
                if (p->path) SDL_free(p->path);
                delete p;
            }
            if (outcome.buffer) SDL_free(outcome.buffer);
        }
        SDL_DestroyAsyncIOQueue(g_queue);
        g_queue = nullptr;
    }
    g_callbackL = nullptr;
}
