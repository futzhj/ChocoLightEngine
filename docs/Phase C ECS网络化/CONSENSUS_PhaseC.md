# CONSENSUS — Phase C ECS 网络化

> **6A 工作流 · Stage 1 末尾产物**
> 固化对齐结果, 作为 Stage 2 (Architect) 输入. 所有不确定性已在 ALIGNMENT 中解决.

---

## 1. 明确的需求描述

把现有的 `Light.ECS` (纯 Lua, world/entity/component/system 模型) 与 Phase BC `Light.Network.Room` (server-authoritative state 同步) 集成, 使:

- **Server 端**: 用户在 ECS world 里 `entity:Add/Set` 修改 component, 引擎自动通过 Room state 把变化广播给所有连接的 client.
- **Client 端**: 用户用同样的 ECS API 查询本地 mirror world (`mirror:Query("Position")`), 拿到与 server 一致的 entity/component 数据.

---

## 2. 用户决策固化 (Q1-Q3)

| # | 决策 | 影响 |
|---|------|------|
| Q1 | **A. 透明 API** | 新增 `world:NetworkSync(room)` 方法, 调用后 entity 的 `Add/Remove/Set` 内部加 dirty hook. 用户写法 100% 不变. |
| Q2 | **A. Per-entity** wire format | `state.entities[id]` 顶层条目. PatchState set 含变化的 entity, delete 含销毁的 entity ID. |
| Q3 | **B. 显式 networked 标记** | `RegisterComponent` 第 3 参数 `{networked=true}`. 默认本地. 仅 networked component 进 wire. |

---

## 3. 技术实现方案

### 3.1 模块结构

仅扩展 `light_ecs.cpp` 内嵌 Lua 脚本, 不新增 C++ 文件. 新增的代码全部在 ECSWorld class 中.

```
Light.ECS (现有 ECSWorld)
├── RegisterComponent(name, defaults, opts?)    [扩展: opts.networked]
├── CreateEntity()                              [扩展: dirty 跟踪]
├── DestroyEntity(entity)                       [扩展: 标记 destroy dirty]
├── NetworkSync(room)                           [NEW: 绑定 Room, 启用同步]
├── _SyncToRoom() (private)                     [NEW: 序列化 dirty entity → PatchState]
└── _BuildEntityState(entity) (private)         [NEW: 过滤 networked component]

entity (扩展)
├── Add(name, data)         [扩展: 若 networked, 标记 dirty]
├── Remove(name)            [扩展: 同上]
├── Set(name, data)         [NEW: 等价于 Add 但语义清晰, 用户也可直接改 entity[name].x]

Light.ECS.MirrorFromRoom(room)  (NEW): 创建 client 端 mirror world
```

### 3.2 Wire format (state schema)

```json
{
  "entities": {
    "1": {
      "Position": {"x": 10, "y": 20},
      "Sprite":   {"image": "hero.png"}
    },
    "2": {
      "Position": {"x": 50, "y": 60}
    }
  }
}
```

**注意**: JSON 的 key 是 string, 但 Lua 的 entity._id 是 number. 序列化时强制 `tostring(id)`, 反序列化时 `tonumber(key)`.

### 3.3 增量同步 (与 Phase BC v2 PatchState 集成)

- **Entity 创建/修改**: server 在 `_SyncToRoom()` 阶段, 把 dirty entity 的完整 state 放入 `PatchState` 的 `set` 表 → `room:PatchState({entities = {[id]=...}})`.
- **Entity 销毁**: 把销毁的 entity ID 放入 `PatchState` 的 `delete` 表 → `room:PatchState({entities = {}}, {"entities."..id})`.
  - **注意**: PatchState 的 delete 是顶层 key 数组. 删除嵌套子 key 不直接支持. **MVP 简化**: server 维护一个 "destroyed_ids" 列表, 每次 sync 时把这些 ID 从 `state.entities` 整体重发 (整个 entities 表 set 一次, 包括所有当前活跃 entity).
  - **优化版本** (后续): 升级 PatchState 支持子 key delete, 或直接发 entity={[id]=null} 让 PushCJsonAsLua 处理 null → nil 删除. 这是 Q4 推迟决策的一部分.

**MVP 选择 — 简化方案**:
- Server 每帧或 entity 变化时, 把 **整个 `state.entities` 表** 通过 `PatchState({entities = ...})` 发出. 这是顶层 key wholesale 替换 (v2 PatchState 子表整个替换语义), 一次性同步全部 entity.
- 缺点: 即使一个 entity 移动, 也要把所有 entity 重发. 但 MVP ≤50 entity 场景下 < 4KB, 完全可接受.
- 优点: 实现简单, 不需要嵌套 patch.

### 3.4 Dirty 跟踪机制

```lua
world._dirty_entities = {}   -- {[id] = true} 待同步的 entity
world._destroyed_ids  = {}   -- {[id] = true} 已销毁待广播的 entity (MVP 不单独用, 全量 entities 重发即可)
world._sync_room      = nil  -- NetworkSync 绑定的 Room.Host
world._has_changes    = false  -- 任何 networked 字段有变化的总开关
```

每次 `entity:Add/Remove/Set`, 若 component 是 networked, 设 `world._has_changes = true`.
`world:Update(dt)` 末尾或显式 `world:_SyncToRoom()` 触发: 若 `_has_changes`, 序列化 `state.entities` 并 `PatchState`.

### 3.5 Client mirror world

新增 `Light.ECS.MirrorFromRoom(room)`:
1. 创建空 ECSWorld.
2. 注册 `room:OnState(function(state, rev) ... end)`.
3. OnState 内部:
   - 遍历 `state.entities[id]`, 对比本地 mirror world.
   - 新 ID → `CreateEntity` (但用 server 的 ID, 不用本地 _nextId), Add 各 component.
   - 已存在 ID → 更新 component data (调 internal 等价 Set).
   - 本地有但 state 没有的 ID → DestroyEntity.

Mirror world 与 server world 不共享 system. 用户可在 client 自己注册渲染 system, 它们 query 拿到 networked 数据后渲染即可.

---

## 4. 任务边界 (与 ALIGNMENT 一致)

### 4.1 范围内 (MVP)

- ✅ `RegisterComponent` 加可选 opts 参数, 支持 `{networked=true}`
- ✅ Entity 增/改/删自动同步 (透明)
- ✅ `world:NetworkSync(room)` 绑定
- ✅ `Light.ECS.MirrorFromRoom(room)` client API
- ✅ 至少 1 个 smoke 脚本验证单进程 API 完整性
- ✅ 1 个双进程 demo (推迟到 C3 阶段决定形式)

### 4.2 范围外 (Phase C 不做)

- ❌ 客户端预测 / lag compensation
- ❌ 物理 / Box2D 集成
- ❌ Interest management (大世界分区)
- ❌ 二进制压缩协议
- ❌ Per-component 嵌套 patch (等 PatchState v3)

---

## 5. 验收标准 (具体可测试)

### 5.1 API 兼容性

- [ ] 现有 `samples/` 中所有用 Light.ECS 的 demo 仍能跑 (无破坏性变更)
- [ ] `RegisterComponent(name, defaults)` 仍合法 (opts 可选)

### 5.2 功能正确性 (smoke 单进程)

新增 `scripts/smoke/ecs_network.lua`:
- [ ] `world:NetworkSync(room)` 不报错
- [ ] `RegisterComponent("Pos", {x=0}, {networked=true})` 标记成功
- [ ] `RegisterComponent("Local", {})` 默认非 networked
- [ ] 创建 entity + Add networked component → `room:PatchState` 被触发 (mock room 验证)
- [ ] Add 非 networked component → 不触发 PatchState
- [ ] DestroyEntity → 触发 PatchState 把该 ID 移除

### 5.3 端到端 (双进程 demo, C3)

- [ ] Server 创建 5 个 entity, client mirror 收到全部 5 个
- [ ] Server 修改某 entity 的 Position, client mirror 下一帧看到新值
- [ ] Server 销毁某 entity, client mirror 不再 Query 到它

### 5.4 CI

- [ ] 全平台 6/6 通过 (build-windows/macos/linux/android/ios/web)
- [ ] `lightc -p` syntax check 全过

---

## 6. 进入 Stage 2 (Architect)

下一步: 创建 `DESIGN_PhaseC.md` 包含:
- 数据流图 (server entity 变化 → wire → client mirror)
- 核心组件 (ECSWorld 扩展, MirrorFromRoom 实现)
- 接口契约 (Lua API 详细签名 + 行为规约)
- 异常处理 (序列化失败, room 断连, OnState 重入)

预计 Stage 2 文档 ~5KB. 用户审查后进 Stage 3 (Atomize, 拆 atomic task).
