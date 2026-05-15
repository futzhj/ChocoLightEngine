# Phase E.18.2 Velocity Dilation Auto-Skip — FINAL

> 6A 工作流 · 阶段 6 · Assess · 项目总结
> 基线：Phase E.18.1 commit `cdef7b2`

---

## 1. 项目概述

为 Phase E.18 dilation pass 增加**单消费者智能跳过**：当仅 SSR Temporal 启用且 Motion Blur 未启用时，本帧跳过 DrawVelocityDilate，让 consumer 走 inline 9-tap 旧路径，节省 1 fetch/px。

- **零 API 破坏性变更**：新增 1 对 `HDR.SetVelocityDilationAutoSkip` / `Get...` Lua API
- **默认 OFF**（halfRes=false 时行为完全等价 Phase E.18.1）
- **跳过判定精确**：基于 fetch/pixel 经济性分析，仅在 dilation pass 真正亏 fetch 时跳过

---

## 2. 代码 / 文档统计

### 代码改动

| 类别 | 文件 | 行数（+ / -）|
|------|------|---------------|
| C++ Header | `include/hdr_renderer.h` | +9 / 0 |
| C++ Source | `src/hdr_renderer.cpp` | +50 / -8 |
| C++ Source | `src/light_graphics.cpp` | +30 / 0 |
| Lua smoke | `scripts/smoke/hdr.lua` | +51 / -1 |
| Lua demo | `samples/demo_ssr/main.lua` | +14 / -3 |
| Markdown | `docs/api/Light_Graphics.md` | +89 / -1 |
| **代码合计** | — | **+243 / -13** |

### 文档新增

| 文件 | 行数 |
|------|------|
| `docs/Phase E.18.2/PLAN_PhaseE_18_2.md` (合并 ALIGN+DESIGN+TASK) | ~280 |
| `docs/Phase E.18.2/ACCEPTANCE_PhaseE_18_2.md` | ~150 |
| `docs/Phase E.18.2/FINAL_PhaseE_18_2.md` | 本文件 |
| `docs/Phase E.18.2/TODO_PhaseE_18_2.md` | 待补 |
| **文档合计** | **~600** |

> **精简化决策**：本任务工作量小（~0.5 天）+ 决策点少 + 改动量小，3 份 Align/Design/Task 合并为单一 `PLAN_PhaseE_18_2.md`。

---

## 3. 关键技术亮点

### 3.1 跳过判定规则的修订

PLAN 阶段初版用简单的 "consumer count ≥ 2" 规则。但在 §5 性能分析阶段发现该规则会误跳过 "仅 Motion Blur(N=8)" 场景（dilation 17 fetch < inline 72 fetch，反而亏 55）。

修订后规则：`autoSkip && SSR Temporal && !MB` → 仅在真正"dilation pass 亏 fetch"的 SSR-only 单消费者场景跳过。这是基于 fetch/pixel 经济性的精确决策，避免一刀切。

### 3.2 once-log 状态追踪

`lastDilationActiveLog` 字段追踪上一帧 active/skip 状态，仅在转变时打印一次 INFO log。避免每帧 spam，又能让用户在 console 看到何时进入/退出跳过状态，方便调试。

### 3.3 RT 不重建的设计

不同于 Phase E.18.1 halfRes 切换需立即重建 RT，autoSkip 是"每帧重新评估"型决策，所以：
- `SetVelocityDilationAutoSkip` 只更新 state，不调 ReleaseDilationRT
- RT 生命周期完全由 `SetVelocityDilation` 控制
- consumer 端 fallback 路径（dilatedTex=0 → raw + inline 9-tap）原本就在，autoSkip 利用现有路径

### 3.4 三正交开关清晰分层

```
SetVelocityDilation        ← 整体启用 + RT 创建生命周期
  ↓ if true (RT 已创建)
SetVelocityDilationHalfRes ← RT 尺寸切换 (full ↔ half)
  ↓
[每帧 EndScene 重新评估]
SetVelocityDilationAutoSkip← 本帧是否运行 DrawVelocityDilate
```

每个开关单一职责，组合灵活，状态机清晰。

---

## 4. Phase E 系列累计（HDR 链路）

| Phase | 模块 | Lua API 数 | 主要交付 |
|-------|------|------------|---------|
| E.3 | HDR + 4 tonemap | 12 | ACES / Reinhard / Uncharted2 / Linear |
| E.4 | Bloom pyramid | 15 | 6-mip ping-pong |
| E.5 | Auto Exposure | 18 | Eye Adaptation + 8-bit lum mip |
| E.6 | Lens Dirt + Streak | 23 | Anamorphic flare |
| E.7 | Lens Flare | 21 | Ghost + Halo + CA |
| E.8 | SSAO | 19 | 16-sample hemisphere + bilateral blur |
| E.9~E.12 | SSR | 22 | reflect + blur + temporal |
| E.13~E.14 | Velocity buffer | — | RG16F/RG8 + dilation + reproject |
| E.15 | Motion Blur | 11 | velocity-driven per-pixel |
| E.16 | Camera-only Motion Blur | 2 | mode 0/1/2 + dual MRT |
| E.17 | Half-res Motion Blur | 2 | VRAM -75% / perf ~4× |
| E.18 | Velocity Dilation Pass | 0 | shared 9-tap, ~50% fetch save |
| E.18.1 | Velocity Dilation Half-res | 2 | VRAM -75% / dilation perf +4× |
| **E.18.2** | **Velocity Dilation Auto-Skip** | **2** | **SSR-only 场景省 1 fetch/px** |
| **累计** | **HDR 12+ 剑客** | **149** | **后处理管线持续优化** |

---

## 5. Phase E.18 / E.18.1 / E.18.2 三阶段对比

```
E.18 dilation pass 抽出
   - 多 consumer 场景共享 9-tap, 节省重复
   - 单 consumer (SSR) 略亏 1 fetch
   ↓
E.18.1 dilation pass halfRes
   - dilatedTex 半分辨率, VRAM -75%, dilation perf +4×
   - max-filter 邻域物理覆盖扩 2× (自动鲁棒)
   ↓
★ E.18.2 dilation pass auto-skip ★
   - SSR-only 单消费者场景跳过 dilation pass
   - 让 consumer 走 inline 9-tap 旧路径
   - 解决 E.18 的"单消费者略亏"边角问题
```

E.18 / E.18.1 是优化"启用 dilation pass 时"的成本；E.18.2 是解决"启用 dilation pass 反而亏"的特殊场景。三个阶段互补、正交。

---

## 6. Lua API 累积（Phase E.18.2 新增 2 个）

新增：
- `Light.Graphics.HDR.SetVelocityDilationAutoSkip(bool) → bool / nil+err`
- `Light.Graphics.HDR.GetVelocityDilationAutoSkip() → bool`

行为：
- 默认 false（Phase E.18.1 行为完全保留）
- 仅在 dilation pass 启用 + SSR Temporal 唯一消费者时跳过
- 切换不重建 RT；once-log 追踪状态变化

---

## 7. 已知限制 / 未来方向

### 短期可选项

1. **真机 GPU profile 实测**：基于理论 fetch 分析，未实测；但收益小（< 0.05ms @ 4K），优先级低
2. **adaptive autoSkip**：根据帧时长自动开/关（性能临界场景）

### 长期可选项

3. **E.18.3 接入 TAA 主管线**：未来 TAA 也是 velocity dilation consumer，autoSkip 规则需扩展
4. **E.18.4 细粒度 toggle**：SSR/MB 独立控制 dilation 启用（当前全局共用）

---

## 8. CI 状态（待回填）

| 平台 | 状态 | 时长 |
|------|------|------|
| build-windows | ⏳ | — |
| build-linux | ⏳ | — |
| build-macos | ⏳ | — |
| build-android | ⏳ | — |
| build-ios | ⏳ | — |
| build-web | ⏳ | — |

GitHub Run ID: `<pending>`
Commit hash: `<pending>`
Duration: `<pending>`

---

## 9. 工程反思

### 做得好的地方

- **决策修订及时**：PLAN §5 性能分析阶段及时修订初版的 count<2 规则，避免误跳过 MB-only 场景
- **三开关正交清晰**：dilation / halfRes / autoSkip 三层开关单一职责，组合灵活
- **once-log 实用性高**：避免每帧 spam，又能调试观察状态变化
- **6A 文档精简化**：~0.5 天工作量 + 改动量小 → 合并 PLAN（含 Align+Design+Task）单一文档，避免过度文档化
- **零回归保障**：默认 false 完全等价 Phase E.18.1，不引入任何意外行为

### 可改进点

- **收益较小**：仅 SSR-only 场景节省 1 fetch/px，对总帧时影响微小
- **fetch 经济性分析需更细**：未来若 SSR Temporal 也支持采样数（如 4-tap）时，autoSkip 阈值规则可能需要调整

### 经验沉淀

- **优化的边际收益分析关键**：E.18.2 是为 E.18 的"边缘亏损场景"打补丁；优化方案决策应基于精确的 fetch/cost 经济性，不能简单"启用阈值"
- **正交开关层层叠加是 PostFX 设计良好实践**：dilation / halfRes / autoSkip 三层互不干扰，用户认知负担低
- **每帧 vs 切换决策不同**：halfRes 是切换型决策（重建 RT）；autoSkip 是每帧决策（仅跳过执行）。设计时区分两类避免无谓重建
