# Phase E.18 Independent Velocity Dilation Pass — ACCEPTANCE

> 6A 工作流 · 阶段 6 · Assess
> 基线：Phase E.17 commit `f8d7e41`

---

## 1. 实施完成度

| 任务 | 文件 | 行数变更 | 状态 |
|------|------|---------|------|
| T1 backend 接口 | `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h` | +57 | ✅ |
| T2 GL33 shader+impl | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +180 | ✅ |
| T3 SSR/MB uniform 控制 | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +8 / -2 | ✅ |
| T4 HDRRenderer 集成 | `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h` | +10 | ✅ |
| | `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp` | +85 | ✅ |
| T5 consumer 切换 | `@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp` | +11 / -3 | ✅ |
| | `@e:/jinyiNew/Light/ChocoLight/src/ssr_renderer.cpp` | +9 / -1 | ✅ |
| T6 smoke / demo / docs | `@e:/jinyiNew/Light/scripts/smoke/motion_blur.lua` | +9 | ✅ |
| | `@e:/jinyiNew/Light/samples/demo_ssr/main.lua` | +2 / -1 | ✅ |
| | `@e:/jinyiNew/Light/docs/api/Light_Graphics.md` | +30 / -7 | ✅ |
| T7 6A 三件套 | `ACCEPTANCE_PhaseE_18.md` / `FINAL_PhaseE_18.md` / `TODO_PhaseE_18.md` | 新增 | ✅ |
| T8 commit+push+CI | git + GitHub Actions | — | ⏳ |

---

## 2. 决策对齐核对（ALIGNMENT 10 决策矩阵）

| # | 决策点 | 已落实位置 | 状态 |
|---|--------|----------|------|
| 1 | dilation pass 时机：HDR EndScene 内、SSR/MotionBlur 之前 | `hdr_renderer.cpp` EndScene 在 SSAO 后 / SSR 前插入 | ✅ |
| 2 | dilatedTex 格式：永远 RG16F（无视 raw format） | GL33 `CreateVelocityDilateRT` glTexImage2D `GL_RG16F` | ✅ |
| 3 | dilatedTex 分辨率：与 raw velocityTex 同（full-res） | HDRRenderer 调 `CreateVelocityDilateRT(w, h)` 即 HDR 全分辨率 | ✅ |
| 4 | 算法复刻：9-tap max-length 与 inline 完全等价 | `FS_VELOCITY_DILATE_SOURCE` 双 profile 与 `FS_SSR_TEMPORAL` 内 `SampleVelocityDilated` 算法一致 | ✅ |
| 5 | uVelocityDilation 上传逻辑：dilationPassActive=true 时强制 0 | `DrawSSRTemporal` `ssrUVDValue` / `DrawMotionBlur` `mbUVDValue` | ✅ |
| 6 | 双 RT 资源（combined + camera-only）：与 cameraVelocityTex 同条件 | HDRRenderer `CreateRT` 内分别判定 | ✅ |
| 7 | 不引入新 Lua API：复用 HDR.SetVelocityDilation | smoke / demo / docs 维持原 API | ✅ |
| 8 | Backend 失败兜底：silent skip + consumer fallback | `SupportsVelocityDilation`→false → dilationActive=false → consumer 拿 raw | ✅ |
| 9 | dilation pass 始终 full-res（即使 motion blur 半分辨率） | `CreateVelocityDilateRT` 用 HDR RT 尺寸，与 `motionBlurRenderer.halfRes` 解耦 | ✅ |
| 10 | uTexel 决定 9-tap 物理覆盖：与 inline 一致 1/(W,H) | shader 内 `uTexel` + backend `1.0f/(float)w, 1.0f/(float)h` | ✅ |

---

## 3. 验收 checklist

### T1 编译通过
- [x] `render_backend.h` 6 个新虚接口签名清晰，默认实现安全（返 0/false）

### T2 GL33 运行时
- [x] `FS_VELOCITY_DILATE_SOURCE` 双 profile（GLES3 + GL33 Core）声明完整
- [x] InitShaders 内编译 + 缓存 4 个 uniform location；`velocityDilateSupported` 标志正确
- [x] Shutdown 内 program 释放 + 字段重置
- [x] `CreateVelocityDilateRT` 失败兜底（FBO 不完整时清理 + return 0）

### T3 uniform 控制
- [x] DrawSSRTemporal 内 `ssrUVDValue = dilationPassActive_ ? 0 : (velocityDilation ? 1 : 0)`
- [x] DrawMotionBlur 内 `mbUVDValue` 同上模式

### T4 HDRRenderer 生命周期
- [x] State 增 4 字段：dilatedVelocityFbo/Tex + dilatedCameraVelocityFbo/Tex
- [x] CreateRT 内创建 dilated RT + log；失败 silent fallback
- [x] ReleaseRT 内统一释放 + SetDilationPassActive(false)
- [x] EndScene 内 dilation pass 执行 + SetDilationPassActive(active) + UnbindFBO
- [x] `GetDilatedVelocityTexture` / `GetDilatedCameraVelocityTexture` 公开 API

### T5 consumer 切换
- [x] MotionBlurRenderer::Process 优先 dilated，fallback raw
- [x] SSRRenderer::Process 优先 dilated，fallback raw（hdr_renderer.h include 添加）

### T6 docs / smoke / demo
- [x] motion_blur.lua 头注释加 Phase E.18 行为升级段（API 数不变 15）
- [x] demo_ssr/main.lua HUD 显示 "(E.18 shared pass)" 标记
- [x] Light_Graphics.md `HDR.SetVelocityDilation` 段补行为升级 + 性能/VRAM 收益表

### T7 6A 文档
- [x] ALIGNMENT / DESIGN / TASK 已完成
- [x] ACCEPTANCE / FINAL / TODO 本次完成

### T8 CI
- [ ] GitHub Actions 6/6 平台 success
- [ ] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 dilatedTex 永远 RG16F
- raw velocityTex 可能是 RG16F 或 RG8（用户切 format）
- shader 内 dilation 通过 `DecodeVelocity` 解码 → 输出 decoded float
- 故 dilatedTex 始终用 RG16F 存 decoded 后的 float，consumer 单点采无需再 decode

### 4.2 uTexel = 1/(W,H) 是 9-tap 物理覆盖关键
- 即使 motion blur 半分辨率（Phase E.17），velocityTex 仍是 full-res
- dilation pass 也 full-res，与 inline 9-tap 物理覆盖一致
- consumer 单点采 dilatedTex 时硬件 bilinear 自动平滑（CLAMP_TO_EDGE + LINEAR）

### 4.3 dilation pass 仅在多 consumer 场景才划算
- 仅 SSR Temporal 启用：9 fetch (dilate) + 1 fetch (sample) = 10 fetch >  9 fetch (inline) ❌
- 仅 Motion Blur 启用：9 + N (samples) ≪ 9N (inline) ✓ 大幅省
- SSR + Motion Blur 同开：9 + 1 + N ≪ 18 + 9N ✓ ~50% 省
- 用户可关 HDR.SetVelocityDilation 切回单点采样（最便宜）

### 4.4 backend silent fallback 是安全网
- `SupportsVelocityDilation()`=false → HDRRenderer 不创建 dilated RT
- `dilatedVelocityFbo`=0 → EndScene 跳过 DrawVelocityDilate
- `dilationPassActive_`=false → DrawSSRTemporal/MotionBlur 走 inline 9-tap
- consumer fetch dilatedTex 拿 0 → fallback raw velocityTex（业务零感知）

---

## 5. 性能预算（理论）

### 5.1 GPU 时间（1080p 估算）

| 配置 | velocity fetch / 帧 | 估算 ms (RTX 3060) |
|------|--------------------|--------------------|
| SSR + MB(N=8) inline 9-tap | (9 × 1) + (9 × 8) = 81 / pixel | 0.6 ms |
| SSR + MB(N=8) shared dilation | 9 + 1 + 8 = 18 / pixel | 0.3 ms |
| **净节省** | **63 fetch / pixel** | **~50%** |

### 5.2 VRAM 开销

| 资源 | 尺寸 | 大小（1080p） |
|------|------|--------------|
| dilatedVelocityTex | RG16F full-res | 8 MB |
| dilatedCameraVelocityTex（可选） | RG16F full-res | 8 MB |
| 双 FBO（无 depth） | 元数据 | <1 KB |

---

## 6. 已知限制

1. **dilation pass 半分辨率（E.18.1 候选）**：当前 dilation pass 仍 full-res，未来可半分辨率（VRAM -75% / 性能 +4×，但 9-tap 邻域物理覆盖变化需校验）。
2. **N=1 单 sample motion blur**：N=1 时 inline 9 fetch < shared 10 fetch，user 应自行关 dilation。
3. **dilation 状态全局开关**：当前所有 consumer 共用同一 dilation 开关，未来可能需要细粒度（SSR 单独 / MB 单独）。

---

## 7. CI 状态（待回填）

| 平台 | 状态 | 时长 |
|------|------|------|
| build-windows | ⏳ | — |
| build-linux | ⏳ | — |
| build-macos | ⏳ | — |
| build-android | ⏳ | — |
| build-ios | ⏳ | — |
| build-web | ⏳ | — |

GitHub Run ID: `<pending>`
Commit hash: `<pending>`
