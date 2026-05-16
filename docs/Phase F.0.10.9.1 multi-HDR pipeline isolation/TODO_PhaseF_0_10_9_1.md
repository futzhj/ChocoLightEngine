# Phase F.0.10.9.1 — Multi-HDR Pipeline Isolation TODO

> 6A · 待办与接力清单. 本 phase (F.0.10.9.1) 已 ✅ 完成.

---

## ✅ 已完成

- §22 per-instance state 隔离 (8 大类 11 子项 PASS)
  - exposure / gamma / tonemap / 5 autoXXX flag / dilation 3 flag /
    velocityFormat / per-instance LUT / 新 instance 全 default
- §23 LUT global 跨 instance 共享 (2 子项 PASS)
- §24 TAA 与 HDR active 不联动验证 (2 子项 PASS)
- 8 相关 smoke 零回归
- C++ 代码 0 改动 (验证型 phase)
- PLAN + FINAL + TODO 文档

---

## 🟡 待办 — CI 验证

- [ ] commit + push → 触发 CI 6 平台
- [ ] 6/6 全绿确认

---

## 🔵 后续接力 (用户决定优先级)

### F.0.10.9.2 — demo live 演示主屏 + PIP 真不同分辨率

**目标**: 在真 GL 环境下演示双 HDR instance 同帧不同分辨率.

**事项**:
- [ ] HDR.Enable(1920, 1080) → default instance (主屏)
- [ ] HDR.CreateInstance + Enable(640, 360) → PIP instance
- [ ] 主屏渲染流程: SetActive(0) + Begin/Draw/End + Tonemap to fullscreen quad
- [ ] PIP 渲染流程: SetActive(1) + Begin/Draw/End + Tonemap to PIP quad
- [ ] 不同 LUT (主屏 暖调 / PIP 冷调) 演示 per-instance 调色独立
- [ ] CI headless 模式自动跳过 (无 GL context)

**工作量**: ~2h
**优先级**: 中 (展示价值, 非阻塞)

### F.0.10.9.x — 已知限制修复

#### x.1 DeleteLUT3D 不清其他 instance 的 lutTexId 引用 (悬挂指针)

**问题**: 多 instance 引用同一 LUT id, 任一 instance 调 DeleteLUT3D 后, 其他 instance 的 lutTexId 仍指向已释放的 GL tex (悬挂).

**修法**: DeleteLUT3D 时遍历所有 instance 的 lutTexId, 清同 id (置 0 + strength=0).

**位置**: `hdr_renderer.cpp::DeleteLUT3D()` + `UnwatchLUT()` + `PollLUTReloads()` (reload 时同样)

**工作量**: ~30min + smoke 1 子项
**优先级**: 低 (用户层不复用 LUT id 即可规避; 真悬挂时崩溃路径明确)

#### x.2 多 instance Bloom/SSR/MB pyramid 跟随

**问题**: Bloom/SSR/MB renderer 单 instance, 多 HDR instance 同帧 Process 共用一个 pyramid; P1/P2 分辨率差异极大时 pyramid 不匹配 (一帧后 resize 一帧后 resize 抖动).

**缓解**: 用户 P2 关 autoBloom/autoSSR/autoMotionBlur (=false), 由用户手动 Bloom.Process(rgn) 在统一 pyramid 上跑 region.

**修法 (重)**: Bloom/SSR/MB 改 multi-instance, 跟 TAA / HDR 一样 g_states 数组化. 工作量 ~6h, 收益: 真物理多 player 不同后处理参数同帧.

**优先级**: 低 (用户层 region API 已能解 90% 场景)

---

## 🟢 可选增强 (用户酌情)

- [ ] HDR.GetState(id) 返 dump 表 (debug/inspector 用)
  - 包含 enabled / width / height / exposure / gamma / tonemap / 全 autoXXX / dilation / LUT 应用
  - smoke 自动校验更紧凑
- [ ] HDR.CloneInstanceState(srcId, dstId) — 把 srcId 的 per-instance 字段拷到 dstId
  - 减用户初始化样板代码 (split-screen P1 设好后批量复制到 P2/P3/P4)
- [ ] MAX_INSTANCES 提升 4 → 8 (split-screen 8 人 / portal 多重)
  - 改 1 行 + g_slot_in_use 数组扩, 内存影响 ~400B 可接受

---

## 📞 用户支持

如遇问题, 提供:
1. **平台**: Windows MSVC / Linux GCC / Linux Clang / macOS / iOS / Web
2. **commit hash**: git log -n 1
3. **失败 smoke**: 完整 stdout (含 PASS / FAIL 行)
4. **场景**: 几个 instance, 各自分辨率, 用了哪些 per-instance API

---

## 📚 文档导航

- `PLAN_PhaseF_0_10_9_1.md` — 6A 验证设计
- `FINAL_PhaseF_0_10_9_1.md` — 验证报告 + 渲染管线解耦确认
- `TODO_PhaseF_0_10_9_1.md` — 本文 (后续接力 + 可选增强)
- `../Phase F.0.10.9 multi-HDR-instance/` — 主 phase (multi-instance 实现)
