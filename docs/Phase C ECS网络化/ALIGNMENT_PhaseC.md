# ALIGNMENT — Phase C ECS 网络化

> **6A 工作流 · Stage 1: Align**
> 模糊需求 → 精确规范. 本文档对齐项目上下文、任务边界、关键决策点.

---

## 0. 任务标题

**Phase C — ECS 与 Room state 打通 (服务端权威 entity 同步)**

---

## 1. 原始需求

> 上下文: 用户在选择 Phase BC 收工后说 "启 Phase C (T11 ECS 整合)".
> 路线图原文 (TODO_PhaseBC.md): "T11 ECS 整合 (16h, Phase C 主线), ECS 与 Room state 打通 (entity 同步)".

需求关键词:
- 把 **现有 Light.ECS (纯 Lua, world/entity/component/system 模型)** 与 **Phase BC Light.Network.Room (server-authoritative state 同步)** 集成
- Server 维护 ECS world, 自动把 entity/component 状态同步到所有 client
- Client 收到后能查询同样的 ECS world (mirror), 用作渲染/UI

---

## 2. 项目上下文分析

### 2.1 现有 Light.ECS (`@/e:/jinyiNew/Light/ChocoLight/src/light_ecs.cpp:1-167`)

**实现**: 167 行内嵌 Lua 脚本, 注册到 `Light.ECS.World`.

**核心 API**:
```lua
local world = Light(Light.ECS.World):New()
world:RegisterComponent("Position", {x=0, y=0})
local e = world:CreateEntity()
e:Add("Position", {x=10, y=20})
e:Get("Position").x         -- 10
world:Query("Position")     -- 所有有 Position 的 entity
world:AddSystem("Move", {"Position","Velocity"}, function(ents, dt) ... end)
world:Update(dt)
world:DestroyEntity(e)
```

**数据结构**:
- `world._entities`: 数组 `{entity}`
- `entity._id`: uint32 自增 (`world._nextId`)
- `entity._comps`: `{compName → table}` (实际数据)
- `entity[compName]`: 便捷引用, 同 `entity._comps[compName]`
- `world._systems`: 数组 `{name, required, func}`

**网络化盲点**:
- ❌ 无 dirty 跟踪
- ❌ 无序列化策略 (function/userdata/cycle 不可序列化)
- ❌ Component 无 "networked" 标记
- ❌ Entity 没有跨进程稳定标识 (server `_nextId` 在 client 无意义)
- ❌ 无 server/client 双视角的概念

### 2.2 现有 Light.Network.Room (Phase BC + v2)

**Server-authoritative 模型**:
```lua
local room = Room.Host('0.0.0.0', port, max)
room:SetState(t)              -- 全量
room:PatchState(set, del)     -- 增量 (Phase BC v2)
room:Broadcast(name, args)    -- 事件
room:Kick(pid, reason)
room:OnJoin / OnLeave / OnInput
```

**Client mirror**:
```lua
local room = Room.Join(host, port, hello)
room:OnReady(fn)
room:OnState(function(state, rev)  -- state 是稳定 table 引用 (Phase BC v2)
    -- state.score, state.players[...]
end)
```

**state 容量限制**: 实测无硬限制, 但 ENet 包 ≤ 64KB. JSON 化 + 单包广播.

### 2.3 现有 demo / smoke 参考

- `samples/demo_udp_echo/main.lua` (本会话刚交付) — 双进程 Room + RPC echo
- `scripts/smoke/network_room.lua` — Room 单进程 stub 测试

---

## 3. 任务边界确认

### 3.1 范围内 (MVP 必做)

- ✅ Server 端 ECS world 创建/修改 entity 时自动同步到所有 client
- ✅ Component 显式标记 `networked = true` 才参与同步 (避免序列化函数/userdata)
- ✅ Client 自动维护一份 mirror world, 用户可调 `world:Query` 渲染
- ✅ Entity create / component update / entity destroy 三种事件全部覆盖
- ✅ 与 Phase BC v2 `PatchState` 集成 — 仅同步变化的 entity (避免每帧全量广播)
- ✅ 写 1 个 demo (`samples/demo_room_sync` 或类似) 验证端到端

### 3.2 范围外 (本 Phase 不做, 可做后续)

- ❌ **客户端预测 / 回滚**: 仅 server-authoritative, client 是哑 mirror
- ❌ **物理同步**: 不与 Light.Physics 集成 (单独 Phase)
- ❌ **大世界分区 / interest management**: 同房间所有 client 看到所有 entity
- ❌ **断线重连 entity 复活**: rejoin 时 server 重发全量 state, client 重建 mirror world
- ❌ **二进制压缩协议**: 复用现有 JSON wire format
- ❌ **System 跨进程**: System (function) 永远只在创建它的进程跑. Server 跑游戏逻辑, client 跑渲染
- ❌ **多 world 实例**: 一个 Room 一个 ECS world

### 3.3 非功能性要求

- 性能: 100 entity × 5 components @ 20Hz 同步频率, 单帧 < 5ms (粗略)
- 兼容: 不破坏现有 Light.ECS API (向后兼容)
- 平台: 桌面 + 移动. Web 不支持 (无 raw UDP, 与 Phase BC 一致)

---

## 4. 需求理解 (对现有项目的理解)

### 4.1 集成点

ECS 与 Room 是两个独立 Lua 模块. Phase C 的核心是构建一个 **桥** (bridge), 而不是侵入式改 ECS 内部. 推荐方案:

**Option A**: 透明集成 — 调 `world:NetworkSync(room)` 后, world 内部 wraps Add/Remove/Set, 自动 patch.
**Option B**: 显式 dirty — 用户调 `entity:MarkDirty()` / `world:Sync()`, 完全可控.
**Option C**: 双世界 — `Light.ECS.NetworkedWorld` 子类, 覆盖 Add/Remove/Update.

**初步倾向 Option A** (透明), 用户体验最好, 但需要在 entity Add/Remove 内部加 hook.

### 4.2 同步颗粒度

**Option 1**: 整个 world 序列化为 state — 简单, 但 100 entity × 5 comp 每次几 KB, 浪费带宽.
**Option 2**: 每 entity 一个 `state.entities[id]` 顶层 key — 配合 PatchState set/delete 完美匹配 v2 增量同步.
**Option 3**: Per-component patch — 更细, 但实现复杂.

**初步倾向 Option 2**:
```
state = {
    entities = {
        [1] = {Position={x=10,y=20}, Sprite={img="hero.png"}},
        [2] = {Position={x=50,y=60}},
    }
}

-- entity 1 移动 → PatchState({entities={[1]={Position={x=11,y=20}, Sprite={img="hero.png"}}}})
-- 注意: Phase BC v2 顶层 set 是 wholesale 替换. 子表 entity 整个传, 但 patch 只传变化的 entity.
```

### 4.3 Entity ID 跨进程稳定性

Server `_nextId` 自增. 序列化到 wire 后, client mirror world 需用 **同一 ID** 索引到本地 entity 对象. 这要求:
- Client 的 `mirror_world._entities[id]` (按 id 索引) — 不能用现有 `world._entities` (数组).

**初步方案**: client mirror world 用 hashmap `{id → entity}`, server world 不变 (向后兼容).

### 4.4 哪些 Lua 类型可序列化?

`NetProto::PushLuaAsCJson` 支持: number, string, bool, table (object/array). 不支持 function, userdata, table cycle.

→ **Component 数据必须是纯数据 table**. 序列化前过滤下划线开头字段 (`_id`, `_comps`, `_world`).

---

## 5. 前置决策点 — 必须确认 (核心架构)

仅 Q1-Q3 阻塞 Stage 2 (架构设计). Q4-Q7 性质偏实现策略, 留到 Stage 3 (Atomize) 或实施期决定.

### Q1. Lua API 风格: 透明 vs 显式?

**选项**:
- **A. 透明 (推荐)**: `world:NetworkSync(room)` 一次, world 自动 wrap Add/Remove/Set 钩子, 用户写法不变.
- **B. 显式**: 每次改完调 `entity:MarkDirty()` 或 `world:Sync()`, 完全可控.
- **C. 双世界**: `Light.ECS.NetworkedWorld` 子类, 接受 room 参数. ECS 模块分叉.

→ **影响**: 后续 entity/world API 形态. A 改动小, B 改动中, C 改动大.

### Q2. 同步颗粒度 (wire format)?

**选项**:
- **A. Per-entity (推荐)**: `state.entities[id]` 顶层条目, 配合 v2 PatchState set/delete 天然契合.
- **B. Whole world**: `state.world` 整体 table, 每次 patch 都是全量化.
- **C. Per-component nested**: `state.entities[id].Position` 嵌套 path. **v2 PatchState 不支持嵌套**, 等同要先升级 PatchState v3.

→ **影响**: wire format, 性能上限. A 最简单且与 v2 兼容.

### Q3. 网络化 component 标记?

**选项**:
- **A. 全部网络化**: 注册的 component 都同步. 实现最简, 但本地 UI 数据 (悬浮提示, debug 高亮) 都会上行.
- **B. 显式标记 (推荐)**: `world:RegisterComponent("Position", defaults, {networked=true})`. 默认本地, 标记才同步.
- **C. 反向标记**: 默认全网络化, `{local=true}` 排除. 用户漏写就泄露.

→ **影响**: RegisterComponent 签名. A 不改, B/C 加第 3 参数.

---

## 6. 后置决策点 — 推迟到实施期 (Stage 3 或更晚)

以下决策不阻塞架构, 留在 TASK_PhaseC.md 中作为"实现待定", 写代码时按最简实现 + 留 hook 即可.

- **Q4 同步频率**: 跟 Update 自然驱动 vs 固定 20Hz vs 手动. 先用最简 (跟 Update), 性能不够再加节流.
- **Q5 Client mirror 创建方式**: 自动注入 vs 显式调用 vs 用户自管. 实现 server 端后再定 client 表达.
- **Q6 Demo 范围**: 仅 smoke vs 单 demo vs 双 demo. 先做 smoke, demo 留到 C3 阶段.
- **Q7 工时切片**: 16h 整体 vs 三段 vs 按 atomic 拆. Stage 3 (TASK) 拆分时自然决定.

---

## 7. 可行性 / 风险评估

### 7.1 技术风险

| 风险 | 等级 | 缓解 |
|------|------|------|
| Lua table cycle 序列化崩溃 | 中 | 过滤 `_world` 反引用, 单元测试覆盖 |
| 大量 entity (>1000) 性能 | 低 | MVP 不优化, demo 用 ≤50 entity |
| Patch 包 > ENet 64KB | 低 | 单 entity 通常 < 100 字节, 1000 entity 也才 100KB. MVP 不分片 |
| ECS API 向后不兼容 | 高 | 严格保持现有 API, 仅 **新增** networked 参数 |
| Server 重连 entity ID 复用 | 中 | rejoin 视为新 session, 全量发. ID 不复用即可 |

### 7.2 集成风险

- **PatchState 不支持嵌套 path**: state.entities[id] 子 entity 的 Position 改了, 必须整个 entity table 重发. → 接受, MVP 不优化.
- **Lua function 不能跨进程**: System 只在 server 跑. Client mirror 没有 System (用户自己加纯渲染 system). → 接受, 文档说明.

---

## 8. 验收标准 (高层)

待 6A Stage 4 (Approve) 后细化, 当前粗略:

- [ ] Server 创建 entity → 所有 client 能 `mirror:Query("Position")` 拿到
- [ ] Server 修改 entity component → client mirror 在下一 tick 看到变化
- [ ] Server 销毁 entity → client mirror 同步移除
- [ ] 1 个 demo 双进程运行, 控制台输出预期序列
- [ ] 现有 ECS smoke (如有) 仍通过
- [ ] CI 全平台 6/6 绿
- [ ] 工时 ≤ 18h (估 16h, 20% buffer)

---

## 9. 决策固化 ✅ (2026-05-11)

**用户决策**: 全用推荐 — **Q1=A, Q2=A, Q3=B**

| # | 选择 | 含义 |
|---|------|------|
| Q1 | **A. 透明 API** | `world:NetworkSync(room)` 调一次, 之后 `entity:Add/Remove/Set` 自动 hook 同步 |
| Q2 | **A. Per-entity 颗粒度** | wire format 用 `state.entities[id]` 顶层条目, 配合 v2 PatchState 增量 |
| Q3 | **B. 显式 networked 标记** | `world:RegisterComponent(name, defaults, {networked=true})`, 默认本地 |

详细共识与验收标准见 `CONSENSUS_PhaseC.md`. Stage 1 (Align) 完成, 进入 Stage 2 (Architect).
