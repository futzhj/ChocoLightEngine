# Phase F.2 渲染架构补齐 — DESIGN (设计) 文档

> **阶段**：6A Workflow — 阶段 2 Architect
> **创建日期**：2026-05-17

---

## 1. 整体架构图

```mermaid
flowchart TB
    subgraph Lua["Lua Layer (新增 30 个绑定)"]
        L1["Light.Graphics.SSAO.{Create/Destroy/SetActive/GetActive/Count/Clone}Instance"]
        L2["Light.Graphics.LensFlare.* (同 6 fn)"]
        L3["Light.Graphics.LensDirt.* (同 6 fn)"]
        L4["Light.Graphics.Streak.* (同 6 fn)"]
        L5["Light.Graphics.AutoExposure.* (同 6 fn)"]
    end

    subgraph CPP["C++ Renderer (5 × g_states[4] + g_active)"]
        C1["SSAORenderer state[4] + 6 fn"]
        C2["LensFlareRenderer state[4] + 6 fn"]
        C3["LensDirtRenderer state[4] + 6 fn"]
        C4["StreakRenderer state[4] + 6 fn"]
        C5["AutoExposureRenderer state[4] + 6 fn"]
    end

    subgraph G1Fix["G1 P0 修复"]
        G1A["HDRRenderer::OnTAAURenderScaleChanged"]
        G1B["+ Bloom/AE/LensDirt/Streak/LensFlare/<br/>SSAO/SSR/MotionBlur::OnHDRResized(renderW, renderH)"]
        G1C["HDRRenderer::OnTAAUDisabled"]
        G1D["+ 上 8 模块::OnHDRResized(outputW, outputH)"]
    end

    subgraph G3Fix["G3 接口对齐"]
        G3A["LitBatchRenderer::OnHDREnabled/Disabled/Resized (stub)"]
    end

    Lua --> CPP
    G1A --> G1B
    G1C --> G1D

    classDef new fill:#d4f4dd,stroke:#2e8b57,stroke-width:2px
    classDef p0 fill:#ffd6d6,stroke:#c92020,stroke-width:2px
    class L1,L2,L3,L4,L5,C1,C2,C3,C4,C5,G3A new
    class G1A,G1B,G1C,G1D p0
```

## 2. G1 (P0) — 数据流修复

```mermaid
sequenceDiagram
    participant User as Lua 用户
    participant TAA as TAARenderer
    participant HDR as HDRRenderer
    participant DOWN as 8 个下游模块

    User->>TAA: SetTAAUEnabled(true)
    TAA->>HDR: OnTAAURenderScaleChanged(renderW, renderH, outputW, outputH)
    HDR->>HDR: ReleaseRT() + CreateRT(renderW, renderH)
    HDR->>HDR: CreateOutputSceneTex(outputW, outputH)
    Note over HDR,DOWN: 修复前: 8 模块 RT 仍是 outputW × outputH<br/>修复后: 8 模块 RT 同步到 renderW × renderH
    HDR->>DOWN: BloomRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: AutoExposureRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: LensDirtRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: StreakRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: LensFlareRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: SSAORenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: SSRRenderer::OnHDRResized(renderW, renderH)
    HDR->>DOWN: MotionBlurRenderer::OnHDRResized(renderW, renderH)
    HDR-->>TAA: true (TAAU 启用)
```

切回 F.0 路径 (OnTAAUDisabled) 镜像调用，参数 `outputW, outputH`。

## 3. G2 (P1) — 多实例模板

完全复用 BloomRenderer F.0.10.9.x.2 模板:

```cpp
// 1. 在 .cpp 内, struct State 之后:
static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };

#define g g_states[g_active]   // 老 fn 透明访问 active instance

// 2. Init() 顶上加 g_active = 0 (显式回到 default)

// 3. 新增 6 fn:
int CreateInstance();    // 找空闲槽 [1, 3], 槽满返 0
bool DestroyInstance(int id);
bool SetActiveInstance(int id);
int GetActiveInstance();
int GetInstanceCount();
int CloneInstance(int srcId);   // 复制参数, 不复制 RT
```

## 4. G3 (P1) — LitBatch stub

```cpp
// lit_batch_renderer.h 加 3 个声明
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);

// lit_batch_renderer.cpp 加 3 个 stub 实现
void OnHDREnabled(int /*w*/, int /*h*/) {}
void OnHDRDisabled() {}
void OnHDRResized(int /*w*/, int /*h*/) {}
```

## 5. 各模块改动总量估算

| 模块 | .h 行 | .cpp 行 | Lua binding 行 |
|------|-------|---------|----------------|
| HDRRenderer (G1) | 0 | +20 | 0 |
| LitBatch (G3) | +12 | +5 | 0 |
| SSAO (G2) | +10 | +85 | +30 |
| LensFlare (G2) | +10 | +75 | +30 |
| LensDirt (G2) | +12 | +75 | +30 |
| Streak (G2) | +9 | +75 | +30 |
| AutoExposure (G2) | +10 | +85 | +35 |
| **合计** | **+63** | **+420** | **+155** |

