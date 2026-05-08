/**
 * @file light_guid.cpp
 * @brief Light.Guid 模块 - SDL_GUID 16 字节标识符与字符串互转 (基于 SDL_guid)
 *
 * Lua API:
 *   Light.Guid.ToString(raw_bytes)   -> "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX", err
 *     - raw_bytes: 长度必须正好 16 的 Lua 字符串 (二进制安全)
 *     - 输出: 32 个大写/小写十六进制字符 (SDL3 默认输出小写)
 *
 *   Light.Guid.FromString(hex_string) -> raw_bytes_16, err
 *     - hex_string: 32 个十六进制字符 (大小写不敏感, 允许不含分隔符)
 *     - 输出: 长度正好 16 的 Lua 字符串 (二进制)
 *
 * 用途: SDL_Joystick/SDL_Gamepad 的设备 GUID 稳定标识, 跨进程/跨启动可比对;
 *       也可作为通用 UUID 格式转换 (16 字节 ↔ 32 hex 字符串).
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.Guid.ToString ====================

static int l_Guid_ToString(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    if (len != sizeof(SDL_GUID)) {
        lua_pushnil(L);
        lua_pushfstring(L, "raw bytes length must be %d, got %d",
                        (int)sizeof(SDL_GUID), (int)len);
        return 2;
    }
    SDL_GUID g;
    SDL_memcpy(&g, raw, sizeof(SDL_GUID));
    // SDL3 输出 32 字符 + NUL, 缓冲至少 33
    char buf[64] = {0};
    SDL_GUIDToString(g, buf, (int)sizeof(buf));
    lua_pushstring(L, buf);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Guid.FromString ====================

static int l_Guid_FromString(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    SDL_GUID g = SDL_StringToGUID(s);
    // SDL3 没有显式失败返回; 若输入非法, 产出 0000... GUID
    // 对 Lua 调用方而言, 返回 16 字节即可; 若需校验可自己对比全零
    lua_pushlstring(L, (const char*)&g, sizeof(SDL_GUID));
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Guid ====================

extern "C" LIGHT_API int luaopen_Light_Guid(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "ToString",   l_Guid_ToString   },
        { "FromString", l_Guid_FromString },
        { nullptr,      nullptr           },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
