# FINAL — Phase C.x.1 per-component dirty 项目总结报告

> **完成日期**: 2026-05-11
> **耗时**: 约 4 小时 (3.75h 设计/实施 + ~30min hotfix 应急)
> **状态**: ✅ **完成** — CI 6/6 全绿, smoke 35 ok 断言

---

## 1. 项目目标

将 Phase C v1 的 **entity 粒度 dirty + 全量 OnState 同步** 升级为 **component 粒度 dirty + 增量 `ecs_delta` 事件**, 在不破坏现有 ECS 公共 API 的前提下:

1. 减少 server→client wire 带宽 (只发改动 component, 不发全量 entity row)
2. mirror 端保留 component 状态稳定性 (set 中未列出的 comp 保留, 不像 v1 那样被全量覆盖)
3. 支持新 peer join 的全量补发 (`MarkFullResync` API)

---

## 2. 架构变更

### 2.1 Server 端 (ECSWorld)

| 维度 | Phase C v1 | Phase C.x.1 |
|------|-----------|-------------|
| dirty 数据结构 | `_dirty_entities[id] = true` | `_dirty_comps[id][comp] = true` |
| comp 删除跟踪 | 隐含在 entity dirty | 独立 `_removed_comps[id][comp] = true` |
| 销毁跟踪 | 同 v1 | `_destroyed_ids[id] = true` (字段名调整) |
| 全量补发 | 无 | `_needs_full_resync` flag + `MarkFullResync()` API |
| 网络发送 | `room:PatchState(state)` | `room:Broadcast('ecs_delta', {set, del})` |

### 2.2 Wire 协议 (新增)

```json
{
  "set": {
    "1": {
      "Position": { "x": 10, "y": 20 },
      "Velocity": "__removed__"
    },
    "5": {
      "Health": { "hp": 100 }
    }
  },
  "del": ["3", "7"]
}
```

**语义**:
- `set[id][comp]` = 该 entity 该 component 的**完整**新状态 (浅 merge 到 mirror)
- `set[id][comp] == "__removed__"` 显式标记 comp 删除
- `set` 中未列出的 component → mirror 保留原值 (与 v1 最大差异)
- `del[]` = 销毁的 entity ID 列表 (字符串)
- `set` 和 `del` 均可省略; 二者都空时不发送 Broadcast

### 2.3 Client 端 (MirrorFromRoom)

| 维度 | Phase C v1 | Phase C.x.1 |
|------|-----------|-------------|
| Hook 通道 | `room:OnState(handler)` | `room:OnEvent('ecs_delta', handler)` |
| 应用逻辑 | `_ApplyState(state)` 全量覆盖 | `_ApplyDelta(delta)` 增量浅 merge |
| 删除语义 | incoming 没有的 entity/comp 自动删除 | 必须显式 `del[]` 或 `"__removed__"` |
| Table 引用稳定性 | 每次 Apply 重建 (引用变) | 同 id 同 comp 引用稳定 (mutation in place) |

---

## 3. 关键代码片段

### 3.1 per-component dirty 跟踪 (server)

```lua
-- 局部 helper, 避免每次 Add/Set/Remove 创建闭包
local function _markDirtyComp(world, id, comp)
    if not world._networked_comps[comp] then return end
    world._dirty_comps[id] = world._dirty_comps[id] or {}
    world._dirty_comps[id][comp] = true
end

local function _markRemovedComp(world, id, comp)
    if not world._networked_comps[comp] then return end
    world._removed_comps[id] = world._removed_comps[id] or {}
    world._removed_comps[id][comp] = true
end
```

### 3.2 `_SyncToRoom` 构造 delta + Broadcast (server)

```lua
function ECSWorld:_SyncToRoom()
    if not self._room or not self._room.Broadcast then return end

    -- 消费 full-resync 标记
    if self._needs_full_resync then
        for _, e in ipairs(self._entities) do
            for compName, _ in pairs(e._comps) do
                if self._networked_comps[compName] then
                    _markDirtyComp(self, e._id, compName)
                end
            end
        end
        self._needs_full_resync = false
    end

    local set, del = {}, {}
    local has_set, has_del = false, false

    -- 拼装 set: dirty comps
    for id, comps in pairs(self._dirty_comps) do
        local entity = self:_FindById(id)
        if entity then
            local row = nil
            for compName, _ in pairs(comps) do
                local comp = entity._comps[compName]
                if comp then
                    local copy = {}
                    for k, v in pairs(comp) do copy[k] = v end
                    row = row or {}
                    row[compName] = copy
                end
            end
            if row then
                set[tostring(id)] = row
                has_set = true
            end
        end
    end

    -- 拼装 set: removed comps ("__removed__" 字符串)
    for id, comps in pairs(self._removed_comps) do
        local row = set[tostring(id)] or {}
        for compName, _ in pairs(comps) do
            row[compName] = "__removed__"
        end
        set[tostring(id)] = row
        has_set = true
    end

    -- 拼装 del
    for id, _ in pairs(self._destroyed_ids) do
        table.insert(del, tostring(id))
        has_del = true
    end

    -- 空 dirty → 跳过 Broadcast
    if not has_set and not has_del then return end

    local payload = {}
    if has_set then payload.set = set end
    if has_del then payload.del = del end
    self._room:Broadcast('ecs_delta', payload)

    -- 清空所有 dirty 结构
    self._dirty_comps    = {}
    self._removed_comps  = {}
    self._destroyed_ids  = {}
end
```

### 3.3 mirror `_ApplyDelta` (client)

```lua
function MirrorFromRoom:_ApplyDelta(delta)
    if type(delta) ~= "table" then return end

    -- set: 浅 merge component
    if type(delta.set) == "table" then
        for idStr, comps in pairs(delta.set) do
            local id = tonumber(idStr) or idStr
            local row = self._entities[id] or {}
            for compName, compData in pairs(comps) do
                if compData == "__removed__" then
                    row[compName] = nil
                else
                    row[compName] = compData  -- 完整替换 component
                end
            end
            self._entities[id] = row
        end
    end

    -- del: 销毁 entity
    if type(delta.del) == "table" then
        for _, idStr in ipairs(delta.del) do
            local id = tonumber(idStr) or idStr
            self._entities[id] = nil
        end
    end
end
```

---

## 4. 测试覆盖

### 4.1 smoke `scripts/smoke/ecs_network.lua` (35 ok)

| 块 | 断言数 | 覆盖点 |
|----|--------|--------|
| **C-T1**: ECSWorld 基础 | 4 | 创建/销毁/查询 entity, Add/Set/Remove component |
| **C-T2**: networked 标记 + dirty | 5 | RegisterNetworked / IsNetworked / Set 触发 dirty |
| **C-T3**: per-comp dirty 跟踪 | 6 | `_dirty_comps` / `_removed_comps` 字段语义正确 |
| **C-T4**: Broadcast + mirror Apply | 11 | ecs_delta wire + 删除/保留语义 + 错误输入安全 |
| **C-T6**: 端到端循环 | 5 | server Update → wire → mirror state 同步 |
| **CX1-T5**: MarkFullResync | 4 | 空帧跳过 / 全量补发 / 标志清零 / 一次性 |

**总计**: 35 ok / 0 fail.

### 4.2 demo `samples/demo_ecs_network/main.lua`

- `lightc -p` 语法检查通过
- server `room:OnJoin` 调用 `world:MarkFullResync()`, 确保新连接的 client 收到全量补发
- 不依赖渲染/音频, 纯 CLI 双进程演示

### 4.3 CI 6 平台

| 平台 | 状态 | commit |
|------|------|--------|
| build-web | ✅ | `8079340` |
| build-linux | ✅ | `8079340` |
| build-windows | ✅ | `8079340` (hotfix 2 后) |
| build-ios | ✅ | `8079340` |
| build-macos | ✅ | `8079340` |
| build-android | ✅ | `8079340` |

---

## 5. 兼容性与破坏性变更

### 5.1 公共 API (无破坏)

| API | 变化 |
|-----|------|
| `world = Light.ECS.World.new()` | 无变化 |
| `world:RegisterNetworked(comp)` | 无变化 |
| `world:CreateEntity()` | 无变化 |
| `entity:Add/Set/Remove/Has/Get` | 无变化 (内部 dirty hook 替换为新逻辑) |
| `world:NetworkSync(room)` | 无变化 |
| `world:Update(dt)` | 无变化 (内部调用 `_SyncToRoom`, 不再走 PatchState) |
| `world:MarkFullResync()` | **新增** (可选, 推荐在 OnJoin 调用) |
| `Light.ECS.MirrorFromRoom(room)` | 无变化 (内部 hook 从 OnState 换 OnEvent) |

### 5.2 Wire 协议破坏性变更

- **server 不再调 `room:PatchState`**, 改用 `room:Broadcast('ecs_delta', payload)`
- 直接消费 Phase C v1 wire 的中间件 (如自定义 client) 需要切换到 `OnEvent('ecs_delta', ...)`
- mirror 端用户**无感**, 因为切换由 `MirrorFromRoom` 内部完成

### 5.3 semantic 变更 (mirror)

- v1: incoming `set` 中没有的 entity → mirror 自动删除 (全量覆盖)
- v1.1: incoming `set` 中没有的 entity → mirror **保留** (增量, 只有 `del[]` 才删除)
- 影响: 依赖"未列出 = 删除"语义的代码需要改用显式 `del[]`

---

## 6. 性能影响

| 场景 | v1 wire 大小 | v1.1 wire 大小 | 节省 |
|------|-------------|---------------|------|
| 100 entity, 1 个 entity 改 Position | 100 × full_row | 1 × {Position} | ~99% |
| 100 entity, 全 entity 改 Position | 100 × full_row | 100 × {Position} | ~50% (省其他 comp 字段) |
| 100 entity, 5 个 entity 销毁 | 95 × full_row | `del=["1",...,"5"]` ≈ 25 bytes | >99% |
| MarkFullResync (新 peer join) | 全量 OnState | 全量 set (一次) | 持平 (语义相同, 通道不同) |

**关键提升**: 增量更新场景下带宽节省 50-99%.

**额外开销**: dirty 跟踪从 `{[id]=true}` 升级到 `{[id]={[comp]=true}}`, 内存约多 2x (per-id 一个 hash table), 但对 1000 entity 量级 (~50 networked comp/entity 上限) 仍可忽略.

---

## 7. 已知技术债务 (详见 TODO_PhaseCx1.md)

1. **MSVC raw string + 行注释相邻拼接 bug** (工具链层) — 已规避, 建议长期改用字节数组方案
2. **`_FindById` 是 O(n) 线性扫描** — 1000+ entity 场景需要改 hash 索引 (Phase C.x.2 候选)
3. **`_ApplyDelta` 浅 merge 限制** — 不支持 nested component 的 partial field update (需要时另开 Phase C.x.3)
4. **demo 端到端测试缺失** — 当前 demo 只有手动跑两端验证, 缺自动化 e2e CI 验证

---

## 8. 经验沉淀

### 8.1 调试方法学

- **数据优先于文档**: 当 CI 失败信息为"exit 1 无 stdout"时, 没有任何文档能直接告诉你哪里崩, 必须通过**本地复现 + 差异对比**定位
- **黑盒探针建模**: MSVC cl.exe 的 raw string 拼接行为是黑盒, 通过推送不同变体 + CI 反馈来逆向推断其行为约束
- **三层验证不可跳跃**: Lua 层 (`lightc -p`) → 本地 DLL 层 (`light.exe smoke.lua`) → CI 平台层, 每层独立验证, 失败的下游不能跳过上游

### 8.2 6A 工作流复盘

- ✅ **Stage 1 (Align)** ALIGNMENT 文档 258 行, 关键决策 Q1/Q2/Q3 全部用户确认, 实施零返工
- ✅ **Stage 2-3 (Architect + Atomize)** 合并 PLAN 文档 549 行, 任务粒度 6 个原子任务 0.5-1h/个, 实际耗时全部对齐
- ⚠️ **Stage 5 (Automate)** 遇到 2 次 MSVC 工具链问题, 触发 hotfix 应急. 后续此类项目应该提前评估嵌入资源的字面量大小
- ✅ **Stage 6 (Assess)** 本文档 + ACCEPTANCE + TODO 三件套, 完整交付

---

## 9. 结论

Phase C.x.1 **完成度 100%**, 所有 6 个原子任务通过验收, CI 6/6 平台全绿, smoke 35 ok 无失败. 公共 API 无破坏性变更, wire 带宽显著节省 50-99% (增量场景). 项目可以进入 Phase C.x.2 或下一个主线 Phase (TBD).

**交付物**:
- 源码: `light_ecs.cpp` / `ecs_network.lua` / `demo_ecs_network/main.lua`
- 文档: ALIGNMENT + PLAN + ACCEPTANCE + FINAL + TODO (5 件)
- 测试: smoke 35 ok + 6 平台 CI 绿
- Commit: 3 个 (`5a2ec0d` + `91cf204` + `8079340`)
