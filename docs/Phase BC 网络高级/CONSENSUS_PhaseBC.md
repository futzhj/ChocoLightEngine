# CONSENSUS — Phase BC（网络高级）最终共识

> **6A 工作流 Stage 2 — Architect 阶段产物 (第一部分)**
> 目的: 锁定 Stage 1 所有决策, 确认所有不确定性已解决, 作为 Stage 3 原子化任务的唯一输入.

---

## 一、明确需求描述

> 在 ChocoLight 中提供完整的游戏实时通信栈: UDP 抽象层 + ENet reliable channel + 简单 RPC + 房间状态同步, 覆盖桌面 + 移动双平台, 交付 3 个递进式 sample + loopback smoke + 完整 6A 文档.

---

## 二、最终决策矩阵

| # | 决议项 | 最终值 | 冻结状态 |
|---|--------|--------|----------|
| **1** | 传输层 | ENet + raw UDP 并存 | 🔒 |
| **2** | 协议层 | Raw UDP + RPC + Room 三层全栈 | 🔒 |
| **3** | 平台覆盖 | 桌面 (libuv) + 移动 (POSIX) | 🔒 |
| **4** | Demo 数量 | 3 (echo + room_sync + boss 战) | 🔒 |
| **5** | 安全可靠性 | ENet reliable channel (不加密) | 🔒 |
| **6** | ENet 版本 | 1.3.18 release tarball | 🔒 |
| **7** | ENet 放置 | `ChocoLight/third_party/enet/` | 🔒 |
| **8** | ENet 链接 | 静态 (`add_subdirectory` + `target_link_libraries`) | 🔒 |
| **9** | RPC 默认 timeout | 5000 ms | 🔒 |
| **10** | RPC 方向 | 仅 client → server (P2P 延后) | 🔒 |
| **11** | Room state 类型 | 仅 JSON 兼容 (table/string/number/bool) | 🔒 |
| **12** | Room 最大人数 | 32 | 🔒 |
| **13** | Master 选举 | 第一个连入; 离开时选次连入者 | 🔒 |
| **14** | State 复制模式 | Full sync (每次变更广播全表) | 🔒 |
| **15** | 默认 tick rate | 20 Hz (可用户调) | 🔒 |
| **16** | Reconnect | 不支持 (留 BC.x) | 🔒 |
| **17** | 序列化 | cJSON (已有依赖) | 🔒 |
| **18** | ENet channel | 0 = reliable ordered, 1 = unreliable sequenced | 🔒 |
| **19** | Demo minigame 类型 | 协作 boss 战 (共享 HP) | 🔒 |
| **20** | Demo 渲染层级 | 2D sprite + 动画 + 特效 (用 Phase AV/AS 资源) | 🔒 |
| **21** | smoke 范围 | 仅 loopback 127.0.0.1 (跨进程手动测) | 🔒 |

---

## 三、技术实现方案

### 3.1 分层架构

```
┌───────────────────────────────────────────────────────────┐
│                    Lua 游戏脚本                            │
├───────────────────────────────────────────────────────────┤
│  Light.Network.Udp / Light.Network.Rpc / Light.Network.Room│  ← Lua API
├───────────────────────────────────────────────────────────┤
│  light_network_udp.cpp / _rpc.cpp / _room.cpp              │  ← C++ 绑定
├───────────────────────────────────────────────────────────┤
│  JSON 序列化 (cJSON 复用) + 协议头定义                     │  ← 协议层
├───────────────────────────────────────────────────────────┤
│  PlatformNet 扩展: Raw UDP API + ENet API                 │  ← 抽象层
├───────────────────────────────────────────────────────────┤
│  libuv / POSIX socket / ENet 静态库                        │  ← 底层
└───────────────────────────────────────────────────────────┘
```

### 3.2 技术约束

| 约束 | 理由 |
|------|------|
| **不修改现有 TCP 路径** | Phase AY T01/T03 刚加固, 零回归风险优先 |
| **cJSON 专用序列化** | 已 link, 零新增依赖 |
| **ENet 纯静态链接** | 与 libuv / box2d / bullet3 风格一致 |
| **Web (Emscripten) 空存根** | 与 PlatformNet 现有移动/Web 条件编译模式一致 |
| **所有 RPC/Room 异步回调** | 禁止同步阻塞 Lua VM (与 Phase AT 一致) |
| **Poll 统一驱动** | ENet service tick 在 `PlatformNet::Poll()` 中与 libuv 协同 |

### 3.3 集成方案

1. **ENet vendor** 到 `ChocoLight/third_party/enet/`
2. **CMake** 条件编译:
   ```cmake
   if(NOT EMSCRIPTEN)
       add_subdirectory(third_party/enet)
       target_link_libraries(Light PRIVATE enet)
       target_compile_definitions(Light PRIVATE CHOCO_NET_ENET_ENABLED)
   endif()
   ```
3. **PlatformNet 扩展** 新增 UDP/ENet API (`light_platform_net.h`)
4. **3 个新 Lua 模块** (`light_network_udp.cpp` / `_rpc.cpp` / `_room.cpp`)
5. **现有 `light_network.cpp` 不改** (仅头文件引用)

---

## 四、验收标准

### 4.1 功能验收 (Must)

| 项 | 标准 |
|----|------|
| **Raw UDP** | loopback 双向 Send/Receive 成功, 包完整性验证 |
| **ENet** | 建立 host + peer 连接, reliable / unreliable 两种发送模式均工作 |
| **RPC Client.Call** | 成功路径: callback(nil, result); 超时: callback('timeout') |
| **Room Create + Join** | 32 人满负载不崩, master 离开自动 migration |
| **State 同步** | 变更后 ≤ 50ms 内所有 client 收到 (loopback 实测) |
| **3 demo** | 全部可独立运行 + README 给出启动步骤 |

### 4.2 质量验收 (Must)

| 项 | 标准 |
|----|------|
| **loopback smoke** | `scripts/smoke/network_advanced.lua` 全绿 |
| **跨平台构建** | Windows MSVC / Linux GCC / macOS Clang / Android NDK / iOS Xcode 全通过 |
| **向后兼容** | Phase AT 既有 `Light.Network.Http*` / `HttpServer*` 零改动 |
| **零内存泄漏** | userdata `__gc` 显式 Close + cJSON 释放 (类比 T01 模式) |
| **文档** | ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO 7 件齐全 |

### 4.3 边界验收 (Should)

| 项 | 标准 |
|----|------|
| **ENet 下线检测** | 5s 无心跳判定 disconnect |
| **RPC id 冲突** | 并发 Call 使用自增 id, 无混淆 |
| **房间满拒绝** | 第 33 人连接被拒, client 得明确 error |
| **Master migration** | 原 master 离开 ≤ 1 tick 内新 master 生效 |

---

## 五、任务边界 (Out of Scope)

明确**不在本阶段实施**, 如用户问起统一回复"Phase BC.x 或新 Phase":

- ❌ DTLS / TLS 加密
- ❌ NAT 穿透 (STUN/TURN)
- ❌ WebRTC (浏览器 UDP 替代)
- ❌ P2P / 双向 RPC
- ❌ Delta state 同步 (仅 full sync)
- ❌ Reconnect 机制
- ❌ 防作弊 server authoritative 严格模式
- ❌ 语音聊天 / WebSocket-over-UDP
- ❌ 多房间 hub 架构

---

## 六、不确定性清理确认

| Stage 1 的疑问 | 确认值 | 冻结 |
|---------------|--------|------|
| Q-A1 ENet 获取方式 | tarball 1.3.18 → `third_party/enet/` | ✅ |
| Q-A2 ENet 目录 | `third_party/enet/` (扁平) | ✅ |
| Q-B1 RPC timeout | 5000ms 默认 (可 per-call 覆盖) | ✅ |
| Q-B2 RPC 方向 | 仅 client → server | ✅ |
| Q-C1 State 类型 | JSON 兼容 | ✅ |
| Q-C2 Master 离开 | migration (选举次连入) | ✅ |
| Q-D1 玩法 | 协作 boss 战 | ✅ |
| Q-D2 渲染 | 2D sprite + 动画 + 特效 | ✅ |
| Q-E1 smoke | loopback only | ✅ |

**所有不确定性已关闭. 可进入 Stage 3 Atomize.**

---

## 七、项目特性规范对齐确认

- ✅ 与 ChocoLight "单体 Light.dll + Lua 绑定" 风格一致
- ✅ 与 PlatformNet 现有抽象 (桌面 libuv / 移动 POSIX / Web 空存根) 模式一致
- ✅ 与 Phase AT 既有 Http/WebSocket 模块不冲突, 仅新增并存
- ✅ 依赖管理与 cgltf/libuv/box2d/bullet3 同风格 (vendor + 静态链接)
- ✅ userdata __gc 资源释放与 Phase AY T01 Http 加固模式一致
- ✅ Lua API 异步回调与 Phase AT/AV Animator 事件模型一致
- ✅ smoke 与现有 `scripts/smoke/*.lua` BDD 断言风格一致

**共识达成. 架构设计细节进入 DESIGN_PhaseBC.md.**
