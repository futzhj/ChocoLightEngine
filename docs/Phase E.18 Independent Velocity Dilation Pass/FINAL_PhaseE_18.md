# Phase E.18 Independent Velocity Dilation Pass — FINAL

> 6A 工作流 · 阶段 6 · Assess · 项目总结
> 基线：Phase E.17 commit `f8d7e41`

---

## 1. 项目概述

将 SSR Temporal / Motion Blur shader 内的 9-tap max-length velocity dilation 抽出为 **独立 HDR EndScene pass**，多 consumer 场景下避免重复计算。

**核心收益**：SSR + Motion Blur 同开时 velocity fetch 节省 ~50%（81 → 18 fetch / pixel @ N=8）。

**业务零感知**：复用 `HDR.SetVelocityDilation` Lua API；backend silent fallback 保证 legacy / GLES 兼容。

---

## 2. 代码 / 文档统计

### 代码改动

| 类别 | 文件 | 行数（+ / -） |
|------|------|--------------|
| C++ Header | `include/render_backend.h` | +57 / 0 |
| C++ Header | `include/hdr_renderer.h` | +10 / 0 |
| C++ Source | `src/render_gl33.cpp` | +188 / -2 |
| C++ Source | `src/hdr_renderer.cpp` | +85 / -2 |
| C++ Source | `src/motion_blur_renderer.cpp` | +11 / -3 |
| C++ Source | `src/ssr_renderer.cpp` | +9 / -1 |
| Lua smoke | `scripts/smoke/motion_blur.lua` | +9 / 0 |
| Lua demo | `samples/demo_ssr/main.lua` | +2 / -1 |
| Markdown | `docs/api/Light_Graphics.md` | +30 / -7 |
| **代码合计** | — | **+401 / -16** |

### 文档新增

| 文件 | 行数 |
|------|------|
| `docs/Phase E.18/ALIGNMENT_PhaseE_18.md` | 179 |
| `docs/Phase E.18/DESIGN_PhaseE_18.md` | 1230 |
| `docs/Phase E.18/TASK_PhaseE_18.md` | 192 |
| `docs/Phase E.18/ACCEPTANCE_PhaseE_18.md` | ~165 |
| `docs/Phase E.18/FINAL_PhaseE_18.md` | 本文件 |
| `docs/Phase E.18/TODO_PhaseE_18.md` | 待补 |
| **文档合计** | **~2100** |

---

## 3. 关键技术亮点

### 3.1 dilation pass 独立化
- 抽出 `FS_VELOCITY_DILATE` shader（GLES3 + GL33 双 profile，~50 行）
- 输入 raw velocityTex（RG16F/RG8 dual-format）、输出 dilatedTex（永远 RG16F + decoded float）
- 算法完全复刻 SSR/MB shader 内 `SampleVelocityDilated`，零视觉回归

### 3.2 backend state 控制
- 新增 `dilationPassActive_` 字段，HDR EndScene 每帧设置
- `SetDilationPassActive(true)` → SSRTemporal/MotionBlur uVelocityDilation 强制 0（单点采）
- `SetDilationPassActive(false)` → 沿用 velocityDilation 旧路径

### 3.3 双 RT 资源（combined + camera-only）
- 与 Phase E.16 双 velocity MRT 联动
- dilatedVelocityFbo / Tex 始终创建（combined）
- dilatedCameraVelocityFbo / Tex 与 cameraVelocityTex 同条件（仅当 backend 支持 + 已创建）

### 3.4 silent fallback 三层安全网
1. backend 不支持 → HDRRenderer 不创建 dilated RT
2. dilated RT 不存在 → EndScene 跳过 DrawVelocityDilate → dilationActive=false
3. consumer fetch dilatedTex 拿 0 → fallback raw velocityTex + inline 9-tap

业务（Lua / 应用层）完全零感知，零迁移代价。

### 3.5 uTexel 必须 full-res
- 即使 motion blur 半分辨率（E.17）+ dilation pass 在 full-res
- `uTexel = 1.0 / vec2(W, H)` 不变 → 9-tap 物理覆盖一致 → 视觉一致

---

## 4. Phase E 系列累计（HDR 链路）

| Phase | 模块 | Lua API 数 | 主要交付 |
|-------|------|------------|---------|
| E.3 | HDR + 4 tonemap | 12 | ACES / Reinhard / Uncharted2 / Linear |
| E.4 | Bloom pyramid | 15 | 6-mip ping-pong |
| E.5 | Auto Exposure | 18 | Eye Adaptation + 8-bit lum mip |
| E.6 | Lens Dirt + Streak | 23 | Anamorphic flare |
| E.7 | Lens Flare | 21 | Ghost + Halo + CA |
| E.8 | SSAO | 19 | 16-sample hemisphere + bilateral blur |
| E.9~E.12 | SSR | 22 | reflect + blur + temporal |
| E.13~E.14 | Velocity buffer | — | RG16F/RG8 + dilation + reproject |
| E.15 | Motion Blur | 11 | velocity-driven per-pixel |
| E.16 | Camera-only Motion Blur | 2 | mode 0/1/2 + dual MRT |
| E.17 | Half-res Motion Blur | 2 | VRAM -75% / perf ~4× |
| E.18 | Velocity Dilation Pass | 0 | shared 9-tap, ~50% fetch save |
| **累计** | **HDR 12 剑客** | **145** | **后处理管线收尾** |

---

## 5. Phase E.18 与前序 Phase 的关系

```
E.13 velocity buffer (RG16F, 单 MRT)
      ↓
E.14 dilation + RG8 dual-format (shader 内 inline 9-tap)
      ↓
E.15 motion blur (复用 inline 9-tap)
      ↓
E.16 camera-only MRT (复用 inline 9-tap × 2 通道)
      ↓
E.17 half-res motion blur (RT 缩小但 9-tap 仍 inline)
      ↓
★ E.18 dilation pass 抽出 (consumer 单点采, ~50% 节省) ★
```

Phase E.18 的价值在多 consumer 并存时显现（SSR Temporal + Motion Blur），与 E.16/E.17 完全正交、独立优化。

---

## 6. Lua API 累积（Phase E.18 无新增）

复用现有 API：
- `Light.Graphics.HDR.SetVelocityDilation(bool)` — 唯一控制开关
- `Light.Graphics.HDR.GetVelocityDilation() → bool` — 状态查询

行为升级（透明）：
- dilation=ON + backend 支持 → 自动走共享 dilation pass
- dilation=ON + backend 不支持 → 自动 fallback inline 9-tap
- dilation=OFF → 单点采样（零 dilation 计算）

---

## 7. 已知限制 / 未来方向

### 短期可选项
1. **dilation pass 半分辨率（E.18.1 候选）**：VRAM -75%、性能 +4×，但邻域覆盖减半（需实验验证视觉差异）
2. **dilation pass 细粒度 toggle**：SSR / MB 独立控制（当前全局共用）

### 长期可选项
3. **TAA 主管线集成**：dilation pass 输出可服务于未来 TAA（不只是 SSR Temporal）
4. **Separable 2-pass dilation**：max-length 算法不能 separable，但若改 average dilation 可拆 N+N（性能 +4.5×）

---

## 8. CI 状态

| 平台 | 状态 | 备注 |
|------|------|------|
| build-windows | ✅ success | runtime smoke 24 PASS + 16 phase 零回归 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [`25900086693`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25900086693)
Commit hash: `e894834`
Date: 2026-05-15 04:22 UTC → 04:29 UTC
Total duration: **7 min** (6/6 平台并行)

---

## 9. 工程反思

### 做得好的地方
- **零 API 变动**：完全复用 `HDR.SetVelocityDilation`，业务零迁移
- **三层 silent fallback**：保证 legacy 后端 / 编译失败 / RT 不可用时业务零感知
- **完整算法复刻**：dilation pass 与 inline 9-tap 数值完全一致，无视觉回归风险
- **6A 文档完备**：~2100 行文档覆盖 align→architect→atomize→approve→automate→assess

### 可改进点
- **未做 GPU pipeline trace 实测**：性能收益基于理论分析，未在真机上验证（建议后续做 Tracy/RGP 测量）
- **单 consumer 场景略亏**：用户文档清晰说明，但未来可加运行时自动判断（仅 1 个 consumer 启用时 silent skip dilation pass）
- **dilation pass 不参与 motion blur 半分辨率**：当前 dilation 永远 full-res，但未来可与 motion blur 同尺寸节省 VRAM

### 经验沉淀
- **多 consumer 场景下抽公共子计算是稳赢策略**：dilation 是典型例子，未来 SSR/SSAO/Motion Blur 等场景的 normal 重建、view-space depth 重建可比照
- **dilatedTex 永远 RG16F 简化了 consumer 端逻辑**：consumer 单点采 .rg 直读 decoded float，无需 dual-format decode 分支
- **backend `dilationPassActive_` state 是 shader 路径切换的关键**：让 shader 端零改动，仅靠 uniform 在两条路径间切换
