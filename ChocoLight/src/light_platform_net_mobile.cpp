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

// 全局句柄列表 (Poll 遍历)
static std::vector<uv_tcp_s*> s_handles;
static bool s_initialized = false;

// 设置非阻塞
static void SetNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ==================== PlatformNet 实现 ====================

namespace PlatformNet {

bool Init() {
    s_initialized = true;
    CC::Log(CC::LOG_INFO, "PlatformNet: initialized (POSIX socket, mobile)");
    return true;
}

void Shutdown() {
    for (auto* h : s_handles) {
        if (h->fd >= 0) { close(h->fd); h->fd = -1; }
        delete h;
    }
    s_handles.clear();
    s_initialized = false;
    CC::Log(CC::LOG_INFO, "PlatformNet: shutdown");
}

void Poll() {
    if (!s_initialized || s_handles.empty()) return;

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

uv_loop_s* GetLoop() { return nullptr; }

}  // namespace PlatformNet

#endif // __ANDROID__ || CHOCO_PLATFORM_IOS
