# Phase F.0.10.10 — Quad-Split Linkage Demo TODO

> 6A · 后续接力. 本 phase 已 ✅ 完成.

---

## ✅ 已完成

- 4 HDR × 4 TAA instance 同帧 quad-split demo
- 460 LOC main.lua + README + LUT 复用
- 真 GL 启动测试 OK (4 instance 全部 Enable)
- 8 smoke 零回归
- PLAN + FINAL + TODO

## 🟡 待办 — CI 验证

- [ ] commit + push → CI 6 平台
- [ ] 6/6 全绿确认

## 🔵 后续接力

### 1. demo_quad_split 视觉对比工具 (低优, ~30min)

写一个 Python/PowerShell 脚本截图 4 quad 各自的关键帧 (前 30 帧 stabilize 后),
一字排开生成对比 PNG. 突出 4 profile (LUT × tonemap × sharpen) 的视觉差异.

### 2. F.0.10.9.x.3 GetState/Clone (中优, ~1.5h)

为 HDR/TAA instance 加:
- `GetState() → table` 返回当前 instance 全部 state 快照
- `CloneInstance(srcId) → newId` 一键复制 profile 到新 instance

让 demo_quad_split 的 setup_taa_instance / per-quad apply_postfx_profile 可以通过
"基于 instance 0 模板克隆 + 微调" 简化, 减少手动 SetXxx × N 调用.

### 3. F.0.10.9.x.2 Bloom/SSR/MB pyramid 多 instance (低优, ~6h)

仿 multi-instance HDR 把 Bloom/SSR/MB 也做 multi-instance, 不再依赖 demo 中
"每帧切 profile" hack. 真正 SDK 级解耦.

但优先级低: 当前 "切 profile" 模式已经能实现 demo 效果, 切 profile 单帧成本
< 1us, 性能不是瓶颈. 加 pyramid multi-instance 会 4× VRAM (64+ MB), 收益不明显.

### 4. F.0.11 demo 截图/录屏 (高优, ~3h)

实现 `Light.Graphics.Screenshot(path)` + `Light.Graphics.RecordVideo(path, fps, dur)`.
demo_quad_split 启动后自动截 30 帧 stabilize 后的画面 → PNG, 直接放 README/wiki.

### 5. demo_quad_split 升级: 6 quad 或 9 quad (低优)

提 MAX_INSTANCES → 6 或 9, 然后 6/9 quad split-screen demo. 但视觉差异已经足够
(4 quad 已经能展示完整 multi-instance), 升级收益边际递减.

## 📚 文档导航

- `PLAN_PhaseF_0_10_10.md` — 6A 设计 (架构 + 帧流程 + 4 profile 表)
- `FINAL_PhaseF_0_10_10.md` — 实现总结 + VRAM 预算 + F.0.10.x 里程碑
- `TODO_PhaseF_0_10_10.md` — 本文
- `../Phase F.0.10.9.3 demo callback migration/` — F.0.10.9.3 (前一 phase)
- `../Phase F.0.10.9.2 multi-HDR demo PIP/` — F.0.10.9.2 (双 HDR instance demo)
