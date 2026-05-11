# demo_ecs_network — Phase C ECS 网络化端到端示例

演示 **Light.ECS + Light.Network.Room** 的 server-authoritative 架构:
服务端运行 ECS 模拟, 客户端通过 mirror world 自动接收并镜像 server 状态.

---

## 启动方式

终端 A — 启动 server:

```powershell
light.exe samples\demo_ecs_network\main.lua server 9101
```

终端 B — 启动 client:

```powershell
light.exe samples\demo_ecs_network\main.lua client 127.0.0.1 9101
```

参数:
- `arg[1]` 模式: `server` 或 `client`
- `arg[2]` server 模式 = 端口 (默认 `9101`); client 模式 = 主机地址 (默认 `127.0.0.1`)
- `arg[3]` client 模式 = 端口 (默认 `9101`)

---

## 演示流程

| 时间 | server 行为 | client 预期 |
|------|------------|------------|
| t=0s | 创建 3 个 networked entity (Position+Velocity), 每帧推进 | mirror 收到初始状态 |
| t=1s..4s | Move 系统每帧改 Position, 自动 PatchState 广播 | mirror 中 3 个 entity 坐标平滑变化 |
| t=5s | 新增 `late_joiner` 第 4 个 entity | mirror 多出 1 个 entity (id=4) |
| t=8s | 销毁 e2 (`diag_mover`) | mirror 中 e2 消失, 仅剩 3 个 |
| t=10s | server 关闭 | client 主循环结束 |

---

## 验证项 (Phase C)

- **C-T1** `RegisterComponent('Position', defaults, {networked=true})` — 显式标记
- **C-T2** `entity:Set('Position', ...)` 触发 dirty 跟踪 (Move 系统每帧调用)
- **C-T3** `world:NetworkSync(room)` 绑定后 `world:Update` 自动 `room:PatchState`
- **C-T4** `ECS.MirrorFromRoom(room)` 创建客户端镜像, 自动 hook `room:OnState`
- 增量同步: `Tag` 组件未标记 networked, **不会** 出现在 client mirror 中
- 增删事件: 第 5s 新增和第 8s 销毁均通过 PatchState 全量替换正确反映

---

## 预期输出片段

server 终端:

```
[server] starting on port=9101
[server] ready, running ~10s with 3 networked entities...
[server room] join pid=1 name=ecs_observer
[server t=1s]
  e1: pos=(0.16, 0.00)
  e2: pos=(99.92, 100.08)
  e3: pos=(50.00, 49.95)
...
[server] spawned late_joiner e4
...
[server] destroyed e2 (diag_mover)
[server] done
```

client 终端:

```
[client] connecting to 127.0.0.1:9101
[client room] ready
[client t=1s] mirror has 3 entities
  e1: pos=(0.16, 0.00) vel=(10.0, 0.0)
  e2: pos=(99.92, 100.08) vel=(-5.0, 5.0)
  e3: pos=(50.00, 49.95) vel=(0.0, -3.0)
...
[client t=6s] mirror has 4 entities
  e1: pos=(...) vel=(...)
  ...
  e4: pos=(...) vel=(0.0, 20.0)
[client t=9s] mirror has 3 entities      -- e2 已被销毁
  e1: ...
  e3: ...
  e4: ...
[client] done
```

注意: server 端 `Tag` 组件 (`name='right_mover'` 等) **不会**出现在 client mirror 中, 因为它未标记 `networked=true`. 这验证了 per-component 同步过滤.

---

## 已知限制 (Phase C v1)

1. **Per-entity 同步粒度** — 任一 networked component 改动会触发该 entity 的全量 row 重建. 字段级 dirty 留作 Phase C v2 优化.
2. **顶层 `entities` 替换** — `_SyncToRoom` 当前用 `room:PatchState({entities=...})` 全量替换. 大量 entity (1k+) 时带宽不优, 后续应改为 per-id set/del.
3. **无兴趣区域过滤** — 所有 client 收到完整 entity 列表, 不支持 AOI / spatial culling.
4. **Mirror 单向** — `MirrorFromRoom` 返回的 world 只读, 客户端不应 `CreateEntity` / `Set`. 主动写会变成本地 only, 下次 server 推送即被覆盖.
5. **Component schema 自治** — server 与 client 必须人工保证 `RegisterComponent` 名称一致, 没有 schema 协商.

---

## 相关文档

- `docs/Phase C ECS网络化/DESIGN_PhaseC.md` — 架构与接口契约
- `scripts/smoke/ecs_network.lua` — 单进程 mock-room smoke (CI 内运行)
- `samples/demo_udp_echo/` — Phase BC 网络层底层 demo (可先跑这个验证 ENet 通)
