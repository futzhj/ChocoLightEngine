# Phase F.0.10.9.x.1 — Cross-Instance LUT id Remap PLAN

> 6A · ALIGN + DESIGN + TASK 合并
> 工作量 ~30min

---

## 1. ALIGN

**问题**: F.0.10.9 multi-instance 后, `g.lutTexId` per-instance, 但 LUT 释放/替换路径仅同步 active instance, 其他 instance 悬挂 / 看不到 hot reload.

**4 处调用点** (老 bug):
1. `DeleteLUT3D(id)` line 819
2. `WatchLUT(path)` 同 path 重注册子路径 line ~1290
3. `UnwatchLUT(id)` line 1312
4. `PollLUTReloads()` line 1371

**边界**:
- **In**: 加 `RemapLUTIdAcrossInstances` 辅助 fn + 4 处接入 + smoke 验证
- **Out**: 多 instance Bloom/SSR/MB pyramid 跟随 (留 F.0.10.9.x.2 低优先)

---

## 2. ARCHITECT

```cpp
// anonymous namespace 内
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

**语义**: `newId=0` 释放路径 (同时清 strength); `newId!=0` remap 路径 (strength 保留).

**4 处接入模板**:
- `DeleteLUT3D`: 替换内联 `if (g.lutTexId == lutTex) {...}` 为 `RemapLUTIdAcrossInstances(lutTex, 0u);`
- `UnwatchLUT`: 同上
- `WatchLUT` 重注册子路径: 同上
- `PollLUTReloads`: 替换 `if (g.lutTexId == oldId) g.lutTexId = newId;` 为 `RemapLUTIdAcrossInstances(oldId, newId);`

**关键细节**: `DeleteLUT3D` 顺序调整, state cleanup 必须在 backend 检查之前 (否则 headless 路径 Remap 不跑).

---

## 3. ATOMIZE

| Step | 内容 | 工作量 |
|------|------|------|
| S1 | 看 4 处调用点 + grep 确认 | 5min |
| S2 | 加 RemapLUTIdAcrossInstances + 4 处 multi_edit 接入 | 10min |
| S3 | smoke §25 (2 子项) + 1 处顺序修 (DeleteLUT3D backend null path) | 10min |
| S4 | 验证 + FINAL + commit + CI | 5min |
| **合计** | | **~30min** |

---

## 4. 验收

| 类型 | 标准 |
|------|------|
| smoke §25 | 25.1 + 25.2 PASS |
| 8 相关 smoke | 零回归 |
| CI | 6/6 绿 |
| Lua API | 仍 78 (无新 fn) |
