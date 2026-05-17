# Phase F.2 渲染架构补齐 — CONSENSUS (共识) 文档

> **阶段**：6A Workflow — 阶段 1.5 Consensus
> **创建日期**：2026-05-17
> **依赖**：[ALIGNMENT_PhaseF_2.md](ALIGNMENT_PhaseF_2.md)

---

## 1. 任务边界

| 范畴 | 包含 | 不包含 |
|------|------|--------|
| 修 P0 G1 | HDR 通知下游 8 个后处理模块 OnHDRResized | shader 改动 |
| 补 P1 G3 | LitBatch 三件套 stub | LitBatch 内部 RT 改造 |
| 补 P1 G2 | SSAO/LensFlare/LensDirt/Streak/AE 各 6 个 multi-instance fn + Lua binding | GetState/SetState 反射 (留 F.x) |
| 测试 | 所有现有 smoke 不回归 + 新增 multi-instance smoke | 视觉真机测试 (留用户) |

## 2. 关键决策

### Q1: G1 修复时是否同时通知 TAA?
- **决策**: 不通知 TAA。
- **原因**: TAA history RT 必须保持 outputRes (history 是 output-res 的累积), `applyTAAUChange_` 已主动清 hasHistory，下次 Process 自动重建为正确尺寸。

### Q2: G2 多实例数 default+3 还是更多?
- **决策**: 沿用 4 (default + 3 user)，与 HDR/TAA/Bloom/SSR/MotionBlur 一致。
- **原因**: split-screen 4 player 已是上限，过多实例无业务支撑。

### Q3: G2 五模块 Clone 是否复制 RT?
- **决策**: 不复制。新 instance 状态 = 未 Enable，待自己 Enable 重建。
- **原因**: 与既有 BloomRenderer.CloneInstance 行为一致，避免 GL 资源所有权混乱。

### Q4: SSAO kernel 是否每 instance 独立?
- **决策**: 共享 default 的 kernel (memcpy 复制)。
- **原因**: kernel 是 deterministic Hammersley 序列，per-instance 无差异化业务价值；多 instance 持有 16×3 floats 仅复制无开销。

### Q5: AutoExposure 多实例的 GetCurrentExposure 语义?
- **决策**: 返当前 active instance 的 currentExposure。
- **原因**: HDRRenderer::EndScene 切到对应 instance 后再 tonemap 取此值，自然 per-instance 适应。

### Q6: G3 LitBatch 的三件套是否真做 stub?
- **决策**: 真 stub (空函数体)。
- **原因**: 当前 LitBatch 内部缓冲与 RT 尺寸无关，加 stub 仅为接口一致；若未来加内部 RT 直接在 stub 里实现即可。

## 3. 验收口径细化

1. ✅ Build clean (Release)
2. ✅ 所有现有 smoke 套件通过 (taa/hdr/bloom/ssao/lens_fx/lens_flare/auto_exposure)
3. ✅ 新 phase_f2_multi_instance.lua: 5 × 6 = 30 项 binding 函数存在 + create/clone/destroy round-trip + 边界拒绝
4. ✅ G1 路径: 写代码可见 BloomRenderer::OnHDRResized(renderW, renderH) 在 OnTAAURenderScaleChanged 内调用
5. (可选, 真机) 用户启用 TAAU 后, 单帧绘制无视觉异常 + Bloom pyramid 显存随 renderScale 缩放
