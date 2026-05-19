#pragma once

#include "light.h"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace LT::Utils {

inline int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string HexEncode(const uint8_t* data, size_t len, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        uint8_t b = data[i];
        out[i * 2] = digits[(b >> 4) & 0x0F];
        out[i * 2 + 1] = digits[b & 0x0F];
    }
    return out;
}

inline bool HexDecode(const char* text, size_t len, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    if ((len % 2) != 0) {
        err = "hex string length must be even";
        return false;
    }

    out.resize(len / 2);
    for (size_t i = 0; i < len; i += 2) {
        int hi = HexValue(text[i]);
        int lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) {
            err = "hex string contains invalid character";
            out.clear();
            return false;
        }
        out[i / 2] = static_cast<char>((hi << 4) | lo);
    }
    return true;
}

inline bool IsURLUnreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '_' || c == '.' || c == '~';
}

inline std::string URLEncode(const char* data, size_t len) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = static_cast<unsigned char>(data[i]);
        if (IsURLUnreserved(c)) {
            out.push_back(static_cast<char>(c));
            continue;
        }
        out.push_back('%');
        out.push_back(digits[(c >> 4) & 0x0F]);
        out.push_back(digits[c & 0x0F]);
    }
    return out;
}

inline bool URLDecode(const char* text, size_t len, std::string& out, std::string& err) {
    out.clear();
    err.clear();
    out.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char c = text[i];
        if (c != '%') {
            out.push_back(c);
            continue;
        }
        if (i + 2 >= len) {
            err = "url escape is incomplete";
            out.clear();
            return false;
        }
        int hi = HexValue(text[i + 1]);
        int lo = HexValue(text[i + 2]);
        if (hi < 0 || lo < 0) {
            err = "url escape contains invalid hex digit";
            out.clear();
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

inline uint32_t CRC32(const uint8_t* data, size_t len, uint32_t seed = 0) {
    uint32_t crc = seed ^ 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

inline uint32_t SHA1LeftRotate(uint32_t value, uint32_t bits) {
    return (value << bits) | (value >> (32u - bits));
}

inline void SHA1(const uint8_t* data, size_t len, uint8_t out20[20]) {
    uint32_t h0 = 0x67452301u;
    uint32_t h1 = 0xEFCDAB89u;
    uint32_t h2 = 0x98BADCFEu;
    uint32_t h3 = 0x10325476u;
    uint32_t h4 = 0xC3D2E1F0u;

    uint64_t bitLen = static_cast<uint64_t>(len) * 8u;
    size_t total = len + 1;
    while ((total % 64) != 56) ++total;

    std::vector<uint8_t> msg(total + 8, 0);
    if (len > 0 && data) {
        std::memcpy(msg.data(), data, len);
    }
    msg[len] = 0x80u;
    for (int i = 0; i < 8; ++i) {
        msg[total + static_cast<size_t>(7 - i)] = static_cast<uint8_t>((bitLen >> (i * 8)) & 0xFFu);
    }

    for (size_t offset = 0; offset < msg.size(); offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t* p = msg.data() + offset + static_cast<size_t>(i * 4);
            w[i] = (static_cast<uint32_t>(p[0]) << 24) |
                   (static_cast<uint32_t>(p[1]) << 16) |
                   (static_cast<uint32_t>(p[2]) << 8) |
                   static_cast<uint32_t>(p[3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = SHA1LeftRotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;

        for (int i = 0; i < 80; ++i) {
            uint32_t f;
            uint32_t k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }

            uint32_t temp = SHA1LeftRotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = SHA1LeftRotate(b, 30);
            b = a;
            a = temp;
        }

        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out20[i * 4] = static_cast<uint8_t>((h[i] >> 24) & 0xFFu);
        out20[i * 4 + 1] = static_cast<uint8_t>((h[i] >> 16) & 0xFFu);
        out20[i * 4 + 2] = static_cast<uint8_t>((h[i] >> 8) & 0xFFu);
        out20[i * 4 + 3] = static_cast<uint8_t>(h[i] & 0xFFu);
    }
}

inline bool IsPathSeparator(char c) {
    return c == '/' || c == '\\';
}

inline char NativePathSeparator() {
#ifdef _WIN32
    return '\\';
#else
    return '/';
#endif
}

inline std::string NormalizePath(const std::string& path, char sep) {
    if (sep == '\0') sep = NativePathSeparator();
    if (path.empty()) return ".";

    size_t pos = 0;
    std::string prefix;
    bool absolute = false;

    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        prefix.assign(path.data(), 2);
        pos = 2;
        if (pos < path.size() && IsPathSeparator(path[pos])) {
            absolute = true;
            ++pos;
        }
    } else if (IsPathSeparator(path[0])) {
        absolute = true;
        while (pos < path.size() && IsPathSeparator(path[pos])) ++pos;
    }

    std::vector<std::string> parts;
    while (pos < path.size()) {
        size_t start = pos;
        while (pos < path.size() && !IsPathSeparator(path[pos])) ++pos;
        std::string part = path.substr(start, pos - start);
        while (pos < path.size() && IsPathSeparator(path[pos])) ++pos;

        if (part.empty() || part == ".") {
            continue;
        }
        if (part == "..") {
            if (!parts.empty() && parts.back() != "..") {
                parts.pop_back();
            } else if (!absolute) {
                parts.push_back(part);
            }
            continue;
        }
        parts.push_back(part);
    }

    std::string out = prefix;
    if (absolute) out.push_back(sep);
    for (size_t i = 0; i < parts.size(); ++i) {
        if (!out.empty() && out.back() != sep) out.push_back(sep);
        out += parts[i];
    }
    if (out.empty()) return absolute ? std::string(1, sep) : std::string(".");
    return out;
}

inline std::string JoinPathParts(lua_State* L, int firstArg, int lastArg) {
    char sep = NativePathSeparator();
    std::string joined;
    for (int i = firstArg; i <= lastArg; ++i) {
        size_t len = 0;
        const char* part = luaL_checklstring(L, i, &len);
        if (len == 0) continue;
        if (!joined.empty() && !IsPathSeparator(joined.back()) && !IsPathSeparator(part[0])) {
            joined.push_back(sep);
        }
        joined.append(part, len);
    }
    return NormalizePath(joined, sep);
}

inline bool ParseUUID(const char* text, size_t len, uint8_t out16[16]) {
    if (!text || !out16 || len != 36) return false;
    int byteIndex = 0;
    for (size_t i = 0; i < len;) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') return false;
            ++i;
            continue;
        }
        if (i + 1 >= len || byteIndex >= 16) return false;
        int hi = HexValue(text[i]);
        int lo = HexValue(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out16[byteIndex++] = static_cast<uint8_t>((hi << 4) | lo);
        i += 2;
    }
    return byteIndex == 16;
}

inline std::string FormatUUID(const uint8_t raw16[16]) {
    std::string hex = HexEncode(raw16, 16, false);
    std::string out;
    out.reserve(36);
    out.append(hex, 0, 8);
    out.push_back('-');
    out.append(hex, 8, 4);
    out.push_back('-');
    out.append(hex, 12, 4);
    out.push_back('-');
    out.append(hex, 16, 4);
    out.push_back('-');
    out.append(hex, 20, 12);
    return out;
}

inline void RegisterPlainTable(lua_State* L, const luaL_Reg* funcs) {
    lua_newtable(L);
    luaL_register(L, nullptr, funcs);
}

inline void RegisterPluginsSubmodule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Plugins");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Plugins");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Plugins");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, name);
        lua_createtable(L, 0, 0);
        if (funcs) {
            luaL_setfuncs(L, funcs, 0);
        }
        lua_rawset(L, -3);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
    }

    lua_remove(L, -2);
    lua_remove(L, -2);
}

}
