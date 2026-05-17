# Phase F.2 渲染架构补齐 — TASK (任务原子化) 文档

> **阶段**：6A Workflow — 阶段 3 Atomize
> **创建日期**：2026-05-17

---

## 任务清单 (按依赖顺序)

### T1 — G1 P0 修复 (HDR TAAU 切换通知下游)

| 子任务 | 文件:行 | 内容 |
|--------|---------|------|
| T1.1 | hdr_renderer.cpp ~788 | OnTAAURenderScaleChanged 末尾追加 8 模块 OnHDRResized(renderW, renderH) |
| T1.2 | hdr_renderer.cpp ~810 | OnTAAUDisabled 切回前追加 8 模块 OnHDRResized(outputW, outputH) |

### T2 — G3 LitBatch HDR 钩子 stub

| 子任务 | 文件 | 内容 |
|--------|------|------|
| T2.1 | lit_batch_renderer.h | 加 OnHDREnabled/Disabled/Resized 三个声明 + 注释 |
| T2.2 | lit_batch_renderer.cpp | 三个空函数体实现 |

### T3 — G2 多实例补齐 (五模块)

每模块 4 步:

| 步 | .cpp 改动 | .h 改动 | Lua binding |
|----|-----------|---------|-------------|
| 1 | struct State 不动, 加 MAX_INSTANCES=4 + g_states[4] + g_active + g_count + g_slot_in_use[4] + #define g g_states[g_active] | 加 6 fn 声明 | 加 6 个 l_X_*Instance + funcs[] 末尾 6 行 |
| 2 | Init() 顶上加 g_active=0 | - | - |
| 3 | 在合适位置追加 6 个 multi-instance fn 实现 | - | - |
| 4 | (DestroyInstance 内调 ReleaseRT/DestroyResources 之类) | - | - |

### 5 模块的 ReleaseRT 函数对应:

| 模块 | RT 释放函数 |
|------|-------------|
| LensDirtRenderer | (无 RT, 直接清 State) |
| StreakRenderer | ReleaseRT |
| LensFlareRenderer | ReleaseRT |
| AutoExposureRenderer | ReleaseLuminanceRT |
| SSAORenderer | DestroyResources |

### T4 — Smoke 验证

| 子任务 | 内容 |
|--------|------|
| T4.1 | 创建 scripts/smoke/phase_f2_multi_instance.lua: 30 个 binding 存在 + 6 类行为校验 |
| T4.2 | 全量跑 taa/hdr/bloom/ssao/auto_exposure/lens_fx/lens_flare smoke |
| T4.3 | Build Release clean |
