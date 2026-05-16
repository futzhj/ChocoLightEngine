# Phase F.0.10.9 — Multi-Instance HDRRenderer TODO

> 6A · 待办与接力清单. 本 phase (F.0.10.9 主体) 已 ✅ 完成,
> 余下事项分为: **CI 验证** / **下次接力 phase** / **可选增强** / **用户支持**.

---

## ✅ 已完成 (本 phase)

- HDR state 数组化 + macro 切换 + 5 新 fn (Create/Destroy/SetActive/GetActive/GetCount)
- Lua API 5 fn 注册 (HDR 47 → 52)
- LUT watch list / callback / hotReload 拆 g_global
- smoke `hdr.lua` §21 — 11 子项 PASS
- demo `demo_taa_split2/main.lua` — F.0.10.9 round-trip probe PASS
- 8 相关 smoke 零回归验证 (hdr/bloom/ssr/auto_exposure/lens_fx/motion_blur/taa/lighting2d)

---

## 🟡 待办 — CI 验证 (本 phase commit 后)

- [ ] commit + push → 触发 GitHub Actions 6 平台
  - Windows MSVC Release / Linux GCC / Linux Clang / macOS / iOS / Web (Emscripten)
- [ ] 6/6 全绿确认 (CI 跑全部 70+ smoke + 50+ demo headless)
- [ ] 若任一平台失败, 复盘修补 (典型: Linux/Web 缺 size_t include / Lua 5.1 兼容)

---

## 🔵 后续接力 phase (用户决定优先级)

### F.0.10.9.1 — Bloom/SSR/MB/SSAO/TAA 与多 HDR instance 联动验证

**目标**: 用户场景 split-screen 双 player **真同帧** 各自 Process Bloom/SSR/MB/Tonemap, 验证 active 切换不污染.

**事项**:
- [ ] 写组合 smoke `scripts/smoke/multi_hdr_pipeline.lua`
  - HDR.CreateInstance + 双 instance Enable 不同分辨率
  - 同帧 Bloom.Process(rgn) + SSR.Process(rgn) + MB.Process(rgn) + Tonemap(rgn) 在 active 切换之间
  - 验证 GetSceneTexture / GetHDRFBO 跟随 active 切换正确返
- [ ] TAA active 是否需自动联动 HDR active? (设计决策: **不联动**, 用户负责 TAA.SetActiveInstance + HDR.SetActiveInstance 同步)
  - smoke 测试错配场景 (HDR=1, TAA=0) 时 TAA.Process 是否退化合理
- [ ] 若发现 Bloom/SSR pyramid 单 instance 限制, 评估扩 multi-pyramid 必要 (短期可让用户复用单 pyramid)

**工作量**: ~1.5h
**优先级**: 高 (split-screen 真多 instance 场景的下一关键里程碑)

### F.0.10.9.2 — demo live 演示 主屏 + PIP 真不同分辨率

**目标**: `demo_taa_split2` 改造或新写 `demo_pip_hdr` 演示主屏 1920×1080 + PIP 640×360.

**事项**:
- [ ] HDR.Enable(1920, 1080) → default instance (主屏)
- [ ] HDR.CreateInstance + Enable(640, 360) → PIP instance
- [ ] 主屏渲染流程: SetActive(0) + Begin/Draw/End + Tonemap to fullscreen quad
- [ ] PIP 渲染流程: SetActive(1) + Begin/Draw/End + Tonemap to PIP quad
- [ ] 不同 LUT (主屏 暖调 / PIP 冷调) 演示 per-instance 调色独立
- [ ] CI headless 模式自动跳过 (无 GL context)

**工作量**: ~2h
**优先级**: 中 (展示价值, 非阻塞)

### F.0.10.9.3 — 跨 instance LUT 切换性能优化 (可选)

**目标**: 多 instance 各自 SetGradingLUT 时, 共用同一 LUT id 不重复绑定.

**事项**:
- [ ] 调研: 当前 SetGradingLUT 是否有 LUT id 缓存比较?
- [ ] 若无, 加 last-bound LUT id state 减重复 glBindTexture 调用
- [ ] 多 instance 切换时性能 micro-bench

**工作量**: ~30min
**优先级**: 低 (LUT 应用 fast path, 实测无瓶颈再做)

---

## 🟢 可选增强 (用户酌情)

- [ ] HDR.GetState(id) 返一个 dump 表 (debug/inspector 用), 包含 enabled/width/height/exposure/...
- [ ] HDR.CloneInstanceState(srcId, dstId) — 把 srcId 的所有 per-instance 参数拷贝到 dstId, 减用户初始化样板代码
- [ ] MAX_INSTANCES 提升到 8 (split-screen 8 人 / portal 多重视角)
  - 内存影响: 4 → 8 instance 多占 ~400B, 可接受
  - 改 1 行 `static constexpr int MAX_INSTANCES = 8;` + `g_slot_in_use` 初始化数组扩

---

## 🔴 已知限制 (不阻塞, 文档化)

- **Bloom/SSR/MB renderer 单 instance**: 多 HDR instance 同帧 Process 共用一个 Bloom pyramid → 若 P1/P2 分辨率差异极大, pyramid 不匹配; **缓解**: 用户 P2 可关 Bloom (autoBloom=false), 或下 phase F.0.10.9.1 评估 multi-pyramid
- **TAA 与 HDR active 不自动联动**: 用户切 HDR.SetActiveInstance(1) 后必须自己 TAA.SetActiveInstance(1); 文档已说明, smoke §21 也覆盖
- **LUT id 全局共享**: 多 instance 各自 SetGradingLUT 应用同一 LUT 时, DeleteLUT3D 一次后所有 instance 失效 (用户负责生命周期); UnwatchLUT/DeleteLUT3D 时 lutTexId 仅同步清当前 active 槽 — 若其他 instance 也引用该 id 会变悬挂
  - **修复方向**: DeleteLUT3D 时遍历所有 instance 清相同 lutTexId; 留 F.0.10.9.x

---

## 📞 用户支持

如遇问题, 提供以下信息以便诊断:

1. **平台**: Windows MSVC / Linux GCC / Linux Clang / macOS / iOS / Web
2. **Light.dll 版本**: git log -n 1 commit hash
3. **失败 smoke**: 完整 stdout (含 [PASS] / [FAIL] / [SKIP] 行)
4. **场景**: 单 instance 还是 multi-instance, 几个 instance, 各自分辨率
5. **复现脚本**: 最小 Lua 复现片段

---

## 📚 文档导航

- `PLAN_PhaseF_0_10_9.md` — 6A 设计 + 任务拆分
- `FINAL_PhaseF_0_10_9.md` — 实现总结 + 验证报告 + Lua API 列表
- `TODO_PhaseF_0_10_9.md` — 本文 (后续接力 + 可选增强)
