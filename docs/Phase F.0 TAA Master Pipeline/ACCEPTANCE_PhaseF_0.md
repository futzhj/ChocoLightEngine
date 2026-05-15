# Phase F.0 TAA 主管线 — ACCEPTANCE

> 6A 工作流 · 阶段 6 (Assess) · Phase E velocity 链路集大成
> 基线: Phase E.18.2 commit `a50154c`
> 关联文档: `PLAN_PhaseF_0.md` / `FINAL_PhaseF_0.md` / `TODO_PhaseF_0.md`

---

## 1. 任务交付完整性

| 任务 | 文件 | 行数变更 | 状态 |
|------|------|---------|------|
| T1 backend 接口声明 | `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h` | +73 / 0 | ✅ |
| T1 backend 双 projection state | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +30 / -2 | ✅ |
| T1 vertex shader 改造 (6 处) | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +12 / -6 | ✅ |
| T2 GLES + GL3.3 TAA shader | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +148 / 0 | ✅ |
| T2 program 字段 + 编译 + 清理 + 4 接口实现 | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +180 / 0 | ✅ |
| T3 TAARenderer 模块 | `@e:/jinyiNew/Light/ChocoLight/include/taa_renderer.h` + `@e:/jinyiNew/Light/ChocoLight/src/taa_renderer.cpp` | +106 + +295 (新建) | ✅ |
| T3 HDR EndScene 集成 | `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp` | +12 / -0 | ✅ |
| T3 light_ui ApplyJitter hook | `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` | +9 / -0 | ✅ |
| T3 CMake 加 taa_renderer.cpp | `@e:/jinyiNew/Light/ChocoLight/CMakeLists.txt` | +1 / 0 | ✅ |
| T4 Lua 绑定 13 函数 + 子表 | `@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp` | +160 / 0 | ✅ |
| T4 smoke (taa.lua) | `@e:/jinyiNew/Light/scripts/smoke/taa.lua` | +210 (新建) | ✅ |
| T4 demo_ssr (Y/J 切换 + HUD) | `@e:/jinyiNew/Light/samples/demo_ssr/main.lua` | +43 / -2 | ✅ |
| T4 Light_Graphics.md TAA 文档 | `@e:/jinyiNew/Light/docs/api/Light_Graphics.md` | +250 / 0 | ✅ |
| T4 CI workflow 加 phaseF0Smoke | `@e:/jinyiNew/Light/.github/workflows/build-templates.yml` | +3 / 0 | ✅ |
| T5 6A 三件套 | `PLAN_PhaseF_0.md` + `ACCEPTANCE_PhaseF_0.md` + `FINAL_PhaseF_0.md` + `TODO_PhaseF_0.md` | 新建 | ✅ |
| T6 commit + push + CI | git + GitHub Actions | — | ✅ |

**累计代码 +1,279 行 / -10 行；新建 4 个文件（taa_renderer.h/.cpp + taa.lua + 6A docs）**

---

## 2. 决策对齐核对（PLAN §1.4 12 决策矩阵）

| # | 决策点 | PLAN 选定 | ACCEPTANCE 实际落实 | 状态 |
|---|--------|-----------|---------------------|------|
| 1 | TAA pipeline 位置 | MotionBlur 之后、Tonemap 之前 | `hdr_renderer.cpp::EndScene` line 511-514 | ✅ |
| 2 | Jitter 注入方式 | backend 双 projection (raster jittered + sample unjittered) | `render_gl33.cpp::ActiveProjection()` + `LoadJitteredProjection` + `vCurClip = uCurViewProj * (uModel * pos)` | ✅ |
| 3 | Jitter sequence | 复用 SSR Halton-2,3 8-sample | `taa_renderer.cpp::kHaltonJitter` 与 SSR 同表 | ✅ |
| 4 | Jitter 启用条件 | TAA Enable 时自动开启 | `light_ui.cpp::l_Window_Call` line 670-674 BeginScene 后 ApplyJitter | ✅ |
| 5 | History RT | 2× RGBA16F full-res ping-pong | `CreateTAAHistoryRT` 创建两张 RGBA16F + bilinear filter | ✅ |
| 6 | Velocity 来源 | combined dilated velocity (E.18 输出) | `taa_renderer.cpp::Process` 取 `HDRRenderer::GetDilatedVelocityTexture()` 优先 | ✅ |
| 7 | Neighborhood clip 算法 | 9-tap AABB RGB clip | `FS_TAA_SOURCE` 内 9 行 cur HDR 邻域采样 + `clamp(hist, mn, mx)` | ✅ |
| 8 | Alpha 默认值 | 0.92 | `taa_renderer.cpp::State::blendAlpha = 0.92f` | ✅ |
| 9 | 默认启用 | OFF | `State::autoEnable = false`，`OnHDREnabled` 仅 if autoEnable 才 Enable | ✅ |
| 10 | Lua API 设计 | `Light.Graphics.TAA.*` 子表 13 函数 | `light_graphics.cpp::taa_funcs[]` + 子表注册 | ✅ |
| 11 | 与 SSR Temporal 关系 | 共存 (用户自负责) | smoke §10 验证；demo_ssr Y 键 toggle 时 print warning 提示用户 | ✅ |
| 12 | Sharpening | Phase F.0 不做 (留 Phase F.0.1) | shader 内仅 mix(cur, hist, alpha) 不加 sharpen filter | ✅ |

**12/12 决策点全部按 PLAN 落实，零偏差。**

---

## 3. 验收检查清单

### T1 backend 接口
- [x] `LoadJitteredProjection(const float*)` 实现 (`render_gl33.cpp:7046`)
- [x] `ClearJitteredProjection()` 实现 (`render_gl33.cpp:7053`)
- [x] `IsJitteredProjectionActive() const` 实现 (`render_gl33.cpp:7058`)
- [x] 内部双 projection state (`projection` unjittered + `jitteredProjection` for raster)
- [x] `ActiveProjection()` helper 在 `ComputeMVP3D()` 内调用 (raster 路径走 jittered)
- [x] `ComputeViewProj3D()` 始终返 unjittered (velocity 路径)
- [x] `GetProjection(out16)` 始终返 unjittered (SSR/SSAO 零改动)
- [x] 6 处 vertex shader (3 GLES + 3 GL3.3) `vCurClip = uCurViewProj * (uModel * <pos>)` 全部改造
- [x] 6 处 vertex shader 加 `uniform mat4 uCurViewProj;` declare
- [x] `CreateTAAHistoryRT(w, h, fbos, texs) → bool` 创建 RGBA16F × 2 + bilinear
- [x] `DeleteTAAHistoryRT(fbos, texs)` 安全清理（含 nullptr 防御）
- [x] `SupportsTAA() const → bool` 反映 backend 能力位

### T2 TAA shader
- [x] GLES 3.0 版 `FS_TAA_SOURCE` 内容完整（line 2095-2164）
- [x] GL 3.3 版 `FS_TAA_SOURCE` 内容完整（line 2616-2684）
- [x] Program 字段 (`programTAA` + 10 个 uniform locations) 在 class state 内
- [x] Init 阶段编译 + uniform location 缓存 + sampler 默认绑 slot 0/1/2
- [x] Shutdown 阶段清理 program + 复位 jitterActive + 全部 uniform location 置 -1
- [x] `DrawTAAPass(...)` 实现完整（13 参数：sampler 3 + uniform 8 + dst FBO + 尺寸）
- [x] `BlitTAAToHDR(srcTex, dstFbo, w, h)` 用临时 FBO 包 srcTex + glBlitFramebuffer

### T3 HDR + light_ui 集成
- [x] `hdr_renderer.cpp` `#include "taa_renderer.h"` 添加
- [x] HDR `Enable()` 末尾调 `TAARenderer::OnHDREnabled(w, h)`
- [x] HDR `Disable()` 顶部调 `TAARenderer::OnHDRDisabled()`（最先关闭，依赖最末端）
- [x] HDR `Resize()` 内调 `TAARenderer::OnHDRResized(w, h)`
- [x] HDR `EndScene()` 在 `MotionBlurRenderer::Process` 后、Tonemap 前调 `TAARenderer::Process`
- [x] `light_ui.cpp` `#include "taa_renderer.h"` 添加
- [x] `Window.Open` 内 `TAARenderer::Init(g_render)` 在 MotionBlur 之后
- [x] `Window:__call` BeginScene 之后调 `TAARenderer::ApplyJitter()`
- [x] 关闭流程 `TAARenderer::Shutdown()` 在 MotionBlur 之前（最先关）
- [x] CMake 加 `${CHOCO_SRC}/taa_renderer.cpp`

### T4 Lua API + smoke + demo + docs
- [x] 13 Lua 函数全部包装：5 lifecycle + 6 params (3 对 Set/Get) + 2 status query
- [x] `taa_funcs[]` 数组 + 子表注册到 `Light.Graphics.TAA`
- [x] `SetNeighborhoodClip` / `SetJitterEnabled` 严格 boolean 类型检查（非 boolean 返 nil + err）
- [x] `SetBlendAlpha` clamp `[0, 1]` 验证
- [x] smoke `taa.lua` 13 函数 surface 检查 + 默认值 round-trip + clamp + type-error + 共存测试
- [x] smoke 加入 CI runtime runner (`build-templates.yml` line 105 + 221)
- [x] demo_ssr Y 键切换 TAA + J 键切换 jitter + HUD 显示状态
- [x] demo_ssr 反向清理加 TAA Disable
- [x] `Light_Graphics.md` Phase F.0 章节：API 速查表 + 13 函数文档 + 完整示例 + 性能预算

### T5 6A 文档
- [x] `PLAN_PhaseF_0.md` (含 Align + Architect + Atomize 三阶段)
- [x] `ACCEPTANCE_PhaseF_0.md` 本次完成
- [x] `FINAL_PhaseF_0.md` 本次完成
- [x] `TODO_PhaseF_0.md` 本次完成

### T6 CI
- [ ] GitHub Actions 6/6 平台 success
- [ ] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 Backend 双 projection 设计的关键

**问题**：TAA 要求 raster 用 sub-pixel jittered projection（实现超采样），但 velocity buffer 必须用 unjittered（保持 reproject 准确）；SSR/SSAO 等 view-space reconstruction 也必须用 unjittered。

**解法（业界标准）**：backend 内部持有两份 projection state，按用途分流：
- **raster 路径**（`ComputeMVP3D` → `gl_Position = uMVP * pos`）：走 `ActiveProjection()` 自动选 jittered（TAA 启用时）
- **velocity 路径**（vertex shader 内 `vCurClip = uCurViewProj * (uModel * pos)`）：用 `UploadVelocityUniforms` 上传的 `uCurViewProj`（始终 unjittered，因 `ComputeViewProj3D()` 用原始 `projection`）
- **重建路径**（SSR/SSAO 取 `GetProjection(out16)`）：始终返 unjittered

**这种设计的优势**：
- 用户透明（Lua 层 `SetPerspective` 不变）
- SSR/SSAO/MotionBlur 零改动（GetProjection 行为不变）
- vertex shader 仅改 1 行（`vCurClip` 改用 `uCurViewProj` 而非 `gl_Position`）

### 4.2 Vertex shader 6 处改造的必要性

**改造前**（错误）：`vCurClip = gl_Position;` （即 `= uMVP * pos = jitteredProj * view * model * pos`）
- TAA 启用时 vCurClip **含 jitter** → fragment shader 内 velocity = (vCurClip - vPrevClip) 含 sub-pixel 抖动
- velocity buffer 含 jitter → TAA reproject 不准（每帧 prevUV 偏 ±0.5 px 随机）→ super-sampling 失效

**改造后**（正确）：`vCurClip = uCurViewProj * (uModel * pos);`
- `uCurViewProj` 由 `UploadVelocityUniforms` 上传（unjittered）
- velocity 不含 jitter → reproject 准确 → super-sampling 生效

**6 处改造**（每个 shader 1 行）：
1. GLES VS3D_SOURCE (Unlit)
2. GLES VS3D_SKIN_SOURCE (Skinned)
3. GLES VS3D_SKIN_MORPH_SOURCE (Skinned + Morph)
4. GL3.3 VS3D_SOURCE
5. GL3.3 VS3D_SKIN_SOURCE
6. GL3.3 VS3D_SKIN_MORPH_SOURCE

### 4.3 与 Phase E.18 dilation pass 的复用关系

`taa_renderer.cpp::Process` 取 velocity tex 的优先级：
```cpp
const uint32_t dilatedV = HDRRenderer::GetDilatedVelocityTexture();
const uint32_t rawV     = g.backend->GetHDRVelocityTex(hdrFbo);
const uint32_t velocityTex = dilatedV ? dilatedV : rawV;
```

- **dilation pass 已跑** (E.18 active)：TAA 单点采 `dilatedTex`（已 9-tap max-length，shader 内 `uVelocityDilation=0`，单 fetch）
- **dilation pass 未跑** (E.18.2 autoSkip 命中 SSR-only 场景)：TAA 走 inline 9-tap fallback（`uVelocityDilation=velocityDilation`）

**与 SSR Temporal / Motion Blur 共享 dilation pass 输出**：3 个消费者公用一份 9-tap 结果（节省 2 × 8 = 16 fetch/px）。

### 4.4 NDC jitter 注入数学

`taa_renderer.cpp::ApplyJitter`：
```cpp
const float ndcOffX = jitterX * 2.0f / width;   // ±0.5 px → ±1/width NDC
const float ndcOffY = jitterY * 2.0f / height;
jitteredProj[8] += ndcOffX;   // col 2, row 0 (z->x clip 影响)
jitteredProj[9] += ndcOffY;   // col 2, row 1
```

修改 `proj[8]` / `proj[9]` 等价于让 `clip_pos.x += ndcOff.x * clip_pos.w`（等价于 NDC 平移 ndcOff 像素），是业界标准做法（UE/Unity/Frostbite 通用）。

---

## 5. 性能验证（理论 + 待真机实测）

### 5.1 fetch/px 分析（1080p 单 TAA pass）

| 操作 | fetch/px |
|------|----------|
| cur HDR 中心采样 | 1 |
| velocity (dilation active 时单点) | 1 |
| neighborhood 9-tap (cur HDR) | 9 |
| history (after reproject) | 1 |
| **小计** | **12 fetch/px** |

加 1 write/px。理论 GPU 时间 ~0.10 ms @ 1080p（带宽限）。

### 5.2 VRAM 增量

- TAA history RT × 2 RGBA16F full-res：1080p = **16 MB**，4K = **64 MB**
- 与 SSR Temporal history (16 MB @ 1080p) 不重叠（独立模块）

### 5.3 真机 GPU profile 待验证项

- [ ] 1080p TAA pass GPU 时长（理论 ~0.10 ms）
- [ ] SSR Temporal + TAA 同开总时长（理论 ~0.20 ms，含双 history fetch）
- [ ] mobile GPU（Adreno/Mali）实测，关注 RGBA16F 带宽

---

## 6. 已知限制 / Phase F.0.x 候选

1. **无 Sharpening 后处理**：TAA 引入 sub-pixel 模糊（高频信息平均），Phase F.0.1 可加 Filmic-style 1-tap 锐化补偿
2. **RGB AABB clip 而非 YCoCg**：YCoCg color-space clip 业界更稳但 shader 复杂度 +30%（候选 Phase F.0.2）
3. **无 Variance clipping**：邻域均值 ± k×方差更鲁棒但需 9-tap 第二次（Phase F.0.3）
4. **无 anti-flicker filter**：高对比度像素可能产生 firefly 闪烁（Phase F.0.4 加 luminance filter）
5. **history RT VRAM 4K 64 MB**：mobile 4K 受限，Phase F.0.5 可加 halfRes history（参考 E.17/E.18.1 模式）
6. **与 SSR Temporal 双 temporal**：反射被 temporal 两次，文档警示用户手动关 SSR.SetTemporalEnabled(false)

---

## 7. CI 状态

| 平台 | 状态 | 状态详情 |
|------|------|------|
| build-windows | ✅ success | runtime smoke 26 PASS (含 taa.lua 13 fn) + Phase E.16/17/18/18.1/18.2 零回归 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: `25914279471`
Commit hash: `bc823760c2c2fec9a00c65effde2078679ecafa2` (short: `bc82376`)
Total duration: **11m01s** (2026-05-15T10:59:14Z → 2026-05-15T11:10:15Z)
Date: 2026-05-15

**✅ 6/6 平台全 PASS, Phase F.0 验收通过。**
