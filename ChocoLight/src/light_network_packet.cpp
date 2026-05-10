/**
 * @file light_network_packet.cpp
 * @brief Phase BC T6 实现 — 网络协议包打包/解包 + cJSON RAII
 */

#include "light_network_packet.h"

#include <cstring>

// htons / htonl 跨平台头
#if defined(_WIN32)
    // Windows: winsock2 (Light.dll 已链接 ws2_32)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
#else
    #include <arpa/inet.h>
#endif

namespace NetProto {

// ==================== JsonScope ====================

JsonScope::~JsonScope() {
    if (root) {
        cJSON_Delete(root);
        root = nullptr;
    }
}

// ==================== Pack / Unpack ====================

std::string Pack(PacketType type, const char* json, size_t jsonLen) {
    // 参数校验
    //   - type 允许任意值 (含 PKT_UNKNOWN), 由调用方语义保证; Unpack 才严格校验
    //   - jsonLen=0 + json=nullptr 视为合法 (空负载场景, e.g. KEEPALIVE)
    if (jsonLen > MAX_PAYLOAD) return std::string();
    if (jsonLen > 0 && !json) return std::string();

    std::string out;
    out.resize(HEADER_SIZE + jsonLen);

    // [0..1] MAGIC (uint16, network byte order)
    uint16_t magicNet = htons(MAGIC);
    std::memcpy(&out[0], &magicNet, 2);

    // [2] TYPE (single byte, no byte order)
    out[2] = (char)(uint8_t)type;

    // [3..6] LEN (uint32, network byte order)
    uint32_t lenNet = htonl((uint32_t)jsonLen);
    std::memcpy(&out[3], &lenNet, 4);

    // [7..] PAYLOAD
    if (jsonLen > 0) {
        std::memcpy(&out[HEADER_SIZE], json, jsonLen);
    }

    return out;
}

bool Unpack(const char* data, size_t len,
            PacketType& outType, std::string& outJson) {
    if (!data || len < HEADER_SIZE) return false;

    // [0..1] magic 校验
    uint16_t magicNet;
    std::memcpy(&magicNet, &data[0], 2);
    if (ntohs(magicNet) != MAGIC) return false;

    // [2] type 校验 (PKT_UNKNOWN=0 视为非法; > PKT_ROOM_KICK=7 也非法)
    uint8_t typeByte = (uint8_t)data[2];
    if (typeByte == PKT_UNKNOWN) return false;
    if (typeByte > PKT_ROOM_KICK) return false;

    // [3..6] payload 长度
    uint32_t lenNet;
    std::memcpy(&lenNet, &data[3], 4);
    uint32_t jsonLen = ntohl(lenNet);
    if (jsonLen > MAX_PAYLOAD) return false;
    if ((size_t)jsonLen + HEADER_SIZE > len) return false;     // 截断

    // 解析成功
    outType = (PacketType)typeByte;
    if (jsonLen > 0) {
        outJson.assign(&data[HEADER_SIZE], jsonLen);
    } else {
        outJson.clear();
    }
    return true;
}

}  // namespace NetProto
