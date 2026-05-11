# TODO — Phase C.x.1 待办清单

> **6A 工作流 · Stage 6 Assess 交付物 (3/3)**
> 精简列出待办事宜、技术债务、用户操作指引

---

## 1. 用户立即关注 (需决策)

### 1.1 是否启动 Phase C.x.2 (索引优化)

**背景**: 当前 `_FindById` 是 O(n) 线性扫描 `self._entities`. 在 1000+ entity 场景下, 每次 `_SyncToRoom` 构造 set patch 时, 对每个 dirty entity 都要扫一次, 总开销 O(dirty × N).

**选项**:
- **A**: 立即启动 Phase C.x.2, 加 `_entities_by_id[id] = entity` 反查 hash → O(1) lookup
- **B**: 推迟到性能瓶颈出现 (实际项目超 500 entity 时再做)
- **C**: 不做 (当前 demo/smoke 量级 < 100 entity, 实际无感)

**推荐**: B (推迟). 现阶段无实测瓶颈, YAGNI.

---

### 1.2 是否启动 Phase C.x.3 (嵌套字段增量)

**背景**: 当前 `set[id][comp]` 是**完整 component 替换**. 若 component 是 `{x=10, y=20, z=30}`, 只改 `x` 也要发整个 table. 真正的 field-level 增量需要 wire 协议扩展:

```json
{ "set": { "1": { "Pos": { "$patch": { "x": 15 } } } } }
```

**选项**:
- **A**: 立即启动 Phase C.x.3
- **B**: 推迟到带宽实测瓶颈出现
- **C**: 不做 (大多数 component 字段少, 整体替换可接受)

**推荐**: C (不做). component 平均 < 10 字段, 整体替换 wire 增量已经够小.

---

### 1.3 是否补充 demo 自动化 e2e CI 验证

**背景**: `samples/demo_ecs_network` 当前只有手动跑 server + client 两进程验证. 缺自动化 CI 端到端测试 (启 server, 启 client, 验证 mirror 状态收敛).

**选项**:
- **A**: 加 `scripts/smoke/demo_ecs_network_e2e.lua` 双进程脚本 + CI 集成 (~2h)
- **B**: 加 `samples/demo_ecs_network/test_e2e.lua` 单进程 mock 端到端 (~1h)
- **C**: 不做 (smoke 已覆盖核心协议, demo 是教学用途)

**推荐**: B (单进程 mock e2e). 投入小, 覆盖 demo 流程关键路径.

---

## 2. 已知技术债务

### 2.1 [工具链] MSVC raw string + 行注释相邻拼接 bug

**严重度**: 中 (规避方案稳定, 不影响功能)
**症状**: `R"LUA(...)LUA"\n//comment\nR"LUA(...)LUA"` 这种相邻 raw string 字面量, 经 MSVC cl.exe 编译后, 拼接结果 binary 损坏, 导致运行时崩溃
**当前规避**: 必须用 `)LUA" R"LUA(` **单行单空格**格式, 严禁中间插入行注释或多行换行
**长期方案**: 改用 `lua2header.py` 把 Lua 转字节数组 (`unsigned char []`), 完全避开字面量长度限制
**影响范围**: 所有需要嵌入大块 Lua/JS/JSON 等字符串到 cpp 的场景

**操作指引** (后续 cpp 嵌入新 Lua 时):
1. 优先评估嵌入脚本字节数; < 16KB 用 single raw string; ≥ 16KB 必须拆分
2. 拆分严格用 `)LUA" R"LUA(` 单行格式
3. 长期 (Phase D+) 考虑统一改造为字节数组嵌入工具链

---

### 2.2 [性能] `_FindById` O(n) 线性扫描

**严重度**: 低 (当前规模无感)
**位置**: `@e:\jinyiNew\Light\ChocoLight\src\light_ecs.cpp` 内嵌 Lua, `ECSWorld:_FindById`
**触发**: 每次 `_SyncToRoom` 构造 set patch, 对每个 dirty entity id 调用一次
**瓶颈估算**: 1000 entity × 100 dirty/帧 × 60fps = 6M 次扫描/秒 (约 6ms @ pure Lua)
**修复方案**: 加 `world._entities_by_id[id] = entity` 反查表, 在 `CreateEntity` / `DestroyEntity` / `_RemoveEntity` 三处同步维护
**预计工作量**: 0.5h

---

### 2.3 [协议] 不支持 component 字段级增量

**严重度**: 低 (当前组件字段少, 整体替换可接受)
**位置**: wire 协议 `set[id][comp]` 是完整替换
**触发**: 高频小幅修改 (如 Position.x 持续微调), wire 中重复发其他字段
**修复方案**: wire 扩展 `{$patch: {fields}}` 语法, mirror `_ApplyDelta` 加 patch 分支
**预计工作量**: 2-3h (协议设计 + server/client 同步改造 + smoke 扩展)

---

### 2.4 [测试] demo 缺自动化 e2e

**严重度**: 中 (用户文档承诺的功能, 但 CI 无验证)
**位置**: `@e:\jinyiNew\Light\samples\demo_ecs_network`
**触发**: demo `main.lua` 改坏时, smoke 不发现, 只有用户手动跑发现
**修复方案**: 见 §1.3
**预计工作量**: 1-2h

---

## 3. 后续可启动的相关 Phase

| 候选 Phase | 主题 | 预计耗时 | 优先级 |
|-----------|------|----------|--------|
| **Phase C.x.2** | `_entities_by_id` 索引 + entity 增删 hash 同步 | 0.5-1h | 低 (规模 < 500 无感) |
| **Phase C.x.3** | wire 协议 field-level patch (`$patch` 语法) | 2-3h | 低 (除非实测瓶颈) |
| **Phase C.x.4** | demo e2e CI 自动化 | 1-2h | 中 |
| **Phase D** | ECS 渲染系统 (Sprite/Mesh component → 渲染管线) | 8-12h | 高 (业务功能核心) |
| **Phase E** | ECS 序列化 (state save/load + replay) | 6-8h | 中 |

---

## 4. 缺少的配置 / 资源

**无**. Phase C.x.1 没有引入新外部依赖、新环境变量、新 .env 配置或新构建参数. 完全是源码内部优化.

---

## 5. 文档 / Tooling 改进

### 5.1 用户文档

- [ ] `@e:\jinyiNew\Light\docs\API_REFERENCE.md` 中 `Light.ECS.World` 章节增加 `MarkFullResync()` API 说明
- [ ] `@e:\jinyiNew\Light\docs\API_REFERENCE.md` 增加 wire 协议章节, 描述 `ecs_delta` event 格式 (供自定义 client 实现者参考)
- [ ] `@e:\jinyiNew\Light\samples\demo_ecs_network\README.md` 在 "预期输出" 章节增加 v1 → v1.1 行为差异说明 (mirror 不再因 set 缺失而删除 component)

**预计工作量**: 1h

### 5.2 Tooling

- [ ] `scripts/tools/extract_embedded_lua.ps1` (从 cpp 提取嵌入 Lua + lightc 校验) — 当前是手动 PowerShell one-liner, 应固化为脚本工具
- [ ] CI 加 raw string 字节数 lint (CMakeLists 或 pre-commit hook 检查 cpp 中 `R"..."` 单段 < 14KB 安全阈值)

**预计工作量**: 2h

---

## 6. 待办速查表

| 优先级 | 项 | 工作量 | 负责模块 |
|--------|----|--------|----------|
| ⭐⭐ | demo e2e CI 自动化 (§1.3) | 1-2h | smoke + samples |
| ⭐⭐ | API_REFERENCE 增加 MarkFullResync + wire 协议 (§5.1) | 1h | docs |
| ⭐ | `_entities_by_id` 索引 (§2.2) | 0.5h | light_ecs.cpp |
| ⭐ | extract_embedded_lua 工具化 (§5.2) | 0.5h | scripts/tools |
| ◯ | wire field-level patch (§2.3) | 2-3h | light_ecs.cpp + smoke |
| ◯ | raw string lint CI 集成 (§5.2) | 1.5h | .github/workflows |

⭐⭐ = 建议近期处理 / ⭐ = 中期 / ◯ = 实测瓶颈再做

---

## 7. 未解决的开放问题

### 7.1 wire 字节数实测缺失

**问题**: Phase C.x.1 设计阶段声称 wire 带宽节省 50-99%, 但**未实测**实际项目场景的字节数对比. 仅基于结构推断.

**操作指引**:
- 后续在真实项目集成 ECS 后, 加 wire 字节数日志 (server `_SyncToRoom` 处 `cjson.encode(payload)` 后打字节数)
- 与 Phase C v1 fallback (重新启用 `PatchState`) 对比, 收集数据写入 FINAL §6 性能表

---

### 7.2 MarkFullResync 多次连续调用语义

**问题**: 文档说 "one-shot", 但若用户在一帧内多次调 `MarkFullResync`, 行为是否符合预期?

**当前实现**: `_needs_full_resync = true` 多次置 true 等价于一次, 下次 `_SyncToRoom` 消费后清零, 等价于一次 full resync. ✅ 行为正确, 但 smoke 未覆盖此场景.

**操作指引**: 若用户报告异常, 加 smoke 断言 "多次 Mark + 单次消费 = 单次 full resync".

---

## 8. 操作指引 (FAQ)

### Q1: 如何在我自己的项目集成 Phase C.x.1?

**答**: API 表面无破坏, 现有 Phase C v1 代码自动享受增量优化. 推荐调整两处:

```lua
-- 1. server 端 room:OnJoin 内显式调 MarkFullResync (新 peer 拿全量 baseline)
room:OnJoin(function(peerId, name)
    print("peer joined:", peerId, name)
    world:MarkFullResync()  -- ← 新增此行
end)

-- 2. client 端 mirror 不再依赖 OnState (内部已自动切到 OnEvent)
local mirror = Light.ECS.MirrorFromRoom(room)
-- 不要再手动 room:OnState 抢通道, 否则会与 mirror 冲突
```

### Q2: wire 兼容老 client 怎么办?

**答**: 当前不兼容. 老 client (hook `room:OnState`) 不会收到 `ecs_delta` event. 解决方案:
- 选项 A: 把所有 client 升级到 Phase C.x.1 版 `MirrorFromRoom`
- 选项 B: server 端同时发 `PatchState` (Phase C v1 兼容通道) **和** `Broadcast('ecs_delta')` (新通道) — **未实现**, 需 Phase C.x.4 添加 fallback 开关

### Q3: 如何调试 wire 内容?

**答**:
```lua
-- server 端劫持 Broadcast 看 payload
local origBroadcast = room.Broadcast
room.Broadcast = function(self, event, payload)
    if event == 'ecs_delta' then
        print("[wire]", event, cjson.encode(payload))
    end
    return origBroadcast(self, event, payload)
end
```

---

## 9. 文档版本与变更

| 版本 | 日期 | 备注 |
|------|------|------|
| 1.0 | 2026-05-11 | Phase C.x.1 Assess 阶段初版, 含 1 hotfix 主修 + 1 hotfix MSVC bug |

文档维护建议: 每次 Phase C.x.N (N ≥ 2) 启动时, 检查本文档 §3 "后续可启动 Phase" 表是否清空对应行; 完成后在 §6 速查表标✓.
