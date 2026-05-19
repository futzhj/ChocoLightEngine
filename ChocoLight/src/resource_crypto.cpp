#include "resource_package.h"

#include <cstring>

namespace LT::Resource {

void DecryptFlsIndex(uint8_t* data, size_t bytes, uint32_t count) {
    if (!data || bytes < static_cast<size_t>(count) * 16u) return;

    uint32_t accumulator = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t* p = data + static_cast<size_t>(i) * 16u;
        uint32_t hash = ReadLE32(p + 0);
        uint32_t flag = ReadLE32(p + 4);
        uint32_t size = ReadLE32(p + 8);
        uint32_t offset = ReadLE32(p + 12);

        uint32_t decOffset = offset ^ static_cast<uint32_t>(16193790u * (i * size) + 223990124u);
        uint32_t decSize = size ^ static_cast<uint32_t>(hash * flag + 914014u);
        uint32_t decFlag = flag ^ static_cast<uint32_t>(hash + accumulator);
        accumulator = static_cast<uint32_t>(accumulator + 243041234u);

        p[4] = static_cast<uint8_t>(decFlag & 0xFFu);
        p[5] = static_cast<uint8_t>((decFlag >> 8) & 0xFFu);
        p[6] = static_cast<uint8_t>((decFlag >> 16) & 0xFFu);
        p[7] = static_cast<uint8_t>((decFlag >> 24) & 0xFFu);
        p[8] = static_cast<uint8_t>(decSize & 0xFFu);
        p[9] = static_cast<uint8_t>((decSize >> 8) & 0xFFu);
        p[10] = static_cast<uint8_t>((decSize >> 16) & 0xFFu);
        p[11] = static_cast<uint8_t>((decSize >> 24) & 0xFFu);
        p[12] = static_cast<uint8_t>(decOffset & 0xFFu);
        p[13] = static_cast<uint8_t>((decOffset >> 8) & 0xFFu);
        p[14] = static_cast<uint8_t>((decOffset >> 16) & 0xFFu);
        p[15] = static_cast<uint8_t>((decOffset >> 24) & 0xFFu);
    }
}

void DecryptFlsData(const uint8_t* input, size_t bytes, uint32_t hash, std::string& out) {
    out.assign(bytes, '\0');
    if (!input || bytes == 0) return;

    uint32_t key = static_cast<uint32_t>((hash + 9581739u) ^ 0x937F4912u);
    uint8_t k[4] = {
        static_cast<uint8_t>(key & 0xFFu),
        static_cast<uint8_t>((key >> 8) & 0xFFu),
        static_cast<uint8_t>((key >> 16) & 0xFFu),
        static_cast<uint8_t>((key >> 24) & 0xFFu),
    };

    for (size_t i = 0; i < bytes; ++i) {
        out[i] = static_cast<char>(input[i] ^ k[i & 3u]);
    }
}

bool LooksLikeKnownPayload(const std::string& data) {
    if (data.empty()) return false;

    const auto starts = [&](const char* s, size_t n) {
        return data.size() >= n && std::memcmp(data.data(), s, n) == 0;
    };

    if (starts("@#$V", 4) || starts("[inf", 4) || starts("[ima", 4) || starts("[fra", 4)) return true;
    if (starts("[Ove", 4) || starts("SetA", 4) || starts("Bull", 4) || starts("Play", 4)) return true;
    if (starts("PNAM", 4) || starts("0.1M", 4) || starts("XPAM", 4)) return true;
    if (starts("\x89PNG", 4) || starts("BM", 2) || starts("\xFF\xD8\xFF", 3)) return true;

    if (data.size() >= 2) {
        uint16_t magic = ReadLE16(reinterpret_cast<const uint8_t*>(data.data()));
        switch (magic) {
            case 0x3049:
            case 0x3054:
            case 0x3149:
            case 0x3154:
            case 0x3449:
            case 0x3454:
            case 0x3549:
            case 0x3554:
            case 0x5053:
            case 0x5052:
            case 0x5054:
                return true;
            default:
                break;
        }
    }
    return false;
}

} // namespace LT::Resource
