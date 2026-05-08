/**
 * @file light_process.cpp
 * @brief Light.Process 模块 - 子进程创建/通信/等待 (基于 SDL_Process)
 *
 * Lua API:
 *   Light.Process.Run(args, capture_stdout) -> handle (light userdata), err
 *     - args: 字符串数组, args[1]=可执行文件, args[2..N]=参数
 *     - capture_stdout: 布尔, true 时管道捕获 stdout/stderr 供 Read 读取
 *   Light.Process.Read(handle)         -> stdout_str, exitcode, err
 *     - 阻塞读取全部 stdout, 返回后进程已结束
 *   Light.Process.Wait(handle, block)  -> exitcode|nil, finished_bool, err
 *     - block=true: 阻塞等待结束; block=false: 立即返回是否结束
 *   Light.Process.Kill(handle, force)  -> ok, err
 *   Light.Process.Destroy(handle)      -> ok, err
 *     - 释放进程资源, 之后 handle 不可再用
 *
 * 平台覆盖: Win/Mac/Linux 完整, iOS/Android/Web 受限或不支持 (按 SDL3 文档)
 *
 * 安全提示: args 直接传给 SDL3, 不做 shell 解析, 不会触发命令注入; 但调用方仍需校验来自不可信源的参数.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <vector>
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== 工具: Lua 字符串数组 -> argv ====================

// 从栈上 idx 处的 table 读取字符串到 owners (持有内存) 与 argv (NULL 结尾).
// 返回是否成功; 失败时已 lua_pushnil + 错误信息留给调用方处理.
static bool BuildArgv(lua_State* L, int idx,
                      std::vector<std::string>& owners,
                      std::vector<const char*>& argv) {
    if (lua_type(L, idx) != LUA_TTABLE) {
        lua_pushnil(L);
        lua_pushstring(L, "args must be a table of strings");
        return false;
    }
    int n = (int)lua_objlen(L, idx);
    if (n <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "args is empty");
        return false;
    }
    owners.reserve(n);
    argv.reserve(n + 1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, idx, i);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            lua_pushnil(L);
            lua_pushfstring(L, "args[%d] is not a string", i);
            return false;
        }
        owners.emplace_back(lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    for (auto& s : owners) argv.push_back(s.c_str());
    argv.push_back(nullptr);
    return true;
}

// ==================== Light.Process.Run ====================

static int l_Process_Run(lua_State* L) {
    std::vector<std::string> owners;
    std::vector<const char*> argv;
    if (!BuildArgv(L, 1, owners, argv)) {
        return 2;  // BuildArgv 已 push nil + err
    }
    bool capture = lua_isnoneornil(L, 2) ? false : (lua_toboolean(L, 2) != 0);

    SDL_Process* p = SDL_CreateProcess(argv.data(), capture);
    if (!p) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, p);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Process.Read ====================

static int l_Process_Read(lua_State* L) {
    SDL_Process* p = (SDL_Process*)lua_touserdata(L, 1);
    if (!p) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushstring(L, "invalid process handle");
        return 3;
    }
    size_t sz = 0;
    int exitcode = -1;
    void* data = SDL_ReadProcess(p, &sz, &exitcode);
    if (!data) {
        lua_pushnil(L); lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 3;
    }
    lua_pushlstring(L, (const char*)data, sz);
    SDL_free(data);
    lua_pushinteger(L, exitcode);
    lua_pushnil(L);
    return 3;
}

// ==================== Light.Process.Wait ====================

static int l_Process_Wait(lua_State* L) {
    SDL_Process* p = (SDL_Process*)lua_touserdata(L, 1);
    if (!p) {
        lua_pushnil(L); lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid process handle");
        return 3;
    }
    bool block = lua_isnoneornil(L, 2) ? true : (lua_toboolean(L, 2) != 0);
    int exitcode = -1;
    bool finished = SDL_WaitProcess(p, block, &exitcode);
    if (finished) {
        lua_pushinteger(L, exitcode);
        lua_pushboolean(L, 1);
    } else {
        lua_pushnil(L);
        lua_pushboolean(L, 0);
    }
    lua_pushnil(L);
    return 3;
}

// ==================== Light.Process.Kill ====================

static int l_Process_Kill(lua_State* L) {
    SDL_Process* p = (SDL_Process*)lua_touserdata(L, 1);
    if (!p) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid process handle");
        return 2;
    }
    bool force = lua_isnoneornil(L, 2) ? false : (lua_toboolean(L, 2) != 0);
    bool ok = SDL_KillProcess(p, force);
    lua_pushboolean(L, ok);
    if (!ok) {
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Process.Destroy ====================

static int l_Process_Destroy(lua_State* L) {
    SDL_Process* p = (SDL_Process*)lua_touserdata(L, 1);
    if (!p) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid process handle");
        return 2;
    }
    SDL_DestroyProcess(p);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Process ====================

extern "C" LIGHT_API int luaopen_Light_Process(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "Run",     l_Process_Run     },
        { "Read",    l_Process_Read    },
        { "Wait",    l_Process_Wait    },
        { "Kill",    l_Process_Kill    },
        { "Destroy", l_Process_Destroy },
        { nullptr,   nullptr           },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
