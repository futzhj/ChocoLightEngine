# Phase E.15 Velocity-driven Motion Blur — FINAL 总结

> 6A 工作流终章 · 项目总结报告

---

## 1. Phase 概览

| 项 | 内容 |
|----|------|
| **Phase 名** | E.15 Velocity-driven Motion Blur |
| **基线 commit** | `d145566` (Phase E.14 + Light_Graphics.md HDR 段) |
| **核心交付** | 单 program per-pixel velocity blur，Lua `Light.Graphics.MotionBlur` 子表（11 fn） |
| **代码增量** | 新文件 2 + 改动 7 + smoke 1 + docs 6 |
| **架构亮点** | 复用 Phase E.14 velocity 设施 100%，零回归 |
| **CI run** | ✅ [`25894807417`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25894807417) success 524s (8m44s) |

## 2. 6A 工作流落地

```
Align     → ALIGNMENT_PhaseE_15.md (10/10 决策, 1 用户拍板 E3)
   ↓
Architect → DESIGN_PhaseE_15.md   (13 节, 2 mermaid, GLSL 伪码完整)
   ↓
Atomize   → TASK_PhaseE_15.md    (T1~T7 + 依赖图 + 风险矩阵)
   ↓
Approve   → 用户拍板 "是, 开始 T1~T7 一气实施"
   ↓
Automate  → T1 backend 接口 → T2 shader + GL33 实现 → T3 module
            → T4 HDR 链路 → T5 light_ui + Lua → T6 smoke + demo
            (零调试、零返工)
   ↓
Assess    → ACCEPTANCE / FINAL / TODO + CI
```

## 3. 架构总览

### 3.1 模块拓扑

```
Lua: Light.Graphics.MotionBlur (11 fn 子表)
    ↓
C++ namespace MotionBlurRenderer (motion_blur_renderer.h/cpp)
    ├ state: enabled / autoEnable / fbo / tex / strength / sampleCount
    └ Process(hdrFbo, hdrTex) → backend->DrawMotionBlur(...)
    ↓
RenderBackend 虚接口 (render_backend.h)
    ├ SupportsMotionBlur()
    ├ CreateMotionBlurRT(w, h, &outTex) → fbo
    ├ DeleteMotionBlurRT(fbo, tex)
    └ DrawMotionBlur(sceneTex, velocityTex, mbFbo, mbTex, dstFbo, w, h, strength, sampleCount)
    ↓
GL33Backend 实现 (render_gl33.cpp)
    ├ FS_MOTION_BLUR_SOURCE shader (GLES3 + GL33 双版本)
    ├ programMotionBlur + 8 locMB_* uniforms
    ├ Pass1: bind motionBlurFbo + 全屏 shader 沿 velocity 多采样
    └ Pass2: glBlitFramebuffer motionBlurFbo → dstFbo (覆盖 sceneTex)
```

### 3.2 渲染管线插入位置

```
HDRRenderer::EndScene()
  ├ Bloom::Process
  ├ LensDirt::Process
  ├ Streak::Process
  ├ SSAO::Process
  ├ SSR::Process
  ├ LensFlare::Process
  ├ ★ MotionBlur::Process (Phase E.15 新插入)
  └ DrawTonemapFullscreen → default fb
```

## 4. 实施统计

### 4.1 代码改动

| 文件 | 类型 | 行数变化 | 说明 |
|------|------|---------|------|
| `ChocoLight/include/render_backend.h` | 改 | +44 | 4 个虚接口 |
| `ChocoLight/include/motion_blur_renderer.h` | **新** | +100 | 命名空间声明 |
| `ChocoLight/src/motion_blur_renderer.cpp` | **新** | +180 | 命名空间实现（纯壳模式） |
| `ChocoLight/src/render_gl33.cpp` | 改 | +290 | shader×2 双源 + 字段 + Init + Shutdown + 4 override |
| `ChocoLight/src/hdr_renderer.cpp` | 改 | +14 | include + 4 集成点 |
| `ChocoLight/src/light_ui.cpp` | 改 | +6 | include + Init + Shutdown |
| `ChocoLight/src/light_graphics.cpp` | 改 | +95 | include + 11 个 l_MB_* + mb_funcs + 子表注册 |
| `ChocoLight/CMakeLists.txt` | 改 | +1 | motion_blur_renderer.cpp |
| `.github/workflows/build-templates.yml` | 改 | +4 | phaseE15Smoke 登记 |
| `scripts/smoke/motion_blur.lua` | **新** | +180 | 6 段 PASS/FAIL 验证 |
| `samples/demo_ssr/main.lua` | 改 | +20 | 模块引用 + M 键 + HUD + 清理 |
| **总计代码** | | **+934** | (新 460 + 改 474) |

### 4.2 文档增量

| 文件 | 类型 | 行数 |
|------|------|------|
| `ALIGNMENT_PhaseE_15.md` | 新 | 152 |
| `DESIGN_PhaseE_15.md` | 新 | 892 |
| `TASK_PhaseE_15.md` | 新 | 1069 |
| `ACCEPTANCE_PhaseE_15.md` | 新 | ~150 |
| `FINAL_PhaseE_15.md` | 新（当前） | ~200 |
| `TODO_PhaseE_15.md` | 新 | ~70 |
| `docs/api/Light_Graphics.md` MotionBlur 段 | 改 | +160 |
| **总计文档** | | **+~2700** |

## 5. CI 验证

**Run [`25894807417`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25894807417)** — commit `162bbca` · 524 秒 (8m44s) · 6/6 全绿

| Job | 状态 |
|-----|------|
| build-windows + runtime smoke (motion_blur.lua + 16 现有 phase smoke) | ✅ |
| build-linux + lua syntax (for-loop) | ✅ |
| build-macos + lua syntax (for-loop) | ✅ |
| build-android | ✅ |
| build-ios | ✅ |
| build-web | ✅ |

## 6. 关键技术决策回顾

| # | 决策 | 选择 | 实施验证 |
|---|------|------|---------|
| 1 | 算法 | Per-pixel linear blur（沿 velocity 多采样） | ✅ shader 完成 |
| 2 | 插入位置 | LensFlare 之后、Tonemap 之前 | ✅ hdr_renderer.cpp |
| 3 | scratch RT 归属 | MotionBlurRenderer 持有，backend 内部 ping-pong | ✅ State.fbo+tex |
| 4 | autoEnable 默认 | false | ✅ State 初值 |
| 5 | 分辨率 | 全分辨率（与 sceneTex 同） | ✅ CreateMotionBlurRT |
| 6 | velocity 消费 | 复用 Phase E.14 inline dilation | ✅ DecodeVelocity + SampleVelocityDilated |
| 7 | API surface | 11 函数（5+2+4） | ✅ mb_funcs[] |
| 8 | 错误约定 | luaL_check + clamp | ✅ light_graphics.cpp + State |
| 9 | 联动 API | OnHDREnabled/Disabled/Resized | ✅ MotionBlurRenderer + HDRRenderer |
| 10 | shader profile | GLES3 + GL33 双 source | ✅ FS_MOTION_BLUR_SOURCE × 2 |
| 11 | E3 max blur | screenDiag × 30% (kMaxBlurUV = 0.4243) | ✅ shader 内置 |

**11/11 全部按计划落地，零返工**。

## 7. 用户面操作指南

### 7.1 Lua 启用

```lua
local Gfx = require 'Light.Graphics'
local MB  = Gfx.MotionBlur

-- 必要前置：HDR 已 Enable
Gfx.HDR.Enable(1280, 720)

-- 启用 MotionBlur
if MB.IsSupported() then
    MB.Enable(1280, 720)
    MB.SetStrength(1.5)        -- 加强 50%
    MB.SetSampleCount(16)      -- 高质量
end

-- 关闭
MB.Disable()
```

### 7.2 demo_ssr 按键

| 按键 | 功能 |
|------|------|
| F | SSR ON/OFF |
| K | velocity dilation ON/OFF (Phase E.14) |
| L | velocity format rg16f ↔ rg8 (Phase E.14) |
| **M** | **MotionBlur ON/OFF (Phase E.15 新增)** |
| R | reset 所有参数到默认 |

## 8. 后续 phase 候选

详见 `TODO_PhaseE_15.md` §3。简表：

| 候选 | 价值 | 复杂度 |
|------|------|--------|
| Phase E.15.1 — `HDR.SetVelocityScale` Lua API | RG8 精度调节 | 低 |
| Phase E.16 — Camera-only motion blur | 区分相机/物体运动 | 中 |
| Phase E.17 — 1/2 分辨率优化 | 移动端性能 | 中 |
| Phase E.18 — Independent velocity dilation pass | 多消费者优化 | 低 |
| Phase F.x — Velocity-driven TAA | 抗锯齿 | 高 |

## 9. 总结

Phase E.15 是 Phase E.13/E.14 velocity buffer 子系统的**第一个真正消费者**（除 SSR Temporal 外）。验证了：

1. **架构可扩展**：现有 RenderBackend / HDRRenderer / Lua 子表三层架构能干净接纳新后处理 module
2. **复用率高**：Phase E.14 的 velocity decode / dilation / format / scale 完全复用，零重复
3. **零回归**：默认 autoEnable=false，老 demo / smoke 完全不受影响
4. **下沉合理**：纯壳模式（MotionBlurRenderer 只管 state），GL 调用全部下沉 backend

**Phase E.15 圆满闭环**。后续 phase 应保持此架构纪律。
