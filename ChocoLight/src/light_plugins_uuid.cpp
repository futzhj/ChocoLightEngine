#include "light.h"
#include "light_utils_core.h"

#include <chrono>
#include <cstdint>
#include <random>
#include <string>

namespace {

static void FillRandom(uint8_t* out, size_t len) {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(
        rd() ^ static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count())
    );
    std::uniform_int_distribution<int> dist(0, 255);
    for (size_t i = 0; i < len; ++i) {
        out[i] = static_cast<uint8_t>(dist(gen));
    }
}

static int l_V4(lua_State* L) {
    uint8_t raw[16];
    FillRandom(raw, 16);
    raw[6] = static_cast<uint8_t>((raw[6] & 0x0F) | 0x40);
    raw[8] = static_cast<uint8_t>((raw[8] & 0x3F) | 0x80);
    std::string out = LT::Utils::FormatUUID(raw);
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

static int l_IsValid(lua_State* L) {
    size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    uint8_t raw[16];
    lua_pushboolean(L, LT::Utils::ParseUUID(text, len, raw) ? 1 : 0);
    return 1;
}

static int l_Parse(lua_State* L) {
    size_t len = 0;
    const char* text = luaL_checklstring(L, 1, &len);
    uint8_t raw[16];
    if (!LT::Utils::ParseUUID(text, len, raw)) {
        lua_pushnil(L);
        lua_pushstring(L, "Parse: invalid UUID string");
        return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(raw), 16);
    return 1;
}

static int l_Format(lua_State* L) {
    size_t len = 0;
    const char* raw = luaL_checklstring(L, 1, &len);
    if (len != 16) {
        lua_pushnil(L);
        lua_pushstring(L, "Format: raw UUID must be exactly 16 bytes");
        return 2;
    }
    std::string out = LT::Utils::FormatUUID(reinterpret_cast<const uint8_t*>(raw));
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}

}

extern "C" LIGHT_API int luaopen_Light_Plugins_UUID(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"V4",      l_V4},
        {"IsValid", l_IsValid},
        {"Parse",   l_Parse},
        {"Format",  l_Format},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPluginsSubmodule(L, "UUID", funcs);
    return 1;
}

extern "C" LIGHT_API int luaopen_uuid(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"v4",       l_V4},
        {"is_valid", l_IsValid},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}
