# Phase E.13 Motion Vector Velocity — ACCEPTANCE 验收文档

> **任务名**：Phase E.13 Motion Vector Velocity（运动向量速度缓冲）
> **状态**：🟡 **源码与静态验证完成，等待 GitHub Actions CI 编译/运行时验证**
> **基线**：Phase E.12 Temporal SSR（depth reverse-reprojection，无 velocity buffer）
> **方案**：HDR FBO RG16F velocity attachment + 全部 3D shader velocity 输出 + Animator/Mesh previous-state 缓存 + SSR Temporal velocity 采样 + 矩阵回退

---

## 1. 任务完成度总览

| 阶段 | 内容 | 状态 |
|------|------|------|
| Align | `ALIGNMENT_PhaseE_13.md` | ✅ |
| Architect | `DESIGN_PhaseE_13.md` + `IMPLEMENTATION_PLAN_PhaseE_13.md` | ✅ |
| Atomize | `TASK_PhaseE_13.md`（T1~T7） | ✅ |
| T1 Backend HDR velocity RT | `CreateHDRFBO` 可选 `outVelocityTex` + `GetHDRVelocityTex` + GL33 RG16F MRT slot 2 + 释放路径 | ✅ |
| T2 Shader velocity outputs | PBR / Unlit / Skin / Skin+Morph 双 profile（GL33 + GLES3）输出 `FragVelocity`，gated by `uHasVelocityHistory` | ✅ |
| T3 Animator prev pose | `prevJointMatrices` / `prevMorphWeights` / `velocityHistoryValid`，`Update()` 复制当前帧到 prev，时间跳变/状态变更复位 | ✅ |
| T4 Draw API prev-state wiring | `SetNextPreviousModelMatrix` one-shot；`Mesh:Draw([tex\|material], [prevMat])`；GPU skin/morph backend 接收 prev joint/morph | ✅ |
| T5 SSR Temporal velocity sampling | `DrawSSRTemporal(..., velocityTex, ...)` + `uVelocityTex` / `uHasVelocityTex`；matrix reprojection 作为 fallback | ✅ |
| T6 smoke / demo / docs | `material_3d.lua` + `ecs_render.lua` 静态覆盖；ECS `MeshRenderer` previous model cache；本验收/FINAL/TODO 文档 | 🟡 |
| T7 Static verification + CI | `git diff --check` + `lightc -p` 已通过；CI 未启动 | 🟡 |

> 🟡 表示静态层已完成，CI 编译 + runtime smoke 与真机视觉验收仍待执行。

---

## 2. 关键实现验收

### 2.1 RenderBackend 接口与 GL33 实现

| 项 | 验收点 | 状态 |
|---|---|---|
| `CreateHDRFBO` | 新增可选 `outVelocityTex` 出参；旧调用兼容（参数有默认 `nullptr`） | ✅ |
| `GetHDRVelocityTex` | 新增虚函数；GL33 通过 `hdrFboVelocityTex` map 返回 RG16F texture id | ✅ |
| `DeleteHDRFBO` | 同步释放 velocity texture，map 条目同步删除 | ✅ |
| `ResetVelocityHistory` | 把 `prevViewProj` 初始化为当前 view-proj 并清 `hasPrevViewProjForVelocity` / `hasNextPrevModel` | ✅ |
| `CommitVelocityHistory` | 帧末保存当前 view-proj，置 `hasPrevViewProjForVelocity=true` | ✅ |
| `SetNextPreviousModelMatrix` | 设置 one-shot prev model；下一次 mesh draw 后自动失效 | ✅ |
| MRT 配置 | `glDrawBuffers` 在 velocity 可用时设置 3 个 attachment（color + normal + velocity） | ✅ |

### 2.2 默认 shader velocity 输出

| 项 | 验收点 | 状态 |
|---|---|---|
| PBR / Unlit static | 顶点输出 `vCurClip` / `vPrevClip`，片元 `FragVelocity = (curUV - prevUV)` | ✅ |
| GPU Skin | 当前帧 joint palette + previous frame `uPrevJointMatrices` UBO（无 prev 时回退当前 joint） | ✅ |
| GPU Skin+Morph | current/prev joint palette + current/prev morph weights，与 CPU 视觉路径一致 | ✅ |
| 双 profile | GLES3 (`#version 300 es`) 与 GL33 (`#version 330 core`) 同步更新 | ✅ |
| 防 NaN | `uHasVelocityHistory=0` 时 velocity 强制 `(0,0)` | ✅ |
| MRT 不破坏 | normal MRT slot 1 行为保持不变 | ✅ |

### 2.3 Animator previous pose cache

| 项 | 验收点 | 状态 |
|---|---|---|
| 字段 | `Animator.prevJointMatrices` / `prevMorphWeights` / `velocityHistoryValid` | ✅ |
| `Animator:Update(dt)` | 推进前复制当前 joint/morph 到 prev，`velocityHistoryValid = !prev empty` | ✅ |
| 时间跳变 | `SetCurrentTime` / `Play` / `Stop` / 立即 transition / `SetMorphWeight` / `ClearMorphWeights` 调用 `ResetAnimatorVelocityHistory` | ✅ |
| GPU 上传 | 同时把 modelMat × prev joint baking 后的 palette 传给 backend | ✅ |
| Lua/userdata 兼容 | 旧 `Animation.DrawSkinnedMesh(mesh, animator, transform, material)` 调用不变 | ✅ |

### 2.4 Mesh / ECS prev-state wiring

| 项 | 验收点 | 状态 |
|---|---|---|
| `Mesh:Draw(prevMat)` | 仅传 prev model table 时走 default texture 路径，自动调用 `SetNextPreviousModelMatrix` | ✅ |
| `Mesh:Draw(textureId, prevMat)` | 老 textureId 路径 + 可选 prev model | ✅ |
| `Mesh:Draw(material, prevMat)` | 新 material 路径 + 可选 prev model | ✅ |
| 错误参数 | `mesh:Draw("invalid")` 走旧 fallback 不崩；非法 prev table 报 Lua error | ✅ |
| ECS `MeshRenderer` | `_DrawMesh(entity, tf, mr, gfx)` 缓存 world model 到 `_mesh_prev_model_cache[entity._id]` | ✅ |
| 可见性变更 | `visible == false` 或 `mr.mesh == nil` 时清缓存，防止重新可见时出现错误大速度 | ✅ |
| 旧 `mesh:Draw()` | 无 prev 参数路径保持兼容 | ✅ |

### 2.5 SSR Temporal velocity sampling

| 项 | 验收点 | 状态 |
|---|---|---|
| backend 接口 | `DrawSSRTemporal(curReflectTex, historyTex, depthTex, velocityTex, dstFbo, w, h, reprojectMat4, invProjMat4, blendAlpha, rejectionMode, hasHistory)` | ✅ |
| `SSRRenderer::Process` | 通过 `backend->GetHDRVelocityTex(hdrFbo)` 取 velocity，0 时由 shader 自动回退矩阵 | ✅ |
| shader 双 profile | `uHasVelocityTex==1` 时 `prevUV = vUV - velocityTex.rg`；否则使用 `uReprojectMat * ndc` | ✅ |
| 越界 reject | prevUV 越界继续按当前像素输出 | ✅ |
| 首帧 / `uHasHistory=0` | 直接输出当前帧，不读取 history | ✅ |
| 默认行为兼容 | 缺 velocity 时与 Phase E.12 一致 | ✅ |

### 2.6 HDR lifecycle 接入

| 项 | 验收点 | 状态 |
|---|---|---|
| `HDRRenderer::CreateRT` | 成功后调用 `ResetVelocityHistory()` | ✅ |
| `HDRRenderer::ReleaseRT` | 释放时调用 `ResetVelocityHistory()` | ✅ |
| `HDRRenderer::EndScene` | tonemap 之后调用 `CommitVelocityHistory()`，把当前 view-proj 推进为下一帧 prev | ✅ |
| Bloom / AE / LensFx / SSAO / SSR 联动 | 不受 velocity 影响，行为保持 Phase E.12 状态 | ✅ |

---

## 3. 验证清单

| 验证项 | 当前状态 | 说明 |
|---|---|---|
| 源码静态一致性 | ✅ | grep 全仓库确认 `DrawSSRTemporal` / `DrawSkinnedMeshMaterial` / `DrawSkinnedMorphMeshMaterial` 签名一致；新增 velocity uniform location 缓存与上传路径完整 |
| `git diff --check` | ✅ | 已运行，无 whitespace error（仅 LF/CRLF warning） |
| Lua 语法检查 `lightc -p` | ✅ | `scripts/smoke/material_3d.lua`、`scripts/smoke/ecs_render.lua` 均 exit 0 |
| 本地 CMake build | 🚫 | 按用户偏好不在本地执行 |
| 本地 `light.exe` runtime smoke | 🚫 | 按用户偏好不在本地执行 |
| GitHub Actions 6 平台 build | ⏳ | 未提交 / 未触发 |
| Windows runtime smoke | ⏳ | 待 CI 跑 |
| 真实窗口视觉验收 | ⏳ | 需桌面 GL3.3 环境对比开/关 velocity（含相机/角色快速运动） |

---

## 4. 已发现并修复的问题

| 问题 | 影响 | 修复 |
|---|---|---|
| 早期 patch 缺 `ResetVelocityHistory/CommitVelocityHistory/SetNextPreviousModelMatrix` 的 GL33 override | velocity history 永远 no-op，所有物体首帧后 prev 与当前重合 | 在 GL33Backend 补 3 个 override，并把 `prevViewProj` 在 Reset 时与当前 view-proj 对齐避免首帧脏数据 |
| `HDRRenderer::EndScene` 未推进 velocity history | 同上 | tonemap 后调用 `CommitVelocityHistory()` |
| `DrawMeshMaterial` 静态 mesh 没上传 velocity uniforms | 静态物体 velocity 全 0 | 在静态 mesh 路径补 `UploadVelocityUniforms` 并消费 one-shot `nextPrevModel` |
| 用户 shader 路径未清 one-shot prev model | 之后的引擎默认 draw 可能错误使用上一次设置的 prev | `userShaderActive || !program3D` 时也 `hasNextPrevModel = false` |
| SSR Temporal 注释残留「无 G-buffer velocity」 | 与新行为矛盾，易误导维护 | 同步更新 `render_backend.h` / `ssr_renderer.h` / `ssr_renderer.cpp` 注释 |
| Animator 缺 previous pose | 蒙皮 mesh 无法计算骨骼级 velocity | 加 `prevJointMatrices/prevMorphWeights/velocityHistoryValid` + Update 复制 + 状态变更复位 |
| ECS 静态 `MeshRenderer` 无 previous transform | 静态物体 transform 动画无 velocity | 加 `_mesh_prev_model_cache`，第二帧起传 `mesh:Draw(..., prevModel)` |

---

## 5. 验收结论

Phase E.13 的生产代码（C++ + GLSL + Lua）与静态验证已完成；当前缺口集中在外部验证：

- **CI 6 平台 build**：未触发
- **Windows runtime smoke**：待 CI 启用
- **真实窗口视觉验收**：用户在桌面 GL3.3 环境下确认 Temporal SSR + 动态物体表现

**最终完成判据**（与 Phase E.12 对齐）：

- GitHub Actions 6 平台 build success
- Windows runtime smoke 中 SSR / material / ECS 相关脚本 0 fail
- 真实窗口环境下，开启 velocity buffer 后动态物体在 Temporal SSR 中无残留拖影且无明显黑帧
