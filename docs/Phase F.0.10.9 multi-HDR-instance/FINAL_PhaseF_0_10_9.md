# Phase F.0.10.9 — Multi-Instance HDRRenderer FINAL

> 6A 工作流 · ASSESS 收尾交付报告
> 状态: ✅ 全部完成 (T0–T4)

---

## 1. 实现总览

把 `HDRRenderer` 从单 instance 单例改成 **multi-instance**, 支持 split-screen / 多窗口 / PIP / portal 等场景下每 player 独立 HDR pipeline.

复刻 TAA F.0.10 已验证模板:
- `g_states[MAX_INSTANCES]` + `g_active` 索引
- `#define g g_states[g_active]` 让现有 100+ fn **零改动**透明作用于 active instance
- 新增 5 个 instance 管理 fn (Create / Destroy / SetActive / GetActive / GetCount)
- LUT 系统的全局位 (watch list / reload callback / hotReload 开关) 抽到 `g_global` 单例

---

## 2. 架构

```
hdr_renderer.cpp 顶部:
┌─────────────────────────────────────────────────────────────┐
│ struct State { /* 50+ 字段 per-instance */ };               │
│ struct GlobalState {                                        │
│     bool                    lutHotReload  = true;           │
│     LUTReloadCallback       lutReloadCb   = nullptr;        │
│     void*                   lutReloadCbUser = nullptr;      │
│     std::vector<WatchEntry> lutWatchList;                   │
│ };                                                          │
│                                                             │
│ static constexpr int MAX_INSTANCES = 4;                     │
│ static State g_states[MAX_INSTANCES];                       │
│ static int   g_active = 0;                                  │
│ static int   g_count  = 1;                                  │
│ static bool  g_slot_in_use[4] = { true, false, false, ... };│
│ static GlobalState g_global;                                │
│ #define g g_states[g_active]                                │
└─────────────────────────────────────────────────────────────┘
```

**Per-instance** (每 instance 独立):
- backend ptr / inited / supported / enabled / paused
- FBO / sceneTex / width / height
- exposure / gamma / tonemap operator
- 5 个 autoXXX flags (TAA/Bloom/SSR/MotionBlur/Tonemap)
- velocityFormat / velocityDilation / dilatedXXX RT / dilation half-res / auto-skip
- per-instance grading LUT 应用 (lutTexId / lutStrength)

**Global** (跨 instance 共享):
- LUT hot reload 开关
- LUT reload callback + userData
- LUT watch list (mtime + lutId 注册表; LUT id 全局共享)

---

## 3. 新 5 函数

### C++ API

```cpp
namespace HDRRenderer {
int  CreateInstance();              // 返 [1, 3]; 满返 0
bool DestroyInstance(int id);       // id=0 (default) 拒绝; 若是 active 自动切回 0
bool SetActiveInstance(int id);     // 切 active; 未分配 id 拒绝
int  GetActiveInstance();
int  GetInstanceCount();            // >=1, default 永远占用
}
```

### Lua API (HDR 47 → 52)

```lua
local id = HDR.CreateInstance()        -- integer; 0 = 槽满
HDR.DestroyInstance(id)                -- boolean
HDR.SetActiveInstance(id)              -- boolean
HDR.GetActiveInstance()                -- integer
HDR.GetInstanceCount()                 -- integer (1..4)
```

### 典型用法

```lua
-- split-screen 双人不同分辨率
HDR.Enable(1920, 1080)             -- default instance: P1 主屏 1080p
local pip = HDR.CreateInstance()
HDR.SetActiveInstance(pip)
HDR.Enable(640, 360)               -- PIP instance: P2 360p
HDR.SetExposure(0.8)               -- per-instance 曝光
HDR.SetGradingLUT(coldLut, 1.0)    -- per-instance 冷调
-- ...渲染 PIP 区...
HDR.SetActiveInstance(0)           -- 切回 P1
-- ...渲染主屏...
HDR.DestroyInstance(pip)
```

---

## 4. Init / Shutdown 行为

- `Init(backend)` 把 `backend` / `supported` / `inited` 写入 **所有 4 个 instance 槽**
  → 任何 `SetActiveInstance(id)` 切到的槽都自动可用 (用户只需再 `Enable(w, h)`)
- `Shutdown()` 遍历所有 instance 释放 RT, 复位 `g_active=0`, `g_count=1`, `g_slot_in_use=[t,f,f,f]`
- `CreateInstance()` 不调 backend, 仅分配槽 + 继承 default 的 backend/supported/inited; **headless 环境** (backend=nullptr) 也能分配槽, 后续 `Enable` 自然失败

---

## 5. 验证

### 5.1 Smoke `scripts/smoke/hdr.lua` §21 (本 phase 新增)

11 子项测试:

| # | 测试 | 状态 |
|---|------|------|
| 21.1 | 初始 count=1, active=0 | PASS |
| 21.2 | CreateInstance #1 → id=1, count=2 | PASS |
| 21.3 | SetActive round-trip (0 ↔ 1) | PASS |
| 21.4 | 创建到槽满 (count=4); 第 4 次返 0 | PASS |
| 21.5 | DestroyInstance(0) 拒绝 (default) | PASS |
| 21.6 | DestroyInstance(99 / -1) 拒绝 | PASS |
| 21.7 | DestroyInstance(active) 自动回 0 | PASS |
| 21.8 | DestroyInstance 二次返 false | PASS |
| 21.9 | 槽位复用 (id2 重 Create 回 2) | PASS |
| 21.10 | SetActiveInstance(已销毁 id) 拒绝 | PASS |
| 21.11 | cleanup count=1 active=0 | PASS |

**总 fn 数**: 47 → **52** (+5).

### 5.2 demo `samples/demo_taa_split2/main.lua`

新增 `F.0.10.9 Multi-Instance` round-trip probe (Create→SetActive→GetActive→Destroy→cleanup) — **PASS**.

### 5.3 零回归验证

8 个相关 smoke 全 PASS:
- `hdr` (52 fn) · `bloom` · `ssr` · `auto_exposure`
- `lens_fx` · `motion_blur` · `taa` · `lighting2d`

老 100+ HDR fn 透明走 `g_states[0]` 单例, 行为完全等价 F.0.10.8.6.

---

## 6. 文件变更

| 文件 | 变更 |
|------|------|
| `ChocoLight/include/hdr_renderer.h` | +5 fn 声明 + multi-instance API doc |
| `ChocoLight/src/hdr_renderer.cpp` | State 拆 (移 3 lut global 字段) + GlobalState + g_states[4] + macro + 5 fn 实现 + Init/Shutdown 适配 |
| `ChocoLight/src/light_graphics.cpp` | +5 Lua wrap + hdr_funcs 注册 |
| `scripts/smoke/hdr.lua` | fn_names +5 + §21 11 子项测试 |
| `samples/demo_taa_split2/main.lua` | +F.0.10.9 round-trip probe |
| `docs/Phase F.0.10.9 multi-HDR-instance/PLAN_PhaseF_0_10_9.md` | 6A PLAN (本 phase) |
| `docs/Phase F.0.10.9 multi-HDR-instance/FINAL_PhaseF_0_10_9.md` | 本文 |
| `docs/Phase F.0.10.9 multi-HDR-instance/TODO_PhaseF_0_10_9.md` | 后续接力清单 |

---

## 7. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN §1 任务对齐 + 边界 + 10 项决策矩阵 + 风险 |
| **Architect** | PLAN §2 State / GlobalState 设计 + macro 模式 + 5 fn 接口 |
| **Atomize** | PLAN §3 拆 T0–T4 + F.0.10.9.{1,2} 接力 |
| **Approve** | 用户隐式确认 (复刻 TAA F.0.10 已验证模板, 0 决策歧义) |
| **Automate** | 本 phase 实现 (T0–T4 一气呵成) |
| **Assess** | 本 FINAL + smoke 52 fn PASS + 8 smoke 零回归 + demo PASS |

---

## 8. Lua API 数

```
F.0.10.8.6 累计 73
F.0.10.9    +5 (CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount)
═══════════════
F.0.10.9    78 fn
```

---

## 9. 后续接力 (F.0.10.9.{1,2})

见 `TODO_PhaseF_0_10_9.md`. 简表:
- **F.0.10.9.1**: Bloom/SSR/MB/SSAO/TAA 与多 HDR instance 联动验证 (组合 smoke)
- **F.0.10.9.2**: demo live 演示主屏 1920×1080 + PIP 640×360 真不同分辨率
