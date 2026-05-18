# Phase G.1.1 — VRAM Tracking v1.1 ALIGNMENT

## 一. 原始需求

> **TODO_PhaseG_1.md §二 P0**: Bloom mipmap chain + SSAO RT + Auto Exposure luminance RT + TAAU outputSceneTex 主动 Track

闭合 G.1 留下的 4 个"未跟踪但价值高"的 RT 类别, 全部沿用 G.1 的 `LT::GpuMem` 基础设施, 只加 hook 点, 不改架构.

## 二. 项目上下文分析

### 2.1 各模块 RT 现状 (代码扫描结果)

#### A. Bloom (`bloom_renderer.cpp`)
- `Enable(w, h)` 调 `backend->CreateBloomPyramid(w, h, levels, fbos[8], texs[8])` 创建 N 级 RGBA16F 纹理
- 每级尺寸 `w/2^i × h/2^i` (最小 1×1), N 由 `g.actualLevels` 记录 (clamp [2, 8])
- `ReleasePyramid()` 调 `backend->DeleteBloomPyramid(fbos, texs, actualLevels)`
- **Multi-instance**: 4 instance, 每个独立 pyramid

**预估占用 (1080p, 5 levels)**: ≈ 22 MB (1920×1080×8 + 半减递推 = base × 4/3)

#### B. SSAO (`ssao_renderer.cpp`)
- `AllocateResources(w, h)` 调 backend 创建:
  - depth RT: `DEPTH_COMPONENT24` at `srcW × srcH` (full-res, 用于 blit HDR depth)
  - AO ping-pong: R16F × 2 at `rtW × rtH` (`max(srcW/2, 32) × max(srcH/2, 32)` 半分辨率)
  - noise tex: 4×4 RGBA8 (~64 bytes, **不跟踪** 太小)
- `DestroyResources()` 反向释放

**预估占用 (1080p)**: depth 8 MB + AO×2 ≈ 1 MB = 9 MB

#### C. Auto Exposure (`auto_exposure_renderer.cpp`)
- `Enable(w, h)` 调 `backend->CreateLuminanceTarget(w, h, ..., &lumW, &lumH)` 创建 R16F mipmap-able 纹理
- 实际尺寸 `max(w/4, 8) × max(h/4, 8)` (~480×270 @ 1080p), 含完整 mipmap chain (`glTexStorage2D` 一次性分配)
- `ReleaseLuminanceRT()` 释放
- **Multi-instance**: 4 instance, 每个独立 lumTex

**预估占用 (1080p)**: 480×270×2 ≈ 260 KB (base 级); 含 mipmap chain ≈ 345 KB (×4/3)

#### D. TAAU outputSceneTex (`hdr_renderer.cpp::OnTAAURenderScaleChanged`)
- TAAU 启用时创建 `outputSceneTex` (RGBA16F at outputW×outputH, 比 sceneTex 的 renderW×renderH 大)
- **G.1 已 Untrack** (`ReleaseRT` line 264-266), **缺 Track** (创建路径在 `OnTAAURenderScaleChanged` line 840, 不在 `CreateRT`)
- 这是 G.1 的"已知遗漏", 此次 v1.1 补全

**预估占用 (1080p output, 0.6× renderScale)**: 1920×1080×8 = 16 MB

### 2.2 复用基础设施

- `LT::GpuMem::Track/Untrack(name, format, w, h)` — G.1 已实现, 不改
- 已知格式: `RGBA16F` / `R16F` / `DEPTH24` 全部已支持 (G.1 公式表覆盖)
- 不增不删 BPP 公式

### 2.3 mipmap 处理决策

**只跟踪 base 级** (Auto Exposure):
- 实际占用 ≈ base × 4/3 (×33% 增量)
- 1080p 下差异 ~85 KB, 全总占用差异 < 0.1%
- 跟踪 base 简化逻辑, 用户看 lumTex 480×270 R16F 体感更直观
- 与 G.1 已接受"silent fallback over-report 1.5MB"的简化哲学一致 (诊断 vs 精度权衡)

**Bloom pyramid**: 每级独立纹理, 逐级 Track (用户看 5 行明细)

## 三. 边界确认 (任务范围)

### ✅ 必做 (v1.1, ~1.8h)
- Bloom pyramid: Enable / ReleasePyramid 各 N 次 Track/Untrack (N = actualLevels)
- SSAO: AllocateResources / DestroyResources Track/Untrack 3 项 (depth + AO×2)
- Auto Exposure: Enable / ReleaseLuminanceRT Track/Untrack lumTex base 级
- TAAU outputSceneTex: 在 `OnTAAURenderScaleChanged` 内补 Track (Untrack 在 G.1 已加)
- smoke 扩展: 验证 4 项启用/关闭 bytes 增减
- ALIGNMENT + FINAL 文档

### ❌ 不做 (留 v2)
- Auto Exposure mipmap chain 精确跟踪 (差 ~33%, < 0.1% 总量)
- SSAO noise tex (64 bytes 太小)
- Lens Dirt / Streak / LensFlare RT (体积小)
- 用户 Image / Mesh / Font (50+ hook 点, v2 P1)

## 四. 决策 (主动)

### 决策 1: Phase 编号 — 用 G.1.1 (sub-version) 还是 G.2 (new phase)?
**自动决策: G.1.1**
原因: 4 项均是 G.1 留下的 TODO P0, 复用全部基础设施, 不改架构, 不增 API. 与 F.0.11.6.x 系列同模式 (sub-version 表"小扩展闭环").

### 决策 2: Bloom 每级独立 Track 还是合并 Track?
**自动决策: 每级独立** (用 `name="Bloom pyramid"`, 不同 size 自动分桶)
原因: 用户看到 "Bloom pyramid 1920×1080 RGBA16F ×1, Bloom pyramid 960×540 RGBA16F ×1, ..." 很直观能理解 mipmap chain. 与 TAA history (×2 ping-pong 同 size 合并 count=2) 形成对比, 反映各自数据结构本质.

### 决策 3: AE mipmap 是否精确跟踪?
**自动决策: 只跟踪 base 级**
原因:
- mipmap chain 多分配 base × 1/3 ≈ 85 KB @ 1080p (相对 81+ MB 总量 < 0.1%)
- 跟踪精确 mipmap 需要 BPP 公式分支, 增 tracker 复杂度收益小
- 用户看 lumTex 480×270 R16F 体感清楚

### 决策 4: SSAO depth tex 用 DEPTH24 还是 DEPTH32F?
**自动决策: DEPTH24** (硬实测代码:render_gl33.cpp:6349 用 GL_DEPTH_COMPONENT24)
注: SSR depth tex 是 DEPTH32F (G.1 已正确), 两者格式不同别混淆.

### 决策 5: 名字命名约定?
**自动决策**:
- `"Bloom pyramid"` — 各 size 自动分桶 (mipmap-like 表现)
- `"SSAO depthTex"` — 单级 DEPTH24
- `"SSAO AO"` — ping-pong 双张同 size 合并 count=2
- `"AE luminance"` — base 级 R16F (mipmap 不跟踪)
- `"HDR outputSceneTex (TAAU)"` — 与 G.1 ReleaseRT Untrack 已用名字一致 (保持对称)

## 五. 验收标准

| 标准 | 期望 |
|------|------|
| `Light.Graphics.GetMemoryStats` API 不变 (零回归) | ✅ |
| Bloom Enable -> Disable bytes 增减 | smoke 增 1 用例 |
| SSAO Enable -> Disable bytes 增减 | smoke 增 1 用例 |
| AE Enable -> Disable bytes 增减 | smoke 增 1 用例 |
| TAAU SetTAAUEnabled(true) -> outputSceneTex 出现在 items | smoke 增 1 用例 (条件: 有 GL ctx) |
| 全 8 套 smoke 0 退化 | CI 全 6 平台绿 |
| Bloom pyramid items 数 = actualLevels | 5 levels @ default |

## 六. 与 G.1 对比

| 维度 | G.1 | G.1.1 |
|------|-----|-------|
| 跟踪类别 | 5 (HDR/TAA/SSR/Dilate/UBO) | +4 (Bloom/SSAO/AE/TAAU) = 9 |
| 实现 | 新模块 + 5 hook | 仅 +4 hook |
| LOC 增量 | +1084 | ~+50 (估) |
| API 变化 | 新增 2 (GetMemoryStats/Reset) | 0 (复用) |
| 架构 | 新模块 LT::GpuMem | 不变 |
| 估时 | 3.5h | 1.8h |

## 七. 实施估时

| 任务 | 估时 |
|------|------|
| Align (本文档) | 0.3h |
| Bloom hook | 0.3h |
| SSAO hook | 0.3h |
| AE hook | 0.2h |
| TAAU outputSceneTex hook | 0.2h |
| smoke 扩展 + 验证 | 0.3h |
| FINAL + commit + CI | 0.3h |
| **合计** | **1.9h** (估时 1.8h, +0.1h buffer) |
