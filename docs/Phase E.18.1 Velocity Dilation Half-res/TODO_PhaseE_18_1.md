# Phase E.18.1 Velocity Dilation Half-Resolution — TODO

> 6A 工作流 · 阶段 6 · Assess · 后续待办与运维清单
> 基线：Phase E.18 commit `8b7c25b`

---

## 1. 必做项（must-have）

无。Phase E.18.1 本身功能完整，无阻塞性 TODO。

---

## 2. 推荐项（recommend）

| 项 | 说明 | 估时 | 优先级 |
|----|------|------|--------|
| 真机 GPU profile 实测 | 用 Tracy / RGP / RenderDoc 测 dilation pass halfRes 实际收益，回填 ACCEPTANCE/FINAL 性能预算章节 | 1h | 中 |
| 视觉对比截图 | demo_ssr 按 `]` 切 dilation halfRes ON/OFF，截图存档（4K + 1080p 各一组） | 30min | 低 |
| CHANGELOG 更新 | `docs/CHANGELOG.md` 加 Phase E.18.1 条目（如果项目有此文档） | 5min | 低 |

---

## 3. 未来候选（future candidates）

### Phase E.18.1.1 — dilation halfRes 自动判定

- **背景**：用户需手动决定 halfRes 是否开启，对新手不友好
- **方案**：根据分辨率自动决定（4K 默认开、1080p 中性、720p 默认关）
- **API**：新增 `HDR.SetVelocityDilationHalfResAuto(bool)`（OFF/AUTO/ON 三态？）
- **风险**：自动行为可能不符用户预期；维持手动开关更可控

### Phase E.18.2 — 运行时智能 single-consumer skip（继承 Phase E.18 TODO）

- 仅 SSR Temporal 或仅 Motion Blur(N≤4) 启用时自动跳过 dilation pass（inline 9-tap 反而更快）
- 当前需用户手动 `SetVelocityDilation(false)`

### Phase E.18.3 — dilation pass 接入 TAA 主管线（继承）

- 未来 TAA 主管线也是 velocity dilation 消费者
- dilation pass 输出可被 TAA reprojection 直接复用

### Phase E.18.4 — 细粒度 toggle（继承）

- 拆分 `HDR.SetVelocityDilation(bool)` 为 `SSR.SetVelocityDilation` + `MotionBlur.SetVelocityDilation`
- 当前全局共用一个开关

---

## 4. 用户引导

### 何时开 dilation halfRes

```lua
-- 1. 移动端 / VR：强烈推荐
Light.Graphics.HDR.SetVelocityDilationHalfRes(true)

-- 2. 4K + 多 consumer (SSR + Motion Blur N=8)：最大收益场景
Light.Graphics.HDR.Enable(3840, 2160)
Light.Graphics.HDR.SetVelocityDilationHalfRes(true)  -- VRAM -12MB, dilation pass +4x

-- 3. 1440p QHD：推荐开
Light.Graphics.HDR.SetVelocityDilationHalfRes(true)
```

### 何时关 dilation halfRes（默认状态）

```lua
-- 1. 720p HD：VRAM 已不大，不需要 (默认行为)
-- 不调用 SetVelocityDilationHalfRes 即可

-- 2. 极宽窄物体高速运动场景（如赛车游戏天线、电线）
Light.Graphics.HDR.SetVelocityDilationHalfRes(false)
```

### 调试 dilation pass

```
-- log 输出关键字 (Phase E.18.1 增加 halfRes 标记)
"HDRRenderer: Phase E.18 dilated combined velocity RT created (storage=960x540, logical=1920x1080, halfRes=ON, ...)"
"HDRRenderer: Phase E.18.1 rebuilt dilated combined velocity RT (storage=960x540, halfRes=ON)"

-- 失败时:
"GL33: Phase E.18 Velocity Dilate FBO incomplete (status=0xNNNN), 960x540 (logical 1920x1080)"
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
| GitHub Run ID | [`25901596673`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25901596673) |
| Commit hash | `254984f` |
| 6/6 平台状态 | ✅ all success |
| Total duration | **6 min** |
| Date | 2026-05-15 05:14 UTC → 05:20 UTC |

已同步更新：
- ✅ `ACCEPTANCE_PhaseE_18_1.md` 第 7 章
- ✅ `FINAL_PhaseE_18_1.md` 第 8 章
- ⏳ `docs/Phase E.18 Independent Velocity Dilation Pass/TODO_PhaseE_18.md` 标记 E.18.1 已完成（本提交）

---

## 7. 总结

Phase E.18.1 实施完整，无阻塞性遗留。推荐项以"实测验证"为主，无强制性 TODO。

Phase E 系列后处理管线优化在 E.18.1 后呈"完成态"：
- VRAM 优化：E.14 RG8 + E.17 motion blur halfRes + E.18.1 dilation halfRes 三层叠加
- 性能优化：E.18 共享 dilation pass + E.18.1 dilation halfRes 双重减负

下一步建议候选：Phase E.19 (SSR Temporal camera-only velocity) 或 Phase F.0 (TAA 主管线)。
