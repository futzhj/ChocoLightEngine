# Phase F.1.4 Dynamic Resolution Scaling — ALIGNMENT 文档

> **阶段**: 6A Workflow — 阶段 1 Align (对齐)
> **基线**: FINAL_PhaseF_1.md (F.1 主路径) / FINAL_PhaseF_1_0_1.md (multi-instance) / FINAL_PhaseF_1_1.md (mip bias)
> **创建日期**: 2026-05-19
> **目标**: 帧率自适应 — 监控帧时间, 自动调节 `renderScale` 维持目标 FPS

---

## 1. 原始需求

> "F.1.4 Dynamic Resolution Scaling (DRS) — 帧率自适应, SetDynamicTarget(targetFPS) API + 滑动窗口帧时间监控 + 超预算自动降 renderScale, 主机/移动端实用. 估时 3-5h."

---

## 2. 项目上下文 (实测)

### 2.1 现有 TAA / TAAU 架构

| 组件 | 路径 | 关键状态 |
|------|------|---------|
| 多实例 state | `taa_renderer.cpp:98` | `g_states[MAX_INSTANCES=4]` + `g_active` |
| RenderScale API | `taa_renderer.cpp:748` | `SetRenderScale(float)` 接受 `[0.5, 1.0]` |
| 应用钩子 | `taa_renderer.cpp:682` | `applyTAAUChange_()` 触发 HDR/Bloom/SSR 重建 + history 重置 |
| Multi-instance 支持 | F.1.0.1 闭合 | per-instance taauEnabled / renderScale |
| 时间源 | `platform_window_sdl3.cpp:605` | `PlatformWindow::GetTime()` 返 double 秒 (SDL_GetPerformanceCounter, us 精度) |

### 2.2 DRS 必须解决的真实问题

1. **RT 重建开销**: `applyTAAUChange_()` 每次切换 ReleaseRT + CreateRT, history 重置 1 帧 (画面短暂 jitter). 不能每帧调.
2. **频繁切换会引入抖动 (oscillation)**: 帧时间天然波动 ±1ms, 简单阈值会 ping-pong.
3. **下行 (升 scale) 风险**: 升 scale = 增加 GPU 负担, 可能瞬间超预算又降回, 需要 hysteresis (滞回).
4. **multi-instance 复杂性**: per-instance scale 独立监控? 还是仅 default instance 启用 DRS?

---

## 3. 需求理解 / 任务边界

### 3.1 包含 (in-scope)

- `Light.Graphics.TAA.SetDynamicTarget(fps)` — 设置目标 FPS, 0 = 关闭 DRS
- `Light.Graphics.TAA.GetDynamicTarget()` — 查询当前目标
- `Light.Graphics.TAA.SetDynamicEnabled(bool)` — 总开关 (与 target 解耦, 便于临时禁用)
- `Light.Graphics.TAA.GetDynamicEnabled()` — 查询
- `Light.Graphics.TAA.UpdateDRS(deltaTimeSec)` — 用户每帧调用, 推进 DRS 状态机
- `Light.Graphics.TAA.GetDynamicStats()` — 返回 `{avgFrameTimeMs, currentScale, scaleAdjustments}`
- 滑动窗口算法 (帧时间均值)
- 限频策略 (N 帧间隔才能调一次)
- Hysteresis (升降阈值不同, 防 ping-pong)
- Smoke 测试 (API 存在性 + 状态机 + 边界)
- demo_taau 集成 (HUD 显示 DRS 状态 + 键位切换)

### 3.2 不包含 (out-of-scope)

- 真实 GPU profiler 整合 (依赖外部 NSight/RenderDoc, 留 F.1.5 探索)
- ML-based DRS (UE5 神经网络预测)
- 跨 instance 联动 DRS (multi-pip 合作降级, 留 F.1.6)
- 像素 quality metrics (例如 SMAA 边缘度量) 反馈

---

## 4. 关键决策点 (需用户确认)

以下 5 个决策点会显著影响实现复杂度与行为. 默认按"业界主流 + 项目一致性"自动决策, 列出供用户审阅:

### Q1. DRS 触发时机 (用户每帧调 vs 引擎自动钩入)

| 方案 | 优势 | 劣势 |
|------|------|------|
| **A. 用户每帧调 `TAA.UpdateDRS(dt)`** ✅ 推荐 | 与 SpriteAnim/Camera 现有 Lua API 一致; 用户掌控时机; 不污染 EndScene | 用户忘调则 DRS 静止 |
| B. 引擎自动钩入 `EndScene` / `Present` | 用户零参与 | 需改 `light_graphics.cpp` 主循环, 难关闭 |

**默认决策**: A (用户每帧调). 理由: 与 ChocoLight 一致的 "explicit > implicit" 哲学, demo_taau 加 1 行调用即可.

---

### Q2. DRS 算法策略 (PI 控制 vs 限频降阶)

| 方案 | 算法 | 行为 |
|------|------|------|
| A. 简单 PI 控制 | 比例项 + 积分项 | 平滑但震荡风险高 |
| **B. 限频离散降阶** ✅ 推荐 | 滑动窗口 + N 帧才调一次 + 4 档预设跳转 | 稳定, 视觉影响可预测 |
| C. 连续 scale [0.5,1.0] | 任意小数 | 平滑但 history 频繁重置 |

**默认决策**: B (限频 + 离散 4 档). 理由:
- 利用现有 4 档预设 (`Performance/Balanced/Quality/Native`), 与 F.1 一致
- 限频 = 至少 N 帧 (默认 60 帧 ≈ 1 秒) 才能调一次, 不频繁重建 RT
- Hysteresis: 帧时间 > target × 1.10 才降, 帧时间 < target × 0.85 才升

---

### Q3. DRS 影响范围 (per-instance vs 仅 default)

| 方案 | 范围 |
|------|------|
| A. 仅 default instance (g_active=0) 支持 DRS | 简单, 与 F.1.0 限制对齐 |
| **B. Per-instance DRS 状态** ✅ 推荐 | 每 instance 独立目标 fps, multi-pip 各自降级 |

**默认决策**: B (per-instance). 理由:
- F.1.0.1 已支持 per-instance TAAU, DRS 自然延续
- Multi-pip 不同视角负担差异大 (主视角重 / mini-map 轻), per-instance 合理
- 实现成本: 把 DRS state 5 字段加入 `State` 结构 (`g_states`), 复用现有 macro `#define g g_states[g_active]`
- **关键约束**: DRS 监控全局帧时间 (PlatformWindow::GetTime), 但调整的是 active instance — 用户切 active 后, 监控对象自动跟随

---

### Q4. 升降阈值与限频参数 (业界默认 vs 用户可调)

| 参数 | 默认 | 业界对照 |
|------|------|---------|
| `targetFPS` | 60 | 主流 |
| 滑动窗口大小 | 30 帧 (~0.5s @ 60fps) | UE5 = 30, Unity = 60 |
| 调整间隔 (cooldown) | 60 帧 (~1s @ 60fps) | 防 ping-pong |
| 降级阈值 (frameTime > target × N) | 1.10 (10% over budget) | UE5 = 1.05, FSR2 demo = 1.15 |
| 升级阈值 (frameTime < target × N) | 0.85 (15% under) | hysteresis ratio 1.30 |
| 单次调整步长 | 1 档预设 | 防大跳跃 |

**默认决策**: 上述默认值, 但全部暴露 Lua API 供调优:
- `TAA.SetDynamicConfig({windowSize=30, cooldownFrames=60, downThreshold=1.10, upThreshold=0.85})`

---

### Q5. 与 SetRenderScale 手动调用的优先级

如果 DRS 启用同时用户手动 `SetRenderScale(0.6)`, 谁赢?

| 方案 | 行为 |
|------|------|
| A. 手动调用立即生效, DRS 在下次 cooldown 周期接管 | 与 PostFX 类参数一致 |
| **B. 手动调用 = 显式 override, 关闭 DRS 自动调** ✅ 推荐 | 防止用户疑惑 "为啥我设了又被改回" |
| C. 手动调用被忽略 (DRS 启用时拒绝) | 太严格 |

**默认决策**: A (与 F.1 一致, 不破现有 API 语义). DRS 状态机重新读 `g.renderScale` 作为起点, 用户手调与 DRS 自动调透明融合.

---

## 5. 疑问澄清

如果默认决策有不合期望的, 请回复. 否则确认进入阶段 2 (Architect).

**潜在歧义点 (主动澄清)**:

1. **DRS 应该 instance-local 还是 global?** — 默认 instance-local (per-instance state), 仅 active instance 调整 (Q3 = B).

2. **目标帧率 0 / 负数 / 极端值如何处理?** — 默认: `target <= 0` 关闭 DRS; `target < 30` clamp 到 30 (避免无意义低目标); `target > 240` clamp 到 240.

3. **第一次启用 DRS 时, 滑动窗口未填满如何处理?** — 默认前 N 帧不调整 (warming up phase), HUD 显示 "DRS warming up...".

4. **DRS 与 TAAU 关系?** — DRS 仅在 `taauEnabled=true` 时生效 (renderScale<1.0 才有意义). DRS 调用 `SetRenderScale` 内部仅在 `taauEnabled` 时 propagate 到 HDR; 否则只更新 `g.renderScale` 字段.

5. **F.1.1 autoMipBias 联动?** — DRS 调用 `SetRenderScale` 触发 `applyTAAUChange_()` → `updateMipBias_()` 自动同步, 无需额外 hook.

---

## 6. 边界确认

| 项 | 是否在 F.1.4 范围 |
|----|------------------|
| 6 Lua API + Smoke | ✅ in |
| Per-instance DRS state | ✅ in |
| 滑动窗口 + Hysteresis + Cooldown | ✅ in |
| Multi-platform CI 验证 | ✅ in (smoke 跑 6 平台) |
| 与 manual SetRenderScale 透明融合 | ✅ in |
| 真实 GPU profiler 整合 | ❌ out (F.1.5+) |
| 跨 instance 联动降级 | ❌ out (F.1.6+) |
| 神经网络/ML 预测 | ❌ out |

---

## 7. 验收标准 (草稿)

- [ ] 6 个新 Lua API 全部存在 (type=function)
- [ ] 默认值: dynamicEnabled=false, dynamicTarget=60, windowSize=30, cooldownFrames=60
- [ ] 类型/边界错误处理: 非数 target → nil + err; target ≤ 0 → 关 DRS
- [ ] 滑动窗口 30 帧均值正确
- [ ] Cooldown 限频正确 (连续 N 帧不调)
- [ ] Hysteresis 正确 (升降阈值不同)
- [ ] Multi-instance 隔离 (instance 0 启 DRS, instance 1 不受影响)
- [ ] 与 manual SetRenderScale 融合 (手调后 DRS 接管, 不抖动)
- [ ] demo_taau HUD 显示 DRS 状态 (target/avgFps/scale/adjustments)
- [ ] CI 6 平台全绿
- [ ] Zero-regression: 4 demo 启动 (demo_ssr / demo_taa_split2 / demo_taau / demo_multi_hdr_pip)

---

## 8. 文档版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — 5 决策点 + 默认决策 + 验收标准草稿 |
