# Phase F.0.10.9 — 真多 HDR target / RT pool PLAN

> 6A · ALIGN + DESIGN + TASK 合并
> 工作量 ~8-10h, 拆 5 sub-phase 接力 (T0~T4)

---

## 1. ALIGN — 任务对齐

### 1.1 目标

HDRRenderer 当前单 instance (匿名 ns `g` 单例), region API (F.0.10.2/3/6) 用 scissor 在同 RT 上分区, 已满足 split-screen 90% 场景. 但**无法**:
- 不同分辨率 (主屏 1080p + PIP 480p)
- portal / mirror 独立 RT
- 真物理多 player 完全独立 HDR pipeline (不同 velocityFormat / dilation / LUT)

F.0.10.9 把单 g 改成 **multi-instance**, 复刻 TAA F.0.10 已验证的 `g_states[MAX_INSTANCES] + macro` 模板.

### 1.2 边界

**In**:
- HDR 5 新 fn: `CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount`
- HDR state 数组化 + macro `#define g g_states[g_active]` 让现有 100+ fn 零改动
- MAX_INSTANCES = 4 (default + 3 user, split-screen 双人/四人足够)
- 每 instance 独立: backend ptr / FBO / sceneTex / dilation RT / tonemap params / autoXXX flags / LUT state
- LUT watch list / reload callback / hotReload **全局共享** (LUT id 也是全局)
- Bloom / SSR / MB / SSAO renderer 通过 `HDRRenderer::GetHDRFBO()` / `GetSceneTex()` 自动随 active instance 切换 — **无需改 renderer**

**Out**:
- backend HDRInstance 抽象 — HDR FBO 已是独立 GL FBO + texture, 每 instance own 一对就行, 不需 backend pool
- Bloom / SSR / MB pyramid 自动跟随 — 这些 renderer 是独立单 instance, 同时只服务 1 个 HDR (active), 后续 phase 如需多 Bloom 再加
- 跨 instance 共享 RT 池 — KISS, 每 instance 独立 RT, RT 复用留给用户层

### 1.3 决策矩阵 (10 项, 全自动)

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | 多 instance 实现 | `g_states[MAX_INSTANCES]` + macro | TAA 已验证模板, 200+ fn 零改动 |
| 2 | MAX_INSTANCES | 4 | 与 TAA 一致, split-screen 4 人足够 |
| 3 | LUT watch list | **全局共享** (不 per-instance) | LUT id 全局, watch 也全局 |
| 4 | reload callback | **全局共享** | 单一回调, Lua 自己 multiplex |
| 5 | hotReload 开关 | **全局共享** | 系统级开关 |
| 6 | exposure/gamma/tonemap | **per-instance** | 每 player 独立环境 |
| 7 | autoXXX 5 flags | **per-instance** | 每 instance 独立 pipeline |
| 8 | velocity dilation | **per-instance** | 每 instance 独立 RT |
| 9 | LUT 应用 | **per-instance** | 每 player 独立调色 |
| 10 | g_states[0] | default singleton | 永占用, 零回归现有 API |

### 1.4 风险

- **中**: HDR state ~50 字段 vs TAA ~30, 内存 4×200B = ~800B 可接受
- **中**: LUT watch list 拆 global, 需 hdr_renderer.cpp 顶部加 file-static `s_lutWatchList` (从 State 拆出)
- **低**: Bloom/SSR/MB/SSAO 已透传 GetHDRFBO/GetSceneTex, 切 active 自动跟随
- **低**: TAA renderer 自己也是 multi-instance, HDR active 切换 → TAA active 不联动 (用户负责对齐, smoke 测之)

---

## 2. DESIGN — 接口设计

### 2.1 HDRRenderer state 拆分

```cpp
namespace {

// === Per-instance state (g_states[MAX_INSTANCES]) ===
struct State {
    RenderBackend* backend;
    bool inited, supported, enabled, paused;
    uint32_t fbo, sceneTex;
    int width, height;
    float exposure, gamma;
    int tonemap;
    bool velocityDilation;
    VelocityFormat velocityFormat;
    uint32_t dilatedVelocityFbo, dilatedVelocityTex;
    uint32_t dilatedCameraVelocityFbo, dilatedCameraVelocityTex;
    bool dilationHalfRes, dilationAutoSkip;
    bool lastDilationActiveLog;
    bool autoTAA, autoBloom, autoSSR, autoMotionBlur, autoTonemap;
    uint32_t lutTexId;
    float lutStrength;
};

static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };
#define g g_states[g_active]

// === Global state (全 instance 共享) ===
struct GlobalState {
    bool              lutHotReload    = true;
    LUTReloadCallback lutReloadCb     = nullptr;
    void*             lutReloadCbUser = nullptr;
    std::vector<LUTWatchEntry> lutWatchList;   // 从 State 拆出来
};
static GlobalState g_global;

}
```

### 2.2 新 5 fn (mirror TAA F.0.10)

```cpp
namespace HDRRenderer {
int  CreateInstance();      // 分配空闲槽 [1, MAX_INSTANCES-1] → id; 满返 0
bool DestroyInstance(int id);     // 释放 RT + 标空闲; id=0 拒绝
bool SetActiveInstance(int id);   // 切 active
int  GetActiveInstance();
int  GetInstanceCount();
}
```

### 2.3 Lua API (5 新 fn, HDR 47 → 52)

```lua
local id = HDR.CreateInstance()      -- → integer (>0 成功 / 0 满)
HDR.DestroyInstance(id)              -- → boolean
HDR.SetActiveInstance(id)            -- → boolean (id 0 = default)
HDR.GetActiveInstance()              -- → integer
HDR.GetInstanceCount()               -- → integer (1..4)
```

### 2.4 数据流 (主循环 hook)

```
Lua: HDR.SetActiveInstance(1)
   └ g_active = 1
   └ macro g 透明展开 → g_states[1]

Lua: HDR.Enable(w, h)
   └ g.fbo / g.sceneTex 写入 [1] 槽
   └ 不影响 [0] default 的 RT

Lua: HDR.BeginScene() / DrawXXX / HDR.EndScene()
   └ 用 g.fbo / g.sceneTex → instance [1] RT

Lua: HDR.SetActiveInstance(0)
   └ g_active = 0
   └ g 切回 [0] default
```

---

## 3. TASK — 拆 5 sub-phase

| Phase | 内容 | 工作量 | 状态 |
|-------|------|------|------|
| **T0** | state 数组化 + macro 切换 + 编译过 + 现有 smoke 零回归 | ~1.5h | 本对话 |
| **T1** | 5 新 fn (Create/Destroy/SetActive/...) + Init/Shutdown 适配 | ~1h | 本对话 |
| **T2** | LUT watch list / callback / hotReload 拆 g_global | ~1h | 本对话 |
| **T3** | Lua API 5 fn + smoke §21 + demo headless probe | ~1.5h | 本对话 |
| **T4** | FINAL + TODO + commit + CI 6/6 | ~0.5h | 本对话 |
| **F.0.10.9.1** | Bloom/SSR/MB/SSAO/TAA 联动验证 (实际 region + multi-HDR 组合 smoke) | ~1.5h | 下次接力 |
| **F.0.10.9.2** | demo live 多 instance 演示 (主屏 + PIP / split-screen 真多 RT) | ~2h | 下次接力 |
| **合计** | | **~9h** | T0~T4 本对话, F.0.10.9.{1,2} 下次 |

### 3.1 本对话目标 (T0~T4)

完成 HDRRenderer 数组化 + 5 fn + Lua API + smoke + demo + commit + CI 6/6. ~5.5h.

**关键边界**:
- 本对话**不动** Bloom/SSR/MB/SSAO/TAA renderer 代码 — 它们自动通过 GetHDRFBO/GetSceneTex 跟随 active 切换
- 本对话**不写** demo live 演示 — headless probe 验证 API 即可

### 3.2 风险缓解

- 每写一段 + 编译验证, 不积累风险
- 每完成 T0/T1/T2/T3 各跑一次 8 smoke 确认零回归
- LUT watch list 拆 global 是最大变更, 单独 T2 处理 + 测试

---

## 4. 验收标准 (本对话 T0-T4)

| 类型 | 标准 |
|------|-----|
| 编译 | Release 通过 + CI 6/6 |
| HDR smoke | 47 → **52 fn**, §21 N PASS |
| 8 smoke | 零回归 |
| demo headless | 24 → **26+ PASS** |
| Lua API | 73 → **78** (+5) |

---

## 5. 后续接力 (F.0.10.9.{1,2})

- **F.0.10.9.1**: split-screen 真多 instance 组合验证 (Bloom + SSR + TAA + 不同分辨率 HDR instance 同帧 swap)
- **F.0.10.9.2**: demo live 演示 (主屏 1080p + PIP 480p 真不同分辨率 HDR pipeline)
