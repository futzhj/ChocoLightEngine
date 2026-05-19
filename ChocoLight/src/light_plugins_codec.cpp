#include "light.h"
#include "light_utils_core.h"

extern "C" {
#include "tiny_hash.h"
}

#include <cstdint>
#include <string>
#include <vector>

namespace {

static int l_Base64Encode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::vector<char> out(((len + 2) / 3) * 4 + 4, 0);
    int outLen = base64_encode(reinterpret_cast<const uint8_t*>(data), len, out.data());
    lua_pushlstring(L, out.data(), outLen);
    return 1;
}

static int l_Base64Decode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::vector<uint8_t> out((len / 4) * 3 + 4, 0);
    int outLen = base64_decode(data, len, out.data());
    if (outLen < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Base64Decode: invalid input");
        return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(out.data()), outLen);
    return 1;
}

static int l_HexEncode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    bool upper = false;
    if (!lua_isnoneornil(L, 2)) {
        luaL_checktype(L, 2, LUA_TBOOLEAN);
        upper = lua_toboolean(L, 2) != 0;
    }
    std::string out = LT::Utils::HexEncode(reinterpret_cast<const uint8_t*>(data), len, upper);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_HexDecode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::string out;
    std::string err;
    if (!LT::Utils::HexDecode(data, len, out, err)) {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_URLEncode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::string out = LT::Utils::URLEncode(data, len);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_URLDecode(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    std::string out;
    std::string err;
    if (!LT::Utils::URLDecode(data, len, out, err)) {
        lua_pushnil(L);
        lua_pushlstring(L, err.data(), err.size());
        return 2;
    }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

}

extern "C" LIGHT_API int luaopen_Light_Plugins_Codec(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"Base64Encode", l_Base64Encode},
        {"Base64Decode", l_Base64Decode},
        {"HexEncode",    l_HexEncode},
        {"HexDecode",    l_HexDecode},
        {"URLEncode",    l_URLEncode},
        {"URLDecode",    l_URLDecode},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPluginsSubmodule(L, "Codec", funcs);
    return 1;
}
