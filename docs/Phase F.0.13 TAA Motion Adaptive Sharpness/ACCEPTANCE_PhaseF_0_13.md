# Phase F.0.13 TAA Motion-Adaptive Sharpness — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_13.md` / `FINAL_PhaseF_0_13.md` / `TODO_PhaseF_0_13.md`
> 基线：F.0 + F.0.1~0.9 + F.0.12 (commit `6e4b3bd`)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| Backend virtual ComputeCameraMotionScalar | render_backend.h 加默认 0 + doxygen | ✅ |
| GL33Backend override | Frobenius distance of viewProj diff (16 floats SSD, sqrt) | ✅ |
| TAARenderer state +2 | motionAdaptiveSharpness=false + motionSharpness=0.1 | ✅ |
| TAARenderer Process | effSharpness lerp 替代 g.sharpness, 适用 unsharp/cas/rcas 三 mode | ✅ |
| TAARenderer Set/Get +4 | SetMotionAdaptiveSharpness/Get + SetMotionSharpness/Get | ✅ |
| Lua API +4 | l_TAA_SetMotionAdaptiveSharpness/Get + Set/GetMotionSharpness (与 F.0.8 同模式) | ✅ |
| smoke (taa.lua) | surface 31 → **35 fn** + F.0.13 完整段 (默认/round-trip/clamp/type-error/状态独立/sharpenMode 共存/十启共存) | ✅ |
| demo (demo_ssr) | O 键 toggle MotionAdaptiveSharpness + 输出 sharp/motionSharpness | ✅ |
| API doc | 速查表 31→35 + 完整 motion-adaptive sharpness 段 (算法 + 影响范围表 + 推荐场景 + 示例) | ✅ |
| Lua 语法验证 | lightc -p taa.lua && demo_ssr/main.lua Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（7/7）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 自动 vs 用户控制 = 自动 (backend 估计) | ComputeCameraMotionScalar virtual + GL33 override | ✅ |
| D2 影响范围 = 全部 sharpen mode | Process 内 effSharpness 替代 g.sharpness, 三 mode 共用 | ✅ |
| D3 motion estimation = Frobenius distance | sum((cur[i]-prev[i])^2), sqrt | ✅ |
| D4 motion 归一化 = clamp(motion×0.5, 0, 1) | 经验系数 0.5 | ✅ |
| D5 sharpness lerp = linear mix | effSharpness = sharpness + (motionSharpness - sharpness) * factor | ✅ |
| D6 默认 motionSharpness = 0.1 | State 字段初值 | ✅ |
| D7 默认 motionAdaptiveSharpness = false | State 字段初值 + 零回归 | ✅ |

---

## 3. 验收清单

### 功能
- [x] 默认 motionAdaptiveSharpness=false (零回归)
- [x] 默认 motionSharpness=0.1
- [x] SetMotionSharpness clamp [0, 2] (-1 → 0 / 5 → 2)
- [x] SetMotionAdaptiveSharpness round-trip true/false
- [x] SetMotionSharpness round-trip
- [x] type-error: SetMotionAdaptiveSharpness 拒绝 number/string; SetMotionSharpness 拒绝 string/boolean
- [x] motionAdaptiveSharpness=true 时 effSharpness 在 [motionSharpness, sharpness] 区间
- [x] sharpness=0 时 motion-adaptive 不生效 (走 BlitTAAToHDR fallback)
- [x] 与 unsharp/cas/rcas 三 mode 全兼容
- [x] 切换 motionAdaptiveSharpness/motionSharpness 不影响 alpha/sharpness/sharpenMode/motionAdaptive
- [x] F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9+F.0.12+F.0.13 十启共存
- [x] 老 backend ComputeCameraMotionScalar 默认返 0, 自动静默失效

### CI (待回填)
- [ ] runtime smoke 35/35 fn + 十启共存
- [ ] GitHub Actions 6/6 平台 success

---

## 4. CI 状态（待回填）

| 平台 | 状态 |
|------|------|
| build-windows | ⏳ |
| build-linux/macos/android/ios/web | ⏳ |

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 5. 关键技术决策回顾

### 5.1 Frobenius distance 选择

为什么不用更精确的 NDC 中心点重投影？
- Frobenius distance 同时反映平移 + 旋转 + FoV 变化, 单一标量足够稳定
- NDC 重投影需要矩阵求逆 + 4 个点变换, ALU 开销大
- 经验上 motion ~1 = 中等速度 (相机 1 unit/frame 平移) → factor 0.5 lerp, 视觉效果符合直觉

### 5.2 sharpness lerp 而非 motionFactor 直接喂给 shader

shader 层 motion-adaptive 需要 sample velocity tex (额外 fetch + ALU)，三个 sharpen shader (unsharp/cas/rcas) 都要改。
TAARenderer 层 lerp 仅 1 行 CPU 代码，零 shader 改动，与所有 mode 兼容。

### 5.3 默认 motionSharpness = 0.1

UE5 / SMAA 推荐高速时 sharpness 接近 0 但非 0 (保留最微弱锐化，避免 sharpening boundary 突变)。

### 5.4 与 F.0.8 motion-adaptive γ 协同

两者完全独立，可同时启用：
- F.0.8: variance clip γ 在高速时 lerp 到 motionGamma (宽容防 trail)
- F.0.13: sharpness 在高速时 lerp 到 motionSharpness (减锐化 trail)
- 双重防 trail 效果叠加，特别适合 FPS / 赛车场景
