# Phase E.13 Motion Vector Velocity — TODO 清单

> 用于追踪 Phase E.13 实施之后剩余的工作项，区分 **必须** / **建议** / **后续阶段**。

---

## 1. 必须项（合入前应完成）

### 1.1 提交本轮改动并触发 CI — ✅ 完成（commit `04d312a`，run `25889613200`）

- **范围**：以下文件 + Phase E.13 文档目录
  - `ChocoLight/include/hdr_renderer.h`
  - `ChocoLight/include/render_backend.h`
  - `ChocoLight/include/ssr_renderer.h`
  - `ChocoLight/src/hdr_renderer.cpp`
  - `ChocoLight/src/light_animation.cpp`
  - `ChocoLight/src/light_ecs.cpp`
  - `ChocoLight/src/light_graphics_mesh.cpp`
  - `ChocoLight/src/render_gl33.cpp`
  - `ChocoLight/src/ssr_renderer.cpp`
  - `scripts/smoke/ecs_render.lua`
  - `scripts/smoke/material_3d.lua`
  - `docs/Phase E.13 Motion Vector Velocity/`（4 份规划 + 3 份验收文档）
- **建议拆分**（与 `TASK_PhaseE_13.md` §3 对齐）：
  1. `feat: add HDR velocity buffer backend plumbing`（T1）
  2. `feat: write velocity from 3D shaders`（T2）
  3. `feat: track previous animation pose for velocity`（T3）
  4. `feat: wire previous transforms into velocity draws`（T4）
  5. `feat: use velocity buffer in temporal SSR`（T5）
  6. `test: cover velocity wiring in mesh + ECS smoke`（T6）
  7. `docs: finalize Phase E.13 acceptance`（T7）
- **推送目标**：仅 `origin`（`https://github.com/futzhj/ChocoLightEngine.git`）

### 1.2 CI 6 平台 build + Windows runtime smoke — ✅ 完成（run `25889613200`，6/6 success，耗时 7m26s）

- GitHub Actions run：6 平台均 success
- Windows runtime smoke：`material_3d.lua` / `ecs_render.lua` 0 fail
- 链接：https://github.com/futzhj/ChocoLightEngine/actions/runs/25889613200

### 1.3 真实窗口视觉验收 — ⏳ 待用户执行

- 在桌面 GL3.3 环境运行
  - `samples/demo_ssr/main.lua`：T/U/I/N 控制 Temporal，对比开/关 velocity（如果 demo 没有显式开关，可临时关闭 HDR 重新启用模拟）
  - 含动画角色（`samples/demo_animation` 或 `samples/demo_morph_target`）的场景：相机/角色快速运动时 SSR 不应再出现 Phase E.12 的拖影
- 输出：截图或屏录留存到 `docs/Phase E.13 Motion Vector Velocity/`（可选）

---

## 2. 建议项

### 2.1 demo 与文档增量 — ✅ 主要项已完成

- ✅ `samples/demo_ssr/main.lua`：HUD 新增 Velocity buffer 状态行（含 Temporal ON/OFF 联动文案）
- ✅ `docs/api/Light_Graphics.md`：新增 `Mesh.New` + `mesh:Draw([textureOrMaterial], [prevModelMat4])` 段（含 Phase E.13 prev model 参数、ECS 集成、示例）
- ✅ `docs/api/Light_Animation.md`：`DrawSkinnedMesh` 段下新增 "Phase E.13 — Velocity buffer 与 previous pose 自动管理" 小节，覆盖内部缓存字段与复位触发清单
- ⏳ `docs/API_REFERENCE.md`：未改动；当前 SSR Temporal 段仍按 Phase E.12 描述（fallback 路径仍可用）。后续若做 API surface 增量再统一同步

### 2.2 smoke 增强 — ✅ 完成

- ✅ `scripts/smoke/animation.lua` 新增 [18] 段，间接验证 Phase E.13 velocity history 复位路径：
  - `Animator:Update(dt)` 连续累积 prev pose（产出合法 jointMatrices）
  - `SetCurrentTime` / `Play` / `Stop` / 立即 transition (`duration=0`) / `SetMorphWeight` / `ClearMorphWeights` 均触发内部复位且不崩
  - 大时间跳变后 jointMatrices 仍合法
- C++ 侧 6 处 `ResetAnimatorVelocityHistory` 调用与 smoke 断言一一对应（已 grep 验证）
- `ssr.lua` 暂未追加 velocity 行为隔离断言（Lua 端无 `HDR.HasVelocityTexture()` getter；纯 Lua 不易精确验证）

---

## 3. 后续阶段候选（不在本期范围）

| 候选 | 动机 | 备注 |
|------|------|------|
| CPU skin / CPU morph velocity 写入 | 当前 CPU fallback 不产生 velocity | 可在 `DrawSkinned[Morph]MeshCPU` 末尾用 prev 顶点二次上传至 secondary VBO，或在 fragment 阶段用 matrix fallback |
| Velocity 驱动 motion blur | 利用现有 RG16F velocity 做后处理 | 在 HDR 后处理链中插入 directional blur |
| Velocity 驱动 TAA | 取代 Phase E.12 reverse depth 作为整帧 TAA | 需要新的 history color RT + dilation pass |
| Roughness-aware Temporal | 高 roughness 像素用 velocity，低 roughness 像素用 matrix | 需要把 roughness 透传到 Temporal shader |
| Velocity dilation | 几何边缘 motion vector 抗错 | 1-pixel max filter |
| 半精度 velocity 格式 | 移动端 VRAM | RG16F → RG8 + scale，需做精度评估 |
| Headless GL CI 视觉 diff | 自动回归 | 引入虚拟 framebuffer + 截图 diff |
