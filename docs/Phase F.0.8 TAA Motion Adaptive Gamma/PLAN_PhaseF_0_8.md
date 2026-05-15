# Phase F.0.8 TAA Motion-Adaptive Variance γ — PLAN

> 6A 工作流 · 阶段 1-3 (Align + Architect + Atomize) 合并精简
> 关联：`ACCEPTANCE_PhaseF_0_8.md` / `FINAL_PhaseF_0_8.md` / `TODO_PhaseF_0_8.md`
> 基线：F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.7 (commit `7b14f46`)
> 目标工作量：3h

---

## 1. 背景与目标

Phase F.0.3 已实现 variance clipping（`clip = mean ± γσ`），但 γ 是**全屏静态值**（默认 1.0）。这导致权衡两难：

- **γ 小（如 0.5）**：clip 严格，静止区域 ghost 完全消除；但快速运动时 trail 增多
- **γ 大（如 2.0）**：clip 宽容，运动场景流畅；但静止区域可能出现细微 ghost

**Phase F.0.8 目标**：引入 UE5 高级形式的 **motion-adaptive variance γ**，根据每像素 velocity 长度动态在 `gammaStatic` 与 `gammaMotion` 之间 lerp：

- 静止区域 (`|vel| ≈ 0`) → `gammaStatic` (严格, 防 ghost)
- 高速运动 (`|vel| > threshold`) → `gammaMotion` (宽容, 防 trail)
- 中间线性插值

---

## 2. 任务范围与边界

### 包含

- shader 改造：FS_TAA (GLES3 + GL33) variance clip 路径用 `dynGamma` 替代 `uVarianceGamma`
- 新增 2 个 uniform: `uMotionGamma` (float) + `uMotionAdaptiveGamma` (int 0/1)
- backend `DrawTAAPass` 接口扩展加 2 个默认参数（向后兼容）
- TAARenderer state 加 `motionGamma` (float, 1.5 默认) + `motionAdaptiveGamma` (bool, false 默认)
- Lua API +4 (Set/GetMotionGamma + Set/GetMotionAdaptive)
- smoke 25 → 29 fn + Phase F.0.8 测试段
- demo Q 键 toggle motionAdaptive + HUD 字段
- API docs 速查表 + Set/GetMotion* 完整文档段
- 6A 文档 4 件套

### 不包含

- ❌ smoothstep / quadratic / cubic 曲线（仅 linear lerp，最简）
- ❌ velocity threshold 用户可调（固定 4 * texel UV，简化 API）
- ❌ 影响其他 clipMode（"rgb" / "ycocg" 不变；仅 "variance" 受 motionAdaptive 影响）
- ❌ FS_TAA 其他逻辑改动

---

## 3. 决策矩阵（8/8 全自动决策）

| # | 决策点 | 选择 | 理由 |
|---|--------|------|------|
| **D1** | velocity 来源 | **复用 shader 已 sample 的 velocity**（reproject 阶段已 fetch） | DRY；零额外 fetch；shader 仅加 length 计算 |
| **D2** | γ 字段策略 | **复用 `varianceGamma` 作 static γ + 新增 `motionGamma`** | 老 API 兼容（F.0.3 用户视角不变）；新增字段语义清晰 |
| **D3** | motion factor 公式 | `clamp(length(velocity) / threshold, 0, 1)`，**`threshold = 4 * texel.x`** | 4 像素 UV 长度 = 屏幕 sub-pixel 到 mid-range 临界点；UE5 类似 |
| **D4** | lerp 曲线 | **linear (`mix`)** | 最少 ALU；smoothstep 收益小但 +3 ALU |
| **D5** | 默认 `motionAdaptive` | **`false`** | 零回归；与 F.0.3 单 γ 行为一致 |
| **D6** | 默认 `motionGamma` | **`1.5`** | UE5 推荐；比 staticγ=1.0 稍宽容 |
| **D7** | `motionGamma` 范围 | **`clamp [0, 4]`** | 与 `varianceGamma` 同模式；上界 4 防过度宽容失去 clip 意义 |
| **D8** | API 命名 | `SetMotionGamma` / `SetMotionAdaptive` | 简洁；`Adaptive` 后缀避免与 `Gamma` 名重复 |

---

## 4. 算法设计

### 4.1 shader 改造（仅 variance 路径）

```glsl
// 现有 (F.0.3)：
vec3 mn = m1 - uVarianceGamma * sigma;
vec3 mx = m1 + uVarianceGamma * sigma;

// 新 (F.0.8)：
float dynGamma = uVarianceGamma;
if (uMotionAdaptiveGamma == 1) {
    // velocity 已在前面 reproject 阶段 sample (uVelocityTex 同 vel 变量)
    float velLen = length(vel);          // UV 单位长度
    // threshold = 4 px UV 长度: motionFactor 0=静止, 1=>=4px/帧
    float motionFactor = clamp(velLen / (uTexel.x * 4.0), 0.0, 1.0);
    dynGamma = mix(uVarianceGamma, uMotionGamma, motionFactor);
}
vec3 mn = m1 - dynGamma * sigma;
vec3 mx = m1 + dynGamma * sigma;
```

**性能**：仅 `+ 4 ALU` (length / clamp / mix)，仅在 `clipMode==2 && motionAdaptiveGamma==1` 时执行。

### 4.2 motion-adaptive 行为

| velocity 长度 | motionFactor | dynGamma (静=1.0, motion=1.5) | 效果 |
|--------------|--------------|------------------------------|------|
| 0 px | 0.0 | 1.0 (= static) | 严格 clip, 防 ghost |
| 1 px | 0.25 | 1.125 | 略宽容 |
| 2 px | 0.5 | 1.25 | 中等 |
| 4+ px | 1.0 | 1.5 (= motion) | 宽容 clip, 防 trail |

### 4.3 与现有 Phase 协同

| Phase | 行为 | F.0.8 影响 |
|-------|------|-----------|
| F.0 / F.0.1 / F.0.2 / F.0.4 / F.0.5 / F.0.6 | 不变 | 0 |
| F.0.3 (variance, motionAdaptive=false) | 单 γ static | 完全等价（默认零回归）|
| F.0.3 (variance, motionAdaptive=true) | 单 γ static | dynGamma lerp 取代单 γ |
| clipMode!=variance | 不影响 | motionGamma uniform 上传但 shader 内不读 |

---

## 5. 状态机与接口契约

### 5.1 TAARenderer state 扩展

```cpp
struct State {
    // ... 现有字段
    float varianceGamma       = 1.0f;   // Phase F.0.3 (static γ)
    float motionGamma         = 1.5f;   // Phase F.0.8: motion-adaptive 高速 γ
    bool  motionAdaptiveGamma = false;  // Phase F.0.8: 默认关 (零回归)
};
```

### 5.2 Lua API（新增 4 个）

```lua
-- Phase F.0.8 — 静止 / 运动 γ 双值 + 开关
TAA.SetMotionGamma(γ)              -- clamp [0, 4], 默认 1.5
TAA.GetMotionGamma() → γ
TAA.SetMotionAdaptive(bool)        -- 默认 false; 开关 motion-adaptive γ
TAA.GetMotionAdaptive() → bool
```

错误处理：
- `SetMotionGamma`: 非 number 用 `luaL_checknumber` raise
- `SetMotionAdaptive`: 非 boolean 用 `luaL_checktype TBOOLEAN` raise（与 SetHalfResHistory 同模式）

### 5.3 Backend `DrawTAAPass` 接口扩展（默认参数向后兼容）

```cpp
virtual void DrawTAAPass(uint32_t curHdrTex, uint32_t historyTex, uint32_t velocityTex,
                         uint32_t historyFbo, int w, int h,
                         float blendAlpha, int neighborhoodClip,
                         int hasHistory, int velocityDilation,
                         float velocityScale, int velocityFormat,
                         int antiFlicker, int clipMode,
                         float varianceGamma,
                         float motionGamma = 1.5f,           // Phase F.0.8 新增
                         int motionAdaptiveGamma = 0) {}     // Phase F.0.8 新增
```

老 caller 自动 motionGamma=1.5 + motionAdaptive=0（行为等价 F.0.3）。

---

## 6. 实施顺序

### T0 (15 min) — PLAN 文档（本文件）

### T1 (45 min) — Backend (shader + uniform)

- L2114 / L2854: GLES3 + GL33 shader 加 `uMotionGamma` + `uMotionAdaptiveGamma` uniform
- L2184 / L2921: 用 `dynGamma` 替代 `uVarianceGamma`（含 length(vel) lerp 公式）
- backend struct: `locTAA_MotionGamma` + `locTAA_MotionAdaptiveGamma`
- Init: glGetUniformLocation
- DrawTAAPass: 多 2 个参数 + glUniform1f / glUniform1i 上传
- render_backend.h: 默认参数 motionGamma=1.5 + motionAdaptiveGamma=0

### T2 (30 min) — TAARenderer

- header: SetMotionGamma / GetMotionGamma / SetMotionAdaptive / GetMotionAdaptive 声明
- impl: state +2 字段 + clamp [0, 4] + Process 传 2 个新参数

### T3 (60 min) — Lua + smoke + demo + docs

- light_graphics.cpp: 4 个 Lua 绑定 + taa_funcs +4 (25 → 29)
- taa.lua: 默认 1.5 / false / round-trip / clamp [0, 4] / type-error / 状态独立 / 七启共存
- demo_ssr: Q 键 toggle motionAdaptive + HUD `motionAdapt=ON/OFF` / `dynγ` 字段
- Light_Graphics.md: 速查表 25 → 29 + Set/Get*Motion* 完整文档段（含 motion factor 表 + 推荐场景）

### T4 (30 min) — 6A 文档 ACCEPTANCE / FINAL / TODO

### T5 (15 min) — commit + push + CI 6/6 监控

---

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 既然 length(vel) 不为 0 时 dynGamma 始终 > static γ，静止区域 ghost 风险其实变大？ | 轻微 ghost | 反 — `motionFactor=0 时 mix 返 static γ`，与 F.0.3 完全相同；ghost 风险等同 F.0.3 |
| velocity 字段精度问题（RG8 量化误差导致 motionFactor 抖动）| 误判 motion factor | RG8 误差 ~0.01px，远小于 4px threshold；不影响 |
| motion 阈值 4 px 在不同分辨率下视觉不一致？ | 4K 下 4px 显得"慢" | uTexel 已经是归一化 UV 单位，4 * texel.x 在不同分辨率自动缩放；OK |
| 用户切 clipMode 到 "rgb"/"ycocg" 时 motion-adaptive 应失效 | 期望明确 | shader 内 `if uClipMode==2` 包裹；非 variance 模式 motionGamma 静默不影响 |

---

## 8. 验收标准

### 功能
- [ ] motion-adaptive 默认 OFF（零回归）
- [ ] motionGamma 默认 1.5，clamp [0, 4]
- [ ] SetMotionAdaptive(bool) round-trip + type-error
- [ ] SetMotionGamma(γ) round-trip + clamp + type-error
- [ ] motion-adaptive ON + clipMode=variance 生效
- [ ] motion-adaptive ON + clipMode=rgb/ycocg 静默 no-op
- [ ] F.0.8 + F.0.1/2/3/4/5/6 七启共存
- [ ] 状态独立（切换不影响其他参数）

### 文档
- [ ] PLAN/ACCEPTANCE/FINAL/TODO 4 文档齐
- [ ] Light_Graphics.md 速查表 25 → 29 + Set/GetMotion* 完整文档段
- [ ] demo_ssr Q 键 + HUD 字段

### CI
- [ ] `lightc -p taa.lua` Exit 0
- [ ] GitHub Actions 6/6 平台 success
- [ ] runtime smoke 29/29 fn + 七启共存 PASS
