# ALIGNMENT — Phase BC（网络高级 — UDP / RPC / 房间同步）

> **6A 工作流 Stage 1 — Align 阶段产物**
> 目的: 把"加 UDP + RPC + 房间同步"这一模糊需求转化为精确、可实施、与 ChocoLight 现有 PlatformNet/Network 架构对齐的规范.

---

## 一、项目和任务特性规范

### 1.1 工作空间

- **项目**: `futzhj/ChocoLightEngine` (`e:\jinyiNew\Light`)
- **引擎**: ChocoLight (Lumen + SDL3 + OpenGL/GLES + Lua 5.1)
- **本阶段**: Phase BC, 紧接 Phase AY (清理收尾) 之后, 首个新功能阶段

### 1.2 范围声明

**本阶段做** (用户选择激进范围 A2+B1+B2+B3+C1+D1+D2+D3+E2):

- 集成 ENet (~5K 行 MIT C 库) 作为 reliable UDP 后端, 与现有 PlatformNet TCP 并存
- 桌面 (libuv) + 移动 (POSIX) 双平台 UDP 抽象层 (PlatformNet 扩展)
- Lua 层 `Light.Network.Udp.*` raw UDP 模块 (Send / Receive / Broadcast)
- Lua 层 `Light.Network.Rpc.*` 简单 RPC (Server.Register + Client.Call, JSON-RPC 风格)
- Lua 层 `Light.Network.Room.*` 房间状态同步 (master client + state replication)
- 3 个 sample: `demo_udp_echo` + `demo_room_sync` + `demo_io_minigame`
- ENet channel-based reliable: 至少 2 channel (0=reliable ordered, 1=unreliable)
- smoke 覆盖 (loopback 127.0.0.1) + 文档闭环

**本阶段不做** (明确排除):

- ❌ DTLS / 加密 (E1 选定: 不可靠 + 不加密, 加密留 HTTPS/WSS)
- ❌ NAT 穿透 (STUN/TURN, 留 Phase BC.x)
- ❌ Steam / GameNetworkingSockets / yojimbo 集成 (与 ENet 并存代价过高)
- ❌ Web (Emscripten) UDP 支持 (浏览器走 WebRTC, 与 native UDP API 不兼容)
- ❌ 防作弊 / server authoritative 严格模式 (master client 信任模型, 适合 LAN/朋友局)
- ❌ 跨 ChocoLight 实例进程通信 (与"网络"无关)

---

## 二、原始需求

> "Phase BC 网络高级 — UDP + RPC + 房间状态同步, 紧接 AT 网络模块, 需双端 demo 验证, 外部库选型需探讨."

来源:
- Phase AY FINAL.md §十"下一阶段建议"中明确列为候选
- 用户在 Phase AY 完成后的 Stage 6 询问中选择"启动新阶段" → 候选清单中选定 BC
- Phase BC 范围决策中选择 "激进范围 (A2+B1+B2+B3+C1+D1+D2+D3+E2)"

业务驱动:
- Phase AT 网络模块仅有 TCP/HTTP/WebSocket, 缺少游戏实时通信刚需的 UDP
- Lua 游戏开发者最常见诉求: "我想做个 .io 多人小游戏" — 需要 UDP + 房间
- 已有 cJSON 依赖, 序列化基建 ready
- libuv 已有 `uv_udp_*` API, 但未通过 PlatformNet 暴露

---

## 三、边界确认

### 3.1 范围矩阵 (按用户选择展开)

| 维度 | 选项 | 决策 | 理由 |
|------|------|------|------|
| **Q1 传输层** | A1/A2/A3 | **A2** ENet + raw UDP 并存 | 用户选激进; ENet 提供 reliable channel 节约自实现工作量 |
| **Q2 协议层** | B1/B2/B3 | **B1+B2+B3** 全栈 | Raw UDP 兜底 + RPC 易用 + 房间同步刚需 |
| **Q3 平台覆盖** | C1/C2 | **C1** 桌面 + 移动 | 与现有 PlatformNet 一致 |
| **Q4 demo 范围** | D1/D2/D3 | **D1+D2+D3** 三 demo | 完整 .io minigame 验证全栈 |
| **Q5 安全可靠性** | E1/E2/E3 | **E2** ENet reliable | E1 不够; E3 加密对游戏过度 |

### 3.2 ENet 集成方式

- **依赖管理**: 直接 vendor 到 `ChocoLight/third_party/enet/` (与 cgltf/libuv 风格一致)
- **链接方式**: 静态链接 (与 libuv/box2d 一致)
- **License 兼容性**: ENet MIT, 与项目 license 兼容
- **CMake**: `add_subdirectory(third_party/enet)` + `target_link_libraries(Light PRIVATE enet)`
- **平台条件编译**:
  - 桌面 / 移动: 启用 ENet
  - Web (Emscripten): 编译时跳过 (与 PlatformNet 移动端逻辑同模式)

### 3.3 Lua API 表面

```lua
-- ============ 1. Raw UDP (B1) ============
local socket = Light.Network.Udp.Open(port)         -- 监听 port (server) 或 0 (client)
socket:Send(host, port, data)                       -- 发送 UDP packet
socket:OnReceive(function(host, port, data) ... end)
socket:Close()

-- ============ 2. RPC (B2) ============
-- Server 端
local server = Light.Network.Rpc.Server(port)
server:Register('greet', function(client, name)
    return 'hello, ' .. name
end)
server:Start()

-- Client 端
local client = Light.Network.Rpc.Client('127.0.0.1', port)
client:Connect()
client:Call('greet', 'world', function(err, result)
    print(result)  -- 'hello, world'
end)

-- ============ 3. Room Sync (B3) ============
local room = Light.Network.Room.Create({
    port = 9000,
    maxPlayers = 32,
    state = { players = {} },          -- 初始状态
})
room:OnPlayerJoin(function(player) ... end)
room:OnStateChange(function(newState, oldState) ... end)
room:Broadcast({ type = 'chat', text = 'hi' })

local client = Light.Network.Room.Join('127.0.0.1', 9000, 'PlayerName')
client:OnState(function(state) ... end)
client:Send({ type = 'move', x = 10, y = 20 })
```

### 3.4 ENet channel 分配方案

| Channel | 模式 | 用途 |
|---------|------|------|
| **0** | Reliable Ordered | RPC 调用 + 关键状态同步 (玩家加入/离开 / 房间元数据) |
| **1** | Unreliable Sequenced | 高频状态广播 (位置 / 输入) |

后续可扩展更多 channel, 但不在本阶段.

### 3.5 序列化方案

- **RPC + Room**: cJSON (现有依赖, 0 新增成本)
- **Raw UDP**: 用户自定义 (引擎不强加)
- **包格式**:
  ```
  RPC Request:  {"id":1, "method":"greet", "params":["world"]}
  RPC Response: {"id":1, "result":"hello, world"}
  Room State:   {"type":"state", "rev":42, "data":{...}}
  Room Event:   {"type":"event", "name":"join", "player":{...}}
  ```

### 3.6 房间同步语义

| 决策点 | 选择 |
|--------|------|
| **Master 选举** | 第一个连入的客户端为 master (服务端仅做转发, 不做逻辑) |
| **State 复制** | Full sync (整表 broadcast on change) — 实施简单, 后续可优化 delta |
| **房间最大人数** | 32 (业界 .io 主流, ENet 默认 peer 上限 4096 远够用) |
| **Master 离开** | 自动选举次连入者; 若无人剩余则销毁房间 |
| **Reconnect** | 不支持 (用户重新 Join), 留 Phase BC.x |
| **Tick rate** | 用户控制 (默认 20 Hz 状态广播, 可调) |

---

## 四、对现有项目的需求理解

### 4.1 关键代码勘察结论

| 项目 | 当前状态 | 引用 |
|------|---------|------|
| **PlatformNet 抽象** | 仅 TCP API: `CreateClient/Connect/Write/StartRead/Close/CreateServer/Listen` | `light_platform_net.h` |
| **PlatformNet 桌面** | libuv `uv_tcp_*`; `uv_udp_*` 已可用未暴露 | `light_platform_net.cpp` |
| **PlatformNet 移动** | POSIX `socket(SOCK_STREAM)`; `SOCK_DGRAM` 已可用未暴露 | `light_platform_net_mobile.cpp` |
| **PlatformNet Web** | 空存根 (Emscripten 不支持 native socket) | `light_platform_net.cpp:9-24` |
| **Light.Network.Http** | TCP 客户端 + WebSocket 升级 | `light_network.cpp:269-528` |
| **Light.Network.HttpServer** | TCP 服务端 | `light_network.cpp:530-617` |
| **cJSON 依赖** | 已存在 (`third_party/cJSON`), 用于 HttpServer routing | `CMakeLists.txt` |
| **libuv 版本** | 已 vendor, `uv_udp_*` API 完整 | `third_party/libuv/` |

### 4.2 集成切入点

1. **PlatformNet 扩展** (新增 UDP API, 不改 TCP):
   ```cpp
   namespace PlatformNet {
       // 新增 (与 TCP 同语义但 SOCK_DGRAM)
       uv_udp_s* CreateUdpSocket();
       bool BindUdp(uv_udp_s*, const char* ip, uint16_t port);
       int SendUdp(uv_udp_s*, const char* host, uint16_t port, const char* data, size_t len);
       void StartUdpRecv(uv_udp_s*, OnUdpRecvCb cb);
       void CloseUdp(uv_udp_s*);

       // ENet 抽象 (新增, 与 raw UDP 并存)
       struct EnetHost;  // forward decl
       EnetHost* EnetCreateHost(const char* ip, uint16_t port, int peerCount, int channels);
       void EnetDestroyHost(EnetHost*);
       void EnetServiceTick(EnetHost*, OnEnetEventCb cb);
       // ... peer connect / send / disconnect ...
   }
   ```

2. **ENet vendor** (`third_party/enet/`):
   - 下载 ENet 1.3.18 release tarball (latest stable)
   - 解压到 `ChocoLight/third_party/enet/`
   - CMake `add_subdirectory` 集成
   - Mobile (Android/iOS): ENet 跨平台原生支持 (内部用 BSD socket)

3. **新增 Lua 模块** (3 个新文件):
   - `light_network_udp.cpp` (~300 行) — Light.Network.Udp.*
   - `light_network_rpc.cpp` (~400 行) — Light.Network.Rpc.* (Server + Client + JSON 序列化)
   - `light_network_room.cpp` (~500 行) — Light.Network.Room.* (Create + Join + State + Broadcast)

4. **复用现有**:
   - cJSON 序列化 (已 link)
   - libuv event loop (PlatformNet::Poll 已驱动)
   - HttpContext userdata GC 模式 (T01 加固过)

### 4.3 风险点

| 风险 | 等级 | 缓解 |
|------|------|------|
| ENet 与 libuv event loop 协同 | 中 | ENet 自带 select-based service tick, 在 PlatformNet::Poll 中统一驱动 |
| 移动端 ENet build (Android/iOS NDK) | 中 | ENet 纯 C, 跨平台久经考验, 加 cmake conditional |
| Web (Emscripten) UDP 不支持 | 高 | 与 PlatformNet 移动端模式一致, 编译时空存根 + 运行时报错 |
| RPC 调用阻塞 Lua VM | 高 | 必须异步 callback 模式, 不暴露同步 Call() |
| Room state full sync 流量 | 中 | 32 人 × 20 Hz × 1KB state = ~640 KB/s, 局域网够用; 加 dirty flag 跳过未变更 |
| ENet license 暗坑 | 低 | MIT, 仅需保留 LICENSE 文件即可 |

---

## 五、疑问澄清 (剩余决策点, 按优先级排序)

### Q-A (高优先级): ENet 版本固定

- **Q-A1**: 拉 ENet 1.3.18 release tarball 还是 git submodule master?
  - **建议**: tarball (与 cgltf/libuv 风格一致, 避免 submodule 复杂度)
  - **是否需要决策**: 否, 默认按 tarball

- **Q-A2**: ENet 源码放置位置?
  - 选项 1: `third_party/enet/` (与 cgltf/libuv 同级)
  - 选项 2: `third_party/networking/enet/` (新分类目录)
  - **建议**: 选项 1, 保持扁平

### Q-B (高优先级): RPC 调用语义

- **Q-B1**: Client.Call 超时是否需要?
  - 选项 1: 用户自定义 setTimeout 包装
  - 选项 2: 内置 timeout 参数 + auto-callback(err='timeout')
  - **建议**: 选项 2 (默认 5 秒)

- **Q-B2**: 是否支持双向 RPC (server 也能 Call client)?
  - 选项 1: 仅 client → server (类 HTTP RPC)
  - 选项 2: 双向 (peer-to-peer 模式)
  - **建议**: 选项 1, 简化模型, P2P 留 Phase BC.x

### Q-C (中优先级): Room 同步细节

- **Q-C1**: state 数据类型限制?
  - 选项 1: 只允许 JSON 兼容类型 (table/string/number/bool)
  - 选项 2: 支持 Lua function (序列化为字符串源码)
  - **建议**: 选项 1 (function 序列化是反模式)

- **Q-C2**: Master 离开时是否保留房间?
  - 选项 1: 立即销毁 (踢出所有人)
  - 选项 2: 选举次连入者继续 (master migration)
  - **建议**: 选项 2, 业界标准

### Q-D (低优先级): Demo 形态

- **Q-D1**: `demo_io_minigame` 具体玩法?
  - 选项 1: 聚集型 .io (吃豆人, 收集分数最高者赢)
  - 选项 2: 对抗型 .io (移动 + 子弹, 最后一个活下来)
  - 选项 3: 协作型 (多人吃食物 + boss 战)
  - **建议**: 选项 1 (最少代码 + 最能演示位置同步)

- **Q-D2**: demo 是否含简易渲染 (角色 sprite + 名字)?
  - 选项 1: 仅文本 console (类 chat-room, 最少代码)
  - 选项 2: 2D 圆点 + 名字 (用 Light.Graphics)
  - 选项 3: 完整 sprite + 动画
  - **建议**: 选项 2 (中等成本, 视觉效果好)

### Q-E (低优先级): smoke 测试范围

- **Q-E1**: smoke 是否含真实双进程测试?
  - 选项 1: 仅 loopback (127.0.0.1, 单进程内 client+server)
  - 选项 2: + 跨进程 (PowerShell start-process 启动 server, 主进程当 client)
  - **建议**: 选项 1 (CI 简单可靠), 跨进程留 demo 手动测试

---

## 六、Stage 1 决议清单 (人工确认)

| # | 决议项 | 默认值 | 待用户确认 |
|---|-------|--------|----------|
| 1 | ENet vendor 路径 | `third_party/enet/` | ✅ 推荐默认 |
| 2 | ENet 版本 | 1.3.18 release tarball | ✅ 推荐默认 |
| 3 | RPC 默认 timeout | 5000 ms | ✅ 推荐默认 |
| 4 | RPC 双向 (P2P) | 否, 仅 client→server | ✅ 推荐默认 |
| 5 | Room state 类型 | 仅 JSON 兼容 | ✅ 推荐默认 |
| 6 | Master 离开 | 选举次连入者 | ✅ 推荐默认 |
| 7 | Demo .io 玩法 | 聚集型 (吃豆人风) | ⚠️ 需确认 |
| 8 | Demo 渲染层级 | 2D 圆点 + 名字 | ⚠️ 需确认 |
| 9 | smoke 跨进程 | 仅 loopback | ✅ 推荐默认 |
| 10 | RPC 序列化 | cJSON | ✅ 推荐默认 |

---

## 七、Stage 1 完成定义

✅ 已完成:
- 项目上下文分析 (PlatformNet / Light.Network 现状)
- 范围声明 + 边界排除清单
- 用户范围确认 (激进选项 A2+B1+B2+B3+C1+D1+D2+D3+E2)
- ENet 集成方式 (vendor + 静态链接)
- Lua API 草案 (Udp / Rpc / Room 三模块)
- ENet channel 分配 + 序列化方案
- 房间同步语义 (master 选举 / state 复制 / 上限 / 离开处理)
- 关键代码切入点勘察
- 风险矩阵 + 缓解策略
- 剩余决策点清单 (Q-A 到 Q-E)

⏳ 待 Stage 1 完成 (用户确认 Q-D1/D2 + 第 6 节决议清单后):
- 进入 Stage 2 Architect (CONSENSUS + DESIGN)
- 系统架构图 + 模块依赖
- ENet ↔ libuv event loop 协同设计
- 接口契约定义 (PlatformNet UDP API + ENet 抽象 API)

---

## 八、下一步动作

待用户回答:
1. **Q-D1** 玩法 (吃豆人 / 对抗 / 协作)
2. **Q-D2** 渲染层级 (console / 2D 圆点 / sprite)
3. **§六决议清单第 1-10 项** 是否全部接受默认

后, 进入 **Stage 2 Architect** — 锁定决策 + 起草 CONSENSUS_PhaseBC.md + DESIGN_PhaseBC.md.
