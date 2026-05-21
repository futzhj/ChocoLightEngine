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

    // SKPE -> SKPW 解密：去 4 字节 "SKPE" magic，剩余整段 reverse + XOR 0x5A
    // 参考：E:\jinyiNew\GGELUA_SDL3\deps\Sources\grr\mygxy\wpk.c:2870-2894
    static bool DecryptSkpeToSkpw(const std::string& body, std::string& out) {
        const size_t n = body.size();
        out.resize(n);
        for (size_t i = 0; i < n; ++i) {
            out[i] = static_cast<char>(static_cast<uint8_t>(body[n - 1 - i]) ^ 0x5A);
        }
        return true;
    }

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
        if (fileSize < 4) {
            err = "IDX too small";
            return false;
        }

        // 读前 4 字节探外层 magic
        std::string magic;
        if (!ReadAt(fp, 0, 4, magic, err)) return false;

        // plain 持有真正的 SKPW 数据：
        //   SKPW 路径 → 原内容直读
        //   SKPE 路径 → 解密结果（reverse + XOR 0x5A）
        std::string plain;
        std::string subtype;

        if (magic == "SKPE") {
            if (fileSize <= 4) {
                err = "SKPE too small";
                return false;
            }
            std::string body;
            if (!ReadAt(fp, 4, fileSize - 4, body, err)) return false;
            if (!DecryptSkpeToSkpw(body, plain)) {
                err = "SKPE decrypt failed";
                return false;
            }
            subtype = "SKPE";
        } else if (magic == "SKPW") {
            if (fileSize < 32) {
                err = "IDX too small";
                return false;
            }
            if (!ReadAt(fp, 0, fileSize, plain, err)) return false;
            subtype = "SKPW";
        } else {
            err = "invalid IDX magic";
            return false;
        }

        // 校验解密结果（SKPE 解出后必须是 SKPW）
        if (plain.size() < 32) {
            err = "IDX too small after decode";
            return false;
        }
        if (plain.substr(0, 4) != "SKPW") {
            err = "decoded magic is not SKPW: " + plain.substr(0, 4);
            return false;
        }

        const auto* h = reinterpret_cast<const uint8_t*>(plain.data());
        uint32_t count = ReadLE32(h + 12);
        uint64_t recordsOffset = 32;
        uint64_t recordsBytes = static_cast<uint64_t>(count) * 28u;
        if (plain.size() < recordsOffset + recordsBytes) {
            err = "invalid IDX record range";
            return false;
        }

        info_.kind = "WPK";
        info_.subtype = subtype;
        info_.path = path_;
        info_.count = count;
        info_.indexOffset = recordsOffset;
        info_.supported = true;

        entries_.reserve(count);
        for (uint32_t i = 0; i < count; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(plain.data() + recordsOffset + static_cast<size_t>(i) * 28u);
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
