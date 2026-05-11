# ACCEPTANCE — Phase E.2.1 · Lighting2D dirty bit

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.2.1**：在 `Lighting2D::State` 加单调递增 `version`，GL33Backend 缓存 `lastUploadedLighting2DVersion`，相等则跳过所有 `glUniform*v`。

---

## 1. 改动摘要

| 文件 | 改动 | 关键点 |
|------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\include\light_lighting2d.h` | `State` 加 `uint32_t version = 1;` + 声明 `GetVersion()` | POD 性质保留；初值 1 与 backend 初值 0 保证首次 upload 一定 mismatch |
| `@e:\jinyiNew\Light\ChocoLight\src\light_lighting2d.cpp` | 6 处 mutator 末尾 `++g_state.version`；新增 `GetVersion()` / `l_GetVersion`；注册表加 `"GetVersion"` 条目 | `SetEnabled` / `SetAmbient` / `Add` / `Update` / `Remove` / `Clear` 全覆盖；幂等 no-op 路径不递增 |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | `GL33Backend` 加 `uint32_t lastUploadedLighting2DVersion = 0;`；`UploadLighting2D` 入口加 version 相等早返回 | 仍保留 `glUseProgram(programLit2D)`（caller 可能切过 program）；只跳过 SOA build + 8 次 `glUniform*v` |
| `@e:\jinyiNew\Light\scripts\smoke\lighting2d.lua` | §16 5 段断言：initial > 0 / Get* 不递增 / 6 mutator 递增 / 幂等 no-op 不递增 / 17th Add 不递增 | 间接验证 backend dirty skip 的前置条件 |

---

## 2. 关键设计

### 2.1 递增语义

- **递增** 所有"改写 state"的 mutator，**即使值相同**（如 `SetEnabled(true)` 两次都 ++）— 保持简单，避免"值相等比较" 复杂度
- **不递增** 幂等 / 失败路径：
  - `RemoveLight(invalid_id)` / `RemoveLight(already_inactive_slot)` → early return
  - `UpdateLight(invalid_id)` / `UpdateLight(inactive_slot)` → early return
  - `AddPointLight` 当 16 slot 满 → early return

### 2.2 哨兵选择

- `State::version` 初值 = **1**
- `GL33Backend::lastUploadedLighting2DVersion` 初值 = **0**
- 首次 `UploadLighting2D` 一定 mismatch（0 ≠ 1）→ 触发首次上传
- uint32 溢出周期 ~497 天（假设 1ms/update），可忽略

### 2.3 为什么 `glUseProgram` 仍在 early return 之前

即使 dirty skip，也**必须**切 program：
- caller（如 `BeginLit2DDraw`）可能切了其它 program（普通 2D / PBR / unlit）
- shader uniform 属于 program state，下一次 draw 要用 `programLit2D` 就必须先 `glUseProgram`
- `glUseProgram` 本身是 GL state switch，但比 `glUniform*v` 便宜得多

---

## 3. 验收清单

| 标准 | 状态 | 证据 |
|------|------|------|
| `State.version` 递增字段 + `GetVersion()` | ✅ | `light_lighting2d.h:58`、`light_lighting2d.cpp:130-132` |
| 6 个 mutator 全覆盖 `++version` | ✅ | smoke §16.3 验证（`SetEnabled` / `SetAmbient` / `AddPointLight` / `UpdateLight` / `RemoveLight` / `AddSpotLight` / `ClearLights`） |
| GL33Backend 缓存 `lastUploadedVersion` + 跳过逻辑 | ✅ | `render_gl33.cpp:1405-1410` |
| Lua `Light.Lighting2D.GetVersion()` 可调用 | ✅ | smoke §16 全部用的是 `mod.GetVersion()` |
| 既有 smoke 28 段不破 | ✅ | runtime 输出确认 E.1 + E.1.6 所有 PASS 保留 |
| 编译通过 Release | ✅ | cmake build success |

---

## 4. 本地验证

```
Light.vcxproj -> Light.dll (build success)
[OK] lightc -p lighting2d.lua

light.exe scripts\smoke\lighting2d.lua:
  ... (前 28 段, E.1.4 + E.1.5 + E.1.6 + E.1.7 既有)
  PASS: E.2.1: GetVersion initial > 0 (v=52)
  PASS: E.2.1: Get* queries do not bump version
  PASS: E.2.1: all 6 mutators (SetEnabled/SetAmbient/Add/Update/Remove/Clear) bump version
  PASS: E.2.1: idempotent mutators (no-op Remove/Update) do not bump version
  PASS: E.2.1: failed Add (16 slot full) does not bump version
  ==== Light.Lighting2D smoke DONE ====        # 33 PASS, 全通过
```

> 初始 v = 52 的来源：§ 1-15 的既有 mutator 调用已经累积推高了 version。之后 § 16 在此基础上继续验证递增行为，而非假定 initial 值。

---

## 5. 性能收益（理论估算）

| 场景 | 上传成本（之前） | 上传成本（E.2.1 后） | 节省 |
|------|------------------|----------------------|------|
| 单帧 N 个 lit sprite，lighting state 不变 | N × (build SOA + 8×glUniform*v) ≈ N × ~20µs | **1** × (build SOA + 8×glUniform*v) + (N-1) × (只 glUseProgram) ≈ 20µs + N×2µs | N=100 时 ~1.8ms/帧 |
| 单帧每个 sprite 都 update 灯（极少） | 同前 | 同前 | 0（按需不跳过） |
| 跨帧同 state | 每帧 ~20µs | ~2µs (只 glUseProgram) | 每帧 18µs |

**注**：实测性能需接 E.2.3 LitBatchRenderer 后一起 benchmark，单独 E.2.1 收益受限于每个 lit sprite 仍是独立 draw call。

---

## 6. 不在本任务范围

- ECS cull 联动（E.2.2）
- LitBatchRenderer 合批（E.2.3）
- `Lighting2D::State::version` 对 Lua 用户 / 文档暴露（目前仅为 smoke 间接验证用）

---

## 7. 下一步

继续 E.2.2（ECS `_UploadLights2D` 加 bounds 参数，AABB-Circle cull）。
