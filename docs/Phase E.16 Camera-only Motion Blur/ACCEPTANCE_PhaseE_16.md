# Phase E.16 Camera-only Motion Blur — ACCEPTANCE

> 6A 工作流 · 阶段 6 · Assess (子节 ① 实施完成度)
> 基线：Phase E.15 commit `4eb22c7` (CI run 25894807417 6/6 success)

---

## 1. 实施完成度（T1~T7）

| T | 任务 | 状态 | 行数 | 文件 |
|---|------|------|------|------|
| **T1** | RenderBackend 接口扩展 | ✅ | +28 | `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h:549-563`、`:590-593`、`:1196-1202` |
| **T2** | GL33Backend HDR FBO MRT slot 3 | ✅ | +85 | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:2629-2632`、`:3912-3916`、`:3969-3992`、`:4028-4047`、`:4088-4092`、`:4110-4115`、`:4139-4145` |
| **T3** | 3D Shader VS×3 + FS×2（GLES3+GL33 双 source = 16 处一致改） | ✅ | +96 | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:135`、`:144-145`、`:175`、`:195-196`、`:237`、`:273-274`、`:286`、`:303`、`:315-335`、`:348`、`:384`、`:470-490`、`:513`、`:522-523`、`:553`、`:573-574`、`:612`、`:646-647`、`:659`、`:676`、`:687-707`、`:720`、`:756`、`:841-861` |
| **T4** | MotionBlur shader + DrawMotionBlur 扩展 | ✅ | +90 | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:2095-2104`、`:2124-2139`、`:2141-2154`、`:2577-2586`、`:2605-2620`、`:2622-2635`、`:2843-2862`、`:4044-4050`、`:6573-6619`、`:6627` |
| **T5** | HDRRenderer + MotionBlurRenderer + Lua API（11→13 fn） | ✅ | +60 | `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h:143-145`、`@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp:75-81`、`:355-358`、`@e:/jinyiNew/Light/ChocoLight/include/motion_blur_renderer.h:89-94`、`@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp:35`、`:185-188`、`:201-211`、`@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp:2897-2910`、`:2927-2929` |
| **T6** | smoke + demo + Light_Graphics.md | ✅ | +130 | `@e:/jinyiNew/Light/scripts/smoke/motion_blur.lua:1-9`、`:34-40`、`:195-236`、`@e:/jinyiNew/Light/samples/demo_ssr/main.lua:351-359`、`:427-437`、`@e:/jinyiNew/Light/docs/api/Light_Graphics.md:1096-1137` |
| **T7** | 6A 文档 + commit + CI | 🚧 | +450 docs | 此文档 + FINAL + TODO 三件套 |
| **总计代码** | | **~489 行** | 8 个源码文件 |
| **总计文档** | | **+ 6A 5 件套 ~2200 行** | |

---

## 2. 设计决策完整保留

ALIGNMENT_PhaseE_16.md §3 共 11 个决策全部贯彻：

| # | 决策 | 实施核对 |
|---|------|----------|
| 1 | 拆分方案 = A1 双 RT | ✅ HDR FBO MRT slot 3 = cameraVelocityTex (RG16F/RG8 跟随 combined) |
| 2 | 第二张 RT 存 camera_only | ✅ shader 公式 `prevVP × curModel × pos`；object_only = combined − camera |
| 3 | Lua mode 类型 = int 0/1/2 | ✅ `l_MB_SetMode(int)` / `l_MB_GetMode() → int` |
| 4 | mode 数值含义 | ✅ 0=combined / 1=camera / 2=object |
| 5 | 默认 mode = 0 | ✅ `motion_blur_renderer.cpp` State `mode = 0` 默认；Phase E.15 行为零回归 |
| 6 | 影响范围 = 只 MotionBlur | ✅ SSR Temporal 数学路径不动 |
| 7 | RT 格式 = 跟随 combined velocity | ✅ CreateHDRFBO 内部用同一 velocityFormat 分支 |
| 8 | RT 创建时机 = HDR.Enable 时总是创建 | ✅ HDRRenderer::CreateRT 总传 `&cameraVelocityTex` 给 backend |
| 9 | Lua API 命名 | ✅ `SetMode / GetMode`（与 SSR `SetRejectionMode` 一致） |
| 10 | Backend 接口扩展 | ✅ 1 个新虚接口（`GetHDRCameraVelocityTex`） + 2 个签名扩展（CreateHDRFBO / DrawMotionBlur） |
| 11 | shader 改动 = 16 处一致改 | ✅ `multi_edit` + `replace_all` 一次命中：VS×3 (1 varying + 1 计算) + FS×2 (1 in + 1 layout + 1 encode) × GLES3/GL33 双 source |

---

## 3. 验收清单

### 3.1 编译与 API

| 项 | 期望 | 结果 |
|---|------|------|
| `render_backend.h` 接口扩展 | ✅ | CreateHDRFBO 增 `outCameraVelocityTex` / 新增 GetHDRCameraVelocityTex / DrawMotionBlur 增 `cameraVelocityTex + mode` |
| GL33 backend 4-attachments HDR FBO | ✅ | hdrFboCameraVelocityTex map + 4 attachments drawBuffers 矩阵（3 个分支） |
| 3D shader VS+FS 16 处一致改 | ✅ | 1 次 multi_edit 6 个 edit + replace_all 同时命中 GLES3+GL33 |
| MotionBlur shader mode 切换 | ✅ | `SampleCameraVelocityDilated` + main 内 if/elif/else mode 分支 |
| DrawMotionBlur safeMode fallback | ✅ | mode∈{1,2} 但 cameraVelocityTex==0 → mode=0；slot 2 占位 |
| HDRRenderer cameraVelocityTex 联动 | ✅ | `CreateRT` 传 outCameraVelocityTex + `GetCameraVelocityTexture()` |
| MotionBlurRenderer SetMode/GetMode | ✅ | g.mode + clamp [0,2] + Process 透传 |
| Lua API 11 → 13 fn | ✅ | `l_MB_SetMode` / `l_MB_GetMode` + `mb_funcs[]` 增 2 条 |

### 3.2 测试

| 项 | 期望 | 结果 |
|---|------|------|
| smoke surface 11 → 13 PASS | ✅ | `motion_blur.lua` fn_names 加 SetMode/GetMode |
| smoke 默认 mode = 0 | ✅ | `pass("GetMode() default = 0 (combined)")` |
| smoke round-trip 1, 2 | ✅ | `pass("SetMode / GetMode round-trip ok (1=camera_only, 2=object_only)")` |
| smoke clamp 下界 0 | ✅ | `pass("SetMode clamp lower bound (0)")` |
| smoke clamp 上界 2 | ✅ | `pass("SetMode clamp upper bound (2)")` |
| `lightc -p motion_blur.lua` exit 0 | ✅ | 本地验证通过 |
| `lightc -p demo_ssr/main.lua` exit 0 | ✅ | 本地验证通过 |

### 3.3 demo + 文档

| 项 | 期望 | 结果 |
|---|------|------|
| demo `;` 键切 mode | ✅ | cycle 0→1→2→0，print 当前 mode 名 |
| HUD 显示 `mode=N (name)` | ✅ | line 输出含 modeNames[m] |
| Keys 提示加 `;=Mode` | ✅ | 第二行末尾 |
| `Light_Graphics.md` MotionBlur 段加 SetMode/GetMode | ✅ | 参数 + 模式映射表 + 设计 + 示例 |
| 完整用法示例加 `MB.SetMode(1)` | ✅ | 已更新 |

### 3.4 零回归保证

| 项 | 验证 |
|---|------|
| mode=0 (默认) 视觉与 Phase E.15 完全一致 | shader 中 `uMode==0` 走 `SampleVelocityDilated`（与原 main() 同算法） |
| HDR FBO 接口默认参数兼容 | `outCameraVelocityTex=nullptr` 时 backend 不创建第二张 RT，行为等同 Phase E.15 |
| SSR Temporal 路径不动 | Phase E.16 完全没改 SSR 模块代码 |
| 现有 16 个 phase smoke | ⏳ CI 上验证（T7-3） |

---

## 4. 性能预算（设计估算，待真机基线）

| 项 | Phase E.15 基线 | Phase E.16 mode=0 | Phase E.16 mode=1 | Phase E.16 mode=2 |
|----|---------------|------------------|------------------|------------------|
| 3D shader FS 增量 | 0 | +极小（1 段 encode） | 同 | 同 |
| MotionBlur Pass1 9-tap dilation | 0.5 ms | 0.5 ms | 0.5 ms | **~0.55 ms**（多 1 套 sampler） |
| MotionBlur Pass2 blit | 0.2 ms | 0.2 ms | 0.2 ms | 0.2 ms |
| VRAM @ 1080p RG16F | 0 | **+4 MB** | 同 | 同 |
| VRAM @ 1080p RG8 | 0 | **+1 MB** | 同 | 同 |

整体：mode=0/1 与 Phase E.15 几乎等价；mode=2 多 ~10% MotionBlur 开销。

---

## 5. 已知限制（推到 TODO）

1. **真机视觉验收**：需在桌面 GL3.3 真窗口下确认 mode=1/2 视觉效果（mesh 旋转 vs 相机平移测试）
2. **camera-only velocity 在 dynamic skinned 物体上的精度**：camera-only 用 `cur joints + cur morph` 假设关节没动，与「关节有动但相机也动」场景的语义有理论差距（实际肉眼通常看不出来）
3. **SSR Temporal 是否选择性用 camera-only velocity** 为未来 phase（详 §8）
4. **mode=2 性能基线** 需真机测量是否 < +10%（设计预算）

---

## 6. CI 状态

待 T7 commit + push 后填入：

```
GitHub Actions run id: <TBD>
Status: <TBD>
Duration: <TBD>
Phase E.16 smoke PASS count: ≥ 21 (16 原 + 5 mode 新增)
```

---

## 7. 推进确认

T7-1 ACCEPTANCE 已完成。下一步：FINAL + TODO + commit + push + CI。
