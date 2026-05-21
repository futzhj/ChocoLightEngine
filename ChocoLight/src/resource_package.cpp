#include "resource_package.h"

#if defined(_WIN32)
#include <cstdio>
#else
#include <cstdio>
#include <sys/types.h>
#endif

namespace LT::Resource {
namespace {

bool SeekFile(FILE* fp, uint64_t offset, int origin) {
#if defined(_WIN32)
    return _fseeki64(fp, static_cast<__int64>(offset), origin) == 0;
#else
    return fseeko(fp, static_cast<off_t>(offset), origin) == 0;
#endif
}

bool TellFile(FILE* fp, uint64_t& out) {
#if defined(_WIN32)
    __int64 pos = _ftelli64(fp);
    if (pos < 0) return false;
    out = static_cast<uint64_t>(pos);
    return true;
#else
    off_t pos = ftello(fp);
    if (pos < 0) return false;
    out = static_cast<uint64_t>(pos);
    return true;
#endif
}

} // namespace

bool ReadFileHead(const std::string& path, size_t bytes, std::string& out, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        err = std::string("open failed: ") + path;
        return false;
    }

    if (bytes == 0) {
        out.clear();
        std::fclose(fp);
        return true;
    }

    out.assign(bytes, '\0');
    size_t n = std::fread(&out[0], 1, bytes, fp);
    std::fclose(fp);
    out.resize(n);

    if (n == 0) {
        err = "empty file";
        return false;
    }
    return true;
}

bool GetFileSize(FILE* fp, uint64_t& outSize) {
    uint64_t cur = 0;
    if (!TellFile(fp, cur)) return false;
    if (!SeekFile(fp, 0, SEEK_END)) return false;
    if (!TellFile(fp, outSize)) return false;
    return SeekFile(fp, cur, SEEK_SET);
}

bool RangeInFile(uint64_t fileSize, uint64_t offset, uint64_t size) {
    if (offset > fileSize) return false;
    if (size > fileSize - offset) return false;
    return true;
}

bool ReadAt(FILE* fp, uint64_t offset, uint64_t size, std::string& out, std::string& err) {
    uint64_t fileSize = 0;
    if (!GetFileSize(fp, fileSize)) {
        err = "failed to determine file size";
        return false;
    }
    if (!RangeInFile(fileSize, offset, size)) {
        err = "read range out of bounds";
        return false;
    }
    if (size > static_cast<uint64_t>(static_cast<size_t>(-1))) {
        err = "read range too large";
        return false;
    }
    if (!SeekFile(fp, offset, SEEK_SET)) {
        err = "seek failed";
        return false;
    }

    out.assign(static_cast<size_t>(size), '\0');
    if (size == 0) return true;

    size_t n = std::fread(&out[0], 1, static_cast<size_t>(size), fp);
    if (n != static_cast<size_t>(size)) {
        err = "read failed";
        return false;
    }
    return true;
}

uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

std::string HexLower(const uint8_t* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[(data[i] >> 4) & 0x0F];
        out[i * 2 + 1] = digits[data[i] & 0x0F];
    }
    return out;
}

bool ProbePackageFile(const std::string& path, PackageInfo& info, std::string& err) {
    std::string head;
    if (!ReadFileHead(path, 32, head, err)) return false;
    if (head.size() < 4) {
        err = "file too small";
        return false;
    }

    const std::string magic = head.substr(0, 4);
    info.path = path;
    info.subtype = magic;

    if (magic == "PFDW" || magic == "WDFP") {
        info.kind = "WDF";
        info.supported = true;
        return true;
    }
    if (magic == "WDFX" || magic == "WDFH" || magic == "SFDW" || magic == "WDFS") {
        info.kind = "WDF";
        info.supported = false;
        return true;
    }
    if (magic == "NXPK" || magic == "MHWD") {
        info.kind = "WDF";
        info.supported = false;
        err = std::string("unsupported WDF subtype: ") + magic;
        return false;
    }
    if (magic == "SKPW") {
        info.kind = "WPK";
        info.subtype = "SKPW";
        info.supported = true;
        return true;
    }
    // SKPE: myxy 私有加密 WPK 变体，解密后内部即标准 SKPW
    // 加密：明文 SKPW 整体 reverse 后 XOR 0x5A，再前置 4 字节 "SKPE" magic
    // 参考：E:\jinyiNew\GGELUA_SDL3\deps\Sources\grr\mygxy\wpk.c:2870-2894
    if (magic == "SKPE") {
        info.kind = "WPK";
        info.subtype = "SKPE";
        info.supported = true;
        return true;
    }
    if (magic == "0SLF") {
        info.kind = "FLS";
        info.subtype = "0SLF";
        info.supported = true;
        return true;
    }

    err = "unknown package format";
    return false;
}

std::unique_ptr<IResourcePackage> OpenPackageFile(const std::string& path, std::string& err) {
    PackageInfo info;
    if (!ProbePackageFile(path, info, err)) return nullptr;
    if (info.kind == "WDF") return OpenWdfPackage(path, err);
    if (info.kind == "WPK") return OpenWpkPackage(path, err);
    if (info.kind == "FLS") return OpenFlsPackage(path, err);
    err = "unknown package format";
    return nullptr;
}

void RegisterPluginsSubmodule(lua_State* L, const char* name, const luaL_Reg* funcs) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Plugins");
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, "Plugins");
        lua_newtable(L);
        lua_rawset(L, -3);
        lua_pushstring(L, "Plugins");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, name);
    lua_rawget(L, -2);
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_pushstring(L, name);
        lua_newtable(L);
        luaL_setfuncs(L, funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, name);
        lua_rawget(L, -2);
    } else {
        luaL_setfuncs(L, funcs, 0);
    }

    lua_remove(L, -2);
    lua_remove(L, -2);
}

} // namespace LT::Resource
