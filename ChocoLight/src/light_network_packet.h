/**
 * @file light_network_packet.h
 * @brief Phase BC T6 — 网络协议包格式 + cJSON RAII helper
 *
 * 包格式 (与 DESIGN_PhaseBC.md §3 一致):
 *
 *   ┌────────┬────────┬──────────┬─────────────────────────┐
 *   │ MAGIC  │ TYPE   │ LEN      │ PAYLOAD (JSON)          │
 *   │ 2 byte │ 1 byte │ 4 byte   │ ≤ 65000 byte            │
 *   │ 0x4243 │ enum   │ network  │                         │
 *   │  ('BC')│        │ byte ord │                         │
 *   └────────┴────────┴──────────┴─────────────────────────┘
 *
 * 总头部 7 byte. PAYLOAD 上限 65000 (留 ~500 字节给 IPv4/UDP 头, UDP 上限 65507).
 *
 * 用于 Light.Network.Rpc 与 Light.Network.Room 的 ENet 消息体, 也可作为
 * Raw UDP 用户协议参考. cJSON 用于 RPC/Room 的负载序列化.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

extern "C" {
#include "cJSON.h"
}

namespace NetProto {

// ==================== 常量 ====================

inline constexpr uint16_t MAGIC        = 0x4243;       // "BC" 字符 (Phase BC 标记)
inline constexpr size_t   HEADER_SIZE  = 7;            // 2 + 1 + 4
inline constexpr size_t   MAX_PAYLOAD  = 65000;        // 留 IPv4/UDP 头空间
inline constexpr size_t   MAX_PACKET   = HEADER_SIZE + MAX_PAYLOAD;

// ==================== 包类型枚举 ====================

enum PacketType : uint8_t {
    PKT_UNKNOWN      = 0,
    PKT_RPC_REQUEST  = 1,        // {"id":N, "method":"...", "params":...}
    PKT_RPC_RESPONSE = 2,        // {"id":N, "result":...} 或 {"id":N, "error":{code,message}}
    PKT_ROOM_STATE   = 3,        // {"rev":N, "data":{...}}
    PKT_ROOM_EVENT   = 4,        // {"name":"...", "args":...}
    PKT_ROOM_INPUT   = 5,        // {"kind":"...", "data":...}
    PKT_ROOM_HELLO   = 6,        // {"name":"...", "meta":...}
    PKT_ROOM_KICK    = 7,        // {"reason":"..."}
};

// ==================== 打包 / 解包 ====================

/// 把 type + JSON 字符串打包成网络可发送的 byte string
///
/// 字节序: MAGIC 用 htons (uint16), LEN 用 htonl (uint32), TYPE 单字节无字节序.
/// 失败条件:
///   - json == nullptr
///   - jsonLen > MAX_PAYLOAD
/// 失败时返回空 string. 成功时返回 长度 = HEADER_SIZE + jsonLen 的 string.
std::string Pack(PacketType type, const char* json, size_t jsonLen);

inline std::string Pack(PacketType type, const std::string& json) {
    return Pack(type, json.data(), json.size());
}

/// 从网络字节流解包
///
/// 成功条件:
///   - len >= HEADER_SIZE
///   - magic 匹配 0x4243
///   - type 在 1..7 范围 (0=PKT_UNKNOWN 视为非法)
///   - JSON 长度 ≤ MAX_PAYLOAD
///   - HEADER_SIZE + JSON 长度 ≤ len (数据完整)
///
/// 成功返回 true, outType + outJson 被赋值
/// 失败返回 false, outType/outJson 不动 (适合循环重试)
bool Unpack(const char* data, size_t len,
            PacketType& outType, std::string& outJson);

// ==================== cJSON RAII helper ====================

/// 自动释放 cJSON 对象, 异常 / early return 时不泄漏
///
/// 用法:
///   JsonScope j(cJSON_Parse(jsonStr));
///   if (!j.get()) return false;
///   cJSON* method = cJSON_GetObjectItem(j.get(), "method");
///   ...
///   // 退出作用域自动 cJSON_Delete
///
/// 移交所有权:
///   cJSON* raw = j.release();   // j 不再持有, 调用方负责
///
/// 不可拷贝 (单一所有权), 可移动语义留作未来需要再加.
struct JsonScope {
    cJSON* root;

    explicit JsonScope(cJSON* r = nullptr) : root(r) {}
    ~JsonScope();

    JsonScope(const JsonScope&)            = delete;
    JsonScope& operator=(const JsonScope&) = delete;

    cJSON* operator->() const { return root; }
    cJSON* get() const        { return root; }

    /// 移交所有权, 此后析构不再 delete
    cJSON* release() {
        cJSON* r = root;
        root = nullptr;
        return r;
    }

    /// 转换为 bool: 是否持有有效对象
    explicit operator bool() const { return root != nullptr; }
};

}  // namespace NetProto
