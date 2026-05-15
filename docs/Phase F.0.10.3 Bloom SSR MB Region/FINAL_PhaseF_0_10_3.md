# Phase F.0.10.3 — Bloom/SSR/MotionBlur Region 项目总结报告

> 6A 工作流 · 阶段 6 (Assess) · FINAL
> 关联: ALIGNMENT / DESIGN / TASK / ACCEPTANCE PhaseF_0_10_3
> 完成日期: 2026-05-16 (windows 时区 UTC+08:00)

---

## 1. 项目背景

F.0.10.2 完成了 TAA 的真物理 split-screen 区域化, 但 split-screen 多 player 时其余 3 个后处理 (Bloom / SSR / MotionBlur) 仍会全屏作用 — 会出现 player 1 的 bloom 泄漏到 player 2 半屏, SSR 反射跨 region 抖动, MotionBlur 历史邻区污染等问题.

F.0.10.3 把 F.0.10.2 的 scissor + 全 size storage 路径模板复用到剩余 3 个后处理, 让用户能完整控制 split-screen 多 player 的后处理时序.

---

## 2. 交付内容

### 2.1 C++ 修改 (~440 行新增)

| 文件 | 修改 | 关键变更 |
|------|------|---------|
| `include/render_backend.h` | +29 行 | 10 个虚接口加 region 默认 0 参数 (5 SSR + 4 Bloom + 1 MB) |
| `include/bloom_renderer.h` | +18 行 | Process region overload 声明 + 22 行文档 |
| `include/ssr_renderer.h` | +22 行 | Process region overload 声明 + 文档 |
| `include/motion_blur_renderer.h` | +15 行 | Process region overload 声明 + 文档 |
| `include/hdr_renderer.h` | +14 行 | 3 对 SetAuto/GetAuto Bloom/SSR/MB 声明 |
| `src/render_gl33.cpp` | +85 行 | 10 个 GL33 实现加 scissor / sub-rect blit + 退出复位 |
| `src/bloom_renderer.cpp` | +60 行 | Process region overload + mip 链 region 缩放; `<algorithm>` include |
| `src/ssr_renderer.cpp` | +55 行 | Process region overload + 5 pass region 透传 + blur 缩半 |
| `src/motion_blur_renderer.cpp` | +20 行 | Process region overload + 老 Process 转发 |
| `src/hdr_renderer.cpp` | +35 行 | 3 个 autoXxx state + EndScene 3 处 gate + 3 对 Set/Get 实现 |
| `src/light_graphics.cpp` | +180 行 | 3 个 l_*_Process + 6 个 l_HDR_SetAuto/GetAuto + 注册 |

### 2.2 Smoke 增量 (36 PASS)

| smoke 文件 | 新增 PASS | 章节 |
|-----------|----------|------|
| `scripts/smoke/hdr.lua` | +12 | §12 SetAutoBloom/SSR/MB 3 对 × 4 PASS + fn_names |
| `scripts/smoke/motion_blur.lua` | +10 | Process defense × 6 + SetAutoMotionBlur × 4 |
| `scripts/smoke/bloom.lua` | +7 | Process defense × 6 + fn_names |
| `scripts/smoke/ssr.lua` | +7 | Process defense × 6 + fn_names |

### 2.3 6A 文档 (4 个)

| 文档 | 内容 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_3.md` | 需求边界 + 现状对照 + 5 处歧义自答 |
| `DESIGN_PhaseF_0_10_3.md` | 三模块逐 pass 改造方案 + 风险矩阵 + 边界质量预期 |
| `TASK_PhaseF_0_10_3.md` | 3 sub-phase × 5 任务原子拆分, 工作量 9-12h 估 |
| `ACCEPTANCE_PhaseF_0_10_3.md` | 验收矩阵 (5 sub-phase × commit + smoke 结果) |
| `FINAL_PhaseF_0_10_3.md` | 本文档 |
| `TODO_PhaseF_0_10_3.md` | 待办清单 |

---

## 3. 关键设计决策

### 3.1 scissor + 全 size storage 路径 (vs shader uvOffset)
- **采用**: 与 F.0.10.2 一致, scissor 限定 raster 写区, 历史 RT/pyramid 全 size 不缩
- **理由**: 跨平台风险零 (GL 1.0 标准), shader 不动, 工作量压缩 50%
- **代价**: ~1px 边界采样泄漏 (与 F.0.10.2 等价, 默认场景肉眼难辨)
- **完美方案**: shader uvOffset/uvScale (留 F.0.10.5+)

### 3.2 Bloom mip 链 region 缩放
- **downsample**: 每级递推 >>1 (curRgn = prevRgn >> 1, clamp 1×1)
- **upsample**: 按 (i-1) 反算 (dRgn = origRgn >> (i-1)), 不递推避免误差累积
- **clamp**: std::max(1, ...) 兜底 1×1 防止越界

### 3.3 SSR blur half-res region 处理
- caller (`SSRRenderer::Process`) 入口算 `blurRgn = (x/2, y/2, max(1, w/2), max(1, h/2))`
- 与 backend `CreateSSRBlurRT` 内部 `max(1, full/2)` 同模式, 不会越界

### 3.4 glBlitFramebuffer 不受 GL_SCISSOR_TEST 影响
- **陷阱**: BlitHDRDepthToSSAO + DrawSSRComposite ① pass 用 glBlitFramebuffer, 加 scissor 无效
- **解决**: 直接用 src/dst rect 限定 region (与 MotionBlur Pass2 同模式)

### 3.5 HDR Auto-Xxx 默认 true (零回归)
- 老用户不动代码 = 自动 EndScene 全屏后处理 (与 F.0.10.2 之前完全等价)
- split-screen 用户显式 `HDR.SetAutoBloom(false)` 等关掉自动, 接手手动 .Process(rgn)

---

## 4. 工作量统计

| 阶段 | 估算 (DESIGN) | 实际 | 偏差 |
|------|-------------|------|------|
| 6A 文档 (Align/Architect/Atomize/Approve) | 1h | 1h | 持平 |
| Sub-Phase 1 (MotionBlur) | 2h | ~1.5h | -25% (template 已熟) |
| Sub-Phase 2 (Bloom) | 2.5h | ~1.5h | -40% (mip 链处理顺利) |
| Sub-Phase 3 (SSR) | 3h | ~2h | -33% (5 pass 一气呵成) |
| Sub-Phase 4 (Assess) | 0.5h | ~0.5h | 持平 |
| **总计** | **9h** | **~6.5h** | **-28%** |

---

## 5. 复用 F.0.10.2 模板的成熟度

F.0.10.2 已建立了完整的工艺模板, F.0.10.3 几乎是机械复用:

| F.0.10.2 模板 | F.0.10.3 实例化 |
|--------------|----------------|
| TAARenderer::Process(rgn) overload | 3 个 module 各自照搬 |
| HDR.SetAutoTAA 开关 + EndScene gate | 3 对 SetAuto Bloom/SSR/MB |
| l_TAA_Process Lua 防御 (0/4 args, w/h>=0, 类型错) | 3 个 l_*_Process 共用模板 |
| 测试 6 PASS defense pattern | 3 个 smoke 各 6 PASS defense |
| backend pass 加默认 0 region 参数 (零回归) | 10 个 backend pass 同模式 |
| GL33 conditional scissor + 退出复位 | 10 个实现同模式 |

→ **模板成熟度 100% 复用**, 工作量从 DESIGN 估 9h 实际压到 6.5h (-28%).

---

## 6. 累计 Lua API 演进

```
F.0     TAA Master Pipeline           1 module    13 fn
F.0.x   TAA 各种 variant (8 个)        +各种      增 ~28 fn
F.0.10  TAA multi-instance            +5 fn      41 fn
F.0.10.2 真物理 split-screen TAA       +4 fn      45 fn
F.0.10.3 sub-phase 1 (MB region)       +7 fn      52 fn  (MB.Process + 6 HDR Auto)
F.0.10.3 sub-phase 2 (Bloom region)    +1 fn      53 fn  (Bloom.Process)
F.0.10.3 sub-phase 3 (SSR region)      +1 fn      54 fn  (SSR.Process)
```

F.0.10.3 是首个把 3 个后处理同时区域化的 Phase, 让 split-screen 用户能完整控制 Bloom/SSR/MB/TAA 4 大后处理的时序.

---

## 7. 后续 Phase 候选

| Phase | 内容 | 估算 |
|-------|------|------|
| **F.0.10.4** | demo_split2 升级 — 用 4 个 .Process(rgn) 跑实际双 player 后处理对比 | 2-3h |
| **F.0.10.5** | shader uvOffset/uvScale 路径 — 完美边界 + 节省 history VRAM | 6-10h |
| **F.0.11+** | 探索方向: VR 渲染 (per-eye region)、ECS DAG 多视口、frame graph | TBD |

---

## 8. 总结

F.0.10.3 顺利完成所有目标, 关键指标:
- ✅ 3 个新 .Process(region) Lua API (MB / Bloom / SSR)
- ✅ 3 对 HDR SetAuto/GetAuto 开关 (Bloom / SSR / MB)
- ✅ 10 个 backend 接口 region 化 (零回归)
- ✅ 8 smoke 全 PASS (含 SSAO / LensFlare / LensFX 复用接口零回归)
- ✅ 36 PASS smoke 增量
- ✅ 实际工作量 -28% (模板高度复用)

**结论**: 累计 Lua API 45 → 54 fn, 后处理 split-screen 工具链已完整.
