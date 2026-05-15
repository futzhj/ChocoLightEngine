# Phase F.0.10 TAA Multi-Instance — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_10.md` / `FINAL_PhaseF_0_10.md` / `TODO_PhaseF_0_10.md`
> 基线：F.0 ~ F.0.14 (commit `91655f6`, CI run 25936869113 6/6 ✅)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| TAARenderer State 数组化 (`g_states[MAX_INSTANCES=4]`) | static State 单例 → 数组 + g_active 索引 | ✅ |
| Macro `#define g g_states[g_active]` | 146 处 `g.X` 零改动透明展开 | ✅ |
| Init/Shutdown 改造 (作用于所有 instance) | Init 写入 backend ptr 全 4 槽; Shutdown 遍历释放 | ✅ |
| 5 个 instance API (Create/Destroy/SetActive/GetActive/GetCount) | C++ + 完整防御性检查 + log | ✅ |
| Lua API +5 fn (TAA 35 → 40) | l_TAA_CreateInstance/Destroy/SetActive/GetActive/GetInstanceCount + taa_funcs[] | ✅ |
| smoke F.0.10 段 (16 PASS) | round-trip + 槽满 + 非法 id + type-error + 参数独立性 + 销毁 active + 槽位复用 | ✅ |
| demo_ssr C 键 4-state lifecycle | 0→1→2→3→cleanup 循环演示 | ✅ |
| 本地 Light.dll build success | ChocoLight\build\bin\Release\Light.dll OK | ✅ |
| Lua 语法验证 | lightc -p taa.lua && demo_ssr Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（8/8）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 多实例策略 = 数组+索引 | `static State g_states[4]` + `g_active` | ✅ |
| D2 MAX_INSTANCES = 4 | `static constexpr int MAX_INSTANCES = 4` | ✅ |
| D3 API 风格 = active 切换 | 35 fn 内 `g` macro 展开到 g_states[g_active] | ✅ |
| D4 default instance auto-init | Init 写入所有 4 槽 backend ptr | ✅ |
| D5 Instance ID 取值 [1, 3] | CreateInstance 跳过 [0], 槽满返 0 | ✅ |
| D6 RT 生命周期 = active instance | Enable/Disable/ReleaseRT 操作 g_active 槽 | ✅ |
| D7 backend 接口不变 | RT handle 已是 state 字段, backend 接口零改动 | ✅ |
| D8 split-screen demo = viewport 切换 | demo_ssr C 键 cycle 演示 (无需 backend 改动) | ✅ |

---

## 3. 验收清单

### 功能
- [x] `GetActiveInstance()` 初始 = 0 (default)
- [x] `GetInstanceCount()` 初始 = 1
- [x] `CreateInstance()` 三次返 1/2/3
- [x] 第 4 次 `CreateInstance()` (槽满) 返 0
- [x] `SetActiveInstance(id)` round-trip
- [x] `SetActiveInstance(99)` / `SetActiveInstance(destroyed)` → nil + err
- [x] `SetActiveInstance("foo")` type-error → nil + err
- [x] Instance 1 与 Instance 2 sharpness 独立 (0.3 vs 1.5)
- [x] Instance 1 与 Instance 2 clipMode/sharpenMode 独立 (variance/rcas vs rgb/cas)
- [x] default instance (0) 未受 user instance 影响 (sharpness=0.5, clipMode=ycocg 保持)
- [x] `DestroyInstance(1)` round-trip, count 减
- [x] `DestroyInstance(0)` 拒绝 (default 保护)
- [x] `DestroyInstance(99)` 非法 id → nil + err
- [x] `DestroyInstance(destroyed)` → nil + err
- [x] 销毁 active 自动切回 default (0)
- [x] 槽位可复用 (Create → Destroy → Create 返同一 id)

### CI (已验证)
- [x] runtime smoke 40/40 fn + 16 PASS 多实例段 (Windows runtime smoke 通过)
- [x] GitHub Actions 6/6 平台 success ([run 25938518533](https://github.com/futzhj/ChocoLightEngine/actions/runs/25938518533))

---

## 4. CI 状态（已验证）

| 平台 | 状态 |
|------|------|
| build-windows | ✅ success |
| build-linux | ✅ success |
| build-macos | ✅ success |
| build-android | ✅ success |
| build-ios | ✅ success |
| build-web | ✅ success |

GitHub Run ID: [`25938518533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25938518533) (修复合集: 51dbb4e)
F.0.10 原始 commit: `f2ab9cf` (CreateInstance 强检 inited, headless smoke 不兼容, fix `51dbb4e`)
Date: 2026-05-15 19:55 UTC

---

## 5. 关键技术决策回顾

### 5.1 为什么用 macro 而非 reference

C++ reference 一旦绑定不可改变 (`State& g = g_states[0]` 后无法切到 [1])。
Macro `#define g g_states[g_active]` 是最干净的零改动方案，文件作用域内有效，不污染外部命名空间。

### 5.2 为什么 Init 写入所有 4 槽 backend ptr

每个 instance 都需要调用 backend->Draw*Pass，所以 backend ptr 必须共享。但 enabled/RT/参数完全独立。Init 一次性写入避免 CreateInstance 时再去读 default。

### 5.3 为什么 default instance (id=0) 不可销毁

老代码 100% 兼容性: 老的 `TAA.Enable(w, h)` 等 35 fn 默认作用于 [0]，若可销毁 default 槽会破坏老 API 语义。

### 5.4 销毁 active 时自动切回 default

避免 active 指向已销毁槽导致后续 35 fn 操作未定义内存。`g_active = (saved == id) ? 0 : saved`。

### 5.5 MAX_INSTANCES = 4 的依据

- split-screen 双人 (2 instance) / 四人 (4 instance) 覆盖
- 静态数组开销小: 4 × sizeof(State) ≈ 4 × 200 bytes = 800 bytes，可接受
- 升级需求：仅改 `static constexpr int MAX_INSTANCES = 8;` 一行即可

### 5.6 完整 split-screen demo 未实现的折中

新建 demo_taa_split 需要 backend FBO 创建 + viewport 切换 + 双 sceneTex 渲染等大量 boilerplate (~2h+)。
本轮选择在 demo_ssr 加 C 键 4-state lifecycle cycle，**验证 instance API 行为正确性**，不验证真 split-screen 视觉差异。
真 split-screen demo 留作 F.0.10.1 (TODO 中标记)。
