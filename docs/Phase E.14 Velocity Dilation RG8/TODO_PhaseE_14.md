# Phase E.14 Velocity Dilation + RG8 Format — TODO 清单

> 用于追踪 Phase E.14 实施之后剩余的工作项，区分 **必须** / **建议** / **后续阶段**。

---

## 1. 必须项（合入前应完成）

### 1.1 提交本轮改动并触发 CI — ✅ 完成（commit `f7150c0`，run `25892207578`）

- **范围**：以下文件 + Phase E.14 文档目录
  - `ChocoLight/include/render_backend.h`
  - `ChocoLight/include/hdr_renderer.h`
  - `ChocoLight/src/render_gl33.cpp`
  - `ChocoLight/src/hdr_renderer.cpp`
  - `ChocoLight/src/ssr_renderer.cpp`
  - `ChocoLight/src/light_graphics.cpp`
  - `samples/demo_ssr/main.lua`
  - `scripts/smoke/hdr.lua`
  - `docs/Phase E.14 Velocity Dilation RG8/`（ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO）
- **建议 commit message**：
  ```
  feat: add Phase E.14 velocity dilation + RG8 format
  ```
- **推送目标**：仅 `origin`（`https://github.com/futzhj/ChocoLightEngine.git`）

### 1.2 CI 6 平台 build + Windows runtime smoke — ✅ 完成（run `25892207578`，6/6 success，耗时 9m17s）

- GitHub Actions run：6 平台均 success
- Windows runtime smoke：`hdr.lua` 16 functions + §8 段 6 检查 0 fail
- 链接：https://github.com/futzhj/ChocoLightEngine/actions/runs/25892207578

### 1.3 真实窗口视觉验收 — ⏳ 待用户执行（唯一剩余必须项）

- 在桌面 GL3.3 环境运行 `samples/demo_ssr/main.lua`
  - **K**：切换 dilation ON/OFF，对比几何边缘 SSR 反射的拖影/halo
  - **L**：切换 RG16F ↔ RG8，对比 VRAM 与视觉细微差异（理论上 RG8 在快速运动场景边缘有 ≤2 像素精度损失）
  - **T**：开启 Temporal（Phase E.12）后再切 K/L 才能完整看到 dilation/format 影响
- 输出：截图或屏录留存到 `docs/Phase E.14 Velocity Dilation RG8/`（可选）

---

## 2. 建议项（不阻塞合入）

### 2.1 demo 与文档增量

- ✅ `samples/demo_ssr/main.lua`：HUD 行扩展为 `Velocity: <fmt> | dilation=<ON/OFF> | reproj=<...>`，K/L 按键已加
- ✅ `scripts/smoke/hdr.lua`：§8 段已覆盖默认值/round-trip/bad-arg/case-sensitive
- ⏳ `docs/api/Light_Graphics.md`：HDR 子表段需补 4 个 API 文档（与 Phase E.13 同样模式补完）
- ⏳ `docs/Phase E.13 Motion Vector Velocity/TODO_PhaseE_13.md`：§3 候选清单标记 dilation/RG8 → Phase E.14 已完成

### 2.2 智能默认进一步收紧（可选）

- `HDR.SetVelocityScale(float)` Lua API：当前 scale 固定 0.25；如真机评估 RG8 在某些场景精度不足，可暴露用户调（clamp [0.05, 1.0]）
- HDR initialization 时 backend 支持 `GL_RG8` 检测：当前直接信任驱动；可加 `glCheckFramebufferStatus` 实测 + 失败时退化 RG16F + warn

---

## 3. 后续阶段候选（不在本期范围）

| 候选 | 动机 | 备注 |
|------|------|------|
| Velocity 驱动 motion blur | 利用 RG16F/RG8 velocity 做后处理 | Phase E.13 §3 候选；现可消费完整 velocity |
| Velocity 驱动 TAA | 取代 Phase E.12 reverse depth 作为整帧 TAA | 需 history color RT + neighborhood clip + camera jitter |
| Roughness-aware Temporal SSR | 高 roughness 用 velocity，低 roughness 用 matrix | 需 GBuffer roughness 透传到 Temporal shader |
| 独立 velocity dilation pass | velocity 多消费者时（motion blur + TAA + SSR）共享 dilated RT | 需 1 个新 RT + 1 个 fullscreen pass |
| CPU skin / morph velocity | 当前 CPU fallback 不写 velocity | Phase E.13 / Phase E.14 共同遗留 |
| Headless GL CI 视觉 diff | 自动回归边缘 halo / 像素级精度 | 需虚拟 framebuffer + 截图 diff 框架 |
| `HDR.SetVelocityScale(f)` API | 用户调 scale | 见 §2.2 |
| RG8 自适应 scale | 按历史 max velocity 动态调 | 复杂度高 |

---

## 4. 已知限制（Phase E.14 不修复，列出供未来参考）

- `kVelocityScale = 0.25` 固定，超出 ±0.25 UV / frame 的运动在 RG8 模式下被 clamp
- dilation 是 shader inline 实现，多消费者场景会重复采样
- RG8 精度 ≈ 2 像素 / 1080p，极慢运动可能出现细微 ghost
- 用户自定义 shader 写 velocity 仍走 raw 路径，与默认 4 个 shader 不一致
