# Phase E.18 Independent Velocity Dilation Pass — TODO

> 6A 工作流 · 阶段 6 · Assess · 后续待办与运维清单
> 基线：Phase E.17 commit `f8d7e41`

---

## 1. 必做项（must-have）

无。Phase E.18 本身功能完整，无阻塞性 TODO。

---

## 2. 推荐项（recommend）

| 项 | 说明 | 估时 | 优先级 |
|----|------|------|--------|
| GPU 性能实测 | 用 Tracy / RenderDoc / RGP 测真机收益，回填 ACCEPTANCE/FINAL 性能预算章节 | 1h | 中 |
| 多 consumer 视觉对比截图 | demo_ssr 切 dilation ON/OFF + SSR/MB 联动，截图存档 | 30min | 低 |
| ChangeLog 更新 | `docs/CHANGELOG.md` 加 Phase E.18 条目（如果项目有此文档） | 5min | 低 |

---

## 3. 未来候选（future candidates）

### Phase E.18.1 — dilation pass 半分辨率（VRAM/perf 优化）— ✅ 已完成

- ✅ Phase E.18.1 已完成（CI run [25901596673](https://github.com/futzhj/ChocoLightEngine/actions/runs/25901596673) 6/6 green，commit `254984f`）
- 实施回顾：backend 接口 `CreateVelocityDilateRT` / `DrawVelocityDilate` 扩 sw/sh 参数；HDRRenderer 增 `dilationHalfRes` state + `ComputeDilationStorageSize`/`ReleaseDilationRT`/`RebuildDilationRT` 内部辅助；Lua API `HDR.SetVelocityDilationHalfRes` / `GetVelocityDilationHalfRes` 默认 false
- 性能：VRAM -75% (1080p dilated RT 16MB→4MB)、dilation pass 自身 perf +4× (fragment count -75%)
- 视觉：max-filter 邻域物理覆盖由 3 raw px 扩到 6 raw px (自动鲁棒)
- 详见 `@e:/jinyiNew/Light/docs/Phase E.18.1 Velocity Dilation Half-res/FINAL_PhaseE_18_1.md`

**原候选方案**（保留参考）：
- **方案**：dilatedVelocityTex 改半分辨率 `((w+1)/2, (h+1)/2)`
- **收益**：VRAM -75%、dilation pass 性能 +4×
- **风险**：邻域物理覆盖减半（uTexel 翻倍），可能 1-px 错配伪影回升；consumer 单点采时硬件 bilinear 上采可能引入轻微模糊
- **建议**：先实测 GPU profile，若 dilation pass 占比 >5% 再考虑

### Phase E.18.2 — 运行时智能判断 single-consumer — ✅ 已完成

- ✅ Phase E.18.2 已完成（CI run [25902219897](https://github.com/futzhj/ChocoLightEngine/actions/runs/25902219897) 6/6 green，commit `b726026`）
- **修订决策**：基于 fetch/px 经济性分析，跳过规则精化为 `autoSkip && SSR Temporal && !MB`（即仅 SSR-only 单消费者场景跳过；MB-only 时 inline 9-tap × N samples 反而亏 55 fetch，故不跳过）
- 实施回顾：HDRRenderer 增 `dilationAutoSkip` state + `lastDilationActiveLog` once-log 追踪；EndScene 内 shouldRun 闸口；Lua API `HDR.SetVelocityDilationAutoSkip` / `GetVelocityDilationAutoSkip` 默认 false
- 性能：仅 SSR-only 场景省 1 fetch/px（~0.01ms @ 1080p）；其他场景无效
- 详见 `@e:/jinyiNew/Light/docs/Phase E.18.2 Velocity Dilation Auto-Skip/FINAL_PhaseE_18_2.md`

**原候选方案**（保留参考）：
- **场景**：仅 SSR 或仅 MB 启用时，dilation pass 反而亏 1 fetch
- **方案**：HDR EndScene 内查询 `SSRRenderer::IsEnabled() + MotionBlurRenderer::IsEnabled()`，仅 ≥2 时启用 dilation pass
- **风险**：自动行为可能不符用户预期，需文档明确
- **替代**：维持当前手动开关（用户自行判断更可控）

### Phase E.18.3 — dilation pass 接入 TAA 主管线

- **背景**：TAA 主管线（如果未来引入）也是 velocity dilation 消费者
- **方案**：dilation pass 输出可被 TAA reprojection 复用
- **依赖**：TAA 主管线先落地

### Phase E.18.4 — 细粒度 dilation toggle

- **方案**：拆分 `HDR.SetVelocityDilation(bool)` 为 `SSR.SetVelocityDilation(bool)` + `MotionBlur.SetVelocityDilation(bool)`
- **风险**：API surface 膨胀，与"统一行为"原则矛盾
- **替代**：维持当前全局开关（更简单）

---

## 4. 用户引导（如用户需手动控制）

### 何时关 dilation pass

```lua
-- 仅启用 SSR Temporal、未启 Motion Blur 时
if not Light.Graphics.MotionBlur.IsEnabled() and Light.Graphics.SSR.GetTemporalEnabled() then
    -- 单 consumer 场景，inline 9-tap 略快
    Light.Graphics.HDR.SetVelocityDilation(false)
    -- 视觉差异：1-px 几何边缘错配可能可见（通常忽略）
end
```

### 何时开 dilation pass（默认行为）

```lua
-- SSR + Motion Blur 同开 → 自动走共享 dilation pass，~50% 节省
Light.Graphics.HDR.SetVelocityDilation(true)  -- 默认值
```

### 调试 dilation pass 失败

```
-- log 输出关键字
"GL33: Phase E.18 velocity dilation pass shader compiled"
"HDRRenderer: Phase E.18 dilated combined velocity RT created"

-- 失败时:
"GL33: Phase E.18 velocity dilation pass shader compile failed; fallback to inline 9-tap"
"GL33: Phase E.18 Velocity Dilate FBO incomplete (status=0xNNNN)"
```

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源 / Lua 模块
- 不需要修改 CI 配置
- 不需要修改 CMake / 依赖

---

## 6. CI 回填（已完成）

| 字段 | 值 |
|------|---|
| GitHub Run ID | [`25900086693`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25900086693) |
| Commit hash | `e894834` |
| 6/6 平台状态 | ✅ all success |
| Total duration | **7 min** |
| Date | 2026-05-15 04:22 UTC → 04:29 UTC |

已同步更新：
- ✅ `ACCEPTANCE_PhaseE_18.md` 第 7 章
- ✅ `FINAL_PhaseE_18.md` 第 8 章
- ⏳ `docs/Phase E.15 Motion Blur/TODO_PhaseE_15.md` 标记 E.18 已完成（本提交）
- ⏳ `docs/Phase E.17 Half-res Motion Blur/TODO_PhaseE_17.md` 标记 E.18 已完成（本提交）

---

## 7. 总结

Phase E.18 实施完整，无阻塞性遗留。推荐项以"实测验证"为主，无强制性 TODO。

后续工作以多 consumer 场景的实际 GPU profile 数据为指导，决定是否进入 E.18.1（半分辨率）。
