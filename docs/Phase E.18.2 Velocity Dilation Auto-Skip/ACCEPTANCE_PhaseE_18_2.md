# Phase E.18.2 Velocity Dilation Auto-Skip — ACCEPTANCE

> 6A 工作流 · 阶段 6 · Assess
> 基线：Phase E.18.1 commit `cdef7b2`

---

## 1. 实施完成度

| 任务 | 文件 | 行数变更 | 状态 |
|------|------|---------|------|
| T1 hdr_renderer.h API 声明 | `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h` | +9 / 0 | ✅ |
| T1 hdr_renderer.cpp State + EndScene + API | `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp` | +50 / -8 | ✅ |
| T2 Lua 绑定 | `@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp` | +30 / 0 | ✅ |
| T3 smoke (hdr.lua) | `@e:/jinyiNew/Light/scripts/smoke/hdr.lua` | +51 / -1 | ✅ |
| T3 demo (main.lua) | `@e:/jinyiNew/Light/samples/demo_ssr/main.lua` | +14 / -3 | ✅ |
| T3 docs (Light_Graphics.md) | `@e:/jinyiNew/Light/docs/api/Light_Graphics.md` | +89 / -1 | ✅ |
| T4 6A 三件套 | `PLAN_PhaseE_18_2.md` (含 ALIGN+DESIGN+TASK) + `ACCEPTANCE_PhaseE_18_2.md` + `FINAL_PhaseE_18_2.md` + `TODO_PhaseE_18_2.md` | 新增 | ✅ |
| T5 commit+push+CI | git + GitHub Actions | — | ✅ |

---

## 2. 决策对齐核对（PLAN 10 决策矩阵 + 性能修订）

| # | 决策点 | 已落实位置 | 状态 |
|---|--------|----------|------|
| 1 | 检测时机：HDR EndScene 内 DrawVelocityDilate 之前 | `hdr_renderer.cpp::EndScene` dilation 块 | ✅ |
| 2 | consumer 判定：SSR Temporal + MotionBlur::IsEnabled | `SSRRenderer::IsEnabled() && SSRRenderer::GetTemporalEnabled()` / `MotionBlurRenderer::IsEnabled()` | ✅ |
| 3 | **修订规则**：`autoSkip && SSR Temporal && !MB` 才跳过（非简单 count<2）| `bool ssrOnly = ssrTemporal && !mbEnabled; shouldRun = !ssrOnly` | ✅ |
| 4 | autoSkip 默认 `false`（保 Phase E.18.1 行为）| `dilationAutoSkip = false` | ✅ |
| 5 | API 命名 `SetVelocityDilationAutoSkip` / `Get...` | `hdr_renderer.h` + `light_graphics.cpp` | ✅ |
| 6 | autoSkip=false → 维持 Phase E.18.1 行为 | `if (g.dilationAutoSkip) {...}` 整体跳过 | ✅ |
| 7 | autoSkip=true && ssrOnly → 本帧 dilationActive=false | `if (shouldRun) { ... DrawVelocityDilate ... }` | ✅ |
| 8 | RT 不释放：仅本帧 skip 执行 | SetVelocityDilationAutoSkip 内不调 ReleaseDilationRT | ✅ |
| 9 | once-log：状态变化时打一次 | `lastDilationActiveLog` 字段追踪 | ✅ |
| 10 | 零回归保障：默认 false 完全等价 Phase E.18.1 | `g.dilationAutoSkip=false` 时整个 if 块 skip | ✅ |

---

## 3. 验收 checklist

### T1 编译通过
- [x] `hdr_renderer.h` 新增 2 个 public API 声明（`SetVelocityDilationAutoSkip` / `Get...`）
- [x] State 增 `dilationAutoSkip = false` + `lastDilationActiveLog = true` 字段
- [x] EndScene 内 dilation 块加 SSR-only 检测 + once-log + shouldRun 闸口

### T2 Lua 绑定
- [x] `l_HDR_SetVelocityDilationAutoSkip` 严格 boolean 检查 → nil + err
- [x] `l_HDR_GetVelocityDilationAutoSkip` 返 boolean
- [x] `hdr_funcs[]` 注册 2 项（Phase E.18.2 标记）

### T3 docs / smoke / demo
- [x] `hdr.lua` smoke 加 §10 round-trip + type-error + no-op 测试（4 个 pass）
- [x] `hdr.lua` 函数表加 2 项；末尾计数 20 functions / "Phase E.3 + E.14 + E.18.1 + E.18.2"
- [x] `demo_ssr/main.lua` 加 `\` 快捷键 + HUD 显示 autoSkip=ON/OFF + Keys help 行更新
- [x] `Light_Graphics.md` 新增 `HDR.SetVelocityDilationAutoSkip` / `Get...` 段（含跳过判定规则 + 收益分析表 + 适用场景表）
- [x] API 速查表加一行 E.18.2

### T4 6A 文档
- [x] PLAN（含 Align+Architect+Atomize 三阶段）已完成
- [x] ACCEPTANCE / FINAL / TODO 本次完成

### T5 CI
- [x] GitHub Actions 6/6 平台 success
- [x] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 跳过判定规则的修订

**初版方案**（PLAN §1.3 #3）：consumer count ≥ 2 才启用 dilation pass
**修订方案**（PLAN §5）：`autoSkip && SSR Temporal && !MB` 才跳过

**修订原因**：fetch/pixel 经济性分析显示——

| 场景 | dilation pass | inline 9-tap | dilation 是否亏 |
|------|---------------|--------------|----------------|
| 仅 SSR Temporal | 10 (9 dilate + 1 sample) | 9 | **亏 1 fetch/px** ★ |
| 仅 Motion Blur(N=8) | 17 (9 + 8) | 9×8 = 72 | 节省 55 fetch/px |
| SSR + MB(N=8) | 18 | 9 + 9×8 = 81 | 节省 63 fetch/px |

简单 count<2 规则会误跳过 "仅 MB" 场景 → 反而损失 55 fetch/px。修订规则只在真正"dilation pass 亏 fetch"时跳过。

### 4.2 once-log 策略

- 每帧 spam log 会污染 console 输出
- `lastDilationActiveLog` 追踪上一帧状态
- 仅在 active↔skip 转变时打一次 INFO log
- SetVelocityDilationAutoSkip 切换时重置 `lastDilationActiveLog = true`，让下次切换能出 log

### 4.3 不重建 RT 的设计

- 与 Phase E.18.1 halfRes 切换不同：autoSkip 决策每帧重新评估
- RT 生命周期由 `SetVelocityDilation` 控制（与 dilation 整体启用绑定）
- 避免反复切换 autoSkip 时的 RT 重建开销
- consumer 端 fallback 路径（dilatedTex=0 → raw + inline 9-tap）原本就在，autoSkip 只是利用现有路径

### 4.4 与 Phase E.18 / E.18.1 的正交关系

```
SetVelocityDilation(true/false)        ← 控制 dilation pass 整体启用 + RT 创建
  ↓ if true
SetVelocityDilationHalfRes(true/false) ← 控制 RT 尺寸 (full vs half)
  ↓
[每帧 EndScene]
SetVelocityDilationAutoSkip(true/false)← 控制本帧是否运行 DrawVelocityDilate
```

三个开关层层正交，互不干扰。任意组合都有清晰语义。

---

## 5. 性能预算（理论）

### 跳过收益（仅适用 SSR Temporal 单消费者场景）

| 配置 | dilation pass cost | inline 9-tap cost | autoSkip 节省 |
|------|--------------------|--------------------|---------------|
| 1080p | 9 dilate + 1 sample = 10 fetch/px ≈ 0.10 ms | 9 fetch/px ≈ 0.09 ms | **~0.01 ms/frame** |
| 4K | 同上 | 同上 | **~0.04 ms/frame** |

### autoSkip 不跳过的场景（无收益也无损失）

| 场景 | autoSkip=true | autoSkip=false | 一致性 |
|------|---------------|----------------|--------|
| 仅 MB | 不跳过（17 fetch） | 不跳过（17 fetch） | ✓ |
| SSR + MB | 不跳过（18 fetch） | 不跳过（18 fetch） | ✓ |
| 都不启 | 不跳过（dilation RT idle） | 不跳过（dilation RT idle） | ✓ |

---

## 6. 已知限制

1. **收益较小**：仅 SSR Temporal 单消费者场景节省 ~1 fetch/px，对总帧时影响微小（< 0.05ms @ 4K）
2. **真机未测**：基于理论 fetch 数分析，未做 GPU profile 实测
3. **不影响 RT VRAM**：dilation RT 仍创建，仅本帧不写入；用户想真正释放 RT 应用 `SetVelocityDilation(false)`
4. **不参与 Motion Blur halfRes 路径**：autoSkip 决策与 motion blur halfRes 完全独立

---

## 7. CI 状态

| 平台 | 状态 | 状态详情 |
|------|------|------|
| build-windows | ✅ success | runtime smoke 25 PASS (hdr.lua 20 fn) + Phase E.16/17/18/18.1 零回归 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [`25902219897`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25902219897)
Commit hash: `b726026`
Total duration: **6 min**
Date: 2026-05-15 05:35 UTC → 05:40 UTC
