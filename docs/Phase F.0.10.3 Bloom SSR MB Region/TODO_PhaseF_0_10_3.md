# Phase F.0.10.3 — TODO 清单

> 6A 工作流 · 阶段 6 (Assess) · TODO
> 关联: ACCEPTANCE / FINAL PhaseF_0_10_3
> 用途: 精简明确哪些待办事宜、需要用户支持的项

---

## 1. 强制项 (本 Phase 不结束不能进下一 Phase)

### 1.1 CI 验证回填 (User Action)

**待办**: push 后等 CI 跑全 smoke (build-templates.yml 已含 hdr / motion_blur / bloom / ssr / taa)

**操作**:
```bash
# 用户在浏览器打开:
# https://github.com/futzhj/ChocoLightEngine/actions
# 看最新一次 main 分支 build-templates 是否 6/6 success
```

**回填位置**: `ACCEPTANCE_PhaseF_0_10_3.md` §1 表格 "CI run" 列 + §7 待办

---

## 2. 可选项 (本 Phase 可不做, 下个 Phase 可考虑)

### 2.1 demo_split2 升级 (F.0.10.4 候选)

**现状**:
- F.0.10.2 已交付 `samples/demo_taa_split2/main.lua` (TAA 双 player)
- 但 demo 内仍 `HDR.SetAutoBloom(true)` 默认, 不展示 Bloom/SSR/MB region

**升级方案** (留 F.0.10.4):
- demo_split2 加 4 个 `.Process(rgn)` 调用展示完整 split-screen 后处理
- 可对比 auto-on vs auto-off 视觉差异 (左半边 Bloom 不泄漏到右半边)

**工作量估**: 2-3h

### 2.2 shader uvOffset/uvScale 路径 (F.0.10.5 候选)

**现状**:
- 边界采样有 ~1px 泄漏 (与 F.0.10.2 同等级, 默认场景肉眼难辨)
- 用户拍板 (DESIGN §2.4): 接受当前精度, 暂不上 shader 改造

**升级方案** (留 F.0.10.5):
- Bloom downsample/upsample shader 加 `uvOffset` + `uvScale` uniform
- SSR temporal shader 加 region clamp
- 完美边界 + 节省 history VRAM (per-region size 而非 full size)

**工作量估**: 6-10h (跨平台测试 GLES web/iOS/android 兼容)

### 2.3 性能 benchmark (DESIGN §4.5)

**现状**: scissor 路径理论 perf ~= 老路径 (硬件本就 scissor 内才执行 fragment)

**验证方案**:
- 在有 GL 上下文的机器上跑 demo_split2 升级版
- 用 `Light.Profiler` 测 BeginScene/EndScene 帧时间
- 关 auto vs 开 region 各自 100 帧均值

**工作量估**: 1-2h (需要桌面 + 监视器, 非 CI)

---

## 3. 已规避的风险 (无需待办)

- ~~mip 链 region 缩半误差~~: upsample 按 (i-1) 反算 + std::max(1, ...) clamp 已解决
- ~~SSR blur 缩半越界~~: caller 用 max(1, w/2) 已解决
- ~~glBlitFramebuffer 不受 scissor~~: 用 src/dst rect 显式控制已解决
- ~~复用 Bloom shader 的 LensFlare/Streak 回归~~: 默认 0 参数零行为变更已验证
- ~~复用 BlitHDRDepthToSSAO 的 SSAO 回归~~: smoke 8/8 PASS 已验证

---

## 4. 用户支持需求

| 需求 | 类型 | 紧急度 |
|------|------|--------|
| CI 验证全 smoke 通过 | 等 push | 高 (本 phase 收尾) |
| demo_split2 升级方向决策 | 设计选择 | 中 (下 phase 起点) |
| shader uvOffset 路径是否优先 | 设计选择 | 低 (默认场景可接受) |

---

## 5. 文档导航

- [ALIGNMENT_PhaseF_0_10_3.md](./ALIGNMENT_PhaseF_0_10_3.md) — 需求边界 + 现状对照
- [DESIGN_PhaseF_0_10_3.md](./DESIGN_PhaseF_0_10_3.md) — 三模块逐 pass 改造方案
- [TASK_PhaseF_0_10_3.md](./TASK_PhaseF_0_10_3.md) — 3 sub-phase × 5 任务原子拆分
- [ACCEPTANCE_PhaseF_0_10_3.md](./ACCEPTANCE_PhaseF_0_10_3.md) — 验收矩阵
- [FINAL_PhaseF_0_10_3.md](./FINAL_PhaseF_0_10_3.md) — 项目总结报告

---

> 当前 Phase 状态: **3 个 sub-phase 全部实施完成 + 36 PASS smoke + 6A 文档齐备**, 仅等 CI 回填结束.
