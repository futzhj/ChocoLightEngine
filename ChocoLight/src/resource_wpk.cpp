#include "resource_package.h"

#include <unordered_map>
#include <utility>

namespace LT::Resource {
namespace {

std::string Dirname(const std::string& path) {
    size_t pos = path.find_last_of("/\\");
    return pos == std::string::npos ? std::string(".") : path.substr(0, pos);
}

std::string Stem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    size_t start = slash == std::string::npos ? 0 : slash + 1;
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot < start) dot = path.size();
    return path.substr(start, dot - start);
}

std::string JoinPath(const std::string& dir, const std::string& name) {
    if (dir.empty() || dir == ".") return name;
    char tail = dir[dir.size() - 1];
    if (tail == '/' || tail == '\\') return dir + name;
#if defined(_WIN32)
    return dir + "\\" + name;
#else
    return dir + "/" + name;
#endif
}

std::string LowerAscii(std::string value) {
    for (char& ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

class WpkPackage final : public IResourcePackage {
public:
    explicit WpkPackage(std::string path)
        : path_(std::move(path)) {}

    bool Init(std::string& err) {
        FILE* fp = std::fopen(path_.c_str(), "rb");
        if (!fp) {
            err = std::string("open failed: ") + path_;
            return false;
        }
        struct FileCloser {
            FILE* fp;
            ~FileCloser() { if (fp) std::fclose(fp); }
        } close{fp};

        uint64_t fileSize = 0;
        if (!GetFileSize(fp, fileSize)) {
            err = "failed to determine IDX size";
            return false;
        }
        if (fileSize < 32) {
            err = "IDX too small";
            return false;
        }

        std::string header;
        if (!ReadAt(fp, 0, 32, header, err)) return false;
        if (header.substr(0, 4) != "SKPW") {
            err = "invalid IDX magic";
            return false;
        }

        const auto* h = reinterpret_cast<const uint8_t*>(header.data());
        uint32_t count = ReadLE32(h + 12);
        uint64_t recordsOffset = 32;
        uint64_t recordsBytes = static_cast<uint64_t>(count) * 28u;
        if (!RangeInFile(fileSize, recordsOffset, recordsBytes)) {
            err = "invalid IDX record range";
            return false;
        }

        std::string records;
        if (!ReadAt(fp, recordsOffset, recordsBytes, records, err)) return false;

        info_.kind = "WPK";
        info_.subtype = "SKPW";
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = recordsOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(records.data() + static_cast<size_t>(i) * 28u);
            ResourceEntry e;
            e.md5 = HexLower(p, 16);
            e.size = ReadLE32(p + 16);
            e.packedSize = e.size;
            e.offset = ReadLE32(p + 20);
            e.archive = ReadLE16(p + 24);
            e.keyText = e.md5;

            keyToIndex_[e.keyText] = entries_.size();
            entries_.push_back(e);
        }
        return true;
    }

    const PackageInfo& Info() const override { return info_; }
    const std::vector<ResourceEntry>& Entries() const override { return entries_; }

    bool Has(const std::string& key) const override {
        return keyToIndex_.find(LowerAscii(key)) != keyToIndex_.end();
    }

    bool Read(const std::string& key, const ReadOptions& opts, std::string& out, std::string& err) override {
        auto it = keyToIndex_.find(LowerAscii(key));
        if (it == keyToIndex_.end()) {
            err = "entry not found";
            return false;
        }

        const ResourceEntry& e = entries_[it->second];
        if (e.archive == 0xFF || e.archive == 0xFFFF) {
            err = "external WPK entry is not stored in archive";
            return false;
        }
        if (opts.maxBytes > 0 && e.size > opts.maxBytes) {
            err = "entry exceeds maxBytes";
            return false;
        }

        std::string archiveName = Stem(path_) + std::to_string(e.archive) + ".wpk";
        std::string archivePath = JoinPath(Dirname(path_), archiveName);
        FILE* fp = std::fopen(archivePath.c_str(), "rb");
        if (!fp) {
            err = std::string("open WPK archive failed: ") + archivePath;
            return false;
        }
        struct FileCloser {
            FILE* fp;
            ~FileCloser() { if (fp) std::fclose(fp); }
        } close{fp};

        return ReadAt(fp, e.offset, e.size, out, err);
    }

private:
    std::string path_;
    PackageInfo info_;
    std::vector<ResourceEntry> entries_;
    std::unordered_map<std::string, size_t> keyToIndex_;
};

} // namespace

std::unique_ptr<IResourcePackage> OpenWpkPackage(const std::string& path, std::string& err) {
    auto pkg = std::make_unique<WpkPackage>(path);
    if (!pkg->Init(err)) return nullptr;
    return pkg;
}

} // namespace LT::Resource
