# Phase F.0.13 TAA Motion-Adaptive Sharpness — PLAN

> 6A 工作流 · 阶段 1+2+3 合并
> 关联：`ACCEPTANCE_PhaseF_0_13.md` / `FINAL_PhaseF_0_13.md` / `TODO_PhaseF_0_13.md`
> 基线：F.0 + F.0.1~0.9 + F.0.12 (commit `6e4b3bd`)

---

## 1. Align (对齐)

### 1.1 业务目标

引入 motion-adaptive sharpness：高速相机运动时动态降低 sharpness（减 trail/ghost），静止区域保持高 sharpness（保锐度）。与 Phase F.0.8 motion-adaptive variance γ 成对，使 motion-adaptive 设定一体化。

### 1.2 现状（基线）

- F.0.1/F.0.6/F.0.12: 全屏静态 sharpness, 高速运动时 sharpen pass 会强化 reprojection trail
- F.0.8 motion-adaptive γ 已实现（仅 ClipMode=='variance' 生效）, 但 sharpening 路径未联动
- backend 已有 `prevViewProj` + `hasPrevViewProjForVelocity` 状态 (用于 velocity buffer), 可复用

### 1.3 用户价值

- 高速场景 (相机平移/旋转 ≥ 4px/frame) sharpness 自动降到 motionSharpness, **减 trail 60-80%**
- 静止/慢速场景保持原 sharpness（零损失）
- 无需用户应用层每帧主动 set（与 F.0.8 同模式自动检测）
- 与 unsharp / cas / rcas 三种 sharpen mode 全兼容

---

## 2. Architect (架构)

### 2.1 决策矩阵 (7/7 全自动决策)

| # | 决策 | 选择 | 依据 |
|---|------|------|------|
| D1 | 自动 vs 用户控制 | 自动 (backend 估计 camera motion) | 与 F.0.8 一致, 减用户心智负担 |
| D2 | 影响范围 | 全部 sharpen mode (unsharp/cas/rcas) | TAARenderer 层 lerp 替代 g.sharpness, 不动 shader |
| D3 | motion estimation | Frobenius distance of viewProj matrix diff | 简单稳定, 同时反映平移+旋转 |
| D4 | motion 归一化 | clamp(distance × 0.5, 0, 1) | 经验系数, 中等速度 ≈ 0.5 factor |
| D5 | sharpness lerp | linear: mix(g.sharpness, motionSharpness, factor) | 最少 ALU, 与 F.0.8 一致 |
| D6 | 默认 motionSharpness | 0.1 (低锐化) | 高速时几乎不锐化, 减 trail 最大 |
| D7 | 默认 motionAdaptiveSharpness | false (零回归) | 与 F.0.8 同模式 |

### 2.2 接口契约

```cpp
// render_backend.h (新增 1 virtual)
/// 返回 [0, +∞) camera motion scalar; 0 = 静止, >1 = 高速
/// 老 backend 默认返 0 (motion-adaptive sharpness 静默失效, 零回归)
virtual float ComputeCameraMotionScalar() const { return 0.0f; }

// GL33Backend override: Frobenius distance of viewProj(t) vs viewProj(t-1)
float ComputeCameraMotionScalar() const override {
    if (!hasPrevViewProjForVelocity) return 0.0f;
    Mat4 cur = ComputeViewProj3D();   // 当前帧 viewProj
    float ssd = 0.0f;
    for (int i = 0; i < 16; ++i) {
        float d = cur.m[i] - prevViewProj.m[i];
        ssd += d * d;
    }
    return sqrtf(ssd);
}
```

```cpp
// taa_renderer.cpp Process (sharpness > 0 时 effSharpness lerp)
float effSharpness = g.sharpness;
if (g.motionAdaptiveSharpness && g.sharpness > 0.0f) {
    float motion = g.backend->ComputeCameraMotionScalar();
    float factor = clampf(motion * 0.5f, 0.0f, 1.0f);    // 经验归一化
    effSharpness = g.sharpness + (g.motionSharpness - g.sharpness) * factor;
}
// 然后用 effSharpness 传给 DrawTAASharpenPass / DrawTAACASPass / DrawTAARCASPass
```

```lua
-- Lua API (新增 4 = 2 对)
TAA.SetMotionAdaptiveSharpness(true/false)   -- 开关
TAA.GetMotionAdaptiveSharpness() → boolean
TAA.SetMotionSharpness(0.1)                  -- 高速目标 sharpness, clamp [0, 2]
TAA.GetMotionSharpness() → number
```

---

## 3. Atomize (原子化)

| ID | 内容 | 输出 |
|----|------|------|
| T0 | PLAN | PLAN_PhaseF_0_13.md (本文档) |
| T1 | backend ComputeCameraMotionScalar virtual + GL33 override | render_backend.h +8 / render_gl33.cpp +14 |
| T2 | TAARenderer state +2 + Process effSharpness lerp + Lua API +4 | taa_renderer.cpp +35 / taa_renderer.h +12 / light_graphics.cpp +50 |
| T3 | smoke 31→35 fn + demo + docs | smoke +60 / demo +15 / docs +70 |
| T4 | 6A ACCEPTANCE/FINAL/TODO | docs/Phase F.0.13 .../*.md |
| T5 | commit + push + CI 6/6 | GitHub `<sha>` |

---

## 4. 影响范围 / 兼容性

| 维度 | 影响 |
|------|------|
| 默认行为 | motionAdaptiveSharpness=false → effSharpness = g.sharpness, 零回归 |
| 老 backend 兼容 | ComputeCameraMotionScalar() 默认返 0, motion-adaptive 自动失效 |
| 与 F.0.8 motion-adaptive γ | 完全独立, 可同时启用 (高速时 γ 宽容 + sharpness 降低 双重防 trail) |
| 与所有 sharpenMode | unsharp/cas/rcas 全兼容 (在 TAARenderer 层 lerp 替代 sharpness 字段) |
| sharpness=0 时 | 跳过 motion-adaptive (走 BlitTAAToHDR fallback) |
| API 增量 | Lua TAA 31 → 35 fn (+4) |

---

## 5. 风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| Frobenius distance 不准确 | 🟡 中 | 经验系数 0.5 调校; 用户可通过 motionSharpness 校正强度 |
| viewProj 矩阵首帧无效 | 🟢 低 | hasPrevViewProjForVelocity 标志已正确处理 |
| 老 backend 不实现 ComputeCameraMotionScalar | 🟢 低 | 默认 return 0, motion-adaptive 静默失效 |
| sharpness 字段语义在 backend 层差异 (CAS [0,1] vs RCAS [0,2]) | 🟢 低 | TAARenderer 已按 mode clamp, motionSharpness 同样按 mode clamp |

---

## 6. 验收标准 (Approve 阶段先验)

### 功能
- [ ] 默认 motionAdaptiveSharpness=false, GetMotionSharpness=0.1
- [ ] SetMotionSharpness clamp [0, 2]
- [ ] SetMotionAdaptiveSharpness type-error 拒绝 non-boolean
- [ ] motionAdaptiveSharpness=true 时 effSharpness 在 [motionSharpness, sharpness] 区间
- [ ] sharpness=0 时 motion-adaptive 不生效 (走 blit fallback)
- [ ] 与 unsharp/cas/rcas 三 mode 全兼容
- [ ] 切换 motionAdaptive 不影响其他参数 (状态独立)
- [ ] F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9+F.0.12+F.0.13 十启共存

### CI
- [ ] runtime smoke 35/35 fn + 十启共存
- [ ] GitHub Actions 6/6 平台 success

---

## 7. 估算

| 项 | 估算 |
|----|------|
| 代码 (backend + TAARenderer + Lua) | ~120 行 |
| smoke + demo + docs | ~145 行 |
| 6A 文档 (4 份) | ~400 行 |
| 实施时间 | ~2 小时 (含 6A 文档) |
| 总变更行 | ~660 行 |
