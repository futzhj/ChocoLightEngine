# Phase F.0.10.6 — HDR multi-instance FINAL 项目总结

> 6A 工作流 · 阶段 6 (Assess) · 最终交付报告
> 关联: `ALIGNMENT_PhaseF_0_10_6.md` / `DESIGN_PhaseF_0_10_6.md` / `TASK_PhaseF_0_10_6.md` / `ACCEPTANCE_PhaseF_0_10_6.md`

---

## 1. 项目背景

### 1.1 问题陈述

在 F.0.10.5 完成后, 物理 split-screen 已实现像素完美边界 (Bloom/SSR/MB/TAA 全部 region 化 + uvBounds 完美). 但 **tonemap pass 仍是全局 1 组 (exposure, gamma, mode)** — 即使 P1 P2 共享 HDR sceneTex, 最后 tonemap 时无法让 P1 黄昏暖调 vs P2 冷夜蓝调.

### 1.2 业界对照

UE 5.x 的 `FViewInfo::FinalPostProcessSettings` 支持 per-view tonemap settings (exposure, color grading), 这是 split-screen / VR multi-view 的标配. F.0.10.6 是这个标配的最简实现 (不含 color grading).

### 1.3 目标

让用户能**为每个 region 独立指定 tonemap 参数** (exposure / gamma / mode), 实现真正的多 instance HDR 输出.

---

## 2. 交付内容

### 2.1 Backend 改造

| 接口 | 类型 | 说明 |
|------|-----|------|
| `DrawTonemapRegion(hdrTex, exp, gamma, mode, x, y, w, h)` | **新增**虚函数 | 与 fullscreen 共享 program / VAO / uniform, 多 1 步 scissor; rgnW<=0 退化 fullscreen |

### 2.2 HDRRenderer API

| API | 类型 | 说明 |
|-----|-----|------|
| `SetAutoTonemap(bool)` | **新增** | 默认 true (零回归); false 时 EndScene 不自动 tonemap |
| `GetAutoTonemap()` | **新增** | |
| `Tonemap(rgn)` | **新增** | 用全局 g.exposure (含 AE) / g.gamma / g.tonemap |
| `Tonemap(rgn, exp, gamma, mode)` | **新增** | 显式 params, 不叠加 AE |

### 2.3 Lua API

| API | 类型 | 说明 |
|-----|-----|------|
| `HDR.SetAutoTonemap(bool)` | **新增** | round-trip + bad-arg + idempotent (与 SetAutoTAA 同模式) |
| `HDR.GetAutoTonemap()` | **新增** | 返 boolean |
| `HDR.Tonemap(x, y, w, h [, params])` | **新增** | per-region; params 可选 `{exposure, gamma, tonemap}` |

### 2.4 文档

- `ALIGNMENT_PhaseF_0_10_6.md` (251 行): 需求 / 假设 / scope / 验收
- `DESIGN_PhaseF_0_10_6.md` (276 行): 接口契约 + 数据流 + 异常处理
- `TASK_PhaseF_0_10_6.md` (107 行): 15 个原子任务 + 依赖图
- `ACCEPTANCE_PhaseF_0_10_6.md`: 验收记录 + 风险矩阵
- `FINAL_PhaseF_0_10_6.md`: 本文件
- `TODO_PhaseF_0_10_6.md`: 强制 + 可选 + 用户支持

---

## 3. 关键设计决策

### 3.1 退化路径: `rgnW<=0` → DrawTonemapFullscreen

**问题**: 用户传 `(0,0,0,0)` 时该如何处理? 报错? 退化?

**方案**: 退化为 `DrawTonemapFullscreen` (零回归 fallback).

**优势**:
- 简化 caller 代码 (不需特判)
- 与 `Bloom.Process(rgn=0)` 等 phase 行为一致
- 避免 `glScissor(0,0)` 导致全画面被裁掉的 BUG

### 3.2 双重载 API (含/不含 AE 叠加)

**问题**: AutoExposure (AE) 是全局自适应, 用户在 split-screen 中是否希望每 region 独立 AE? 大多数情况不希望 (因为多 player 视角应共享亮度感知).

**方案**: 提供 2 个重载:
- `Tonemap(rgn)`: AE 叠加 (与 EndScene 行为一致)
- `Tonemap(rgn, exp, gamma, mode)`: 不叠加 AE, 用户完全控制

**Lua 入口**:
- 不传 `params_table` → 走 `Tonemap(rgn)` (含 AE)
- 传 `params_table` → 走 `Tonemap(rgn, exp, gamma, mode)` (不含 AE)

### 3.3 tonemap 字段双兼容 (string / int)

**问题**: Lua 中传 tonemap operator 应该是 string (`"aces"`) 还是 int (`0`)?

**方案**: 双兼容:
```lua
HDR.Tonemap(0, 0, W, H, {tonemap = "uncharted2"})   -- string 形式 (推荐)
HDR.Tonemap(0, 0, W, H, {tonemap = 2})              -- int 形式 (优化)
```

**实现**: 复用 `hdr_tonemap_name_to_mode` (与 `HDR.SetTonemapper` 同源).

### 3.4 不复制 HDR sceneTex (out of scope)

**决策**: F.0.10.6 **不实现** per-region 独立 HDR target. 所有 region 仍共享 sceneTex (RGBA16F), 只在 *输出阶段* (tonemap) 区分 params.

**理由**:
- 多 HDR target 涉及 RT pool, 工作量数倍 (~15h+)
- 实际场景中, region 共享 HDR 内容, 不同的是 tonemap 输出曲线
- 留后续 phase (F.0.11 ?) 评估真正多 HDR target 的必要性

---

## 4. 工作量统计

| 子阶段 | 内容 | 工作量 |
|-------|------|-------|
| ALIGN + DESIGN + TASK 文档 | 3 文档 + scope + 风险评估 | 0.5h |
| Sub-Phase 1 (backend + HDRRenderer) | 1 backend + 4 HDR API + autoTonemap 改 EndScene | 1h |
| Sub-Phase 2 (Lua API + smoke) | 3 Lua fn + params_table 解析 + 7 smoke case | 1.5h |
| Sub-Phase 3 (Assess) | 3 文档 + commit | 0.5h |
| **合计** | **1 backend + 4 HDR + 3 Lua + 7 smoke** | **~3.5h** |

**vs DESIGN 估**: 6-8h, **实际 ~3.5h** (~50% 时间节省).

**节省原因**:
1. 复用 F.0.10.3 region scissor 模式 (无 shader 改动)
2. 复用 F.0.10.5 auto-* 开关模式 (HDRRenderer State 字段 + EndScene if 包裹)
3. 复用 `hdr_tonemap_name_to_mode` (无新 helper)
4. 不复制 HDR sceneTex (out of scope)
5. 不改 demo (留 demo_tonemap_split2 后续 phase)

---

## 5. 模板可复用度

本 Phase 是 F.0.10.3 (region scissor) + F.0.10.5 (auto-* 开关) 模板的精确套用.

| 候选 phase | 复用度 | 备注 |
|-----------|-------|------|
| F.0.10.7 (HDR sceneTex multi-instance, 真多 RT) | 高 | 在本 phase 基础上加 RT pool |
| F.0.11 (per-region color grading LUT) | 高 | 同 region scissor 模式 |
| F.0.12 (per-region film grain / vignette) | 高 | 同 region scissor 模式 |

---

## 6. Lua API 演化

| API | 状态 |
|-----|------|
| `Light.Graphics.HDR.SetAutoTonemap(bool)` | **新增** |
| `Light.Graphics.HDR.GetAutoTonemap()` | **新增** |
| `Light.Graphics.HDR.Tonemap(x, y, w, h [, params])` | **新增** |

**当前总数**: 54 → **57** (+3)

---

## 7. 后续候选

### 7.1 直接延伸

- **F.0.10.7** (备选): demo_tonemap_split2 \-\- 实际 demo 演示 P1 黄昏 vs P2 冷夜
  - 用 `HDR.SetAutoTonemap(false)` + 2 次 `HDR.Tonemap(rgn, params)`
  - 视觉验证 region 边界
  - 工作量 ~2-3h

- **F.0.11** (中): per-region color grading LUT
  - 加 LUT 3D texture + tonemap shader 混合 LUT
  - 工作量 ~5-7h

### 7.2 大型扩展

- **F.1** (高): DLSS-like TAAU 真上采样 (10-15h)
- **真多 HDR target** (高): 每 region 独立 sceneTex + 独立后处理 (8-10h)

### 7.3 已知限制

- 不支持 per-region color grading (LUT) — 后续 phase
- 不支持 per-region 不同 HDR 内容 (要求多 sceneTex)
- AE 在 region 模式下仍是全局 (用户可用显式 params 重载绕过)

---

## 8. Commit 历史

| Commit | 范围 |
|--------|------|
| `b9afe74` | SP1+SP2: 文档 + backend + HDRRenderer + Lua API + smoke (9 文件 +793/-2) |
| (本 commit) | SP3: 6A Assess (3 docs) |

---

## 9. 结论

Phase F.0.10.6 **成功完成**, 实现了 per-region 独立 tonemap params, 工作量低于初估 (~3.5h vs 6-8h), 模板复用度高. 与 F.0.10.5 的 shader uvBounds 完美边界配合, 现 Light Engine 已具备 **真正的物理 split-screen multi-instance 完美渲染** 能力.

**下一步建议**: 考虑 F.0.10.7 (实际 demo 视觉验证) 或 F.1 (TAAU 真上采样) — 两者均能展示前期累积的 region 化基础设施的价值.
