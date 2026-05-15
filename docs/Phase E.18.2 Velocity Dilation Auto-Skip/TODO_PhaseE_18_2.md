# Phase E.18.2 Velocity Dilation Auto-Skip — TODO

> 6A 工作流 · 阶段 6 · Assess · 后续待办与运维清单
> 基线：Phase E.18.1 commit `cdef7b2`

---

## 1. 必做项（must-have）

无。Phase E.18.2 本身功能完整，无阻塞性 TODO。

---

## 2. 推荐项（recommend）

| 项 | 说明 | 估时 | 优先级 |
|----|------|------|--------|
| 真机 GPU profile 实测 | 用 Tracy / RGP 测 SSR-only 单消费者场景下 autoSkip 实际收益（仅 ~0.01ms @ 1080p，可能在测量误差范围内）| 30min | 低 |
| HUD 改进 | demo_ssr 添加实时 fetch/px 估算显示，方便用户直观判断 autoSkip 是否合理 | 30min | 低 |

---

## 3. 未来候选（future candidates）

### Phase E.18.3 — dilation pass 接入 TAA 主管线（继承）

- 未来 TAA 主管线也是 velocity dilation consumer
- autoSkip 规则需扩展为 `consumers = SSR.Temporal + MB + TAA`
- 跳过条件需重新分析（TAA 通常 4-8 sample，介于 SSR Temporal 和 Motion Blur 之间）
- 依赖 Phase F.0 TAA 主管线先落地

### Phase E.18.4 — 细粒度 toggle（继承）

- `HDR.SetVelocityDilation(bool)` 拆分为 `SSR.SetVelocityDilation` + `MotionBlur.SetVelocityDilation`
- 当前全局共用一个开关
- autoSkip 在细粒度场景下更精准

### Phase E.18.x — adaptive autoSkip（新提案）

- 根据每帧渲染耗时自动开/关 autoSkip
- 性能临界场景（接近预算）自动切到 autoSkip ON
- 风险：自动行为可能不符用户预期；维持手动开关更可控
- 优先级：低（autoSkip 收益小，自动调度复杂度不值得）

---

## 4. 用户引导

### 何时开 autoSkip

```lua
-- 1. SSR-only 单消费者场景（如室内反射 demo）
Light.Graphics.HDR.Enable(1280, 720)
Light.Graphics.SSR.Enable(1280, 720)
Light.Graphics.SSR.SetTemporalEnabled(true)
-- Motion Blur 未启用
Light.Graphics.HDR.SetVelocityDilationAutoSkip(true)  -- 节省 1 fetch/px
```

### 何时关 autoSkip（默认状态）

```lua
-- 1. 预计会启用 Motion Blur（即使当前未启）
Light.Graphics.HDR.SetVelocityDilationAutoSkip(false)  -- 默认值

-- 2. 多消费者通用场景（autoSkip 不会跳过，但保持简单状态）

-- 3. 调试期 dilation pass 行为（autoSkip 引入条件分支可能干扰）
```

### 调试 autoSkip 行为

```
-- log 输出关键字 (Phase E.18.2 once-log)
"HDRRenderer: Phase E.18.2 dilation pass auto-skipped (SSR-only, consumer fallback inline 9-tap)"
"HDRRenderer: Phase E.18.2 dilation pass active (multi-consumer or non-SSR-only)"

-- 状态可通过 HUD 实时查看 (demo_ssr 已显示 autoSkip=ON/OFF)
-- '\' 键切换 autoSkip ON/OFF
```

---

## 5. 资源 / 配置依赖

### 无新增

- 不需要新增贴图 / shader 资源
- 不需要修改 CI 配置
- 不需要修改 CMake / 依赖

---

## 6. CI 回填（已完成）

| 字段 | 值 |
|------|---|
| GitHub Run ID | [`25902219897`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25902219897) |
| Commit hash | `b726026` |
| 6/6 平台状态 | ✅ all success |
| Total duration | **6 min** |
| Date | 2026-05-15 05:35 UTC → 05:40 UTC |

已同步更新：
- ✅ `ACCEPTANCE_PhaseE_18_2.md` 第 7 章
- ✅ `FINAL_PhaseE_18_2.md` 第 8 章
- ⏳ `docs/Phase E.18 Independent Velocity Dilation Pass/TODO_PhaseE_18.md` 标记 E.18.2 已完成（本提交）

---

## 7. 总结

Phase E.18.2 实施完整，无阻塞性遗留。收益较小（仅 SSR-only 场景省 1 fetch/px），但解决了 Phase E.18 的"单消费者 SSR 反而亏 fetch"的边缘问题，使 dilation pass 在所有场景下都至少不亏。

Phase E 系列后处理管线优化在 E.18.2 后呈"完成态"：
- **VRAM 三层优化**：E.14 RG8 + E.17 motion blur halfRes + E.18.1 dilation halfRes
- **性能三层优化**：E.18 共享 dilation pass + E.18.1 dilation halfRes + E.18.2 single-consumer skip
- **API 数累计**：HDR 子表 20 functions（E.18.1 +2、E.18.2 +2）

下一步建议候选：Phase E.19 (SSR Temporal camera-only velocity) 或 Phase F.0 (TAA 主管线)。
