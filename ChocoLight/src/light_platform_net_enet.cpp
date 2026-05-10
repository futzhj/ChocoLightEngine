/**
 * @file light_platform_net_enet.cpp
 * @brief Phase BC T4 — ENet reliable UDP 封装 (跨桌面 + 移动)
 *
 * 实现 light_platform_net.h 中的 EnetXxx API 集合.
 *
 * 平台覆盖:
 *   - 桌面 (Windows/Linux/macOS): 编译 + 链接 enet 静态库
 *   - 移动 (Android/iOS): 编译 + 链接 enet 静态库 (ENet 用 BSD socket 跨平台)
 *   - Web (Emscripten): 整文件不参与编译, ENet 空存根在 light_platform_net.cpp
 *
 * Init/Shutdown 集成:
 *   - PlatformNet::Init() 在桌面/移动分别调 EnetGlobalInit (本文件 export)
 *   - PlatformNet::Shutdown() 调 EnetGlobalShutdown
 *   - PlatformNet::Poll() 调 EnetTickAll, 自动 service 所有活跃 host
 */

#if !defined(__EMSCRIPTEN__)

#include "light_platform_net.h"
#include "light.h"

#include <enet/enet.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

// ==================== 内部状态 (anonymous namespace) ====================

namespace {

bool s_enetInitialized = false;

// 活跃 host 列表 (Poll 中遍历)
std::vector<ENetHost*> s_hosts;

// host -> 用户事件回调 (EnetSetEventCb 注册)
std::unordered_map<ENetHost*, PlatformNet::OnEnetEventCb> s_cbs;

// host -> 帧末 idle 回调 (EnetSetFrameCb 注册)
// 不同于 s_cbs (仅在有 ENet 事件时触发), s_frameCbs 每帧无条件调用
std::unordered_map<ENetHost*, PlatformNet::OnEnetFrameCb> s_frameCbs;

// EnetPeerAddress 静态返回 buffer
char s_addrBuf[64] = {0};

}  // anonymous namespace

// ==================== PlatformNet 实现 ====================

namespace PlatformNet {

// 内部 init/shutdown (供 light_platform_net.cpp / _mobile.cpp 在 Init/Shutdown 调用)
// 不在 public header, 但跨 .cpp 链接需要 extern 可见 → 放在 PlatformNet 命名空间
bool EnetGlobalInit() {
    if (s_enetInitialized) return true;
    if (enet_initialize() != 0) {
        CC::Log(CC::LOG_WARN, "PlatformNet: enet_initialize() failed");
        return false;
    }
    s_enetInitialized = true;
    CC::Log(CC::LOG_INFO, "PlatformNet: ENet %d.%d.%d initialized",
            ENET_VERSION_MAJOR, ENET_VERSION_MINOR, ENET_VERSION_PATCH);
    return true;
}

void EnetGlobalShutdown() {
    if (!s_enetInitialized) return;
    // 主动销毁所有 host (用户未显式销毁的也清理)
    for (auto* h : s_hosts) {
        enet_host_destroy(h);
    }
    s_hosts.clear();
    s_cbs.clear();
    s_frameCbs.clear();
    enet_deinitialize();
    s_enetInitialized = false;
}

// PlatformNet::Poll 末尾调用, 驱动所有 host 一次 service
void EnetTickAll() {
    if (!s_enetInitialized || s_hosts.empty()) return;

    // 用副本遍历, 防止 cb 中销毁 host 导致迭代器失效
    auto hostsSnap = s_hosts;
    for (auto* host : hostsSnap) {
        // host 可能在 cb 中被销毁, 重新校验
        if (std::find(s_hosts.begin(), s_hosts.end(), host) == s_hosts.end()) {
            continue;
        }
        ENetEvent ev;
        while (enet_host_service(host, &ev, 0) > 0) {
            // 转换 ENetEvent → PlatformNet::EnetEvent
            EnetEvent out{};
            out.peer = (EnetPeer*)ev.peer;
            out.channel = (int)ev.channelID;

            bool dispatchable = true;
            switch (ev.type) {
                case ENET_EVENT_TYPE_CONNECT:
                    out.type = EnetEventType::CONNECT;
                    break;
                case ENET_EVENT_TYPE_DISCONNECT:
                    out.type = EnetEventType::DISCONNECT;
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                    out.type = EnetEventType::RECEIVE;
                    out.data = ev.packet ? (const char*)ev.packet->data : nullptr;
                    out.len  = ev.packet ? (int)ev.packet->dataLength  : 0;
                    break;
                default:
                    dispatchable = false;
                    break;
            }
            if (dispatchable) {
                auto it = s_cbs.find(host);
                if (it != s_cbs.end() && it->second) {
                    it->second(out);
                }
            }
            if (ev.type == ENET_EVENT_TYPE_RECEIVE && ev.packet) {
                enet_packet_destroy(ev.packet);
            }
        }
        // 帧末 idle cb (无论是否有事件都调用一次)
        // 用 host_in_snap 校验 — 用户 cb 可能销毁 host
        if (std::find(s_hosts.begin(), s_hosts.end(), host) != s_hosts.end()) {
            auto fit = s_frameCbs.find(host);
            if (fit != s_frameCbs.end() && fit->second) {
                fit->second();
            }
        }
    }
}

// ==================== Public API ====================

EnetHost* EnetCreateHost(const char* ip, uint16_t port,
                          int maxPeers, int channels) {
    if (!s_enetInitialized) {
        if (!EnetGlobalInit()) return nullptr;
    }
    if (maxPeers <= 0 || channels <= 0) return nullptr;

    ENetAddress addr;
    ENetAddress* bindAddr = nullptr;
    if (ip != nullptr) {
        // 指定 IP → 监听模式 (server / 双向 host)
        if (strcmp(ip, "0.0.0.0") == 0) {
            addr.host = ENET_HOST_ANY;
        } else {
            if (enet_address_set_host(&addr, ip) != 0) {
                CC::Log(CC::LOG_WARN,
                        "PlatformNet: ENet address parse failed: '%s'", ip);
                return nullptr;
            }
        }
        addr.port = port;
        bindAddr = &addr;
    }
    // ip == nullptr → bindAddr nullptr → 纯 client 模式 (不监听端口)

    ENetHost* host = enet_host_create(bindAddr,
                                       (size_t)maxPeers,
                                       (size_t)channels,
                                       0,         // incomingBandwidth = 不限
                                       0);        // outgoingBandwidth = 不限
    if (!host) {
        CC::Log(CC::LOG_WARN,
                "PlatformNet: enet_host_create failed (ip=%s port=%u peers=%d ch=%d)",
                ip ? ip : "(client)", port, maxPeers, channels);
        return nullptr;
    }
    s_hosts.push_back(host);
    return (EnetHost*)host;
}

void EnetDestroyHost(EnetHost* host) {
    if (!host) return;
    auto* eh = (ENetHost*)host;
    auto it = std::find(s_hosts.begin(), s_hosts.end(), eh);
    if (it != s_hosts.end()) s_hosts.erase(it);
    s_cbs.erase(eh);
    s_frameCbs.erase(eh);
    enet_host_destroy(eh);
}

EnetPeer* EnetConnect(EnetHost* localHost,
                       const char* host, uint16_t port,
                       int channels) {
    if (!localHost || !host || channels <= 0) return nullptr;
    auto* eh = (ENetHost*)localHost;

    ENetAddress addr;
    if (enet_address_set_host(&addr, host) != 0) {
        CC::Log(CC::LOG_WARN,
                "PlatformNet: EnetConnect address parse failed: '%s'", host);
        return nullptr;
    }
    addr.port = port;

    ENetPeer* peer = enet_host_connect(eh, &addr, (size_t)channels, 0);
    if (!peer) {
        CC::Log(CC::LOG_WARN,
                "PlatformNet: enet_host_connect failed (%s:%u)", host, port);
        return nullptr;
    }
    return (EnetPeer*)peer;
}

void EnetDisconnect(EnetPeer* peer, uint32_t data) {
    if (!peer) return;
    enet_peer_disconnect((ENetPeer*)peer, (enet_uint32)data);
}

bool EnetSend(EnetPeer* peer, int channel,
               const char* data, int len, bool reliable) {
    if (!peer || !data || len <= 0 || channel < 0) return false;
    auto* p = (ENetPeer*)peer;
    if ((size_t)channel >= p->channelCount) return false;

    // ENet flag 语义:
    //   ENET_PACKET_FLAG_RELIABLE = 保证送达 + 顺序 (适用 RPC / 关键状态)
    //   0 (默认)                   = unreliable sequenced (旧包丢弃, 不重传)
    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;

    ENetPacket* packet = enet_packet_create(data, (size_t)len, flags);
    if (!packet) return false;
    int r = enet_peer_send(p, (enet_uint8)channel, packet);
    if (r != 0) {
        // enet_peer_send 失败时 packet 不会被自动释放
        enet_packet_destroy(packet);
        return false;
    }
    return true;
}

void EnetSetEventCb(EnetHost* host, OnEnetEventCb cb) {
    if (!host) return;
    s_cbs[(ENetHost*)host] = std::move(cb);
}

void EnetSetFrameCb(EnetHost* host, OnEnetFrameCb cb) {
    if (!host) return;
    if (!cb) {
        s_frameCbs.erase((ENetHost*)host);
    } else {
        s_frameCbs[(ENetHost*)host] = std::move(cb);
    }
}

const char* EnetPeerAddress(EnetPeer* peer) {
    if (!peer) {
        s_addrBuf[0] = '\0';
        return s_addrBuf;
    }
    auto* p = (ENetPeer*)peer;
    char ipBuf[64] = {0};
    if (enet_address_get_host_ip(&p->address, ipBuf, sizeof(ipBuf)) != 0) {
        std::snprintf(s_addrBuf, sizeof(s_addrBuf), "?:%u", p->address.port);
        return s_addrBuf;
    }
    std::snprintf(s_addrBuf, sizeof(s_addrBuf), "%s:%u", ipBuf, p->address.port);
    return s_addrBuf;
}

uint32_t EnetPeerID(EnetPeer* peer) {
    if (!peer) return 0;
    return (uint32_t)((ENetPeer*)peer)->incomingPeerID;
}

int EnetBroadcast(EnetHost* host, int channel,
                   const char* data, int len, bool reliable) {
    if (!host || !data || len <= 0 || channel < 0) return 0;
    auto* eh = (ENetHost*)host;
    if ((size_t)channel >= eh->channelLimit) return 0;

    enet_uint32 flags = reliable ? ENET_PACKET_FLAG_RELIABLE : 0;
    ENetPacket* packet = enet_packet_create(data, (size_t)len, flags);
    if (!packet) return 0;

    // enet_host_broadcast 把 packet 推入所有已连接 peer 的发送队列.
    // 它内部对每个 peer 调 enet_peer_send, 若 packet referenceCount 为 0
    // 会 packet 自动释放, 否则保留. 不需手动 destroy 后续.
    enet_host_broadcast(eh, (enet_uint8)channel, packet);

    // 统计已连接 peer 数 (返回值语义"投递包数")
    int sent = 0;
    for (size_t i = 0; i < eh->peerCount; ++i) {
        if (eh->peers[i].state == ENET_PEER_STATE_CONNECTED) ++sent;
    }
    return sent;
}

bool EnetDisconnectPeerById(EnetHost* host, uint32_t peerId, uint32_t userData) {
    if (!host) return false;
    auto* eh = (ENetHost*)host;
    if ((size_t)peerId >= eh->peerCount) return false;
    ENetPeer* p = &eh->peers[peerId];
    if (p->state == ENET_PEER_STATE_DISCONNECTED ||
        p->state == ENET_PEER_STATE_ZOMBIE) {
        return false;
    }
    enet_peer_disconnect(p, (enet_uint32)userData);
    return true;
}

}  // namespace PlatformNet

#endif  // !__EMSCRIPTEN__
