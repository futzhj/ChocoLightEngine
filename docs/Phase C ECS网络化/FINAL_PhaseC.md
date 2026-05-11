# FINAL — Phase C ECS 网络化 项目总结

> **6A 工作流 · Stage 6 (Assess) 收尾文档**
> 项目级回顾, 沉淀经验. 阅读对象: 后续维护者 / 阶段开发者.

---

## 1. 项目目标回顾

把 ChocoLight 引擎现有 `Light.ECS` (纯 Lua, world / entity / component / system) 与 Phase BC `Light.Network.Room` (server-authoritative state 同步) 集成, 让用户:

- 在 server 端用既有 ECS API 写游戏逻辑, 引擎自动把 networked component 变化通过 Room state 广播
- 在 client 端用 `MirrorFromRoom` 拿到一份只读 mirror world, 直接 query 拿 server 状态

最小侵入, 100% 向后兼容.

---

## 2. 交付内容总览

### 2.1 代码 (4 个文件)

```
ChocoLight/src/light_ecs.cpp                  modified  (~+200 Lua, lua_rawset fix)
scripts/smoke/ecs_network.lua                 new       247 行
samples/demo_ecs_network/main.lua             new       180 行
samples/demo_ecs_network/README.md            new        85 行
```

### 2.2 6A 文档 (7 份)

```
docs/Phase C ECS网络化/
├── ALIGNMENT_PhaseC.md     Stage 1 对齐: 现状分析 + Q1-Q3 决策
├── CONSENSUS_PhaseC.md     Stage 1 末: 决策固化 + 验收标准
├── DESIGN_PhaseC.md        Stage 2: 架构 + 数据流图 + 接口契约
├── TASK_PhaseC.md          Stage 3: 6 个 atomic task + 依赖图
├── ACCEPTANCE_PhaseC.md    Stage 6: 逐项验收记录
├── FINAL_PhaseC.md         Stage 6: 本文 (项目总结)
└── TODO_PhaseC.md          Stage 6: 待办与后续可做项
```

### 2.3 Commits (Phase C)

| Commit | 主题 |
|--------|------|
| `ea05eaa` | docs(phase-c): 6A stages 1-3 (Align/Consensus/Design/Atomize) |
| `d755f50` | feat(phase-c): ECS networked components + Room state sync (C-T1..T5) |
| `7864072` | fix(phase-c): use lua_rawset for Light.ECS to bypass OOP metatable |
| `eaf596b` | feat(phase-c): demo_ecs_network end-to-end sample (C-T6) |

---

## 3. 关键设计决策

### 3.1 Q1 — 透明 API (用户决策 A)

**选择**: 不引入 NetworkedEntity / NetworkedWorld 等单独类型, 直接给 ECSWorld 加 `NetworkSync(room)`.

**理由**:
- 用户既有代码 `entity:Add("Position", {x=1})` 一字不改
- 减少 API 表面积 (一个新方法 vs 一组新类)
- 客户端 mirror 用 `MirrorFromRoom` 工厂, 仍是 ECSWorld 实例, 符合 Liskov

**代价**: ECSWorld 内部多了一些 `if self._networked_comps[name] then ...` 分支, 少量可读性损失.

### 3.2 Q2 — Per-entity wire 粒度 (用户决策 A)

**选择**: `state.entities[id]` 顶层映射, 每个 entity 一行.

**理由**:
- 与 PatchState v2 的"顶层 key set/del"语义直接对接
- 服务端 `PatchState({entities = {...}})` 一次同步全部, 实现简单
- 单 entity 改动重发整 row 在 ≤50 entity 场景下带宽 < 4KB, MVP 完全可接受

**代价**: 1k+ entity 时不优. 留待 Phase C.x 升级到 per-component patch.

### 3.3 Q3 — 显式 networked 标记 (用户决策 B)

**选择**: `RegisterComponent("Position", defaults, {networked=true})`.

**理由**:
- 用户清楚区分本地状态 vs 同步状态 (避免误传 input flag / cooldown 等只在 server 用的字段)
- 性能可控 (只有显式标记的 component 进 dirty 跟踪)
- 与 `samples/demo_ecs_network` 中 `Tag` 留在 server-only 的需求自然契合

**代价**: 用户需要额外一行声明; 但跟 Phase BC `room:RegisterRpc` 风格一致.

---

## 4. 实现亮点

### 4.1 Lua OOP 框架坑位 (lua_rawset)

**问题**: `luaopen_Light_ECS` 用 `lua_setfield(L, -2, "ECS")` 把 module 挂到 `Light` 全局, 触发 OOP framework metatable `__newindex` 链, 走到 fallback `error('object is a static module')`, 导致整个引擎启动失败.

**根因**: `_G.Light` 是 `light_module.cpp` 注入的特殊 callable global, 其 metatable 不允许任意属性写入.

**修复**: 改用 `lua_rawset` 绕过 metatable, 直接写入 raw table.

**沉淀**: `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp:358-364` 已加详细中文注释指引后续维护者. memory `cb79c6b3` 也已固化此知识.

### 4.2 Mirror 端引用稳定性 (R5)

**需求**: 客户端用户可能缓存 `local pos = entity.Position`, 之后 server 推新值, 用户期望 `pos.x` 自动更新.

**实现**: `_ApplyState` 对已有 component 用浅 merge (`for k,v in pairs(incoming) do target[k] = v end` + 删除 incoming 没有的字段), 而非整 table 替换. component table 本身引用稳定.

**验证**: smoke `C-T4: shallow-merge keeps Pos table reference stable ok`.

### 4.3 透明 dirty 触发

`entity:Add` / `Set` / `Remove` / `world:DestroyEntity` 内部统一查 `world._networked_comps[name]`, 决定是否设 `_has_changes`. 用户写法不变, 引擎内部完成同步决策.

### 4.4 _SyncToRoom 与 v2 PatchState 直接对接

```lua
self._sync_room:PatchState({entities = entitiesTable})
```

复用 Phase BC v2 wire 协议, 不发明私有协议. mirror 端通过 `room:OnState` 自动收到 reconciled state.

---

## 5. 经验教训

### 5.1 教训: ChocoLight luaopen 写法陷阱

任何新模块 `luaopen_Light_XXX` 把 module 挂到 `_G.Light` 时, **必须用 `lua_rawset`** 而非 `lua_setfield`. 否则触发 OOP framework fallback. 此前 Phase AV `light_animation.cpp` 也踩过同样的坑 (commit 88954f4 → fix run 25610604731).

**预防**: memory `cb79c6b3` 已收录该规则. 创建新模块时必看.

### 5.2 经验: 6A 工作流缩短了决策时间

本次 Phase C 用户在 ALIGNMENT 阶段直接选 `Q1=A, Q2=A, Q3=B` 推荐组合, 跳过反复讨论. CONSENSUS / DESIGN / TASK 文档变成"参考素材", Stage 5 实施直接落地. 关键: Stage 1 决策清单要少而精 (只问真歧义点), 用户才愿意一次性决.

### 5.3 经验: smoke + demo 双轨

- **smoke** (`scripts/smoke/ecs_network.lua`) — mock room, 单进程, CI 自动跑, 覆盖正确性
- **demo** (`samples/demo_ecs_network/`) — 真实双进程, 用户手动验证端到端, 不进 CI

两者目标不同, 不互斥. CI 卡 smoke (硬质量门), demo 给用户开箱即用感.

### 5.4 经验: PatchState v2 顶层 wholesale 替换 = ECS 同步免费红利

Phase BC v2 PatchState 设计的"顶层 key set 整个替换 / delete 数组移除"恰好与 ECS `state.entities` 整体重发的简化方案完美匹配. 不需要嵌套 patch 协议, 实现 `_SyncToRoom` 仅 ~15 行 Lua.

---

## 6. 性能 / 资源占用 (估算)

MVP 场景 (≤50 networked entity, 每 entity 平均 2 个 networked component, 每 component ~30 byte):

| 项 | 量 |
|----|---|
| 每帧 wire 大小 | ~3 KB JSON |
| 60 fps 持续 | ~180 KB/s = 1.4 Mbps (远低于 10 Mbps 实际限制) |
| Lua 内存增量 | < 50 KB (dirty / mirror_by_id 索引) |
| CPU overhead (server) | _SyncToRoom < 0.1 ms / 帧 |
| CPU overhead (client) | _ApplyState < 0.05 ms / 帧 |

> 注: 上述为 napkin 估算, 实测留给 Phase C.x bench. Phase C MVP 目标场景已充裕.

---

## 7. 兼容性矩阵

| 维度 | 状态 |
|------|------|
| Phase 2 ECS API (`Light.ECS.World.new`, `entity:Add/Get/Has`) | ✅ 全部保留 |
| Phase BC `Light.Network.Room` v1 (Host/Join/PatchState/Kick) | ✅ 直接复用 |
| Phase BC v2 (PatchState set+del, RPC timeout) | ✅ 直接复用 |
| 现有 `samples/` 中 ECS demo | ✅ 不受影响 (无破坏性变更) |
| Lumen Lua 5.1 (`unpack` / `table.unpack`) | ✅ 内部用 `table.unpack`, 用户层无感 |

---

## 8. 后续路线图建议

### 8.1 Phase C.x — 优化 (按优先级)

1. **Per-component dirty** — 减少带宽, 大场景必备
2. **AOI / spatial culling** — 大世界场景
3. **Schema 校验** — server / client `RegisterComponent` 自动握手
4. **二进制压缩** — 替换 JSON, 大场景带宽优化
5. **Bench / 性能基准** — 1k entity 量级压测

### 8.2 Phase D 候选 (Phase C 之后)

- **Phase D — 持久化** — DB.SQLite + ECS, server side 状态落盘
- **Phase D' — 输入预测 / lag compensation** — 客户端预测 + 回滚
- **Phase D'' — 动画 + 网络** — Animator state 同步 (与 Phase AV 联动)

详见 `@e:\jinyiNew\Light\docs\Phase C ECS网络化\TODO_PhaseC.md`.

---

## 9. 致谢与署名

- 实现 / 文档: Cascade (AI agent)
- 决策 / 验收: 用户
- 工作流: 6A (Align → Architect → Atomize → Approve → Automate → Assess)
- 基础设施: Phase BC `Light.Network.Room` + ENet + cJSON

Phase C v1 完成. 🎉
