# Phase E.15 Velocity-driven Motion Blur — ALIGNMENT 对齐文档

> **任务名**：Phase E.15 Velocity-driven Motion Blur（基于 velocity buffer 的相机/物体运动模糊）
> **基线**：Phase E.14 Velocity Dilation + RG8（commit `f7150c0` / `bcb0f48` / `d145566`）
> **目标**：在 HDR 后处理链 LensFlare 之后、Tonemap 之前插入 motion blur pass，消费现有 RG16F/RG8 velocity buffer

---

## 1. 项目上下文分析

### 1.1 现有后处理 module 范式（强约束）

| 项 | 规范 |
|----|------|
| **形态** | 命名空间，不是类。`namespace BloomRenderer / SSRRenderer / ...` |
| **生命周期** | `Init(backend)` / `Shutdown()` 配合 `light_ui.cpp` 主循环 |
| **可控性** | `Enable(w, h)` / `Disable()` / `IsEnabled()` / `IsSupported()` / `Resize(w, h)` |
| **HDR 联动** | `OnHDREnabled / OnHDRDisabled / OnHDRResized` 内部 API（HDRRenderer 通知） |
| **autoEnable** | `SetAutoEnable / GetAutoEnable` (默认 false，仅 Bloom 默认 true) |
| **管线 hook** | `Process(...)` 由 `HDRRenderer::EndScene` 内部按顺序调用 |
| **Lua API** | 独立子表 `Light.Graphics.XXX` 在 `light_graphics.cpp` 注册 |
| **错误约定** | bad-arg → `nil + err string`（避 MSVC longjmp，Phase E.14 已统一） |

### 1.2 HDR 后处理链顺序（hdr_renderer.cpp:280-310）

```
HDRRenderer::EndScene()
  ↓ UnbindFBO() + 切回 default
  ↓ BloomRenderer::Process(hdrFbo, hdrTex)              // E.4 提亮 pyramid
  ↓ LensDirtRenderer::Process(hdrFbo, bloomTopTex, ...) // E.6 dirt × bloom
  ↓ StreakRenderer::Process(hdrFbo, hdrTex)             // E.6 镜头条纹
  ↓ SSAORenderer::Process(hdrFbo, hdrTex)               // E.8 阴部 AO
  ↓ SSRRenderer::Process(hdrFbo, hdrTex)                // E.9 屏幕空间反射
  ↓ LensFlareRenderer::Process(hdrFbo, hdrTex)          // E.7 镜头光斑
  ↓ ★ Phase E.15 — MotionBlurRenderer::Process(...)     // 新插入点
  ↓ DrawTonemapFullscreen(g.sceneTex, exposure, gamma, tonemap)
  ↓ CommitVelocityHistory()
```

### 1.3 已就绪的 velocity 设施（Phase E.13 + E.14）

| 设施 | 接入方式 |
|------|---------|
| `RenderBackend::GetHDRVelocityTex(fbo)` | 拿 velocity texture id（RG16F 或 RG8） |
| `RenderBackend::GetActiveVelocityFormat()` | 当前格式（RG16F=0 / RG8=1） |
| `RenderBackend::GetVelocityScale()` | RG8 解码 scale（默认 0.25） |
| `RenderBackend::GetVelocityDilation()` | dilation 开关（默认 ON） |
| GLSL `DecodeVelocity(raw, format, scale)` helper | SSRTemporal shader 已有，可直接复用为 .glsl include 或复制 |
| GLSL `SampleVelocityDilated(...)` | 同上，3x3 max-length 邻域 |

**直接结论**：MotionBlur shader 可以**完整复用** Phase E.14 的 velocity decode + dilation 逻辑，不重写。

---

## 2. 边界确认

### 2.1 范围内（Phase E.15 必做）

- 新建 `motion_blur_renderer.h/cpp`（命名空间模块）
- `RenderBackend` 加 `SupportsMotionBlur / CreateMotionBlurRT / DeleteMotionBlurRT / DrawMotionBlur` 4 个虚接口
- `GL33Backend` 实现这 4 个接口 + 1 个新 fullscreen shader
- `HDRRenderer::EndScene` 链路加 1 行 `MotionBlurRenderer::Process(g.fbo, g.sceneTex)`
- HDR 联动 hook：`OnHDREnabled/Disabled/Resized`
- Lua 子表 `Light.Graphics.MotionBlur` 11 个 API
- smoke `scripts/smoke/motion_blur.lua` 全表面 + bad-arg + round-trip
- demo `samples/demo_ssr/main.lua` 加 1 个按键（M）切 MotionBlur ON/OFF + HUD
- 文档 6 件套（ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO）

### 2.2 范围外（Phase E.15 不做）

| 不做 | 原因 / 后续 phase |
|------|-------------------|
| Camera-only motion blur 模式 | 需要分离 camera velocity 与 object velocity，G-buffer 改造，Phase E.15.x 增量 |
| 1/2 分辨率 motion blur | 第一版全分辨率简单，后续按性能评估 |
| 独立 velocity dilation pass | Phase E.14 inline 方案已足够；TODO §3 候选 |
| Per-object motion blur skip mask | 高级特性，第一版不做 |
| Stochastic / TAA 配合 | 当前 SSR Temporal 已用 velocity，motion blur 是另一消费者 |

### 2.3 与 Phase E.14 的兼容性约束

| 约束 | 验证方式 |
|------|---------|
| velocity buffer 现有写入路径不改 | `Phase AS / E.13 / E.14` 的 3D shader 不动 |
| velocity buffer 内存布局不变 | 仍是 HDR FBO 的 attachment 2 (RG16F/RG8) |
| 默认行为不变 | MotionBlurRenderer 默认 disabled + autoEnable=false |
| Lua HDR 子表 16 functions 不变 | MotionBlur 是新子表，独立挂载 |

---

## 3. 需求理解

### 3.1 用户视角

```lua
-- 完整启用流程（与 Bloom/SSR 一致）
Light.Graphics.HDR.Enable(1280, 720)
Light.Graphics.MotionBlur.Enable(1280, 720)  -- 默认 autoEnable=false，需手动

-- 可选：让 HDR.Enable 自动联动
Light.Graphics.MotionBlur.SetAutoEnable(true)

-- 调参
Light.Graphics.MotionBlur.SetStrength(1.5)        -- [0, 4], 默认 1.0
Light.Graphics.MotionBlur.SetSampleCount(16)      -- [1, 32], 默认 8

-- 关闭
Light.Graphics.MotionBlur.Disable()
```

### 3.2 视觉预期

| 场景 | 表现 |
|------|------|
| 静止相机 + 静止物体 | 无 blur（velocity = 0） |
| 静止相机 + 移动物体 | 物体方向性 blur，背景清晰 |
| 移动相机 + 静止物体 | 物体随相机轨迹 blur，前景比远景 blur 弱 |
| 旋转相机 | 切线方向 blur（边缘强 / 中心弱） |
| 快速运动（>±540 px / frame, RG8 模式） | velocity 饱和但 blur 仍合理（最大约 540 px） |

### 3.3 性能预期（典型 1080p 默认 8 采样）

| 项 | 估算 |
|----|------|
| 单 pass fullscreen | ~0.5 ms |
| 加上 ping-pong blit | ~0.7 ms 总 |
| 16 采样高质量 | ~1.0 ms |
| VRAM 增量 | 1× RGBA16F @ scene size = 8 MB @ 1080p |

---

## 4. 关键决策

### 4.1 已自动拍板（基于现有项目 + 行业标准）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | **算法** | Per-pixel linear blur（单 pass，沿 velocity 多采样） | 行业基线、单 pass、与现有 SSR/SSAO/Bloom 复杂度一致；MDFG 2012 / Stochastic 是 Phase E.16+ 候选 |
| 2 | **插入位置** | LensFlare 之后、Tonemap 之前 | blur 包含所有后处理结果，但不模糊 lens flare 自身 |
| 3 | **scratch RT 归属** | `GL33Backend` 持有 ping-pong RGBA16F RT | 与 SSAO blurFbo 同模式；MotionBlurRenderer 不直接持 GL 资源 |
| 4 | **autoEnable 默认** | `false` | 与 LensDirt/Streak/LensFlare/SSAO/SSR 一致；Bloom 是唯一例外 |
| 5 | **render 分辨率** | 全分辨率（与 sceneTex 同尺寸） | 第一版简单；1/2 分辨率优化进 Phase E.15.x 候选 |
| 6 | **velocity 消费** | 复用 Phase E.14 inline dilation + decode | 不引入独立 dilation pass；shader 复制 helpers 即可 |
| 7 | **API surface** | 11 函数子表（5 lifecycle + 2 联动 + 2 强度 + 2 采样数） | 与 Bloom 13 / LensFlare 21 / LensDirt 10 数量级一致 |
| 8 | **错误约定** | Set* 系列 bad-arg 用 `nil + err`，Set numeric 用 clamp | 与 Phase E.14 / LensFlare 一致 |
| 9 | **自动联动 API** | `OnHDREnabled(w, h) / OnHDRDisabled() / OnHDRResized(w, h)` | 与现有所有 module 一致 |
| 10 | **shader profile** | GLES3 + GL33 双 source（与 SSRTemporal 一致） | 第一版仅 GL3.3 后端启用 |

### 4.2 待用户拍板（关键决策）

#### 决策 A：第一版是否暴露 `SetMode(camera-only / global)`？

| 选项 | 说明 | 推荐 |
|------|------|------|
| **A1 全局 blur 单一模式** | 仅 per-pixel 全局 blur（含相机 + 物体）。第一版 API 最简，11 函数 | ⭐ 推荐 |
| A2 含 camera-only 切换 | 需要 G-buffer 拆分 camera velocity / object velocity，工作量翻倍，第一版不该做 |

**默认选 A1**。

#### 决策 B：strength 参数语义

| 选项 | 说明 | 推荐 |
|------|------|------|
| **B1 直接缩放 velocity** | `blurredUV = uv - velocity * strength * t`，1.0 = 物理位移 | ⭐ 推荐 |
| B2 时间窗口（exposure） | `strength = shutter_time / frame_time`，需要 dt 输入 | 需要主循环传 dt，复杂 |

**默认选 B1**（与 SSR temporal alpha 风格一致）。

#### 决策 C：scratch RT 是否独立持有还是复用现有？

| 选项 | 说明 |
|------|------|
| **C1 独立 motionBlurFbo / Tex** | GL33Backend 加 2 个新字段；OnHDREnabled 时创建 | ⭐ 推荐 |
| C2 复用 SSAO blurFbo | 时序冲突（SSAO 已 Process 完）；命名混乱；不推荐 |

**默认选 C1**。

#### 决策 D：合并 phase 还是单独 phase？

| 选项 | 说明 |
|------|------|
| **D1 单独 Phase E.15** | 边界清晰，CI/文档独立 | ⭐ 推荐 |
| D2 合并到 Phase E.14.x | 范围已合 dilation+RG8，再加 motion blur 太杂 |

**默认选 D1**。

---

## 5. 询问用户的关键决策点

仅 1 个问题，其余按默认推进。

**问题**：MotionBlur 的 `velocity scale to blur` 行为，希望第一版怎么处理 velocity scaled clamp？

举例：场景中相机突然回头 90°，单帧 velocity 在 RG8 模式下饱和（被 clamp 到 ±540 px）：

| 选项 | 行为 |
|------|------|
| **E1 直接用饱和后的 velocity** | blur 长度被 clamp，最大 540px。简单且不出错；快速运动时 blur 视觉「断掉」 |
| **E2 在 shader 内额外 clamp 到屏幕对角线 50%** | 防止过长 blur trail 把画面糊死；但限制 strength=4 的玩家自定义 |
| **E3 同时做 max-blur-distance 软限** | 用 `min(velocity_pixels, screenDiag * 0.3)` 软限。最稳 | ⭐ 行业默认 |

我建议 **E3**（与 COD/Frostbite 一致），但需要用户确认是否同意作为第一版默认。

---

## 6. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| 全分辨率 8 采样 1080p 性能预算 ≈ 0.7ms | 中端移动端紧张 | autoEnable=false 默认关；后续 Phase E.15.x 加 1/2 分辨率 |
| RG8 + 大 strength 边缘 banding | 视觉 | strength clamp [0, 4]；用户调高时自检 RG8 警告 |
| ping-pong blit 增加 1 个 fullscreen pass | 带宽 | 唯一选择，无优化空间 |
| velocity 在物体边界处突变 → blur 撕裂 | 视觉 | 复用 Phase E.14 dilation（默认 ON）抑制；第一版不做更复杂的 reconstruction filter |
| SSR Temporal + Motion Blur 双 velocity 消费 | 架构 | velocity buffer 是只读，无冲突；架构验证点 |

---

## 7. 验收标准（与后续 ACCEPTANCE 对齐）

| 项 | 判据 |
|---|------|
| C++ 编译 | CI 6 平台 build success |
| 运行时 smoke | `motion_blur.lua` 11 表面 + 4 段（默认 / round-trip / bad-arg / clamp）0 fail |
| 视觉 | demo_ssr 按 M 切换 ON/OFF 时，快速摄像机切换可见拖影；静止时无伪影 |
| 性能 | 1080p 默认 8 采样 ≤ 1.0ms（GL3.3 桌面） |
| 兼容性 | Phase E.13 / E.14 旧 demo / smoke 视觉无回归 |
| 默认行为 | autoEnable=false，HDR.Enable 不会自动拉起 MotionBlur（用户必须显式 Enable） |

---

## 8. 待办（进入 DESIGN 前）

- [ ] **询问用户**：决策 E（max blur distance 软限）是否同意 E3 作为第一版默认
- [ ] 写 `DESIGN_PhaseE_15.md`（含 mermaid + GLSL 伪码 + ping-pong 数据流）
- [ ] 写 `TASK_PhaseE_15.md`（T1~T7 + 依赖图）
- [ ] Approve 后进入 Automate
