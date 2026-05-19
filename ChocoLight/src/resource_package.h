#pragma once

#include "light.h"
#include "light_lua_helpers.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace LT::Resource {

constexpr uint32_t LT_MAGIC_PACKAGE = LT::Magic4('P', 'K', 'G', 'C');

struct PackageInfo {
    std::string kind;
    std::string subtype;
    std::string path;
    uint32_t count = 0;
    uint64_t indexOffset = 0;
    bool supported = false;
};

struct ResourceEntry {
    std::string keyText;
    uint32_t id = 0;
    uint64_t offset = 0;
    uint64_t size = 0;
    uint64_t packedSize = 0;
    uint32_t archive = 0;
    std::string md5;
};

struct ReadOptions {
    bool raw = false;
    bool decode = true;
    uint64_t maxBytes = 0;
};

class IResourcePackage {
public:
    virtual ~IResourcePackage() = default;
    virtual const PackageInfo& Info() const = 0;
    virtual const std::vector<ResourceEntry>& Entries() const = 0;
    virtual bool Has(const std::string& key) const = 0;
    virtual bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) = 0;
};

struct PackageHandle {
    uint32_t magic = LT_MAGIC_PACKAGE;
    std::unique_ptr<IResourcePackage> package;
};

bool ReadFileHead(const std::string& path, size_t bytes, std::string& out, std::string& err);
bool GetFileSize(FILE* fp, uint64_t& outSize);
bool ReadAt(FILE* fp, uint64_t offset, uint64_t size, std::string& out, std::string& err);
bool RangeInFile(uint64_t fileSize, uint64_t offset, uint64_t size);
uint16_t ReadLE16(const uint8_t* p);
uint32_t ReadLE32(const uint8_t* p);
std::string HexLower(const uint8_t* data, size_t len);

bool ProbePackageFile(const std::string& path, PackageInfo& info, std::string& err);
std::unique_ptr<IResourcePackage> OpenPackageFile(const std::string& path, std::string& err);

std::unique_ptr<IResourcePackage> OpenWdfPackage(const std::string& path, std::string& err);
std::unique_ptr<IResourcePackage> OpenWpkPackage(const std::string& path, std::string& err);
std::unique_ptr<IResourcePackage> OpenFlsPackage(const std::string& path, std::string& err);

void RegisterPluginsSubmodule(lua_State* L, const char* name, const luaL_Reg* funcs);

} // namespace LT::Resource
