# TODO — Phase BC（网络高级）后续待办

> **6A 工作流 Stage 6 — 收尾询问产物**
> 列出所有遗留的、需要用户决策或外部支持的事项, 配合操作指引.
> 用户阅读本文档后即可决定下一步执行方向.

---

## 一、推迟到 v2 的任务（最高优先 — 影响开发者上手）

### 1.1 T10 · `demo_udp_echo` 双端样例

**状态**: ⏸ 推迟
**预估工时**: 3h
**前置依赖**: T5 (`Light.Network.Udp`) — 已完成 ✅

**需要用户决定**:
1. 是否在 `samples/` 下新增 `demo_udp_echo/` 目录? (默认推荐 yes)
2. 是否要双端 lua (一个 server.lua + 一个 client.lua) 还是单一 main.lua 两端共用?
   - 默认推荐: 单 main.lua + 命令行参数 `--mode=server|client`

**操作指引** (用户决定后告诉我):
```
"开始 T10, 单 main.lua 模式"
or
"开始 T10, 双 lua 模式"
or
"跳过 T10, 直接 T11"
```

---

### 1.2 T11 · `demo_room_sync` 多人位置同步样例

**状态**: ⏸ 推迟
**预估工时**: 5h
**前置依赖**: T8 (`Light.Network.Room`) — 已完成 ✅

**需要用户决定**:
1. 演示风格:
   - **A 选项**: 命令行版 (4 个进程在 console 打 ASCII 网格), 易跑
   - **B 选项**: 图形版 (用 Light.Graphics 画 sprite), 需 window subsystem 已就绪 (Phase 3 ✅)
2. 是否在演示中包含输入预测 (client-side prediction)?
   - 默认推荐: 不包含 (留到 Phase BD 再做)

**操作指引**:
```
"T11 选 A 命令行版"
or
"T11 选 B 图形版, 含输入预测"
or
"跳过"
```

---

### 1.3 T12 · `demo_io_minigame` boss 战完整 demo

**状态**: ⏸ 推迟
**预估工时**: 20h (最大单任务)
**前置依赖**: T8 + Phase 3 sprite + animation + particles

**需要用户决定**:
1. 是否真的要在 Phase BC 内完成 minigame? 该任务工时 ≈ Phase BC 其他全部任务总和.
2. **建议**: 拆出独立 Phase BD (multiplayer minigame) 立项, 让 BC 维持纯引擎能力交付.

**操作指引**:
```
"T12 推迟到 Phase BD 独立立项"  (推荐)
or
"T12 开始, 用最简化 boss 战 (1 boss + 4 player + ASCII 战斗 log)"
or
"跳过"
```

---

## 二、增强项（中优先 — 影响生产可用性）

### 2.1 Room state 增量 patch (替代全量广播)

**当前实现**: `room:SetState(t)` 总是序列化整表 + 广播.
**问题**: state 表 > 4KB 时, 每次更新带宽放大严重.
**方案**: 实现 RFC 6902 JSON Patch:
```lua
room:PatchState({{op="replace", path="/score", value=10}})
```
内部计算 patch → 序列化 patch only → 广播 PKT_ROOM_STATE_PATCH.

**预估工时**: 6h
**优先级**: 中 (state < 4KB 时无影响)

**操作指引**:
```
"T-PATCH 启动" (我会创建 ALIGNMENT_PhaseBC_Patch.md 走 6A)
```

---

### 2.2 Kick 包真实送达 (PKT_ROOM_KICK by-peer-id)

**当前实现**: `room:Kick(peer_id)` 仅 disconnect, 不发 KICK 包.
**问题**: 客户端只能从 ENet DISCONNECT 推断 (无 reason 字符串).
**方案**: PlatformNet 增暴露 `EnetSendByPeerId(host, peer_id, ch, data, len, reliable)`, T8 Kick 在 disconnect 前先发 PKT_ROOM_KICK.

**预估工时**: 2h
**优先级**: 低 (功能上等价, 仅缺友好错误信息)

---

### 2.3 RPC timeout 机制 ✅ 已完成 (commit `125f4c2`)

**当前实现**: `client:Call(method, params, cb [, timeout_ms])`. 第 4 参数可选, > 0 时启用超时.
**实施**:
- 新增 PlatformNet API: `EnetSetFrameCb(host, cb)` — 每帧 idle 回调
- RpcClient 新增 `deadlinesRef` (Lua table {id → deadline_ms})
- `ScanTimeouts(c)` 通过 frame cb 每帧扫描, 触发 `cb({code=-32001, message="timeout"}, nil)`
- DISCONNECT / Receive / Close 各处同步清理两表

**实际工时**: 1h (低于估算)
**CI 验证**: ✅ 全平台 6/6 通过 (run 25639242640)

---

### 2.4 WebRTC 桥 (Web 端 ENet 替代)

**当前实现**: Web 平台 PlatformNet ENet 函数全部 stub 返回 nullptr.
**问题**: 浏览器不支持 raw UDP, 不能用 ENet, Phase BC 在 Web 不可用.
**方案**: 用 emscripten + WebRTC DataChannel 写第二个后端 `light_platform_net_webrtc.cpp`. 难度高但解锁 Web multiplayer.

**预估工时**: 30h
**优先级**: 中-高 (取决于 Web 端是否要 multiplayer)

**操作指引**: 此项太大, 建议独立 Phase BF 立项.

---

## 三、技术债（低优先 — 不影响功能）

### 3.1 `PushCJsonAsLua` / `PushLuaAsCJson` 重复

**位置**: `light_network_rpc.cpp` 与 `light_network_room.cpp` 各有一份独立实现.
**建议**: 抽到 `light_json_lua.h` (header-only) 或 `light_json_lua.cpp` 单独编译单元.
**预估工时**: 1h
**理由**: DRY, 但当前重复仅 ~80 行, 不强求.

---

### 3.2 ENet 1.3.18 维护性

**问题**: 上游 ENet 自 2024-01 已停 release, 长期可能有 CVE 不修.
**备选**: enet-csharp / 自主 fork.
**建议**: Phase BC v2 评估前不动作, 当前协议稳定.

---

## 四、缺失的配置 / 外部资源

**当前状态**: 无缺失. ✅

T1-T9 全部用 vendored 第三方库 (ENet + cJSON), 不依赖外部 API key, 不依赖账户.

如果未来加 WebRTC 桥, 需要 STUN/TURN 服务器 (可用 Google 免费 STUN `stun.l.google.com:19302` 测试, 生产建议自部署 coturn).

---

## 五、推荐下一步行动 (按优先级)

| # | 行动 | 工时 | 原因 |
|---|------|------|------|
| **1** | T10 demo_udp_echo (3h) | 3h | 最低成本验证 T5 端到端可用 |
| **2** | RPC timeout (3h) | 3h | 生产可用性, 阻止应用层永远等待 |
| **3** | T11 demo_room_sync (5h) | 5h | 验证 T8 端到端 |
| **4** | Patch state 增量 (6h) | 6h | 大 state 优化, 用户感知不明显 |
| **5** | T12 / Phase BD minigame | 20h+ | 大 demo, 建议独立立项 |
| **6** | WebRTC 桥 (30h) | 30h | 解锁 Web multiplayer, 但工作量大 |

---

## 六、用户当前需要回答的问题

请告诉我下一步要做哪个任务, 例如:

1. **"开始 T10"** — 启动 demo_udp_echo 实现 (~3h, 包含 6A 子流程)
2. **"开始 RPC timeout 增强"** — 给 `client:Call` 加超时参数
3. **"Phase BC 收工, 进 Phase BD"** — 完结当前阶段, 立项下个 Phase
4. **"什么都不做, 留作后续"** — 标记 v2 待办, 处理其他工作

如未回答, 默认建议: **方案 3** (Phase BC 核心已交付, 进入下个阶段).

---

## 七、附录: 6A 工作流闭环检查

| Stage | 文档 | 状态 |
|-------|------|------|
| 1 Align | `ALIGNMENT_PhaseBC.md` | ✅ |
| 1 Align | `CONSENSUS_PhaseBC.md` | ✅ |
| 2 Architect | `DESIGN_PhaseBC.md` | ✅ |
| 3 Atomize | `TASK_PhaseBC.md` | ✅ |
| 4 Approve | (用户口头确认) | ✅ |
| 5 Automate | T1-T9 实施 + commit 序列 | ✅ |
| 6 Assess | `ACCEPTANCE_PhaseBC.md` | ✅ |
| 6 Assess | `FINAL_PhaseBC.md` | ✅ |
| 6 Assess | `TODO_PhaseBC.md` | ✅ (本文档) |

**6A 流程闭环 ✅.**
