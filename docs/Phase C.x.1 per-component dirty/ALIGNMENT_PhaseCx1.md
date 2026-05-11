# ALIGNMENT — Phase C.x.1 per-component dirty 优化

> **6A 工作流 · Stage 1**
> 紧接 Phase C v1, 解决 `TODO_PhaseC.md §2.1 + §2.2` 列出的 wholesale 替换带宽问题.

---

## 1. 项目上下文

### 1.1 Phase C v1 收尾时的实现

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:219-233` 当前 `_SyncToRoom`:

```lua
function ECSWorld:_SyncToRoom()
    local entitiesTable = {}
    for _, e in ipairs(self._entities) do
        local row = self:_BuildEntityState(e)
        if row then
            entitiesTable[tostring(e._id)] = row
        end
    end
    self._sync_room:PatchState({entities = entitiesTable})
    self._dirty_entities = {}
    self._destroyed_ids = {}
    self._has_changes = false
end
```

**性能特征**:
- 任一 entity 任一 networked component 改动 → 重建整个 `entitiesTable` (所有活跃 networked entity 全量)
- `_dirty_entities` 字段实际上**只用作 `_has_changes` 触发**, dirty entity ID 信息没有真正利用
- PatchState 顶层 set `entities` → wholesale 替换语义, client 用 incoming 全量重 reconcile

### 1.2 Phase BC v2 PatchState 协议

`@e:\jinyiNew\Light\ChocoLight\src\light_network_room.cpp:689-731` (相关区域):
- `room:PatchState(set, del)`
- `set` 是顶层 key 表, 整 key 替换
- `del` 是顶层 key 数组, 整 key 删除
- **不支持嵌套 key 路径** (如 `"entities.1.Position"`)

### 1.3 Mirror 端 reconcile

`@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:249-316` `_ApplyState`:
- 收到完整 `state.entities` 表
- 遍历: 新增/更新 entity, 浅 merge component
- 删除: 本地有但 incoming 没有的 entity → 销毁

**当前依赖**: server 必须每次推送 entities **全量**, 否则 mirror 会错误销毁未变化的 entity.

---

## 2. 任务范围 (边界确认)

### 2.1 范围内 (Phase C.x.1 MVP)

- ✅ **Per-entity dirty 真正利用** — 只重发 dirty entity, 不动 idle entity
- ✅ **Per-component dirty 跟踪** — 一个 entity row 只含 dirty component
- ✅ **Mirror 端增量 apply** — 不依赖收到全量 entities 才工作
- ✅ **保留 baseline 全量** — 新 client join 时仍能拿到完整状态
- ✅ smoke 扩展验证 per-component delta 行为

### 2.2 范围外 (留 Phase C.x.2+)

- ❌ AOI / 兴趣区域过滤 (Phase C.x.2 候选)
- ❌ 二进制压缩协议
- ❌ Schema 协商
- ❌ 字段级 dirty (component 内部某字段才是 dirty 源, 不重发整 component)
- ❌ Phase BC PatchState 协议升级 (避免 v3 改动级联)

---

## 3. 现有项目对齐分析

### 3.1 关键约束

1. **不能升级 PatchState 协议** — Phase BC 已稳定, 升级会拖时间. v3 留作后续独立 phase.
2. **不能破坏现有 mirror reconcile** — `_ApplyState(state)` 现在期望 `state.entities` 全量. 若变成增量必须明确语义.
3. **新 client join 仍要能 catchup** — 必须有 baseline 机制让晚到的 client 拿到全状态.
4. **API 表面尽量稳定** — `world:NetworkSync(room)` / `MirrorFromRoom` 用户调用形式不变.

### 3.2 备选实现路径对比

| 路径 | 思路 | 工作量 | 协议 | 优点 | 缺点 |
|------|------|-------|------|------|------|
| **X. 升级协议** | PatchState v3 嵌套 key set/del | ~10h+ | 需改 Phase BC | 协议层统一 | 改动级联大, 影响 BC 稳定 |
| **Y. 全走 Broadcast** | 抛弃 state, 全用 `Broadcast('ecs_delta', ...)` 事件 | ~4h | 不动 BC | ECS 自治 | join 时无 baseline, 需额外协议 |
| **Z. 混合 (推荐)** | baseline = 一次 SetState (join 时) + 增量 = ecs_delta event | ~3-4h | 不动 BC | 协议解耦, 兼容 | 略复杂, mirror 两路 hook |

**推荐 路径 Z**: 与"3-4h"预期吻合, 不动 Phase BC, 复用既有 SetState/Broadcast 接口.

---

## 4. 关键决策点 (Q1-Q3)

### Q1: Dirty 跟踪结构

| 选项 | 数据结构 | 优劣 |
|------|---------|------|
| **A (推荐)** | `_dirty_comps[id] = {comp1=true, comp2=true}` (per-entity comp set) | 自然分组, 序列化时直接 `for comp in pairs(_dirty_comps[id])`; 内存比 B 略多 |
| B | `_dirty_flat["1:Position"] = true` (flat string key) | 内存紧凑; 但每次序列化要 string parse/split, 慢 |

### Q2: 增量传输路径

| 选项 | 机制 | 优劣 |
|------|------|------|
| **A (推荐)** | `room:Broadcast('ecs_delta', {set=..., del=...})` 事件 | 不动 PatchState 协议; mirror 端 OnEvent 即可 apply |
| B | 仍走 `room:PatchState`, 但 ECS 自管 entities top-level wholesale | 不解决根本问题 (PatchState 顶层语义) |
| C | 升级 PatchState v3 嵌套 key | ~10h+ 工作量, 范围外 |

### Q3: Baseline 触发时机 (已澄清)

**已确认事实** (Phase BC `light_network_room.cpp` 源码):
- `room.Host` 没有暴露 per-peer Send Lua API (只有 `SetState` / `PatchState` / `Broadcast` / `Kick`)
- 但 BC 框架内部在 `HostHandleHello` 中**自动**调 `SendStateToPeer` (`@e:\jinyiNew\Light\ChocoLight\src\light_network_room.cpp:279`), 给新 join 的 peer 推一次 `h->stateRef` 全量
- `SetState(...)` 会立刻触发 `BroadcastState` 给所有 peer (全量, 浪费带宽)

| 选项 | 时机 | 优劣 |
|------|------|------|
| A | server 每帧 SetState 同步整 entities | ❌ 浪费带宽 (退化到 v1) |
| B | 周期性 SetState (每 N 秒) | ❌ idle entity 仍周期性重发 |
| C | server 不动 SetState, 全部走 `Broadcast('ecs_delta')` 增量, 新 peer 通过 ecs_delta 拿全量 | ❌ 新 peer 收不到 idle entity (它们不在 delta 中) |
| **D (推荐)** | server hook `room:OnJoin`, 收到新 peer 时设 `_needs_full_resync=true`, 下次 `_SyncToRoom` 把所有 entity 当作 dirty 重发一次 (走 ecs_delta 全量形式) | ✅ 平时带宽最省; 新人来短暂全场重传; mirror 端协议统一 |

**核心机制**:
- ECS server 不再用 SetState/PatchState. 全部走 `Broadcast('ecs_delta', {set=..., del=...})`
- 平时: dirty 跟踪只发改动部分
- 新 peer join: 触发一次"全量 delta" (整 entities 当成 dirty), 让所有 client (包括新人) 拿到完整快照
- mirror client 只 hook `room:OnEvent('ecs_delta', ...)`, 不需要 hook OnState

---

## 5. 详细推荐方案 (路径 Z 落地)

### 5.1 Server 端

```lua
-- 新增 dirty 跟踪
world._dirty_comps = {}   -- {[entity_id] = {[comp_name] = true}}
world._destroyed_ids = {} -- {[id] = true} 已销毁待广播

-- entity:Add/Set/Remove 内部
function _markDirty(world, id, compName)
    local set = world._dirty_comps[id]
    if not set then
        set = {}
        world._dirty_comps[id] = set
    end
    set[compName] = true
    world._has_changes = true
end

-- _SyncToRoom 改造
function ECSWorld:_SyncToRoom()
    -- 1. 构造增量 patch
    local set_patch = nil
    for id, dirtySet in pairs(self._dirty_comps) do
        local entity = self:_FindById(id)
        if entity then
            local row = {}
            for compName, _ in pairs(dirtySet) do
                local comp = entity._comps[compName]
                if comp and self._networked_comps[compName] then
                    row[compName] = self:_ShallowCopy(comp)
                end
            end
            if next(row) then
                set_patch = set_patch or {}
                set_patch[tostring(id)] = row
            end
        end
    end

    local del_patch = nil
    for id, _ in pairs(self._destroyed_ids) do
        del_patch = del_patch or {}
        table.insert(del_patch, tostring(id))
    end

    -- 2. 广播 delta event (仅当有变化)
    if set_patch or del_patch then
        self._sync_room:Broadcast('ecs_delta', {
            set = set_patch,
            del = del_patch,
        })
    end

    -- 3. 同时更新 server-side state.entities (供新 client join 时 baseline)
    --    用 SetState 的方式: server 维护一个完整 `_baseline_entities`, 每次 sync 后更新到 state
    --    具体: _SyncToRoom 末尾把 _BuildFullEntities() 结果通过 SetState 推一次
    --    缺点: 这又变成全量 SetState... 这违背了优化目标!
    --
    --    替代方案: 不走 SetState, 而是手动维护 server 端 state, room:GetState() 用 ECS 自维护版本
    --    实际上: Room 的 state 有 _state field, 直接 mutate + 不调 PatchState 也是可行的
    --    或者: 在 OnJoin 时, 通过 self._sync_room:Send(pid, 'ecs_baseline', fullEntities) 单点推送
    --
    --    简化方案 (MVP): _SyncToRoom 末尾仍调一次 SetState 但只在 dirty 时, 仅更新 entities key.
    --    SetState 内部应该会触发一次 OnState 给所有 client, 但因为 mirror 端也 hook OnState 做 baseline
    --    reconcile, 所以需要协议明确: OnState = baseline (全量), ecs_delta = incremental (增量).
    --    Mirror 收到 OnState 时, 全量 reset entities; 收到 ecs_delta 时, 应用增量.

    self._dirty_comps = {}
    self._destroyed_ids = {}
    self._has_changes = false
end
```

### 5.2 实际 Z 落地最简模型

为避免上述复杂度, **真正最简的 Z**:

1. **Server 端** —
   - 每帧 `_SyncToRoom` **只做** `Broadcast('ecs_delta', ...)`
   - 不再调 PatchState. (即 Phase C v1 的 PatchState 全量替换被新协议替代)
   - 但 server 需自维护一份"当前完整 entities", 用于新 client join 时单独发 baseline

2. **Server 自维护 baseline** —
   - 每次 _SyncToRoom 后, 把"当前完整 entities" 缓存到 `world._baseline`
   - server 端 `world:OnPeerJoin(function(pid) self._sync_room:SendTo(pid, 'ecs_baseline', self._baseline) end)`
   - 但 Light.Network.Room 是否有 `SendTo(pid, name, args)` per-peer broadcast? 需确认

3. **Mirror 端 (client)** —
   - hook `room:OnEvent('ecs_baseline', function(args) self:_ApplyFullState(args) end)`
   - hook `room:OnEvent('ecs_delta', function(args) self:_ApplyDelta(args) end)`
   - 不再 hook `OnState` (因为不再用 PatchState)

4. **更简化** — 直接复用现有 `room:SetState({entities=full})` 在 server 启动 + 重大事件时;  ecs_delta 走 broadcast. mirror 双 hook (OnState 全量重置, OnEvent('ecs_delta') 应用增量).

---

## 6. 疑问澄清

### 6.1 Q4 (额外): Light.Network.Room 是否支持 per-peer Send (类似 SendTo)?

待查源码. 若不支持, 必须用 Broadcast (所有人都收 baseline). 这浪费带宽但实现最简.

### 6.2 Q5 (额外): mirror 端如何区分"baseline OnState" vs "OnState 也可能被其他逻辑触发"?

如果用户在 server 端独立调 `room:SetState`, mirror 同样会收到 OnState. 这是约定问题 — 文档明确 "Mirror 模式下不要手动改 room.state".

---

## 7. 推荐快速决策路径

为了最大化实施速度, 推荐用户:

- **Q1 = A** per-entity comp set
- **Q2 = A** Broadcast('ecs_delta')
- **Q3 = A** (实质等价: 利用现有 SetState/PatchState 推 baseline + ecs_delta 走 broadcast 增量)

如果接受推荐, 直接进 CONSENSUS + DESIGN + TASK 合并文档, 立即实施.

---

## 8. 进入 Stage 1 末尾决策

请用户对 Q1-Q3 给出答复, 或选 **"全用推荐"** 一键通过.
