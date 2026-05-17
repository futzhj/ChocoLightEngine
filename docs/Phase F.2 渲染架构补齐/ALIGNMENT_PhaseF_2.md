# Phase F.2 渲染架构补齐 — ALIGNMENT (对齐) 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **创建日期**：2026-05-17
> **基线**：Phase F.0.11.6 + F.1.1 完结后
> **目的**：通过审视识别既有渲染架构的隐性缺口，与用户对齐补齐范围。

---

## 1. 任务来源

用户要求"继续完善并完成所有渲染方面的架构，处理完了再开始非渲染"。HANDOFF 文档明确列出的渲染待办仅 F.1.2 (可选) 一项，需要审视代码识别隐性缺口。

## 2. 审视范围与方法

- 9 个后处理模块: HDR / TAA / Bloom / SSR / MotionBlur / SSAO / LensFlare / LensDirt / Streak / AutoExposure
- 工具模块: BatchRenderer / LitBatchRenderer
- 核心检查点:
  1. 多实例 (Multi-Instance) 一致性
  2. TAAU 联动盲点
  3. Resize 路径完整性
  4. backend 抽象漏洞
  5. GLES3 兼容性
  6. 资源生命周期

## 3. 识别的缺口

### 🔴 P0 (正确性 Bug) — G1: TAAU 切换未通知下游后处理模块

- **位置**: [hdr_renderer.cpp:738-794](../../ChocoLight/src/hdr_renderer.cpp#L738-L794) `OnTAAURenderScaleChanged` / `OnTAAUDisabled`
- **设计意图** (DESIGN F.1 §2.1): "Bloom/SSAO/SSR/MotionBlur 全部 @ render-res"
- **实际实现**: 重建 HDR FBO 为 renderRes 时未调用下游模块的 `OnHDRResized(renderW, renderH)`
- **后果**:
  - sceneTex = renderW × renderH，但 Bloom pyramid 仍是 outputW × outputH
  - SSAO depth/AO RT 仍是 outputRes，与 sceneTex 比例错位
  - LensFlare/Streak/MotionBlur 同问题
  - 性能未享受 TAAU 红利 (低分辨率主路径但下游仍跑高分辨率)

### 🟡 P1 (架构一致性) — G2: 多实例覆盖不全

| 模块            | 多实例支持 | 备注 |
|-----------------|-----------|------|
| HDRRenderer     | ✅        | F.0.10.6 |
| TAARenderer     | ✅        | F.0.10 |
| BloomRenderer   | ✅        | F.0.10.9.x.2 |
| SSRRenderer     | ✅        | F.0.10.9.x.2 |
| MotionBlurRenderer | ✅     | F.0.10.9.x.2 |
| **SSAORenderer**     | ❌    | 待补 |
| **LensFlareRenderer**| ❌    | 待补 |
| **LensDirtRenderer** | ❌    | 待补 |
| **StreakRenderer**   | ❌    | 待补 |
| **AutoExposureRenderer**| ❌  | 待补 |

**用户场景影响**: split-screen / PIP 场景下后 5 类效果只能全屏共享参数，与 Phase F.0.10.x 多实例设计目标不一致。

### 🟢 P1 (接口一致性) — G3: LitBatchRenderer 缺 HDR 联动钩子

- **位置**: [lit_batch_renderer.cpp](../../ChocoLight/src/lit_batch_renderer.cpp)
- **现状**: 其他后处理都有 `OnHDREnabled/Disabled/Resized` 三件套，唯独 LitBatch 没有
- **风险**: 接口不一致使未来加内部缓存的人踩坑

## 4. 已确认范围

用户拍板: **P0 + P1 全接上** (G1 + G3 + G2 五模块)

## 5. 验收口径

- 全量 smoke 测试 (taa/hdr/bloom/ssao/auto_exposure/lens_fx/lens_flare) 通过
- 新增 phase_f2_multi_instance.lua 验证 5 模块 multi-instance API 行为
- Build 干净 (无 warning regression)
