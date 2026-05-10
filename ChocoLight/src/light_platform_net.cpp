/**
 * @file light_platform_net.cpp
 * @brief PlatformNet 实现 — 基于 libuv 的跨平台网络抽象
 */

#include "light_platform_net.h"
#include "light.h"

#if defined(__EMSCRIPTEN__)
// Web: 网络不可用 (libuv 和 POSIX socket 均不适用), 空存根
namespace PlatformNet {
bool Init() { return false; }
void Shutdown() {}
void Poll() {}
uv_tcp_s* CreateClient() { return nullptr; }
void Connect(uv_tcp_s*, const char*, uint16_t, OnConnectCb) {}
void StartRead(uv_tcp_s*, OnReadCb) {}
void StopRead(uv_tcp_s*) {}
void Write(uv_tcp_s*, const char*, size_t) {}
void Close(uv_tcp_s*) {}
uv_tcp_s* CreateServer(const char*, uint16_t) { return nullptr; }
bool Listen(uv_tcp_s*, int, OnAcceptCb) { return false; }
uv_loop_s* GetLoop() { return nullptr; }
// Phase BC T2: UDP Web 空存根 (浏览器无 raw UDP)
uv_udp_s* CreateUdpSocket() { return nullptr; }
bool BindUdp(uv_udp_s*, const char*, uint16_t) { return false; }
bool SendUdp(uv_udp_s*, const char*, uint16_t, const char*, size_t) { return false; }
bool StartUdpRecv(uv_udp_s*, OnUdpRecvCb) { return false; }
void StopUdpRecv(uv_udp_s*) {}
void CloseUdp(uv_udp_s*) {}
uint16_t GetUdpLocalPort(uv_udp_s*) { return 0; }
// Phase BC T4: ENet Web 空存根 (浏览器走 WebRTC, 不走 native ENet)
EnetHost* EnetCreateHost(const char*, uint16_t, int, int) { return nullptr; }
void EnetDestroyHost(EnetHost*) {}
EnetPeer* EnetConnect(EnetHost*, const char*, uint16_t, int) { return nullptr; }
void EnetDisconnect(EnetPeer*, uint32_t) {}
bool EnetSend(EnetPeer*, int, const char*, int, bool) { return false; }
void EnetSetEventCb(EnetHost*, OnEnetEventCb) {}
const char* EnetPeerAddress(EnetPeer*) { return ""; }
uint32_t EnetPeerID(EnetPeer*) { return 0; }
int EnetBroadcast(EnetHost*, int, const char*, int, bool) { return 0; }
bool EnetDisconnectPeerById(EnetHost*, uint32_t, uint32_t) { return false; }
void EnetSetFrameCb(EnetHost*, OnEnetFrameCb) {}
} // namespace PlatformNet
// Android/iOS: 由 light_platform_net_mobile.cpp 提供 POSIX socket 实现
#elif defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
// 空文件, 实现在 light_platform_net_mobile.cpp
#else

#include <uv.h>
#include <cstring>
#include <cstdlib>

// ==================== 内部状态 ====================

static uv_loop_t* s_loop = nullptr;

// 回调上下文: 附着在 uv_handle_t::data 上
struct NetHandleData {
    PlatformNet::OnReadCb   readCb;
    PlatformNet::OnConnectCb connectCb;
    PlatformNet::OnAcceptCb  acceptCb;
};

static NetHandleData* GetData(uv_tcp_s* h) {
    return h ? (NetHandleData*)((uv_handle_t*)h)->data : nullptr;
}

static NetHandleData* EnsureData(uv_tcp_s* h) {
    auto* hh = (uv_handle_t*)h;
    if (!hh->data) {
        hh->data = new NetHandleData{};
    }
    return (NetHandleData*)hh->data;
}

// ==================== libuv 回调 ====================

// 内存分配回调 (libuv 读取时调用)
static void alloc_cb(uv_handle_t*, size_t suggested, uv_buf_t* buf) {
    buf->base = (char*)malloc(suggested);
    buf->len  = (unsigned long)suggested;
}

// 读取回调
static void read_cb(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
    auto* nd = (NetHandleData*)((uv_handle_t*)stream)->data;
    if (nd && nd->readCb) {
        if (nread > 0) {
            nd->readCb(buf->base, (int)nread);
        } else if (nread < 0) {
            // EOF 或错误
            nd->readCb(nullptr, (int)nread);
        }
    }
    if (buf->base) free(buf->base);
}

// 连接回调
static void connect_cb(uv_connect_t* req, int status) {
    auto* handle = (uv_tcp_s*)req->handle;
    auto* nd = GetData(handle);
    if (nd && nd->connectCb) {
        nd->connectCb(status);
    }
    free(req);
}

// 写完成回调
struct WriteReq {
    uv_write_t req;
    uv_buf_t   buf;
};

static void write_cb(uv_write_t* req, int /*status*/) {
    auto* wr = (WriteReq*)req;
    free(wr->buf.base);
    free(wr);
}

// 关闭回调: 释放 NetHandleData
static void close_cb(uv_handle_t* handle) {
    if (handle->data) {
        delete (NetHandleData*)handle->data;
        handle->data = nullptr;
    }
    free(handle);
}

// 服务器连接回调
static void connection_cb(uv_stream_t* server, int status) {
    if (status < 0) return;
    auto* nd = (NetHandleData*)((uv_handle_t*)server)->data;
    if (!nd || !nd->acceptCb) return;

    auto* client = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    uv_tcp_init(s_loop, client);
    ((uv_handle_t*)client)->data = nullptr;

    if (uv_accept(server, (uv_stream_t*)client) == 0) {
        nd->acceptCb(client);
    } else {
        uv_close((uv_handle_t*)client, close_cb);
    }
}

// DNS 解析回调上下文
struct ResolveCtx {
    uv_getaddrinfo_t req;
    uv_tcp_s*        handle;
    uint16_t         port;
};

static void resolve_cb(uv_getaddrinfo_t* req, int status, struct addrinfo* res) {
    auto* ctx = (ResolveCtx*)req->data;
    if (status < 0 || !res) {
        auto* nd = GetData(ctx->handle);
        if (nd && nd->connectCb) nd->connectCb(status);
        free(ctx);
        if (res) uv_freeaddrinfo(res);
        return;
    }

    // 构建目标地址
    struct sockaddr_storage addr;
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    // 设置端口 (IPv4/IPv6)
    if (addr.ss_family == AF_INET) {
        ((struct sockaddr_in*)&addr)->sin_port = htons(ctx->port);
    } else if (addr.ss_family == AF_INET6) {
        ((struct sockaddr_in6*)&addr)->sin6_port = htons(ctx->port);
    }

    auto* creq = (uv_connect_t*)malloc(sizeof(uv_connect_t));
    uv_tcp_connect(creq, ctx->handle, (const struct sockaddr*)&addr, connect_cb);

    uv_freeaddrinfo(res);
    free(ctx);
}

// ==================== PlatformNet 实现 ====================

namespace PlatformNet {

// Phase BC T4: 跨 .cpp 链接 ENet 内部 init/tick/shutdown
//   实现位于 light_platform_net_enet.cpp (条件编译 !EMSCRIPTEN)
bool EnetGlobalInit();
void EnetGlobalShutdown();
void EnetTickAll();

bool Init() {
    if (s_loop) return true;
    s_loop = (uv_loop_t*)malloc(sizeof(uv_loop_t));
    if (uv_loop_init(s_loop) != 0) {
        free(s_loop);
        s_loop = nullptr;
        CC::Log(CC::LOG_ERROR, "PlatformNet: uv_loop_init failed");
        return false;
    }
    CC::Log(CC::LOG_INFO, "PlatformNet: initialized (libuv %s)", uv_version_string());
    // Phase BC T4: ENet 全局初始化 (失败不阻塞 libuv, ENet 模块降级不可用)
    EnetGlobalInit();
    return true;
}

void Shutdown() {
    if (!s_loop) return;
    // Phase BC T4: 先停 ENet (销毁所有 host, 防止 libuv close 与 ENet socket 抢)
    EnetGlobalShutdown();
    // 关闭所有活跃句柄
    uv_walk(s_loop, [](uv_handle_t* h, void*) {
        if (!uv_is_closing(h)) uv_close(h, close_cb);
    }, nullptr);
    // 运行直到所有句柄关闭
    while (uv_loop_alive(s_loop)) {
        uv_run(s_loop, UV_RUN_ONCE);
    }
    uv_loop_close(s_loop);
    free(s_loop);
    s_loop = nullptr;
    CC::Log(CC::LOG_INFO, "PlatformNet: shutdown");
}

void Poll() {
    if (s_loop) uv_run(s_loop, UV_RUN_NOWAIT);
    // Phase BC T4: 驱动所有活跃 ENet host 一次 service
    EnetTickAll();
}

uv_tcp_s* CreateClient() {
    if (!s_loop) return nullptr;
    auto* h = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    if (uv_tcp_init(s_loop, h) != 0) {
        free(h);
        return nullptr;
    }
    ((uv_handle_t*)h)->data = nullptr;
    return h;
}

void Connect(uv_tcp_s* handle, const char* host, uint16_t port, OnConnectCb cb) {
    if (!handle || !s_loop) return;
    auto* nd = EnsureData(handle);
    nd->connectCb = cb;

    // 尝试直接解析为 IP
    struct sockaddr_in addr4;
    struct sockaddr_in6 addr6;
    if (uv_ip4_addr(host, port, &addr4) == 0) {
        auto* req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
        uv_tcp_connect(req, handle, (const struct sockaddr*)&addr4, connect_cb);
        return;
    }
    if (uv_ip6_addr(host, port, &addr6) == 0) {
        auto* req = (uv_connect_t*)malloc(sizeof(uv_connect_t));
        uv_tcp_connect(req, handle, (const struct sockaddr*)&addr6, connect_cb);
        return;
    }

    // DNS 异步解析
    auto* ctx = (ResolveCtx*)malloc(sizeof(ResolveCtx));
    ctx->handle = handle;
    ctx->port   = port;
    ctx->req.data = ctx;

    struct addrinfo hints = {};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    uv_getaddrinfo(s_loop, &ctx->req, resolve_cb, host, nullptr, &hints);
}

void StartRead(uv_tcp_s* handle, OnReadCb cb) {
    if (!handle) return;
    auto* nd = EnsureData(handle);
    nd->readCb = cb;
    uv_read_start((uv_stream_t*)handle, alloc_cb, read_cb);
}

void StopRead(uv_tcp_s* handle) {
    if (handle) uv_read_stop((uv_stream_t*)handle);
}

void Write(uv_tcp_s* handle, const char* data, size_t len) {
    if (!handle || !data || len == 0) return;
    auto* wr = (WriteReq*)malloc(sizeof(WriteReq));
    wr->buf.base = (char*)malloc(len);
    memcpy(wr->buf.base, data, len);
    wr->buf.len = (unsigned long)len;
    uv_write(&wr->req, (uv_stream_t*)handle, &wr->buf, 1, write_cb);
}

void Close(uv_tcp_s* handle) {
    if (!handle) return;
    if (!uv_is_closing((uv_handle_t*)handle)) {
        uv_close((uv_handle_t*)handle, close_cb);
    }
}

uv_tcp_s* CreateServer(const char* ip, uint16_t port) {
    if (!s_loop) return nullptr;
    auto* h = (uv_tcp_t*)malloc(sizeof(uv_tcp_t));
    if (uv_tcp_init(s_loop, h) != 0) {
        free(h);
        return nullptr;
    }
    ((uv_handle_t*)h)->data = nullptr;

    struct sockaddr_in addr;
    uv_ip4_addr(ip, port, &addr);
    if (uv_tcp_bind(h, (const struct sockaddr*)&addr, 0) != 0) {
        CC::Log(CC::LOG_ERROR, "PlatformNet: bind %s:%u failed", ip, port);
        uv_close((uv_handle_t*)h, close_cb);
        return nullptr;
    }
    return h;
}

bool Listen(uv_tcp_s* server, int backlog, OnAcceptCb cb) {
    if (!server) return false;
    auto* nd = EnsureData(server);
    nd->acceptCb = cb;
    int r = uv_listen((uv_stream_t*)server, backlog, connection_cb);
    if (r != 0) {
        CC::Log(CC::LOG_ERROR, "PlatformNet: listen failed (%s)", uv_strerror(r));
        return false;
    }
    return true;
}

uv_loop_s* GetLoop() { return s_loop; }

}  // namespace PlatformNet

// ==================== Phase BC T2: UDP libuv 桌面实现 ====================
//
// 设计要点:
//   - UdpHandleData 与 NetHandleData 分开: UDP 没有 connect/accept 概念,
//     只需 recv 回调 + 包级生命周期; 共用 NetHandleData 会膨胀字段
//   - 用 uv_udp_t 而非 uv_tcp_t: libuv 强类型区分两者, recv API 也不同
//   - alloc_cb 不能复用 (TCP 用的是 alloc_cb 接收 stream, UDP 接收 datagram),
//     但语义一致, 用同一份 alloc_cb 即可

namespace {

struct UdpHandleData {
    PlatformNet::OnUdpRecvCb recvCb;
};

UdpHandleData* GetUdpData(uv_udp_s* h) {
    return h ? (UdpHandleData*)((uv_handle_t*)h)->data : nullptr;
}

UdpHandleData* EnsureUdpData(uv_udp_s* h) {
    auto* hh = (uv_handle_t*)h;
    if (!hh->data) {
        hh->data = new UdpHandleData{};
    }
    return (UdpHandleData*)hh->data;
}

// UDP 句柄关闭回调: 释放 UdpHandleData + handle 内存
void udp_close_cb(uv_handle_t* handle) {
    if (handle->data) {
        delete (UdpHandleData*)handle->data;
        handle->data = nullptr;
    }
    free(handle);
}

// UDP 接收回调: 解析对端地址 + 触发用户 cb
void udp_recv_cb(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf,
                  const struct sockaddr* addr, unsigned /*flags*/) {
    auto* nd = (UdpHandleData*)((uv_handle_t*)handle)->data;
    if (nread > 0 && addr && nd && nd->recvCb) {
        // 解析对端地址 → 字符串 + 端口
        char ipBuf[64] = {0};
        uint16_t port = 0;
        if (addr->sa_family == AF_INET) {
            const auto* a4 = (const struct sockaddr_in*)addr;
            uv_ip4_name(a4, ipBuf, sizeof(ipBuf));
            port = ntohs(a4->sin_port);
        } else if (addr->sa_family == AF_INET6) {
            const auto* a6 = (const struct sockaddr_in6*)addr;
            uv_ip6_name(a6, ipBuf, sizeof(ipBuf));
            port = ntohs(a6->sin6_port);
        }
        nd->recvCb(ipBuf, port, buf->base, (int)nread);
    }
    // libuv 文档: nread==0 是 "no data this poll", 不是错误, 也无 free 责任
    // nread<0 是错误, buf->base 仍需 free
    if (buf && buf->base) free(buf->base);
}

// UDP 发送完成回调: 释放 send req + 数据缓冲
struct UdpSendReq {
    uv_udp_send_t req;
    uv_buf_t      buf;
};

void udp_send_cb(uv_udp_send_t* req, int /*status*/) {
    auto* sr = (UdpSendReq*)req;
    free(sr->buf.base);
    free(sr);
}

}  // anonymous namespace

namespace PlatformNet {

uv_udp_s* CreateUdpSocket() {
    if (!s_loop) return nullptr;
    auto* h = (uv_udp_t*)malloc(sizeof(uv_udp_t));
    if (uv_udp_init(s_loop, h) != 0) {
        free(h);
        return nullptr;
    }
    ((uv_handle_t*)h)->data = nullptr;
    return h;
}

bool BindUdp(uv_udp_s* sock, const char* ip, uint16_t port) {
    if (!sock || !ip) return false;
    // 试 IPv4 再试 IPv6
    struct sockaddr_in  a4;
    struct sockaddr_in6 a6;
    if (uv_ip4_addr(ip, port, &a4) == 0) {
        return uv_udp_bind(sock, (const struct sockaddr*)&a4, 0) == 0;
    }
    if (uv_ip6_addr(ip, port, &a6) == 0) {
        return uv_udp_bind(sock, (const struct sockaddr*)&a6, 0) == 0;
    }
    return false;
}

bool SendUdp(uv_udp_s* sock, const char* host, uint16_t port,
              const char* data, size_t len) {
    if (!sock || !host || !data || len == 0) return false;
    struct sockaddr_storage addr;
    if (uv_ip4_addr(host, port, (struct sockaddr_in*)&addr) != 0 &&
        uv_ip6_addr(host, port, (struct sockaddr_in6*)&addr) != 0) {
        return false;
    }
    auto* sr = (UdpSendReq*)malloc(sizeof(UdpSendReq));
    sr->buf.base = (char*)malloc(len);
    memcpy(sr->buf.base, data, len);
    sr->buf.len = (unsigned long)len;
    int r = uv_udp_send(&sr->req, sock, &sr->buf, 1,
                         (const struct sockaddr*)&addr, udp_send_cb);
    if (r != 0) {
        free(sr->buf.base);
        free(sr);
        return false;
    }
    return true;
}

bool StartUdpRecv(uv_udp_s* sock, OnUdpRecvCb cb) {
    if (!sock) return false;
    auto* nd = EnsureUdpData(sock);
    nd->recvCb = std::move(cb);
    int r = uv_udp_recv_start(sock, alloc_cb, udp_recv_cb);
    return r == 0 || r == UV_EALREADY;
}

void StopUdpRecv(uv_udp_s* sock) {
    if (sock) uv_udp_recv_stop(sock);
}

void CloseUdp(uv_udp_s* sock) {
    if (!sock) return;
    if (!uv_is_closing((uv_handle_t*)sock)) {
        uv_close((uv_handle_t*)sock, udp_close_cb);
    }
}

uint16_t GetUdpLocalPort(uv_udp_s* sock) {
    if (!sock) return 0;
    struct sockaddr_storage addr;
    int len = (int)sizeof(addr);
    if (uv_udp_getsockname(sock, (struct sockaddr*)&addr, &len) != 0) return 0;
    if (addr.ss_family == AF_INET) {
        return ntohs(((struct sockaddr_in*)&addr)->sin_port);
    } else if (addr.ss_family == AF_INET6) {
        return ntohs(((struct sockaddr_in6*)&addr)->sin6_port);
    }
    return 0;
}

}  // namespace PlatformNet

#endif // !__EMSCRIPTEN__ && !__ANDROID__ && !CHOCO_PLATFORM_IOS (libuv 桌面实现)
