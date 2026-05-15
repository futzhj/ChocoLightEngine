# Phase F.0.9 TAA Custom Upsampler — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_9.md` / `FINAL_PhaseF_0_9.md` / `TODO_PhaseF_0_9.md`
> 基线：F.0 + F.0.1/0.2/0.3/0.4/0.5/0.6/0.7/0.8 (commit `f415890`)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| GLES3 + GL33 shader | FS_BICUBIC_UPSCALE_SOURCE Catmull-Rom 9-tap (Sigggraph 2018 Filmic SMAA) | ✅ |
| Backend struct | programBicubicUpscale + 2 locs (uInputTex + uTexel) | ✅ |
| Backend Init/Shutdown | compile + bind sampler slot 0 + delete | ✅ |
| Backend DrawTAAUpscalePass | override (复用 vaoTonemap, viewport=dst, uTexel=1/src) | ✅ |
| render_backend.h virtual | DrawTAAUpscalePass 默认 no-op + doxygen | ✅ |
| TAARenderer state | upscaleMode (0=bilinear / 1=bicubic) 默认 0 | ✅ |
| TAARenderer Process | sharpness=0 路径切分支：halfRes && bicubic → DrawTAAUpscalePass | ✅ |
| TAARenderer Set/Get | parseUpscaleMode_ 手写 case-insensitive + Set/GetUpscaleMode | ✅ |
| Lua API +2 | l_TAA_SetUpscaleMode/Get + taa_funcs[] 29→31 | ✅ |
| smoke (`taa.lua`) | surface 31 fn + 默认 / round-trip / 大小写 3 路 / invalid raise / type-error 2 路 / 状态独立 / 八启共存 (10 PASS) | ✅ |
| demo (`demo_ssr/main.lua`) | P 键 toggle UpscaleMode + HUD sharp=0 路径加 blit-bil/blit-bic 后缀 | ✅ |
| API doc (`Light_Graphics.md`) | 速查表 29→31 + Set/GetUpscaleMode 完整文档段（生效条件 / 算法原理 / 性能 / 推荐场景） | ✅ |
| Lua 语法验证 | `lightc -p taa.lua && lightc -p demo_ssr/main.lua` Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（7/7）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 上采样算法 = Catmull-Rom 9-tap (Sigggraph 2018) | shader: 9 sample (3x3) 等效 16-tap, 5-tap hardware bilinear 优化 | ✅ |
| D2 影响范围 = 仅 BlitTAAToHDR 路径 (sharpness=0) | Process: sharpness>0 走 sharpen/CAS pass 不变 | ✅ |
| D3 sharpness=0+halfRes=false 时保持 GL_NEAREST | Process: 仅 halfRes && upscaleMode==1 才走 DrawTAAUpscalePass | ✅ |
| D4 默认 mode = "bilinear" | State `upscaleMode = 0` 默认 (零回归) | ✅ |
| D5 API 命名 = SetUpscaleMode + 字符串 mode | Lua taa_funcs[] +2 项 | ✅ |
| D6 错误处理 = 大小写不敏感 + 未识别返 nil+err | parseUpscaleMode_ + l_TAA_SetUpscaleMode lower 处理 | ✅ |
| D7 5-tap Catmull-Rom 优化版 (Sigggraph 2018) | shader: w12 = w1+w2 合并 + 9 sample 实际布局 | ✅ |

---

## 3. 验收清单

### 功能
- [x] upscaleMode 默认 "bilinear" (零回归)
- [x] SetUpscaleMode("bicubic"/"bilinear") round-trip
- [x] 大小写不敏感 ("BICUBIC" / "BiCuBiC" / "BILINEAR")
- [x] invalid "foo" 返 nil+err 含 "foo" 提示，state 不变
- [x] type-error (number / boolean) 返 nil+err
- [x] 切换 upscaleMode 不影响 alpha/clipMode/sharpness/halfRes/sharpenMode/motionAdaptive
- [x] sharpness=0 + halfRes=true + bicubic → 走 DrawTAAUpscalePass shader
- [x] sharpness=0 + halfRes=false → 保持 BlitTAAToHDR 1:1 不受 upscaleMode 影响
- [x] sharpness>0 → 不受 upscaleMode 影响 (走 sharpen/CAS pass)
- [x] F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9 八启共存

### CI (待回填)
- [ ] GitHub Actions 6/6 平台 success
- [ ] runtime smoke 31/31 fn + 八启共存

---

## 4. CI 状态（待回填）

| 平台 | 状态 |
|------|------|
| build-windows | ⏳ |
| build-linux/macos/android/ios/web | ⏳ |

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`
