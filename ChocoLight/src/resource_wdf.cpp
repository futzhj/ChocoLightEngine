#include "resource_package.h"

#include <unordered_map>
#include <utility>

namespace LT::Resource {
namespace {

class WdfPackage final : public IResourcePackage {
public:
    WdfPackage(FILE* fp, std::string path)
        : fp_(fp), path_(std::move(path)) {}

    ~WdfPackage() override {
        if (fp_) std::fclose(fp_);
    }

    bool Init(std::string& err) {
        uint64_t fileSize = 0;
        if (!GetFileSize(fp_, fileSize)) {
            err = "failed to determine file size";
            return false;
        }

        std::string header;
        if (!ReadAt(fp_, 0, 12, header, err)) return false;

        const std::string magic = header.substr(0, 4);
        if (magic == "NXPK" || magic == "MHWD") {
            err = std::string("unsupported WDF subtype: ") + magic;
            return false;
        }
        if (magic != "PFDW" && magic != "WDFP") {
            err = std::string("unsupported WDF subtype: ") + magic;
            return false;
        }

        const auto* h = reinterpret_cast<const uint8_t*>(header.data());
        uint32_t count = ReadLE32(h + 4);
        uint32_t indexOffset = ReadLE32(h + 8);
        uint64_t indexBytes = static_cast<uint64_t>(count) * 16u;
        if (!RangeInFile(fileSize, indexOffset, indexBytes)) {
            err = "invalid WDF index range";
            return false;
        }

        std::string index;
        if (!ReadAt(fp_, indexOffset, indexBytes, index, err)) return false;

        info_.kind = "WDF";
        info_.subtype = magic;
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = indexOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(index.data() + static_cast<size_t>(i) * 16u);
            ResourceEntry e;
            e.id = ReadLE32(p + 0);
            e.offset = ReadLE32(p + 4);
            e.size = ReadLE32(p + 8);
            e.packedSize = ReadLE32(p + 12);
            e.keyText = std::to_string(e.id);

            if (!RangeInFile(fileSize, e.offset, e.size)) {
                err = "invalid WDF entry range";
                return false;
            }

            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }

    bool Has(const std::string& key) const override {
        return keyToIndex_.find(key) != keyToIndex_.end();
    }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(key);
        if (it == keyToIndex_.end()) {
            err = "entry not found";
            return false;
        }

        const ResourceEntry& e = entries_[it->second];
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) {
            err = "entry exceeds maxBytes";
            return false;
        }

        return ReadAt(fp_, e.offset, e.size, out, err);
    }

private:
    FILE* fp_ = nullptr;
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenWdfPackage(const std::string& path, std::string& err) {
    FILE* fp = std::fopen(path.c_str(), "rb");
    if (!fp) {
        err = std::string("open failed: ") + path;
        return nullptr;
    }

    auto pkg = std::make_unique<WdfPackage>(fp, path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
