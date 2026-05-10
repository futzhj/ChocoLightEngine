/**
 * @file light_platform_net.h
 * @brief 跨平台网络抽象层 — 基于 libuv
 *
 * 封装 TCP 客户端/服务器操作, 替代直接 WinSock/POSIX socket 调用。
 * 设计目标: 与现有 Lua API 兼容, 每帧调用 PlatformNet::Poll() 驱动事件循环。
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>

// 前向声明 libuv 类型, 避免头文件污染
struct uv_loop_s;
struct uv_tcp_s;
struct uv_udp_s;       // Phase BC T2: UDP 句柄 (mobile 端用 fd-based 自定义 struct)
struct uv_connect_s;

namespace PlatformNet {

// ==================== 回调类型 ====================

/// 连接建立回调 (status: 0=成功, <0=失败)
using OnConnectCb   = std::function<void(int status)>;
/// 数据接收回调 (data=nullptr 且 len<0 表示对端关闭/错误)
using OnReadCb      = std::function<void(const char* data, int len)>;
/// 新客户端连接回调 (clientHandle: 已接受的 TCP 句柄, 调用者拥有)
using OnAcceptCb    = std::function<void(uv_tcp_s* clientHandle)>;
/// Phase BC T2: UDP 数据接收回调
///   fromHost: 来源 IP 字符串 ("192.168.1.1" / "::1")
///   fromPort: 来源端口 (host order)
///   data/len: 数据指针 (调用方拷贝, 回调返回后失效)
using OnUdpRecvCb   = std::function<void(const char* fromHost, uint16_t fromPort,
                                          const char* data, int len)>;

// ==================== 生命周期 ====================

/// 初始化 libuv 事件循环 (应用启动时调用一次)
bool Init();
/// 关闭事件循环并释放资源
void Shutdown();
/// 驱动一次事件循环迭代 (每帧调用, 非阻塞)
void Poll();

// ==================== TCP 客户端 ====================

/// 创建 TCP 客户端句柄 (返回 nullptr 失败)
uv_tcp_s* CreateClient();
/// 异步连接到远程主机
void Connect(uv_tcp_s* handle, const char* host, uint16_t port, OnConnectCb cb);
/// 开始读取数据 (持续触发 cb 直到 StopRead 或关闭)
void StartRead(uv_tcp_s* handle, OnReadCb cb);
/// 停止读取
void StopRead(uv_tcp_s* handle);
/// 发送数据 (异步, 内部拷贝 data)
void Write(uv_tcp_s* handle, const char* data, size_t len);
/// 关闭句柄并释放关联资源
void Close(uv_tcp_s* handle);

// ==================== TCP 服务器 ====================

/// 创建 TCP 服务器并绑定地址, 返回句柄 (nullptr 失败)
uv_tcp_s* CreateServer(const char* ip, uint16_t port);
/// 开始监听连接
bool Listen(uv_tcp_s* server, int backlog, OnAcceptCb cb);

// ==================== Phase BC T2: Raw UDP ====================
//
// 与 TCP 风格一致: 句柄以不透明指针 uv_udp_s* 暴露, 实际类型在 .cpp 中定义.
//   - 桌面: uv_udp_t (libuv 原生)
//   - 移动: 自定义 fd-based struct (与 TCP uv_tcp_s 同处方)
//   - Web : 全部空存根
//
// 使用模式:
//   auto* sock = CreateUdpSocket();
//   BindUdp(sock, "0.0.0.0", 9000);
//   StartUdpRecv(sock, [](const char* host, uint16_t port,
//                          const char* data, int len) { ... });
//   SendUdp(sock, "127.0.0.1", 9000, "hello", 5);
//   // ... 每帧 Poll() ...
//   CloseUdp(sock);

/// 创建 UDP socket (返回 nullptr 失败)
uv_udp_s* CreateUdpSocket();

/// 绑定到本地地址
///   ip   = "0.0.0.0" / "127.0.0.1" / "::" 等; 不解析 DNS
///   port = 0 表示让系统分配, 实际端口可用 GetUdpLocalPort 查询
/// 失败返回 false (端口占用/格式错误)
bool BindUdp(uv_udp_s* sock, const char* ip, uint16_t port);

/// 发送数据报到目标 host:port
///   host = "127.0.0.1" / "192.168.1.1" / "::1" 等 (不解析 DNS, 用户自行解析)
///   data 内部拷贝, 调用方可立即释放
/// 返回 true 表示已加入发送队列 (实际发送在 libuv 异步)
bool SendUdp(uv_udp_s* sock, const char* host, uint16_t port,
              const char* data, size_t len);

/// 开始接收数据报, cb 在 Poll 中触发 (主线程同步)
bool StartUdpRecv(uv_udp_s* sock, OnUdpRecvCb cb);

/// 停止接收 (回调不再触发, 但 socket 仍存活, 可重新 StartUdpRecv)
void StopUdpRecv(uv_udp_s* sock);

/// 关闭 socket 并释放资源 (回调随之失效)
void CloseUdp(uv_udp_s* sock);

/// 查询本地实际绑定端口 (BindUdp(port=0) 后用), 失败返回 0
uint16_t GetUdpLocalPort(uv_udp_s* sock);

// ==================== Phase BC T4: ENet (reliable UDP) ====================
//
// 封装 ENet 1.3.18 reliable UDP library. 与 raw UDP API 并存:
//   - Raw UDP: 用户自定协议, 无重传/排序保证
//   - ENet:    channel-based, 可选 reliable/unreliable, 自动重传 + congestion control
//
// 平台覆盖: 桌面 + 移动 (Web 编译时空存根, 浏览器走 WebRTC)
// 编译宏:   CHOCO_NET_ENET_ENABLED (CMake 设置, 与 EMSCRIPTEN 互斥)
//
// 典型用法:
//   // Server (host)
//   auto* host = EnetCreateHost("0.0.0.0", 9000, /*maxPeers=*/32, /*channels=*/2);
//   // Client (peer to remote)
//   auto* host = EnetCreateHost(nullptr, 0, 1, 2);
//   auto* peer = EnetConnect(host, "127.0.0.1", 9000, 2);
//   ...
//   // 每帧由 PlatformNet::Poll() 自动调 EnetServiceTick
//   EnetSend(peer, 0, payload, len, /*reliable=*/true);
//   ...
//   EnetDestroyHost(host);

struct EnetHost;       // 不透明 (实际 = ENetHost*)
struct EnetPeer;       // 不透明 (实际 = ENetPeer*)

enum class EnetEventType : int {
    NONE       = 0,
    CONNECT    = 1,
    DISCONNECT = 2,
    RECEIVE    = 3,
};

struct EnetEvent {
    EnetEventType type;
    EnetPeer*     peer;        // 触发事件的 peer (CONNECT/DISCONNECT/RECEIVE 均有效)
    int           channel;     // RECEIVE 时为收包 channel (CONNECT/DISCONNECT 为 0)
    const char*   data;        // RECEIVE 时为包数据 (调用方在回调内拷贝, 返回后失效)
    int           len;         // RECEIVE 时为包长度
};

using OnEnetEventCb = std::function<void(const EnetEvent&)>;

/// 创建 ENet host
///   ip       = nullptr 表示纯 client 模式 (不绑定监听); "0.0.0.0" 监听所有接口
///   port     = 监听端口 (client 模式忽略, 通常传 0)
///   maxPeers = 最大同时连接 peer 数 (server: 房间容量; client: 通常 1)
///   channels = channel 数 (DESIGN §3.4: 0=reliable ordered, 1=unreliable seq)
/// 返回 nullptr 表示创建失败 (端口占用 / Web 平台 / ENet 未初始化)
EnetHost* EnetCreateHost(const char* ip, uint16_t port,
                          int maxPeers, int channels);

/// 销毁 host (会断开所有 peer 并释放). 调用后 host/peer 指针失效
void EnetDestroyHost(EnetHost* host);

/// 主动连接到远程 host
///   localHost = 本机 EnetHost (client 或 server 都可发起连接)
///   host/port = 目标地址
///   channels  = 必须 ≤ localHost 创建时的 channels
/// 返回 EnetPeer* 表示已加入连接队列 (实际 CONNECT 事件由 EnetServiceTick 触发);
/// nullptr 表示参数错或资源耗尽
EnetPeer* EnetConnect(EnetHost* localHost,
                       const char* host, uint16_t port,
                       int channels);

/// 主动断开 peer
///   data = 用户数据 (会传到对端 DISCONNECT 事件的 peer-data, 用于 KICK reason 等)
void EnetDisconnect(EnetPeer* peer, uint32_t data);

/// 通过 channel 发送数据
///   channel  = 0..N-1, N = host 创建时的 channels
///   reliable = true: 保证送达 + 顺序; false: 不保证
/// 返回 false 表示参数错或 channel 越界
bool EnetSend(EnetPeer* peer, int channel,
               const char* data, int len, bool reliable);

/// 注册 host 事件回调 (持续生效, 直到 host 销毁或重新注册)
///
/// 设计 (DESIGN §6.1): PlatformNet::Poll() 自动遍历所有活跃 host 并 service
/// 一次, 收到的事件用此 cb 同步分发. 用户无需手动驱动.
///
/// cb 在主线程触发, 与 Lua VM 同帧, 适合直接调 Lua callback ref. 无需 mutex.
void EnetSetEventCb(EnetHost* host, OnEnetEventCb cb);

/// 查询 peer 远程地址 ("192.168.1.100:9000" 风格), 返回静态 buffer (下次调用覆盖)
const char* EnetPeerAddress(EnetPeer* peer);

/// 查询 peer ID (host 内部分配的小整数, 0 ~ maxPeers-1)
uint32_t    EnetPeerID(EnetPeer* peer);

/// 广播数据到 host 下所有已连接 peer (Phase BC T8 房间事件 / 状态用)
///   channel  = 0..N-1, 同 EnetSend
///   reliable = true: 所有 peer 都用 RELIABLE flag; false: 默认 unsequenced
/// 返回成功投递包数 (queue 入队, 不保证到达)
int EnetBroadcast(EnetHost* host, int channel,
                   const char* data, int len, bool reliable);

/// 按 peer ID 查找并断开 (T8 房间 Kick 用)
///   返回 true 表示找到并发出 disconnect 请求
bool EnetDisconnectPeerById(EnetHost* host, uint32_t peerId, uint32_t userData);

/// 注册 host 的"帧末 idle 回调" — 每次 PlatformNet::Poll 中 EnetTickAll
/// 处理完该 host 的所有事件后, 无论是否有事件都会调用一次此 cb.
/// 用途: 周期性扫描 (例如 RPC pending timeout 检查), 不依赖 ENet 事件触发.
/// 传 nullptr 取消注册.
typedef std::function<void()> OnEnetFrameCb;
void EnetSetFrameCb(EnetHost* host, OnEnetFrameCb cb);

// ==================== 辅助 ====================

/// 获取底层 libuv 事件循环 (高级用途)
uv_loop_s* GetLoop();

}  // namespace PlatformNet
