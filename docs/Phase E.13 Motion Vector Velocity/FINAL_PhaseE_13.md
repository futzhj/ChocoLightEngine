# Phase E.13 Motion Vector Velocity — FINAL 项目总结报告

> **任务**：Phase E.13 — Motion Vector Velocity（运动向量速度缓冲）
> **基线**：Phase E.12 Temporal SSR
> **状态**：🟡 源码 + 静态验证完成，等待 CI 与真机视觉确认
> **范围**：HDR velocity RT + 3D 默认 shader velocity 输出 + Animator/Mesh previous-state + SSR Temporal velocity 采样 + 矩阵回退

---

## 1. 一句话总结

Phase E.13 在 Phase E.9~E.12 SSR / HDR 管线之上引入 **Motion Vector Velocity Buffer**：

- ✅ **HDR FBO 第三 attachment**：RG16F velocity（MRT slot 2）
- ✅ **3D 默认 shader 全部输出 velocity**：static / skin / skin+morph 共四类，GL33 + GLES3 双 profile
- ✅ **Animator previous joint / morph cache**：连续 `Update(dt)` 自动累积，时间跳变安全复位
- ✅ **静态 mesh previous model**：`Mesh:Draw(..., prevModel)` + ECS `_mesh_prev_model_cache`
- ✅ **SSR Temporal 用 velocity 优先回投**：缺 velocity 时无缝回退 Phase E.12 matrix reprojection
- ✅ **HDR lifecycle 推进**：`Reset/Commit VelocityHistory` 在 RT 创建/释放/EndScene 接入

---

## 2. 实施过程回顾（6A 工作流）

| 阶段 | 输入 | 输出 | 关键产出 |
|------|------|------|---------|
| Align | 用户继续 Phase E.13 | 需求与边界对齐 | `ALIGNMENT_PhaseE_13.md` |
| Architect | 对齐输出 | 架构图、数据流、接口契约 | `DESIGN_PhaseE_13.md` + `IMPLEMENTATION_PLAN_PhaseE_13.md` |
| Atomize | 设计文档 | T1~T7 原子任务拆分 | `TASK_PhaseE_13.md` |
| Automate | T1~T6 | C++ / GLSL / Lua / docs 实现 | 源码、smoke 与 ECS 改动 |
| Assess | 实现结果 | 验收 / 总结 / TODO | `ACCEPTANCE_PhaseE_13.md` / `FINAL_PhaseE_13.md` / `TODO_PhaseE_13.md` |

---

## 3. 核心改动清单

### 3.1 Backend / GL33

| 文件 | 改动 |
|------|------|
| `ChocoLight/include/render_backend.h` | `CreateHDRFBO` 增加可选 `outVelocityTex`；新增 `GetHDRVelocityTex` / `ResetVelocityHistory` / `CommitVelocityHistory` / `SetNextPreviousModelMatrix`；`DrawSkinnedMeshMaterial` / `DrawSkinnedMorphMeshMaterial` 新增 prev joint/morph 参数；`DrawSSRTemporal` 新增 `velocityTex` 参数；同步注释 |
| `ChocoLight/src/render_gl33.cpp` | HDR FBO RG16F MRT slot 2；`hdrFboVelocityTex` map；`UploadVelocityUniforms`；`ResetVelocityHistory/CommitVelocityHistory/SetNextPreviousModelMatrix` 实现；PBR/Unlit/Skin/Morph shader 双 profile 输出 velocity；`uPrevJointMatrices` UBO；SSR Temporal shader 新增 `uVelocityTex/uHasVelocityTex` |

### 3.2 HDR / SSR / Renderer

| 文件 | 改动 |
|------|------|
| `ChocoLight/src/hdr_renderer.cpp` | `CreateRT` 请求 velocityTex；`CreateRT/ReleaseRT` 调 `ResetVelocityHistory`；`EndScene` 调 `CommitVelocityHistory` |
| `ChocoLight/include/hdr_renderer.h` | 新增 `GetVelocityTexture()` |
| `ChocoLight/src/ssr_renderer.cpp` | `Process` 传 `backend->GetHDRVelocityTex(hdrFbo)` 给 Temporal pass；注释同步 |
| `ChocoLight/include/ssr_renderer.h` | 注释同步 Temporal pass 签名变化 |

### 3.3 Animation / Mesh / ECS

| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_animation.cpp` | `Animator` 新增 `prevJointMatrices/prevMorphWeights/velocityHistoryValid`；`Update` 复制 prev；时间跳变/状态变更复位；GPU skin/morph draw 路径同时上传 prev joint/morph |
| `ChocoLight/src/light_graphics_mesh.cpp` | `Mesh:Draw` 支持 `(prevMat)` / `(textureId, prevMat)` / `(material, prevMat)`；非法 prev table 报错；旧调用兼容 |
| `ChocoLight/src/light_ecs.cpp` | `_DrawMesh(entity, tf, mr, gfx)` 缓存 world model；隐藏/缺 mesh 时清缓存；第二帧起向 `mesh:Draw` 传 prev model |

### 3.4 Smoke / 文档

| 文件 | 改动 |
|------|------|
| `scripts/smoke/material_3d.lua` | `mesh:Draw(prevMat)` / `(0, prevMat)` / `(material, prevMat)` 静态覆盖 |
| `scripts/smoke/ecs_render.lua` | D-T5+：mock mesh 验证 ECS 第二帧把第一帧 model matrix 传入 `mesh:Draw` |
| `docs/Phase E.13 Motion Vector Velocity/ACCEPTANCE_PhaseE_13.md` | 验收文档 |
| `docs/Phase E.13 Motion Vector Velocity/FINAL_PhaseE_13.md` | 本文 |
| `docs/Phase E.13 Motion Vector Velocity/TODO_PhaseE_13.md` | 后续动作清单 |

---

## 4. 技术设计重点

### 4.1 数据流

```text
[Animator Update]
    ├ prev = current (joint/morph)
    └ current = ComputeJointMatrices(...)

[Lua/ECS Draw 调用]
    ├ static mesh : mesh:Draw(material, prevModelMat4)
    │      └ SetNextPreviousModelMatrix(prevModel) → DrawMeshMaterial
    └ skinned mesh: Animation.DrawSkinnedMesh(...)
           └ bake modelMat × jointMat / prevJointMat → DrawSkinned[Morph]MeshMaterial

[GL33 backend]
    ├ uPrevViewProj / uPrevModel / uPrevJointMatrices / uPrevMorphWeights
    ├ uHasVelocityHistory（首帧/重置后为 0）
    └ MRT slot 2 写入 RG16F velocity = (curUV - prevUV)

[HDRRenderer]
    ├ Begin/EndScene 间所有 3D draw 写 velocity
    └ EndScene 末尾 CommitVelocityHistory()（推进 prevViewProj）

[SSRRenderer Temporal pass]
    └ uHasVelocityTex == 1 → prevUV = vUV - velocity
       否则保留 Phase E.12 reverse depth reprojection
```

### 4.2 关键不变量

- HDR FBO 创建失败时不会进入 velocity 路径；旧 backend 仍可用 `CreateHDRFBO` 而不要求 velocity。
- `uHasVelocityHistory=0` 时 shader 强制输出 `(0,0)`，**永远不会**在首帧引入伪 velocity。
- `Animator.velocityHistoryValid` 仅在 `Update(dt)` 自然推进后置 true；任何时间跳变/状态切换都重置。
- `SetNextPreviousModelMatrix` 是 **one-shot**：每次 mesh draw 调用后由 backend 自动清空。
- ECS `MeshRenderer.visible == false` 或 `mr.mesh == nil` 强制清空缓存，重新可见后从“无 prev” 重新开始。
- SSR Temporal 在 velocityTex=0 时与 Phase E.12 行为完全一致，保留向后兼容。

### 4.3 API 兼容

| 接口 | 兼容性说明 |
|------|-----------|
| `RenderBackend::CreateHDRFBO` | velocity 出参默认 `nullptr`，旧调用不变 |
| `RenderBackend::DrawSkinnedMesh[Morph]Material` | 新增 prev joint/morph 参数有默认值 `nullptr/0` |
| `RenderBackend::DrawSSRTemporal` | 新增 `velocityTex`，传 0 时行为不变 |
| `Mesh:Draw` | 旧 `mesh:Draw()` / `mesh:Draw(0)` / `mesh:Draw(material)` 完全兼容 |
| `Animation.DrawSkinnedMesh` | Lua 入口签名不变；prev pose 完全由 Animator 内部管理 |
| `ECSWorld:_DrawMesh` | 新增 entity 参数后只在 ECS 内部使用，无 Lua 入口破坏 |

---

## 5. 质量评估

### 5.1 代码质量

| 维度 | 评价 | 备注 |
|------|------|------|
| 架构一致性 | ✅ | 沿用 RenderBackend 虚接口 + HDRRenderer / SSRRenderer 模块模式 |
| 向后兼容 | ✅ | 所有新参数有默认值/可选；旧 API 与 smoke 不破坏 |
| 资源管理 | ✅ | velocity texture 由 HDR FBO 同生命周期管理；`hdrFboVelocityTex` map 在 `DeleteHDRFBO` 同步清理 |
| 防御性 | ✅ | `uHasVelocityHistory` gate、`velocityHistoryValid` 复位、用户 shader 路径清 one-shot |
| 注释/文档同步 | ✅ | `render_backend.h` / `ssr_renderer.h` / `ssr_renderer.cpp` 注释更新；新增 ACCEPTANCE/FINAL/TODO |

### 5.2 测试质量

| 测试维度 | 覆盖 |
|----------|------|
| `Mesh:Draw` 参数兼容 | `material_3d.lua` 覆盖旧调用 + 三种 prev 模式 |
| ECS 静态 MeshRenderer prev | `ecs_render.lua` D-T5+ 验证第二帧 prev model |
| 静态 + 动态 mesh velocity 上传路径 | 源码 grep + 静态一致性检查 |
| 蒙皮 prev pose 复位 | 源码层面覆盖；smoke 暂未直接验证 |
| 真实窗口视觉 | ⏳ 需用户运行 demo_ssr 等并对比 |
| CI runtime smoke | ⏳ 待 CI 触发 |

---

## 6. 验证状态

| 项 | 状态 |
|---|---|
| 源码静态一致性 | ✅ 已完成 |
| 文档一致性 | ✅ 已同步（ACCEPTANCE / FINAL / TODO） |
| 本地 CMake build | 🚫 按用户偏好不执行 |
| 本地 `light.exe` smoke | 🚫 按用户偏好不执行 |
| `lightc -p` Lua 语法检查 | ✅ `scripts/smoke/material_3d.lua` + `scripts/smoke/ecs_render.lua` 通过 |
| `git diff --check` | ✅ 通过 |
| CI 6 平台 build | ⏳ 未触发（待提交） |
| Windows runtime smoke | ⏳ 待 CI |
| 真实窗口视觉验收 | ⏳ 等待用户在桌面 GL3.3 环境下确认 demo_ssr |

---

## 7. 已知限制

| 限制 | 影响 | 后续方向 |
|------|------|----------|
| CPU skin / CPU morph 路径未写 velocity | CPU 回退路径下 velocity 缺失，shader 仍按矩阵 fallback | 后续可在 CPU 回退里手工组装 velocity，或直接禁用 CPU 路径下的 Temporal |
| 用户 shader 路径不强制输出 velocity | 第三方自定义 shader 不会写入 velocity | 文档提示；引擎默认 shader 始终覆盖 |
| 真实窗口视觉无 CI 自动回归 | 只能依赖 smoke + 用户手测 | 后续可引入截图 diff / CI headless GL |
| velocity texture 增加 VRAM | 1080p 约 +8MB（RG16F），与 Phase E.12 history RT 叠加 | 移动端可考虑半精度或 conditional disable |
| `SetNextPreviousModelMatrix` 是 one-shot | 调用方必须每帧设置；少调一次就退化到 `prevModel = curModel` | 设计如此，避免缓存悬挂；ECS / Lua 高层 API 已封装 |

---

## 8. 结论

Phase E.13 Motion Vector Velocity 完成了源码、文档、静态/Lua 语法验证；目前差异于 Phase E.12 的部分主要在外部验证：CI 编译 + Windows runtime smoke + 真机视觉。

整体上，velocity buffer 已经成为 ChocoLight 渲染管线的可选基础设施：

- Temporal SSR 在动态相机/物体场景下能用更准确的 velocity 重投影
- Animator 与 ECS 已带上 previous-state，能产生 GPU velocity
- 默认 shader 全覆盖，缺 prev 数据时 graceful 退化
- 任何旧 demo / smoke / Lua 调用都保持兼容

后续推进见 `TODO_PhaseE_13.md`。
