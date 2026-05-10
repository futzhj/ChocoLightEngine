/**
 * @file light_platform_net_mobile.cpp
 * @brief PlatformNet 移动端实现 — 基于 POSIX socket (Android/iOS)
 * @note 替代 libuv, 使用非阻塞 socket + select/poll 实现异步 IO
 *
 * 设计:
 *   - 与 libuv PlatformNet API 100% 兼容
 *   - 非阻塞 socket + 每帧 Poll (select 超时 0ms)
 *   - 支持 DNS 解析 (getaddrinfo, 同步但移动端通常很快)
 *   - 支持 TLS/SSL: Android 通过 JNI SSLSocket, iOS 通过 Security.framework
 *     (本版使用明文 TCP, TLS 在上层通过 HTTPS URL 处理)
 */

#if defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)

#include "light_platform_net.h"
#include "light.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <algorithm>

// iOS 没有 MSG_NOSIGNAL, 用 SO_NOSIGPIPE 替代
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// ==================== 内部类型 ====================

// 模拟 uv_tcp_s (对外是不透明指针)
struct uv_tcp_s {
    int fd;             // socket 文件描述符
    bool connecting;    // 正在连接
    bool connected;     // 已连接
    bool isServer;      // 是否是服务端 socket
    bool closing;       // 正在关闭
    PlatformNet::OnConnectCb connectCb;
    PlatformNet::OnReadCb    readCb;
    PlatformNet::OnAcceptCb  acceptCb;
    bool reading;       // 是否在读取
    int backlog;
};

// Phase BC T3: 模拟 uv_udp_s (POSIX SOCK_DGRAM)
struct uv_udp_s {
    int  fd;            // UDP socket fd, -1 = 已关闭
    bool reading;       // StartUdpRecv 后为 true
    bool closing;       // CloseUdp 后为 true (Poll 末段清理)
    PlatformNet::OnUdpRecvCb recvCb;
};

// 全局句柄列表 (Poll 遍历)
static std::vector<uv_tcp_s*> s_handles;
static std::vector<uv_udp_s*> s_udpHandles;     // Phase BC T3
static bool s_initialized = false;

// 设置非阻塞
static void SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ==================== PlatformNet 实现 ====================

namespace PlatformNet {

// Phase BC T4: 跨 .cpp 链接 ENet 内部 init/tick/shutdown
//   实现位于 light_platform_net_enet.cpp
bool EnetGlobalInit();
void EnetGlobalShutdown();
void EnetTickAll();

bool Init() {
    s_initialized = true;
    CC::Log(CC::LOG_INFO, "PlatformNet: initialized (POSIX socket, mobile)");
    // Phase BC T4: ENet 全局初始化 (失败不阻塞 mobile TCP, ENet 模块降级不可用)
    EnetGlobalInit();
    return true;
}

void Shutdown() {
    // Phase BC T4: 先停 ENet (销毁所有 host)
    EnetGlobalShutdown();
    for (auto* h : s_handles) {
        if (h->fd >= 0) { close(h->fd); h->fd = -1; }
        delete h;
    }
    s_handles.clear();
    // Phase BC T3: 清理 UDP 句柄
    for (auto* h : s_udpHandles) {
        if (h->fd >= 0) { close(h->fd); h->fd = -1; }
        delete h;
    }
    s_udpHandles.clear();
    s_initialized = false;
    CC::Log(CC::LOG_INFO, "PlatformNet: shutdown");
}

void Poll() {
    if (!s_initialized) return;

    // Phase BC T4: ENet host 始终需要 service, 即使 TCP/UDP 都为空
    // (放在末尾避免与 select 时序冲突)

    if (s_handles.empty() && s_udpHandles.empty()) {
        EnetTickAll();
        return;
    }

    // 构建 fd_set
    fd_set readSet, writeSet, errSet;
    FD_ZERO(&readSet);
    FD_ZERO(&writeSet);
    FD_ZERO(&errSet);
    int maxFd = -1;

    for (auto* h : s_handles) {
        if (h->fd < 0 || h->closing) continue;
        if (h->connecting) {
            FD_SET(h->fd, &writeSet);
            FD_SET(h->fd, &errSet);
        }
        if (h->reading || h->isServer) {
            FD_SET(h->fd, &readSet);
        }
        if (h->fd > maxFd) maxFd = h->fd;
    }

    // Phase BC T3: 把 UDP socket 也加入 readSet (有 reading 时)
    for (auto* h : s_udpHandles) {
        if (h->fd < 0 || h->closing) continue;
        if (h->reading) {
            FD_SET(h->fd, &readSet);
            if (h->fd > maxFd) maxFd = h->fd;
        }
    }

    if (maxFd < 0) return;

    struct timeval tv = {0, 0};  // 非阻塞
    int ret = select(maxFd + 1, &readSet, &writeSet, &errSet, &tv);
    if (ret <= 0) return;

    // 遍历处理 (拷贝列表防止回调中修改)
    auto handles = s_handles;
    for (auto* h : handles) {
        if (h->fd < 0 || h->closing) continue;

        // 连接完成检测
        if (h->connecting && (FD_ISSET(h->fd, &writeSet) || FD_ISSET(h->fd, &errSet))) {
            h->connecting = false;
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(h->fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == 0) {
                h->connected = true;
                if (h->connectCb) h->connectCb(0);
            } else {
                if (h->connectCb) h->connectCb(-err);
            }
        }

        // 读取数据
        if (h->reading && h->fd >= 0 && FD_ISSET(h->fd, &readSet)) {
            char buf[4096];
            ssize_t n = recv(h->fd, buf, sizeof(buf), 0);
            if (n > 0) {
                if (h->readCb) h->readCb(buf, (int)n);
            } else if (n == 0) {
                // 对端关闭
                if (h->readCb) h->readCb(nullptr, -1);
                h->reading = false;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                if (h->readCb) h->readCb(nullptr, -errno);
                h->reading = false;
            }
        }

        // 服务端接受连接
        if (h->isServer && h->fd >= 0 && FD_ISSET(h->fd, &readSet)) {
            struct sockaddr_storage addr;
            socklen_t addrLen = sizeof(addr);
            int clientFd = accept(h->fd, (struct sockaddr*)&addr, &addrLen);
            if (clientFd >= 0) {
                SetNonBlocking(clientFd);
                auto* client = new uv_tcp_s{};
                client->fd = clientFd;
                client->connected = true;
                s_handles.push_back(client);
                if (h->acceptCb) h->acceptCb(client);
            }
        }
    }

    // 清理已关闭的句柄
    s_handles.erase(
        std::remove_if(s_handles.begin(), s_handles.end(),
            [](uv_tcp_s* h) {
                if (h->closing && h->fd < 0) { delete h; return true; }
                return false;
            }),
        s_handles.end());

    // Phase BC T3: 处理 UDP socket recvfrom (datagram 边界保留)
    auto udpHandles = s_udpHandles;
    for (auto* h : udpHandles) {
        if (h->fd < 0 || h->closing) continue;
        if (!h->reading || !FD_ISSET(h->fd, &readSet)) continue;
        // UDP 数据报最大 64KB; 一帧多个包时循环读直到 EAGAIN
        char buf[65536];
        struct sockaddr_storage from;
        while (true) {
            socklen_t fromLen = sizeof(from);
            ssize_t n = recvfrom(h->fd, buf, sizeof(buf), 0,
                                  (struct sockaddr*)&from, &fromLen);
            if (n <= 0) break;
            if (!h->recvCb) continue;
            // 解析对端地址
            char ipBuf[64] = {0};
            uint16_t port = 0;
            if (from.ss_family == AF_INET) {
                const auto* a4 = (const struct sockaddr_in*)&from;
                inet_ntop(AF_INET, &a4->sin_addr, ipBuf, sizeof(ipBuf));
                port = ntohs(a4->sin_port);
            } else if (from.ss_family == AF_INET6) {
                const auto* a6 = (const struct sockaddr_in6*)&from;
                inet_ntop(AF_INET6, &a6->sin6_addr, ipBuf, sizeof(ipBuf));
                port = ntohs(a6->sin6_port);
            }
            h->recvCb(ipBuf, port, buf, (int)n);
        }
    }

    // Phase BC T3: 清理已关闭的 UDP 句柄
    s_udpHandles.erase(
        std::remove_if(s_udpHandles.begin(), s_udpHandles.end(),
            [](uv_udp_s* h) {
                if (h->closing && h->fd < 0) { delete h; return true; }
                return false;
            }),
        s_udpHandles.end());

    // Phase BC T4: 驱动所有活跃 ENet host 一次 service
    EnetTickAll();
}

uv_tcp_s* CreateClient() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    SetNonBlocking(fd);
#ifdef SO_NOSIGPIPE
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif

    auto* h = new uv_tcp_s{};
    h->fd = fd;
    s_handles.push_back(h);
    return h;
}

void Connect(uv_tcp_s* handle, const char* host, uint16_t port, OnConnectCb cb) {
    if (!handle || handle->fd < 0) return;
    handle->connectCb = cb;

    // DNS 解析 (同步, 移动端通常毫秒级)
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%u", port);

    struct addrinfo* res = nullptr;
    int ret = getaddrinfo(host, portStr, &hints, &res);
    if (ret != 0 || !res) {
        CC::Log(CC::LOG_WARN, "PlatformNet: DNS resolve '%s' failed: %s",
                host, gai_strerror(ret));
        if (cb) cb(-1);
        return;
    }

    // 非阻塞 connect
    ret = connect(handle->fd, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);

    if (ret == 0) {
        // 立即连接成功
        handle->connected = true;
        if (cb) cb(0);
    } else if (errno == EINPROGRESS) {
        // 异步连接中
        handle->connecting = true;
    } else {
        CC::Log(CC::LOG_WARN, "PlatformNet: connect to %s:%u failed: %s",
                host, port, strerror(errno));
        if (cb) cb(-errno);
    }
}

void StartRead(uv_tcp_s* handle, OnReadCb cb) {
    if (!handle) return;
    handle->readCb = cb;
    handle->reading = true;
}

void StopRead(uv_tcp_s* handle) {
    if (handle) handle->reading = false;
}

void Write(uv_tcp_s* handle, const char* data, size_t len) {
    if (!handle || handle->fd < 0 || !data || len == 0) return;
    // 非阻塞发送 (大数据可能需要分片, 这里简化处理)
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(handle->fd, data + sent, len - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // 缓冲区满, 简单 busy-wait (生产环境应用 writable 回调)
            break;
        } else {
            break;
        }
    }
}

void Close(uv_tcp_s* handle) {
    if (!handle) return;
    if (handle->fd >= 0) {
        close(handle->fd);
        handle->fd = -1;
    }
    handle->closing = true;
    handle->reading = false;
    handle->connected = false;
}

uv_tcp_s* CreateServer(const char* ip, uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    SetNonBlocking(fd);

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &addr.sin_addr);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CC::Log(CC::LOG_ERROR, "PlatformNet: bind %s:%u failed: %s", ip, port, strerror(errno));
        close(fd);
        return nullptr;
    }

    auto* h = new uv_tcp_s{};
    h->fd = fd;
    h->isServer = true;
    s_handles.push_back(h);
    return h;
}

bool Listen(uv_tcp_s* server, int backlog, OnAcceptCb cb) {
    if (!server || server->fd < 0) return false;
    server->acceptCb = cb;
    server->backlog = backlog;
    if (listen(server->fd, backlog) < 0) {
        CC::Log(CC::LOG_ERROR, "PlatformNet: listen failed: %s", strerror(errno));
        return false;
    }
    return true;
}

// ==================== Phase BC T3: UDP POSIX 实现 ====================

uv_udp_s* CreateUdpSocket() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return nullptr;
    SetNonBlocking(fd);
    auto* h = new uv_udp_s{};
    h->fd = fd;
    s_udpHandles.push_back(h);
    return h;
}

bool BindUdp(uv_udp_s* sock, const char* ip, uint16_t port) {
    if (!sock || sock->fd < 0 || !ip) return false;
    int opt = 1;
    setsockopt(sock->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        // 0.0.0.0 / "any" 通过 INADDR_ANY 处理
        if (strcmp(ip, "0.0.0.0") == 0) {
            addr.sin_addr.s_addr = htonl(INADDR_ANY);
        } else {
            return false;
        }
    }
    if (bind(sock->fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        CC::Log(CC::LOG_WARN, "PlatformNet: UDP bind %s:%u failed: %s",
                ip, port, strerror(errno));
        return false;
    }
    return true;
}

bool SendUdp(uv_udp_s* sock, const char* host, uint16_t port,
              const char* data, size_t len) {
    if (!sock || sock->fd < 0 || !host || !data || len == 0) return false;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return false;
    ssize_t n = sendto(sock->fd, data, len, 0,
                       (const struct sockaddr*)&addr, sizeof(addr));
    return n == (ssize_t)len;
}

bool StartUdpRecv(uv_udp_s* sock, OnUdpRecvCb cb) {
    if (!sock) return false;
    sock->recvCb = std::move(cb);
    sock->reading = true;
    return true;
}

void StopUdpRecv(uv_udp_s* sock) {
    if (sock) sock->reading = false;
}

void CloseUdp(uv_udp_s* sock) {
    if (!sock) return;
    if (sock->fd >= 0) {
        close(sock->fd);
        sock->fd = -1;
    }
    sock->closing = true;
    sock->reading = false;
}

uint16_t GetUdpLocalPort(uv_udp_s* sock) {
    if (!sock || sock->fd < 0) return 0;
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (getsockname(sock->fd, (struct sockaddr*)&addr, &len) != 0) return 0;
    if (addr.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in*)&addr)->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6*)&addr)->sin6_port);
    }
    return 0;
}

uv_loop_s* GetLoop() { return nullptr; }

}  // namespace PlatformNet

#endif // __ANDROID__ || CHOCO_PLATFORM_IOS
