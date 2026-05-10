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

// ==================== 辅助 ====================

/// 获取底层 libuv 事件循环 (高级用途)
uv_loop_s* GetLoop();

}  // namespace PlatformNet
