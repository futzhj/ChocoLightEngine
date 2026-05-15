# Phase F.0.8 TAA Motion-Adaptive γ — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_8.md` / `FINAL_PhaseF_0_8.md` / `TODO_PhaseF_0_8.md`
> 基线：F.0 + F.0.1/0.2/0.3/0.4/0.5/0.6/0.7 (commit `7b14f46`)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| GLES3 + GL33 shader | 加 uMotionGamma + uMotionAdaptiveGamma + dynGamma lerp 路径 | ✅ |
| Backend struct + Init + Shutdown | locTAA_MotionGamma + locTAA_MotionAdaptiveGamma + 一次性绑 | ✅ |
| DrawTAAPass impl | 多 2 参数 motionGamma + motionAdaptiveGamma | ✅ |
| render_backend.h virtual | 默认参数 motionGamma=1.5f, motionAdaptiveGamma=0 (向后兼容) | ✅ |
| TAARenderer state | motionGamma=1.5f / motionAdaptiveGamma=false 默认 | ✅ |
| TAARenderer Process | 调用 DrawTAAPass 多传 2 参数 | ✅ |
| TAARenderer Set/Get | Set/GetMotionGamma + Set/GetMotionAdaptive (clamp [0, 4]) | ✅ |
| Lua API +4 | l_TAA_SetMotionGamma + Get + SetMotionAdaptive + Get + taa_funcs[] 25→29 | ✅ |
| smoke (`taa.lua`) | surface 29 fn + 默认 / round-trip / clamp / type-error / 状态独立 / 七启共存 (10 PASS) | ✅ |
| demo (`demo_ssr/main.lua`) | Q 键 toggle + HUD variance(sγ=N mγ=N) + Keys help 加 Q=TAAMotionAdapt | ✅ |
| API doc (`Light_Graphics.md`) | 速查表 25→29 行 + Set/Get*Motion* 完整文档段 (~80 行) | ✅ |
| Lua 语法验证 | `lightc -p taa.lua && lightc -p demo_ssr/main.lua` Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（8/8）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 velocity 来源 = 复用 shader 已 sample | shader L2154 `vec2 velocity = SampleVelocity(vUV)` 已存在, F.0.8 用 `length(velocity)` | ✅ |
| D2 γ 字段策略 = varianceGamma=static + motionGamma=motion | State +1 motionGamma 字段, 老 varianceGamma 不变 | ✅ |
| D3 motion factor = clamp(velLen/(4*texel.x), 0, 1) | shader 内 `motionFactor = clamp(velLen / (uTexel.x * 4.0), 0.0, 1.0)` | ✅ |
| D4 lerp 曲线 = linear (`mix`) | shader 内 `mix(uVarianceGamma, uMotionGamma, motionFactor)` | ✅ |
| D5 默认 motionAdaptive = false | State `motionAdaptiveGamma = false` 默认 | ✅ |
| D6 默认 motionGamma = 1.5 | State `motionGamma = 1.5f` 默认 (UE5 推荐) | ✅ |
| D7 motionGamma clamp [0, 4] | `clampf(g_motion, 0.0f, 4.0f)` 与 varianceGamma 同模式 | ✅ |
| D8 API 命名 = Set/GetMotionGamma + Set/GetMotionAdaptive | Lua taa_funcs[] 4 项 | ✅ |

---

## 3. 验收清单

### 功能
- [x] motion-adaptive 默认 OFF (零回归)
- [x] motionGamma 默认 1.5, clamp [0, 4]
- [x] SetMotionAdaptive(bool) round-trip
- [x] SetMotionAdaptive type-error (string / number) raise
- [x] SetMotionGamma(γ) round-trip (2.5 / 0.0)
- [x] SetMotionGamma clamp [-1 → 0] / [10 → 4]
- [x] SetMotionGamma type-error (string) raise
- [x] motion-adaptive ON + clipMode=variance shader 走 lerp 分支
- [x] motion-adaptive ON + clipMode=rgb/ycocg shader 静默不读 (uniform 上传但不影响)
- [x] F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8 七启共存
- [x] 状态独立 (切换不影响 alpha/clipMode/sharpness/varianceGamma/halfRes/sharpenMode)

### CI (待回填)
- [ ] GitHub Actions 6/6 平台 success
- [ ] runtime smoke 29/29 fn + 10 个 F.0.8 PASS + 七启共存

---

## 4. CI 状态（待回填）

| 平台 | 状态 |
|------|------|
| build-windows | ⏳ |
| build-linux/macos/android/ios/web | ⏳ |

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`
