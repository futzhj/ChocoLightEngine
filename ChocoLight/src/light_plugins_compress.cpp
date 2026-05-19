#include "light.h"
#include "light_utils_core.h"

#include "miniz.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace {

static const size_t kMaxDecompressedSize = 64u * 1024u * 1024u;

static int ClampLevel(int level) {
    const int minLevel = static_cast<int>(MZ_NO_COMPRESSION);
    const int maxLevel = static_cast<int>(MZ_BEST_COMPRESSION);
    return std::max(minLevel, std::min(maxLevel, level));
}

static uint32_t Adler32Local(const uint8_t* data, size_t len) {
    uint32_t a = 1;
    uint32_t b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

static int l_Compress(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    int level = ClampLevel(static_cast<int>(luaL_optinteger(L, 2, MZ_DEFAULT_COMPRESSION)));

    mz_ulong outLen = mz_compressBound(static_cast<mz_ulong>(len));
    std::vector<unsigned char> out(outLen ? outLen : 1);
    int ret = mz_compress2(out.data(), &outLen, reinterpret_cast<const unsigned char*>(data), static_cast<mz_ulong>(len), level);
    if (ret != MZ_OK) {
        lua_pushnil(L);
        lua_pushstring(L, "Compress: compression failed");
        return 2;
    }
    lua_pushlstring(L, reinterpret_cast<const char*>(out.data()), static_cast<size_t>(outLen));
    return 1;
}

static int l_Decompress(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    size_t cap = len > 0 && len <= (kMaxDecompressedSize / 4) ? len * 4 : kMaxDecompressedSize;
    if (cap < 256) cap = 256;
    if (cap > kMaxDecompressedSize) cap = kMaxDecompressedSize;

    while (cap <= kMaxDecompressedSize) {
        std::vector<unsigned char> out(cap ? cap : 1);
        mz_ulong outLen = static_cast<mz_ulong>(cap);
        int ret = mz_uncompress(out.data(), &outLen, reinterpret_cast<const unsigned char*>(data), static_cast<mz_ulong>(len));
        if (ret == MZ_OK) {
            lua_pushlstring(L, reinterpret_cast<const char*>(out.data()), static_cast<size_t>(outLen));
            return 1;
        }
        if (ret != MZ_BUF_ERROR || cap == kMaxDecompressedSize) {
            lua_pushnil(L);
            lua_pushstring(L, ret == MZ_BUF_ERROR ? "Decompress: output exceeds safety limit" : "Decompress: invalid compressed data");
            return 2;
        }
        cap = std::min(cap * 2, kMaxDecompressedSize);
    }

    lua_pushnil(L);
    lua_pushstring(L, "Decompress: output exceeds safety limit");
    return 2;
}

static int l_CRC32(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint32_t crc = LT::Utils::CRC32(reinterpret_cast<const uint8_t*>(data), len);
    lua_pushnumber(L, static_cast<lua_Number>(crc));
    return 1;
}

static int l_Adler32(lua_State* L) {
    size_t len = 0;
    const char* data = luaL_checklstring(L, 1, &len);
    uint32_t adler = Adler32Local(reinterpret_cast<const uint8_t*>(data), len);
    lua_pushnumber(L, static_cast<lua_Number>(adler));
    return 1;
}

static int l_Version(lua_State* L) {
    lua_pushstring(L, MZ_VERSION);
    return 1;
}

}

extern "C" LIGHT_API int luaopen_Light_Plugins_Compress(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"Compress",   l_Compress},
        {"Decompress", l_Decompress},
        {"Deflate",    l_Compress},
        {"Inflate",    l_Decompress},
        {"CRC32",      l_CRC32},
        {"Adler32",    l_Adler32},
        {"Version",    l_Version},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPluginsSubmodule(L, "Compress", funcs);
    return 1;
}

extern "C" LIGHT_API int luaopen_zlib(lua_State* L) {
    static const luaL_Reg funcs[] = {
        {"compress",   l_Compress},
        {"decompress", l_Decompress},
        {"deflate",    l_Compress},
        {"inflate",    l_Decompress},
        {"crc32",      l_CRC32},
        {"adler32",    l_Adler32},
        {"version",    l_Version},
        {nullptr, nullptr}
    };
    LT::Utils::RegisterPlainTable(L, funcs);
    return 1;
}
