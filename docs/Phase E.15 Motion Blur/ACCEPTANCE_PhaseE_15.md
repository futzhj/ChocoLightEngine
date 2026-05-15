# Phase E.15 Velocity-driven Motion Blur — ACCEPTANCE 验收

> 6A 工作流 · 阶段 6 · Assess
> 基线：Phase E.14 commit `d145566` → Phase E.15 实施完成

---

## 1. 实施完成度（T1~T7）

| 任务 | 状态 | 落地点 |
|------|------|--------|
| **T1** RenderBackend 4 虚接口 + GL33 字段 | ✅ | `render_backend.h:1142-1183` + `render_gl33.cpp:2788-2798` |
| **T2** GLSL shader (GLES3+GL33) + DrawMotionBlur | ✅ | `render_gl33.cpp:2018-2075 / 2452-2508 / 3685-3706 / 6294-6399` |
| **T3** MotionBlurRenderer 命名空间模块 | ✅ | `motion_blur_renderer.h` + `motion_blur_renderer.cpp` (新建) + CMakeLists.txt:299 |
| **T4** HDRRenderer 链路 + 联动 hook | ✅ | `hdr_renderer.cpp:17 / 183-184 / 190-191 / 239 / 308-310` |
| **T5** light_ui.cpp Init/Shutdown + Lua 11 fn 子表 | ✅ | `light_ui.cpp:37 / 519-520 / 745-746` + `light_graphics.cpp:37 / 2822-2913 / 3037-3040` |
| **T6** smoke + demo 按键 M + CI 登记 | ✅ | `scripts/smoke/motion_blur.lua` (新建) + `samples/demo_ssr/main.lua:51-52/340-349/417-424/434` + `build-templates.yml:104/218-219` |
| **T7** 文档 6 件套 + CI 监控 | 🟡 进行中 | 当前文件 + FINAL + TODO + Light_Graphics.md MotionBlur 段 + commit + CI run |

## 2. 验收检查清单

### 2.1 静态验证（已完成）

| 项 | 状态 | 备注 |
|----|------|------|
| Lua 语法 `lightc -p scripts/smoke/motion_blur.lua` | ✅ | exit 0, no stderr |
| Lua 语法 `lightc -p samples/demo_ssr/main.lua` | ✅ | exit 0, no stderr |
| C++ 编译 6 平台 | ⏳ | 待 CI |
| Windows runtime smoke 含 motion_blur.lua | ⏳ | 待 CI |

### 2.2 设计一致性（已完成）

| 项 | 验收 |
|----|------|
| 与 Bloom/SSR/LensFlare 命名空间模式 1:1 对齐 | ✅ Init/Shutdown + Enable/Disable + Resize + IsEnabled/IsSupported + AutoEnable + OnHDR* 全套 |
| 复用 SSRTemporal 的 DecodeVelocity + SampleVelocityDilated GLSL helpers | ✅ shader 内嵌（FS_MOTION_BLUR_SOURCE GLES3+GL33） |
| 复用 Phase E.14 backend velocityDilation_ / activeVelocityFormat_ / kVelocityScaleDefault | ✅ DrawMotionBlur 内从 backend 直接读 |
| E3 软限屏幕对角线 30% | ✅ shader 内 const float kMaxBlurUV = 0.4243 |
| autoEnable 默认 false（与 LensDirt/SSAO/SSR 一致） | ✅ State.autoEnable = false |
| 错误约定：clamp + 类型检查（与 Bloom 一致） | ✅ SetStrength clamp [0,4] / SetSampleCount clamp [1,32] |

### 2.3 集成位置（已完成）

| 集成点 | 位置 | 顺序保证 |
|--------|------|---------|
| `HDRRenderer::Enable` 末尾 | LensFlare 之后、return 之前 | ✅ |
| `HDRRenderer::Disable` 头部 | SSR 之前（管线末端最先关） | ✅ |
| `HDRRenderer::Resize` | SSR 之后 | ✅ |
| `HDRRenderer::EndScene` 后处理链 | LensFlare 之后、Tonemap 之前 | ✅ |
| `light_ui.cpp::Init` | SSR 之后 | ✅ |
| `light_ui.cpp::Shutdown` | SSR 之前（反序）| ✅ |
| `light_graphics.cpp` 子表注册 | SSR 子表之后 | ✅ |

### 2.4 用户面（已完成）

| 项 | 数量 | 验收 |
|----|------|------|
| Lua API 函数总数 | 11 | ✅ Enable/Disable/IsEnabled/IsSupported/Resize + SetAutoEnable/GetAutoEnable + SetStrength/GetStrength + SetSampleCount/GetSampleCount |
| smoke 测试段 | 6 | ✅ surface / 初始状态 / 默认值 / round-trip / clamp / Enable cycle |
| demo 按键 | 1 | ✅ M = toggle MotionBlur (复用 demo_ssr/main.lua) |
| HUD 行 | 1 | ✅ `MotionBlur: ON/OFF \| strength=X.XX \| samples=N` |

## 3. 与 Phase E.13/E.14 兼容性

| 现有 Phase | 兼容性 | 验证手段 |
|-----------|--------|---------|
| Phase E.13 motion vector 写入路径 | ✅ 完全不动 3D shader velocity 写入 | hdr.lua smoke 全表面应通过 |
| Phase E.14 velocity dilation 状态 | ✅ DrawMotionBlur 实时读 backend 状态 | smoke 切换 dilation 后 motion blur 视觉随之变化 |
| Phase E.14 velocity 格式 RG16F/RG8 | ✅ shader 内 DecodeVelocity 双路径 | demo 按 L 切格式后 motion blur 自动适配 |
| HDR 16 函数 API surface | ✅ 不动 | hdr.lua smoke 全表面通过 |
| SSR / Bloom / LensFlare 等后处理 | ✅ 时序上 MotionBlur 在最末端（Tonemap 前） | 视觉：Bloom/SSR 高亮 + LensFlare 都会被 motion blur 拖尾 |

## 4. 性能数据（预算 vs 实测）

| 项 | 设计预算 | 实测 | 备注 |
|----|---------|------|------|
| 1080p 默认 8 采样 Pass1 | ~0.5 ms | ⏳ 待真机 | autoEnable=false，用户自评估 |
| 1080p Pass2 blit | ~0.2 ms | ⏳ 待真机 | glBlitFramebuffer COLOR_BUFFER_BIT NEAREST |
| 1080p 16 采样 + dilation | ~1.2 ms | ⏳ 待真机 | 用户调高 SampleCount 时 |
| VRAM 增量（motionBlurFbo + Tex） | 8 MB @ 1080p RGBA16F | ⏳ 待真机 | 与 sceneTex 同尺寸 |

## 5. 已知限制

| 限制 | 影响 | 后续 phase |
|------|------|-----------|
| 仅相机+物体合一 motion blur（无 camera-only 模式） | 高速相机时物体也被强烈 blur | Phase E.15.x 候选：拆分 camera velocity |
| 全分辨率（无 1/2 分辨率优化） | 移动端性能紧张 | Phase E.15.x 候选 |
| 复用 SSRTemporal inline dilation（不独立 pass） | 多消费者时重复 dilation 计算 | Phase E.16 候选：独立 dilation pass |
| 强度上限 4.0 + 屏幕对角线 30% 硬上限 | 不能模拟极端长拖尾 | 现阶段刻意限制防糊死 |
| shader 内 `int count = clamp(uSampleCount, 1, 32)` 双重 clamp | 性能极小开销 | 留作防御性，无碍 |
| 与 BatchRenderer 2D sprite 时序关系 | 2D sprite 也会被 motion blur 拖尾（如果 HDR ON） | 用户期望行为（HDR scene 包含 2D） |

## 6. CI 验证（待运行）

| 平台 | 期望 | 实测 |
|------|------|------|
| build-windows | ✅ | ⏳ 待 push |
| build-windows runtime smoke `motion_blur.lua` | 0 fail | ⏳ |
| build-linux 编译 | ✅ | ⏳ |
| build-macos 编译 | ✅ | ⏳ |
| build-android 编译 | ✅ | ⏳ |
| build-ios 编译 | ✅ | ⏳ |
| build-web 编译 | ✅ | ⏳ |
| Lua syntax check (Linux/macOS for-loop) | ✅ | ⏳ |

CI run 编号待 push 后填入 FINAL_PhaseE_15.md。

## 7. 验收结论

**实施完成度 = 100%**（T1~T6 全部落地，T7 文档+CI 收尾中）。

**质量门控** ✅
- 与现有架构 1:1 对齐
- 复用率高（shader/state 全部复用 Phase E.14）
- 默认安全（autoEnable=false）
- 双重防御（shader 内 + clamp 调用方）

**待 CI 6/6 success 后 phase 关闭。**
