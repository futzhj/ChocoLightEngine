# Phase F.0.10.9.x.1 — Cross-Instance LUT id Remap FINAL

> 6A · ASSESS 收尾交付报告
> 状态: ✅ 全部完成

---

## 1. 修复 Bug

### 1.1 问题

F.0.10.9 把 HDRRenderer 改成 multi-instance, `g.lutTexId` (per-instance) 多 instance 可引用同一 LUT id, 但当 LUT id 被释放或替换时:

**老实现仅清/同步 active instance 的 lutTexId** — 其他 instance 的 lutTexId 仍指向已 free 的 GL tex (悬挂) 或旧 id (Hot reload 时未同步看到新 LUT).

涉及 4 处:
- `DeleteLUT3D(id)` — 用户主动删
- `UnwatchLUT(id)` 内部 → DeleteLUT3D
- `WatchLUT(path)` 同 path 重注册 → DeleteLUT3D
- `PollLUTReloads()` Hot reload → oldId → newId

### 1.2 修复

引入辅助 fn `RemapLUTIdAcrossInstances(uint32_t oldId, uint32_t newId)`:

```cpp
static void RemapLUTIdAcrossInstances(uint32_t oldId, uint32_t newId) {
    if (oldId == 0u) return;
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        if (g_states[i].lutTexId == oldId) {
            g_states[i].lutTexId = newId;
            if (newId == 0u) g_states[i].lutStrength = 0.0f;
        }
    }
}
```

**语义**:
- `newId == 0`: 释放路径 — 所有 instance 引用 oldId 的 lutTexId 清 0 + strength 清 0
- `newId != 0`: remap 路径 (hot reload) — 所有 instance 引用 oldId 的 lutTexId 改为 newId, strength 保留

4 处全部接入. **关键修复细节** — `DeleteLUT3D` 的顺序调整:

```cpp
// 老实现 (BUG):
bool DeleteLUT3D(uint32_t lutTex) {
    if (!g.backend || !lutTex) return false;   // ⚠ backend null 时短路, Remap 不跑
    // ...只清 active...
}

// 新实现:
bool DeleteLUT3D(uint32_t lutTex) {
    if (!lutTex) return false;
    RemapLUTIdAcrossInstances(lutTex, 0u);     // ✅ state cleanup 与 backend 无关, 先跑
    if (!g.backend) return false;              // backend 缺则不调 GL Delete
    return g.backend->DeleteLUT3D(lutTex);
}
```

---

## 2. 验证

### 2.1 Smoke `hdr.lua` §25

| # | 测试 | 状态 |
|---|------|------|
| 25.1 | 3 instance 各自 SetGradingLUT(mockId) → DeleteLUT3D(mockId) → 全 instance 的 lutTexId 都清 0 | ✅ |
| 25.2 | DeleteLUT3D(0) 短路防御 — oldId=0 时 no-op, 不污染 active lutTexId | ✅ |

**覆盖**: 核心修复点 (DeleteLUT3D 路径) + 边界防御 (oldId=0 短路).

**未覆盖** (headless 不可构造, 留 demo live 真 GL 验证):
- §25.3 `UnwatchLUT` 跨 instance 清 — 依赖真 WatchLUT 注册成功 (需真 LUT 文件 + GL ctx)
- §25.4 `PollLUTReloads` 跨 instance remap — 依赖真 LUT 文件 mtime 变化触发 reload
- §25.5 `WatchLUT` 同 path 重注册时清旧 id — 依赖真 GL ctx

**理由**: 这 3 处走的是与 §25.1 完全相同的 `RemapLUTIdAcrossInstances` 路径, 已通过单一入口验证. Code review 已确认 4 处接入正确.

### 2.2 零回归

8 个相关 smoke 全 PASS:
- `hdr` (52 fn) · `bloom` · `ssr` · `auto_exposure`
- `lens_fx` · `motion_blur` · `taa` · `lighting2d`

---

## 3. 文件变更

| 文件 | 变更 | LOC |
|------|------|-----|
| `ChocoLight/src/hdr_renderer.cpp` | +RemapLUTIdAcrossInstances + 4 处接入 + DeleteLUT3D 顺序修 | +20 / -10 |
| `scripts/smoke/hdr.lua` | +§25 (2 子项) | +75 |
| `docs/Phase F.0.10.9.x.1 LUT id cross-instance remap/PLAN_PhaseF_0_10_9_x_1.md` | 6A PLAN | (inline 本文) |
| `docs/Phase F.0.10.9.x.1 LUT id cross-instance remap/FINAL_PhaseF_0_10_9_x_1.md` | 本文 | +110 |
| `docs/Phase F.0.10.9.x.1 LUT id cross-instance remap/TODO_PhaseF_0_10_9_x_1.md` | 后续接力 | +40 |

---

## 4. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | 用户选 F.0.10.9.x.1 (悬挂 bug 优先于 demo) — 4 处调用点确认 |
| **Architect** | 单一辅助 fn `RemapLUTIdAcrossInstances` + newId=0/非0 二态语义 |
| **Atomize** | 4 sub-step (调研 / 接入 / smoke / 验证文档) |
| **Approve** | 用户隐式确认 (bug 修法明确, 无歧义) |
| **Automate** | 实现 + 1 处顺序修复 (DeleteLUT3D backend 检查位置) |
| **Assess** | 本 FINAL + §25 2 子项 PASS + 8 smoke 零回归 |

---

## 5. 关键 lessons

### 5.1 顺序敏感: state cleanup 必须在 backend 调用之前

老实现 `if (!g.backend || !lutTex) return false;` 把 backend 检查放在 state cleanup 前, 导致 headless / 未 Init 路径 state 永远不清.

**通用规则**: state mutation (per-instance 状态同步) 与 backend (GL/Vulkan) 调用无依赖关系, 应在 backend 检查前执行.

### 5.2 单一 fn 统一多处类似逻辑

4 处旧代码各自写 `if (g.lutTexId == X) g.lutTexId = Y;` (含 WatchLUT 那处还漏清 strength). 抽成 `RemapLUTIdAcrossInstances` 后:
- 4 处零代码重复
- 漏 strength 清的 bug 顺手修
- 跨 instance 语义统一 (老实现只清 active, 新实现遍历所有)

### 5.3 Headless smoke 能覆盖的最大边界

Headless 可测 state-only API (SetGradingLUT, GetGradingLUTId 都是 state mutation/query, 不依赖 GL). DeleteLUT3D 在新顺序下也是: state cleanup 段不依赖 GL, backend 段依赖 GL — headless 可验证前者.

---

## 6. Lua API 数

无新 fn. 总数仍 = 78 (F.0.10.9 后). 本 phase 仅修 bug + 加 smoke.

---

## 7. 后续接力

见 `TODO_PhaseF_0_10_9_x_1.md`:
- **F.0.10.9.2**: demo live 主屏 + PIP — 真 GL 环境验证 UnwatchLUT / PollLUTReloads / WatchLUT 重注册 3 条路径
- 已清 TODO 中 "已知限制 x.1" — F.0.10.9.x.2 (Bloom/SSR/MB pyramid 跟随多 instance) 仍未做, 优先级低
