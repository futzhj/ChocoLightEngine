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
struct uv_connect_s;

namespace PlatformNet {

// ==================== 回调类型 ====================

/// 连接建立回调 (status: 0=成功, <0=失败)
using OnConnectCb   = std::function<void(int status)>;
/// 数据接收回调 (data=nullptr 且 len<0 表示对端关闭/错误)
using OnReadCb      = std::function<void(const char* data, int len)>;
/// 新客户端连接回调 (clientHandle: 已接受的 TCP 句柄, 调用者拥有)
using OnAcceptCb    = std::function<void(uv_tcp_s* clientHandle)>;

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

// ==================== 辅助 ====================

/// 获取底层 libuv 事件循环 (高级用途)
uv_loop_s* GetLoop();

}  // namespace PlatformNet
