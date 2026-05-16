# Phase F.0.10.9.x.1 — Cross-Instance LUT id Remap TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- `RemapLUTIdAcrossInstances` 辅助 fn (anonymous namespace)
- 4 处接入: DeleteLUT3D / UnwatchLUT / WatchLUT (重注册) / PollLUTReloads
- DeleteLUT3D 顺序修复 (state cleanup 在 backend 检查前, headless 也跑)
- smoke §25 2 子项 PASS
- 8 相关 smoke 零回归

---

## 🟡 待办 — CI 验证

- [ ] commit + push → 触发 CI 6 平台
- [ ] 6/6 全绿确认

---

## 🔵 后续接力

### F.0.10.9.2 — demo live 演示主屏 + PIP (中优先 ~2h)

真 GL 环境下顺便验证本 phase 的 §25.3 / §25.4 / §25.5 (headless 不可构造的路径):
- UnwatchLUT 跨 instance 清
- PollLUTReloads 跨 instance remap (hot reload)
- WatchLUT 同 path 重注册时清旧 id

### F.0.10.9.x.2 — Bloom/SSR/MB pyramid 跟随多 HDR instance (低优先 ~6h)

仍未做. Bloom/SSR/MB renderer 改 multi-instance, 跟 TAA / HDR 同模板 g_states 数组化. 收益: 真物理多 player 不同后处理参数同帧渲染 (P1 高质 Bloom + P2 低质 Bloom).

**缓解 (临时)**: 用户 P2 关 autoBloom/autoSSR/autoMotionBlur, 由用户手动 Bloom.Process(rgn) 在统一 pyramid 上跑 region API.

---

## 🟢 可选增强

- [ ] `HDR.GetState(id)` 返 dump 表 (debug/inspector 用)
- [ ] `HDR.CloneInstanceState(srcId, dstId)` 减用户多 instance 初始化样板
- [ ] `MAX_INSTANCES` 提升 4 → 8 (split-screen 8 人 / portal 多重)

---

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_x_1.md` — 6A 设计
- `FINAL_PhaseF_0_10_9_x_1.md` — 实现总结 + 验证
- `TODO_PhaseF_0_10_9_x_1.md` — 本文
- `../Phase F.0.10.9 multi-HDR-instance/` — F.0.10.9 主 phase
- `../Phase F.0.10.9.1 multi-HDR pipeline isolation/` — F.0.10.9.1 验证 phase
