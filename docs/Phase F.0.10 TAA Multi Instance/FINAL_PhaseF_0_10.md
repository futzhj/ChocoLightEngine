# Phase F.0.10 TAA Multi-Instance — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告

---

## 1. 交付物

| 文件 | 变更 |
|------|------|
| `ChocoLight/include/taa_renderer.h` (5 fn 声明 + doxygen) | +40 行 |
| `ChocoLight/src/taa_renderer.cpp` (state 数组化 + macro + Init/Shutdown 改造 + 5 fn 实现) | +120 行 |
| `ChocoLight/src/light_graphics.cpp` (5 Lua bindings + taa_funcs[]) | +85 行 |
| `scripts/smoke/taa.lua` (F.0.10 段 16 PASS + fn_names + banner) | +140 行 |
| `samples/demo_ssr/main.lua` (C 键 4-state lifecycle + Keys help) | +35 行 |
| `docs/Phase F.0.10 TAA Multi Instance/` (PLAN/ACCEPTANCE/FINAL/TODO) | ~550 行 |

**累计**: 代码 ~280 行 + smoke ~140 行 + demo ~35 行 + 文档 ~550 行 ≈ **1000 行**

---

## 2. 核心方案

`TAARenderer` namespace 引入多实例化: 同进程内最多 **MAX_INSTANCES = 4** 个独立 TAA 状态 (default + 3 user)，每个独立 history RT + 14 sub-phase 参数 + jitter state。老 35 fn 默认作用于 `g_states[0]` (default)，新加 5 fn 切换 active instance。

### 关键设计 (Macro 透明展开)

```cpp
namespace TAARenderer {
namespace {
    struct State { /* 现有 ~25 字段 */ };

    // Phase F.0.10 — multi-instance support
    static constexpr int MAX_INSTANCES = 4;
    static State g_states[MAX_INSTANCES];
    static int   g_active = 0;
    static int   g_count  = 1;
    static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };

    // 现有 35 fn 内部沿用 `g.X` 写法; macro 透明展开到 active instance
    #define g g_states[g_active]
}
```

这让 146 处 `g.X` 引用零改动，重构风险极低。

### Init/Shutdown 改造

- `Init(backend)`: 把 backend ptr / supported / inited 写入**所有 4 槽** (每个 instance 都需要 backend)
- `Shutdown()`: 遍历 4 槽释放 RT + 重置所有 state 字段 + 复位 instance 分配状态

### 5 个新 API

```cpp
int  CreateInstance();         // 找空闲槽返 ID [1, 3], 槽满返 0
bool DestroyInstance(int id);  // 拒绝 id=0; 销毁 active 自动切回 0
bool SetActiveInstance(int id);// 切换 active, 后续 35 fn 作用于 [id]
int  GetActiveInstance();      // default = 0
int  GetInstanceCount();       // [1, 4]
```

---

## 3. Lua API 演示 (split-screen 双人示例)

```lua
local TAA = Light.Graphics.TAA
TAA.Init(...)

-- 创建 player1 + player2 instance
local p1 = TAA.CreateInstance()    -- → 1
local p2 = TAA.CreateInstance()    -- → 2

-- 各自配置 (Player 1: 锐利 / Player 2: 柔和)
TAA.SetActiveInstance(p1); TAA.Enable(W/2, H); TAA.SetSharpness(1.2); TAA.SetSharpenMode("rcas")
TAA.SetActiveInstance(p2); TAA.Enable(W/2, H); TAA.SetSharpness(0.4); TAA.SetClipMode("variance")

-- 每帧 split-screen 渲染
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
    win:SetViewport(0,   0, W/2, H); ...draw player 1 scene...; TAA.Process(hdrFbo, hdrTex)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
    win:SetViewport(W/2, 0, W/2, H); ...draw player 2 scene...; TAA.Process(hdrFbo, hdrTex)

-- 收尾
TAA.SetActiveInstance(0)
TAA.DestroyInstance(p1)
TAA.DestroyInstance(p2)
```

---

## 4. Phase F.0 系列累计 (本轮完成后)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0 ~ F.0.14 | 主管线 + 11 优化 (sharpenMode×3 + upscaleMode×3 + 13 fn opt) | 35 |
| **F.0.10** | **multi-instance (5 fn)** | **+5 (40)** |

**累计**: 40 fn / 6 shader / 5 backend pass / 5 backend 接口扩展 / 3 demos

---

## 5. CI 状态

| 项 | 值 |
|----|----|
| GitHub Run ID | [`25938518533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25938518533) |
| Result | **6/6 platforms success** ✅ |
| F.0.10 commit | `f2ab9cf` |
| Fix commit | `51dbb4e` (CreateInstance 不要求 default inited, headless smoke 兼容) |
| Date | 2026-05-15 19:55 UTC |

**发现问题 + 修复**:
- F.0.10 原始 commit `f2ab9cf` Windows runtime smoke FAIL: `First CreateInstance() must return 1, got 0`
- 根因: `CreateInstance()` 内 `if (!g_states[0].inited) return 0` 强要求 default 槽已 Init.
  TAARenderer::Init 由 light_ui.cpp 在 window 创建时调用, headless smoke 无 window → inited=false → CreateInstance 永返 0.
- fix commit `51dbb4e`: CreateInstance 去掉 inited 强检, 仅分配槽位 + 复位 state.
  新 instance 继承 default 的 backend (可能 nullptr) / supported / inited.
  后续 Enable() 因 backend=nullptr 自然失败, 符合预期 (与 Init 单 instance 行为一致).

---

## 6. 工程反思

### 做得好

1. **Macro 方案零改动 146 处 `g.X`**: 重构风险极低，老 35 fn 行为完全保持
2. **零回归保障**: default instance (id=0) 全数继承老 single State 行为
3. **8/8 决策全自动**: 静态数组 + 索引切换是最干净的多实例方案
4. **完整防御性**: 非法 id / 未分配 id / Destroy(0) 全部 nil+err
5. **smoke 覆盖完整**: 16 PASS 涵盖 round-trip / 边界 / 参数独立性 / 销毁 active / 槽位复用
6. **API 一致性**: 5 fn 命名风格与 SSR/HDR 一致 (Create/Destroy + Set/Get + Count)

### 可改进

1. **未实现真 split-screen demo**: 折中用 demo_ssr C 键 cycle 验证 API, 真 split-screen 视觉差异需 demo_taa_split (留 F.0.10.1)
2. **没有 GPU pass 多实例验证**: smoke 仅验证 namespace 状态独立性, 不验证 backend pass 调用顺序在不同 instance 间无干扰
3. **MAX_INSTANCES=4 写死**: 升级需重编译; 动态分配会增加内存管理复杂度

---

## 7. Phase F.0.x 后续候选

- **F.0.10.1** — 真 split-screen demo_taa_split (双 viewport + 双 sceneTex + HUD 对比, 2h)
- F.0.11 — Demo 截图 / 录屏 (3h)
- F.0.15 — TAA-driven CAS strength scaling (2h)
- F.1 — DLSS-like TAAU
