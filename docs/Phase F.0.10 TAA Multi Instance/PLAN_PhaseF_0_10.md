# Phase F.0.10 TAA Multi-Instance Split-Screen — PLAN

> 6A 工作流 · 阶段 1+2+3 合并
> 关联：`ACCEPTANCE_PhaseF_0_10.md` / `FINAL_PhaseF_0_10.md` / `TODO_PhaseF_0_10.md`
> 基线：F.0 ~ F.0.14 (commit `91655f6`, CI run 25936869113 6/6 ✅)

---

## 1. Align (对齐)

### 1.1 业务目标

`TAARenderer` 当前为 namespace + 静态 `static State g` 单例。引入**多实例化**支持：同一进程内多个独立 TAA 状态（每个独立 history RT + 全部 14 个 sub-phase 参数），用于 **split-screen / 多视口 / 编辑器双窗 / VR 双眼独立 TAA** 场景。

### 1.2 现状（基线）

- TAARenderer 35 个 namespace fn 共享 `static State g`
- 全局 backend 指针 / global jitter state / global history RT pair
- 多视口场景下必须先 Disable+Resize+Enable 切换，无法并行

### 1.3 用户价值

- **真正的多实例**: 同帧渲染多个独立 TAA 输出（split-screen）
- **零回归**: 默认 `g_states[0]` 等价老 namespace state，老代码 100% 兼容
- **API 稳定**: 35 个老 fn 不动，新加 5 个 instance fn (总 40)

---

## 2. Architect (架构)

### 2.1 决策矩阵 (8/8 全自动决策)

| # | 决策 | 选择 | 依据 |
|---|------|------|------|
| D1 | 多实例策略 | `states[MAX_INSTANCES]` 数组 + `g_active` 索引切换 | 静态分配，无动态内存；切换 O(1) |
| D2 | MAX_INSTANCES | `4` (default + 3 user) | split-screen 双人/四人足够；超出可后续扩展 |
| D3 | API 风格 | active instance 切换模式 (不带 handle 参数) | 现有 35 fn 内部仅替换 `g` → `g_states[g_active]`；改动量最小 |
| D4 | default instance | `g_states[0]` 自动初始化 | 老代码访问 namespace fn 自动作用于 [0]，零回归 |
| D5 | Instance ID 取值 | `[1, MAX_INSTANCES-1]` (= 1, 2, 3) | `0` 保留给 default；`CreateInstance` 失败返 0 |
| D6 | RT 生命周期 | `Enable(w,h)` 仍按 active instance 工作 | 与现有 35 fn 模式一致 |
| D7 | backend 接口 | 不变 | RT handle 已是 instance-local，backend pass 调用方传入 |
| D8 | split-screen demo | 同 framebuffer + viewport 切换 | 不需新 backend 接口；与 demo_ssr 同 hdrFbo |

### 2.2 State 字段拓展

```cpp
namespace TAARenderer {

namespace {
    struct State {  // 现有 ~25 字段不变
        RenderBackend* backend = nullptr;
        bool enabled = false;
        // history RT, jitter state, 14 sub-phase 参数 ...
    };

    static constexpr int MAX_INSTANCES = 4;
    static State g_states[MAX_INSTANCES];   // 取代原 static State g;
    static int   g_active = 0;              // 当前 active index
    static int   g_count  = 1;              // 已分配数 (default 总占 1 槽)

    // helper
    inline State& g() { return g_states[g_active]; }
}

// 现有 35 fn 内部:
//   旧: g.field
//   新: g_states[g_active].field   或   g().field
```

### 2.3 接口契约

```cpp
namespace TAARenderer {
    // ===== 现有 35 fn (零变更) =====
    bool Init(RenderBackend*);
    void Shutdown();
    bool IsInited();
    bool Enable(int, int);
    void Disable();
    // ... 35 个 fn 全部保持签名 ...

    // ===== Phase F.0.10 新加 5 fn =====

    /// 创建新 TAA instance, 返 ID [1, MAX_INSTANCES-1]
    /// @return 0 = 失败 (MAX_INSTANCES 已满 / backend 不支持)
    /// 新 instance 默认 disabled, 需 SetActiveInstance() + Enable() 启用
    int CreateInstance();

    /// 销毁 instance, 同时 Disable() 释放 RT
    /// 不能销毁 id=0 (default); active 是该 id 时自动切回 0
    /// @return false 当 id 非法 / id=0 / 槽未分配
    bool DestroyInstance(int id);

    /// 切换 active instance
    /// @return false 当 id 非法 / 槽未分配
    bool SetActiveInstance(int id);

    /// 获取当前 active instance ID (default = 0)
    int GetActiveInstance();

    /// 获取已分配 instance 数 (含 default, 范围 [1, MAX_INSTANCES])
    int GetInstanceCount();
}
```

### 2.4 Lua API (新加 5 fn)

```lua
local id1 = TAA.CreateInstance()        -- → number 1..3, 或 0 (失败)
local id2 = TAA.CreateInstance()        -- → 2

-- 各 instance 独立 enable
TAA.SetActiveInstance(id1); TAA.Enable(640, 480); TAA.SetSharpness(0.5)
TAA.SetActiveInstance(id2); TAA.Enable(640, 480); TAA.SetSharpness(1.5)

-- 每帧 split-screen
TAA.SetActiveInstance(id1); TAA.ApplyJitter(); ...draw left...; TAA.Process(hdrFbo, hdrTex)
TAA.SetActiveInstance(id2); TAA.ApplyJitter(); ...draw right...; TAA.Process(hdrFbo, hdrTex)

-- 收尾
TAA.SetActiveInstance(0)                -- 复位 default
TAA.DestroyInstance(id1)                -- 自动 Disable + 释放 RT
TAA.DestroyInstance(id2)
```

### 2.5 split-screen demo (demo_taa_split)

```
左半屏 (viewport 0..W/2):           右半屏 (viewport W/2..W):
  instance 1                          instance 2
    sharpness = 0.0                     sharpness = 1.5
    halfResHistory = true               halfResHistory = false
    upscaleMode = "lanczos"             sharpenMode = "rcas"
    (走 F.0.14 高画质上采样)            (走 F.0.12 强锐化)
```

实现：
- 每帧 2 个 TAA pass（同 hdrFbo, viewport 切换）
- 左 / 右 instance 各自独立 history（无 ghost 串扰）
- HUD 显示两侧参数对比
- 按 R 复位、ESC 退出

---

## 3. Atomize (原子化)

| ID | 内容 | 估计输出 |
|----|------|----------|
| T0 | PLAN | 本文档 (~200 行) |
| T1 | TAARenderer State 重构: `g` → `g_states[]` + helper `g()` | ~30 处替换 + 4 行 state 字段 |
| T2 | 5 fn instance API + 内部 fn 转发 default state | ~60 行 |
| T3 | Lua API +5 fn + smoke 多实例段 + taa_funcs 表 | ~80 行 + smoke ~60 行 |
| T4 | 新建 demo_taa_split (~250 行) + Light_Graphics.md 速查表 +5 行 + 完整 instance 段 | ~280 行 |
| T5 | 6A ACCEPTANCE/FINAL/TODO | ~500 行 |
| T6 | commit + push + CI 验证 6/6 | git diff |

---

## 4. 影响范围 / 兼容性

| 维度 | 影响 |
|------|------|
| 默认行为 | `g_states[0]` 完全等价老 `static State g`，零回归 |
| 老 namespace API | 35 fn 签名零变化 |
| backend 接口 | 不变 |
| Lua 老调用 | `TAA.Enable(w, h)` 默认作用于 `g_states[0]` |
| smoke 老 5 段 | 全部通过（仍跑 default instance） |

---

## 5. 风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| `g_active` 切换时未调 `Enable` 导致 Process 崩 | 🟡 中 | Process 内现有 enabled 守卫已防御，每个 state 独立 |
| MAX_INSTANCES=4 不够用 | 🟢 低 | 升级 const 即可；split-screen 多人足够 |
| 多 instance 同时 jitter 互扰 | 🟢 低 | 每 instance 独立 frameCounter / curJitter (state 字段内) |
| Lua 用户误传 id=0 给 DestroyInstance | 🟢 低 | id=0 是 default，DestroyInstance 拒绝并返 nil+err |
| demo_taa_split 实现复杂 | 🟡 中 | viewport 切换 + scissor 即可，不改 backend |

---

## 6. 验收标准

### 功能
- [ ] `CreateInstance` 返 [1, 3] 或 0
- [ ] `MAX_INSTANCES` 满载时 CreateInstance 返 0
- [ ] `SetActiveInstance` round-trip
- [ ] `SetActiveInstance` 非法 id 返 nil+err
- [ ] `DestroyInstance(0)` 拒绝
- [ ] `DestroyInstance(active)` 自动切回 default
- [ ] 两个 instance 独立 sharpness/clipMode 互不影响
- [ ] 销毁后 GetInstanceCount() 减少
- [ ] demo_taa_split 双视口可视化差异

### CI
- [ ] runtime smoke 40/40 fn + 多实例段
- [ ] GitHub Actions 6/6 平台 success

---

## 7. 估算

| 项 | 估算 |
|----|------|
| T1 重构 | ~1.5h |
| T2 instance API | ~0.5h |
| T3 Lua + smoke | ~1h |
| T4 demo_taa_split | ~2h |
| T5 6A docs | ~1h |
| T6 commit + CI | ~0.5h + CI 等待 |
| **总计** | **~6.5h + CI** |

---

## 8. Phase F.0 系列累计 (本次完成后)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0 ~ F.0.14 | 主管线 + 11 优化 (sharpenMode×3 + upscaleMode×3 + 13 fn opt) | 35 |
| **F.0.10** | **multi-instance + split-screen** | **+5 (40)** |

**累计**: 40 fn / 6 shader / 5 backend pass / 5 backend 接口扩展 / **4 demos** (含 demo_taa_split)
