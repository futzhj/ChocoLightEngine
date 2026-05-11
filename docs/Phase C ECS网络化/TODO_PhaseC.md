# TODO — Phase C ECS 网络化

> **6A 工作流 · Stage 6 (Assess) 末尾产物**
> Phase C v1 主体已结案. 此文档列出未尽事项 / 后续可做项 / 用户需配合的事.
> 阅读对象: 用户 (决定优先级) / 后续维护者 (执行).

---

## 1. 用户需要立即关注的事 (高优先级)

### 1.1 ✅ 等 CI run `25644098066` 最终状态

- **状态**: 已推送 commit `eaf596b` (C-T6 demo), 在跑 `25644098066`
- **预期**: 6/6 平台 success (本次仅新增 Lua + Markdown, 无 C++ 改动, 风险极低)
- **若失败**: 极可能是 Lua 5.1 syntax 边角问题 (`unpack` vs `table.unpack` 等), 我会迅速修
- **操作**: 等 ~5 分钟后告诉我"看下 CI", 我汇报结果

### 1.2 (可选) 手动跑 demo_ecs_network 双终端

```powershell
# 终端 A (server)
light.exe samples\demo_ecs_network\main.lua server 9101

# 终端 B (client, 在 server 启动后再运行)
light.exe samples\demo_ecs_network\main.lua client 127.0.0.1 9101
```

预期效果: client 每秒打印 mirror 中 entities 坐标, 与 server 端打印保持同步; t=5s 出现 e4, t=8s e2 消失.

---

## 2. 已知限制 (留给 Phase C.x 优化, 中优先级)

### 2.1 Per-component / per-field dirty 跟踪

**现状**: 任一 networked component 改动 → 该 entity 整 row (所有 networked component) 重打.

**问题**: 一个 entity 改 Position 的 x, y, z 任一字段, 都会重发整个 Position + 其他 networked component.

**改进思路**:
- `_dirty_entities[id] = {comp1=true, comp2=true}` 记录 component 级
- `_BuildEntityState(entity, dirtyComps)` 仅打 dirty 部分
- 配合 PatchState v3 子 key set/del

**预估**: ~3h, Phase C.x

### 2.2 顶层 entities wholesale 替换

**现状**: 一个 entity 移动 → `room:PatchState({entities = {[id]=row}})` 把 `entities` 顶层 key 整体替换.

**问题**: PatchState v2 顶层 key 替换语义对于 entities 这种 "增量字典" 不优. 应该 per-id set/del.

**改进思路**:
- 升级 PatchState v3 支持 `set: {"entities.1": {...}, "entities.2": {...}}` 嵌套 key 语法
- `_SyncToRoom` 改用 `set = {["entities."..id] = row}` + `del = {"entities."..destroyed_id}`

**预估**: ~5h (含 Phase BC 协议升级), Phase C.x

### 2.3 无兴趣区域 / AOI 过滤

**现状**: 所有 client 收完整 entity 列表.

**问题**: MMO / 大世界场景, 每 client 只关心自己附近 ~50 个 entity, 全广播浪费带宽.

**改进思路**:
- `world:RegisterAOIQuery(peer_id, function(entity) return distance(entity, peer) < 100 end)`
- `_SyncToRoom` 对每个 peer 单独打 set, 用 `room:Send(peer_id, ...)` 替代 `Broadcast`

**预估**: ~6h, Phase C.x 进阶

### 2.4 Schema 协商缺失

**现状**: server 注册 `RegisterComponent("Position", {x=0, y=0}, {networked=true})`, client 也要手动注册同名同 schema, 否则 mirror 不知道 default 值.

**问题**: schema 不一致时 client 静默失败 (字段 nil 表现).

**改进思路**:
- server 在 OnReady 时把 networked schema list (`{name, defaults}`) 通过 `room:Broadcast('ecs_schema', ...)` 推一次
- client mirror `OnEvent('ecs_schema', ...)` 自动 RegisterComponent
- 或者写到 initial state 第一份 PatchState 里

**预估**: ~2h, Phase C.x

### 2.5 二进制压缩协议

**现状**: PatchState 走 cJSON, ASCII text, 大量重复字段名 (`"Position"`, `"Velocity"`).

**问题**: 大场景 1k+ entity 时, JSON 字段名占 60%+ 带宽.

**改进思路**:
- 协议表: server 启动时把 component name → uint16 id 的映射表推到 client
- wire 用 component_id 替代名字
- 进一步: 用 MessagePack / FlatBuffers 替代 JSON

**预估**: ~10h+, Phase C.x 高级 (相当于 Phase BC v3)

---

## 3. 后续可启动的相关 Phase (低优先级 / 探索)

### 3.1 Phase D 候选 — 持久化

- **目标**: server 端 ECS state 周期性写到 SQLite (复用 `Light.DB.SQLite`)
- **关键 API**: `world:Persist(db, table_name)` / `world:Restore(db, table_name)`
- **依赖**: 完成 Phase C v1 (本 phase) ✅
- **预估**: ~1 周

### 3.2 Phase D' 候选 — 客户端预测 + lag compensation

- **目标**: client 在 input 提交后立即本地应用 (预测), 收到 server state 后回滚 + 重放
- **关键挑战**: 时间戳同步, snapshot interpolation, 输入序列化
- **依赖**: Phase C v1 ✅, Phase BC RPC v2 ✅
- **预估**: ~2 周

### 3.3 Phase D'' 候选 — 动画 + 网络

- **目标**: Animator 当前播放 clip / param 通过 ECS networked component 同步
- **架构**: 把 `AnimationStateComp = {clip, time, param}` 标记 networked
- **依赖**: Phase AV ✅ + Phase C v1 ✅
- **预估**: ~3 天

---

## 4. 文档 / Tooling 改进 (低优先级)

### 4.1 API 文档自动生成

**现状**: 用户读 `light_ecs.cpp` 顶部 docstring 或 `DESIGN_PhaseC.md` 找 API.

**改进**: 写 `docs/api/Light_ECS.md` 类似 Phase AV 的 `Light_Animation.md` 格式.

**预估**: ~1h

### 4.2 demo_ecs_network 增加图形输出

**现状**: 纯文本 print.

**改进**: 用 `Light.Graphics` 在 client 端真实绘制 entity (像 simple_walk 那样).

**预估**: ~3h, 不强求

---

## 5. 缺少的配置 / 资源

**当前 Phase C 不需要任何额外配置或第三方资源**. 全部依赖现有 ChocoLight + Phase BC.

---

## 6. 待办速查表 (按用户操作分类)

| 你 (用户) 要做 | 我 (Cascade) 要做 | 状态 |
|----------------|---------------------|------|
| 等 CI run 25644098066 完, 告诉我结果 | — | ⏳ in_progress |
| (可选) 双终端跑 demo_ecs_network | — | 📋 等用户 |
| 决定下一步 phase | 实施 | 📋 等用户 |
| — | Phase C.x 选 1 项做 | 📋 待用户启动 |

---

## 7. 操作指引

### 7.1 启动 Phase C.x v1 优化 (per-component dirty)

```
告诉我: "启 Phase C.x.1 per-component dirty"
```

我会:
1. 走 6A Stage 1-3 (Align/Consensus/Design/Task) — ~10 分钟
2. 等你 approve
3. Stage 5 实施 — ~3h
4. CI + Stage 6 Assess

### 7.2 启动 Phase D (持久化)

```
告诉我: "启 Phase D 持久化"
```

我会发更详细的 Stage 1 文档征求决策.

### 7.3 暂停 / 跳到其他主题

直接说要做的事, 不必客气. Phase C 已结案, 切换无包袱.

---

## 8. 未解决的开放问题 (留给将来)

1. **多 server 拓扑** — 当前是单 Room.Host, 多服务器分区 (sharding) 怎么做?
2. **Authority 切换** — 某 entity 从 server A 迁移到 server B, mirror 怎么无缝切?
3. **回放 / 录制** — 把 PatchState 流写入文件以便后续重放, 用于复盘 / 测试?

这些都是 Phase E+ 级别的探索, 暂无 ETA.
