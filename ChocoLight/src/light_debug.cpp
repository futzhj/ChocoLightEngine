/**
 * @file light_debug.cpp
 * @brief Light.Debug 模块 — 调试工具
 * @note 还原自 Light.dll 地址范围 0x1800A5540-0x1800A5760
 */

#include "light.h"

// ==================== PrintError ====================
// 原始地址: sub_1800A54F0
// Lua: Light.Debug.PrintError() — 打印栈顶错误信息到 stderr

static int l_PrintError(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    if (msg) {
        std::cerr << msg << std::endl;
    }
    lua_settop(L, -2);
    return 0;
}

// ==================== PrintStack ====================
// 原始地址: sub_1800A5540
// Lua: Light.Debug.PrintStack() — 打印完整的 Lua 栈内容

static int l_PrintStack(lua_State* L) {
    int top = lua_gettop(L);
    for (int i = 1; i <= top; ++i) {
        int t = lua_type(L, i);
        switch (t) {
            case LUA_TSTRING:
                std::cout << "\"" << lua_tostring(L, i) << "\"";
                break;
            case LUA_TBOOLEAN:
                std::cout << (lua_toboolean(L, i) ? "true" : "false");
                break;
            case LUA_TNUMBER:
                std::cout << lua_tonumber(L, i);
                break;
            default:
                std::cout << lua_typename(L, t) << ":" << lua_topointer(L, i);
                break;
        }
        if (i < top) std::cout << "\t";
    }
    std::cout << std::endl;
    return 0;
}

// ==================== PrintTraceStack ====================
// 原始地址: sub_1800A5760
// Lua: Light.Debug.PrintTraceStack() — 打印 traceback 信息

static int l_PrintTraceStack(lua_State* L) {
    const char* msg = lua_tostring(L, -1);
    int top = lua_gettop(L);
    
    if (msg) {
        // 尝试获取 debug.traceback
        lua_getfield(L, LUA_REGISTRYINDEX, "debug");
        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_getfield(L, -1, "traceback");
            if (lua_type(L, -1) == LUA_TFUNCTION) {
                lua_pushstring(L, "");
                lua_pushinteger(L, 2);
                lua_call(L, 2, LUA_MULTRET);
                
                const char* tb = lua_tostring(L, -1);
                // 输出错误消息 + traceback
                std::cerr << msg << tb << std::endl;
            }
        }
    }
    
    // 恢复栈
    int newTop = lua_gettop(L);
    lua_settop(L, top - newTop - 2);
    return 0;
}

// ==================== luaopen_Light_Debug ====================
// 原始地址: 0x1800A5970 (导出)

static const luaL_Reg debug_funcs[] = {
    {"PrintError",      l_PrintError},
    {"PrintStack",      l_PrintStack},
    {"PrintTraceStack", l_PrintTraceStack},
    {NULL, NULL}
};

int luaopen_Light_Debug(lua_State* L) {
    LT::RegisterModule(L, "Debug", debug_funcs);
    return 1;
}
