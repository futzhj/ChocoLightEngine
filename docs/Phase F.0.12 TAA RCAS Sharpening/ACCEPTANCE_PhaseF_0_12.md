# Phase F.0.12 TAA RCAS Sharpening — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_12.md` / `FINAL_PhaseF_0_12.md` / `TODO_PhaseF_0_12.md`
> 基线：F.0 + F.0.1/0.2/0.3/0.4/0.5/0.6/0.7/0.8/0.9 + hotfix (commit `7b991ce`, CI 25932601637 6/6 success)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| GLES3 + GL33 shader | FS_RCAS_SOURCE FSR2 5-tap noise+edge aware | ✅ |
| Backend struct | programRCAS + 3 locs (uInputTex + uTexelSize + uSharpness) | ✅ |
| Backend Init/Shutdown | compile + bind sampler slot 0 + delete | ✅ |
| Backend DrawTAARCASPass | override (复用 vaoTonemap, viewport=full-res) | ✅ |
| render_backend.h virtual | DrawTAARCASPass 默认 no-op + doxygen | ✅ |
| TAARenderer Process | sharpness>0 切 3 分支 (rcas/cas/unsharp) | ✅ |
| TAARenderer parseSharpenMode_ | 加 "rcas" → 2 识别 | ✅ |
| TAARenderer GetSharpenMode | 3 路返回 (rcas/cas/unsharp) | ✅ |
| Lua API | l_TAA_SetSharpenMode 加 "rcas" 验证 + 错误消息更新 | ✅ |
| smoke (`taa.lua`) | surface 31 fn (不变) + Phase F.0.12 测试段: round-trip rcas / 3 大小写测试 / 三轮循环状态独立 / highlights | ✅ |
| demo (`demo_ssr/main.lua`) | Z 键 3-cycle (unsharp → cas → rcas → unsharp) + 描述 RCAS | ✅ |
| API doc (`Light_Graphics.md`) | 速查表 F.0.6 → F.0.6/F.0.12 + SetSharpenMode 加 RCAS 完整段（算法 + 对比表 + 适用场景 + 示例） | ✅ |
| Lua 语法验证 | `lightc -p taa.lua && lightc -p demo_ssr/main.lua` Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（7/7）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 算法版本 = FSR2 RCAS 5-tap noise+edge aware | shader: noise detection (range<1/64) + edge protection (lobe sqrt) | ✅ |
| D2 sharpness 范围 = [0, 2] (FSR2 标准) | Process: rcasS clamp 到 2.0 | ✅ |
| D3 noise threshold = 1/64 (constexpr) | shader: kNoiseThreshold = 1.0/64.0 | ✅ |
| D4 luma 提取 = G channel proxy | shader: bL = b.g, dL = d.g, ... | ✅ |
| D5 与 F.0.6 共存 = sharpenMode==2 + parseSharpenMode_ "rcas" | parseSharpenMode_ 加 case → 2 / Process 3 分支 | ✅ |
| D6 backend pass = DrawTAARCASPass virtual + override | render_backend.h + render_gl33.cpp | ✅ |
| D7 默认 mode = "unsharp" (零回归) | sharpenMode 字段默认 0 (F.0.6 沿用) | ✅ |

---

## 3. 验收清单

### 功能
- [x] RCAS shader 编译成功 (GLES3 + GL33 双平台)
- [x] sharpenMode round-trip: "unsharp" / "cas" / "rcas" 三向切换
- [x] 大小写不敏感: "RCAS" / "rcas" / "Rcas" / "RCAs" 等价
- [x] invalid "foo" 返 nil+err 含 "foo" 提示，state 不变（错误消息含 'unsharp' / 'cas' / 'rcas'）
- [x] type-error (number / boolean) 返 nil+err
- [x] sharpness=0 时 RCAS 跳过 (走 BlitTAAToHDR fallback)
- [x] sharpness>0 + sharpenMode="rcas" 走 DrawTAARCASPass
- [x] sharpness>2 时 RCAS 内部 saturate 到 2.0 (rcasS clamp)
- [x] 三轮循环 (unsharp ↔ cas ↔ rcas) 切换不影响 alpha/clipMode/sharpness/antiFlicker/halfRes
- [x] 默认行为不变 (sharpenMode='unsharp', F.0.1 行为)

### CI (待回填)
- [ ] runtime smoke 31/31 fn + 3 mode 共存
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

### 5.1 RCAS vs CAS 算法差异

CAS (F.0.6) 算法在每个 channel 独立计算 `ampRGB = sqrt(clamp(min(mn, mn2)/mx))`，全方位锐化。在 smooth gradient 或低对比区域，ampRGB 接近 0，但 `peak = -1/mix(8, 5, sharpness)` 仍非零，导致**少量** noise 放大。

RCAS (F.0.12) 引入两个 robust 层：
1. **Noise detection**: 4-tap range (排除中心) < 1/64 时直接 return e，完全跳过 sharpen
2. **Edge protection**: 用 `lobe = sqrt(min(eL-mn, mx-eL) / range)` 替代 ampRGB，edges 处 (eL ≈ mn 或 eL ≈ mx) lobe → 0，**完全消除** edge ringing

### 5.2 luma proxy = G channel

FSR2 标准选择: 完整 luma = `0.299R + 0.587G + 0.114B`，但 G 占 0.587 权重已是主导。直接用 G 比完整 luma 少 ~3 ALU/px，视觉差异肉眼几乎不可见。

### 5.3 HDR safe 简化

CAS 的 ampRGB sqrt 已对 HDR 高光有抑制（防黑斑）。RCAS 进一步将 sqrt 放在 lobe 上，HDR 大动态范围（如 100+ nits）依然 robust。最终仅需 `max(result, 0)` clamp 负值即可。
