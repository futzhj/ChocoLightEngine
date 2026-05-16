# Phase F.0.10.9.2 — Multi-HDR Demo (Main + PIP) TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- `samples/demo_multi_hdr_pip/main.lua` 真 GL callback model demo
- `samples/demo_multi_hdr_pip/README.md`
- 2 个新 Lua API: `HDR.BeginScene()` + `HDR.EndScene()` (multi-instance lifecycle 控制)
- smoke `hdr.lua` §26 (2 子项) + fn count 52 → 54
- 8 相关 smoke 零回归
- demo 本地真 GL 模式验证 (Main 1600×900 + PIP 480×270 双独立 fbo)

---

## 🟡 待办 — CI 验证

- [ ] commit + push → CI 6 平台
- [ ] 6/6 全绿确认

---

## 🔵 后续接力 (用户决定优先级)

### F.0.10.9.x.2 — Bloom/SSR/MB pyramid 多 instance (低优先, ~6h)

仍未做. Bloom/SSR/MB renderer 单 instance, 多 HDR instance 同帧 Process 共用一个 pyramid; P1 高分辨率 + P2 低分辨率时 pyramid 不匹配抖动. 缓解: 用户关 autoBloom/SSR/MB 用 region API.

### 可选增强 GetState / Clone (中优先, ~1h)

- `HDR.GetState(id)` 返 dump 表 (debug/inspector 用)
- `HDR.CloneInstanceState(srcId, dstId)` 减用户多 instance 初始化样板

### MAX_INSTANCES 4 → 8 (低优先, ~30min)

split-screen 8 人 / portal 多重视角支持. 改 1 行 + smoke 适配.

### 修复其他 demo (中优先, ~2h)

所有 `samples/demo_*/main.lua` (含 demo_taa_split2 / demo_bloom / demo_hdr / ...) 都用错的 immediate-mode API (`win:BeginFrame/EndFrame/IsOpen/IsKeyPressed/DrawText/PollEvents`), 真 GL 模式跑不起来. 建议批量改成 callback model 仿照本 demo. 可作为独立 phase `F.0.10.9.3 demo migration`.

---

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_2.md` — 6A 设计 (验证矩阵 + 帧流程图)
- `FINAL_PhaseF_0_10_9_2.md` — 实现总结 + lessons
- `TODO_PhaseF_0_10_9_2.md` — 本文
- `../Phase F.0.10.9 multi-HDR-instance/` — F.0.10.9 主 phase
- `../Phase F.0.10.9.1 multi-HDR pipeline isolation/` — F.0.10.9.1 state 隔离验证
- `../Phase F.0.10.9.x.1 LUT id cross-instance remap/` — F.0.10.9.x.1 LUT 悬挂修
