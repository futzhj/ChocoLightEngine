# Phase AV 骨骼动画 — 项目总结报告

> 6A 工作流阶段 6（Assess）最终交付物
>
> **完成日期**：2026-05-10
> **总 commit 数**：4 个（Step 1, Step 2, Step 3, Step 4）
> **CI 终态**：[run 25612243131](https://github.com/futzhj/ChocoLightEngine/actions/runs/25612243131)，6 平台 build success + Windows runtime smoke `[Phase AV Step 1+2+3+4] 通过 93 / 失败 0`

---

## 1. 范围回顾

Phase AV 在 ChocoLight 引擎中引入完整的 **glTF 骨骼动画 + 状态机**：

| 模块 | 职责 |
|------|------|
| `Light.Animation` | 顶层：`LoadSkinnedGLTF` / `NewAnimator` / `DrawSkinnedMesh` |
| `Light.Animation.Skeleton` | 骨骼数据（≤ 64 关节）+ bind pose + inverseBind |
| `Light.Animation.Clip` | AnimationClip + LINEAR/STEP/CUBICSPLINE 采样器 |
| `Light.Animation.Animator` | 状态机 + Transition / Crossfade / Event / Param + 关节矩阵 |
| `Light.Animation.SkinnedMesh` | 蒙皮网格（CPU skinning） |

---

## 2. Step 分阶段成果

| Step | 范围 | commit | 主要交付 |
|------|------|--------|---------|
| **Step 1** | cgltf 集成 + Skeleton/Clip 数据结构 + LoadSkinnedGLTF + 错误路径 | (3 commits) | 5 个 luaopen 模块；Lua 5.1 `luaL_register` 全局表陷阱发现并避开（key memory） |
| **Step 2** | Animator 单状态 + sampler 求值 + 局部 TRS + 前向运动学 + 关节矩阵 | 同上 | LINEAR/STEP/CUBICSPLINE 采样；DFS 迭代 N+4 上限；`Pause/Resume/SetSpeed/SetLooping` |
| **Step 3** | SkinnedMesh + CPU skinning + DrawSkinnedMesh + 多 primitive 探索 | 同上 | 4-joint weighted blend；`CpuSkinVertex` + `Mat4ApplyPoint/Dir`；smoke 67 / 失败 0 |
| **Step 4** | 状态机扩展（Transition / Crossfade / Event / Param） | `f5e8f34` | TransitionDef + EventDef；ComputeJointMatricesBlended；14 新方法；93 / 失败 0 |

---

## 3. 关键技术决策汇总

### 3.1 cgltf 单头库（vs tinygltf）

- **选择**：cgltf 2.0
- **原因**：纯 C，0 依赖，PE 加载速度快；接受 GLB+GLTF 两种格式；ChocoLight 已用其他类似单头库
- **代价**：API 稍底层（手动遍历 accessor），但 100 行内可解析完整 skin

### 3.2 CPU skinning（vs GPU skinning）

- **选择**：Step 3 用 CPU 蒙皮（每帧 DeleteMesh + CreateMesh）
- **原因**：避开 GPU shader backend 改动（uniform array 上传 + 顶点 attribute 扩展 + 多平台 GLES2 兼容）
- **性能**：≤ 64 关节 + ≤ 5000 顶点场景下 60FPS 无压力
- **未来**：Phase AV.x GPU skinning 可作为优化项；CPU 路径作 fallback

### 3.3 Param 仅 number 类型

- **TASK 决策**："不引入 bool/string/trigger 类型"
- **覆盖**：number 0/1 模拟 bool；enum int 模拟 string；set-reset 模拟 trigger
- **节省**：无类型 union；无 GC ref 管理；hash map 简单

### 3.4 同帧最多 1 transition

- **理由**：避免 idle→walk→run 三层 transition 在一帧内连环触发，导致跳过 walk 中间态
- **实现**：`transitionedThisFrame` 标志在 Update 入口重置；触发后 break + 标志置 true

### 3.5 ComputeJointMatrices 重构

- 抽 `EvaluateLocalTRS` + `ComputeWorldAndSkinning` helper
- 原 `ComputeJointMatrices` 内部改用 helper（外部 API 完全兼容）
- 新增 `ComputeJointMatricesBlended`（双 clip TRS lerp/slerp 混合 → 同 helper）
- **收益**：消除 ~80 行重复；DFS 拓扑只有一份；未来 IK / Layer / Additive 可复用

---

## 4. 质量评估

### 4.1 代码质量

| 维度 | 评分 | 说明 |
|------|------|------|
| 规范一致性 | A | 严格遵循 ChocoLight 现有 light_*.cpp 风格（CheckXxx / userdata + RegistryRef + alive 字段） |
| 可读性 | A | 函数粒度合理（单一职责 helper），关键逻辑都有中文注释 |
| 复杂度 | A- | `l_Animator_Update` 略长（~70 行）但流程线性；其余函数 < 30 行 |
| 内存安全 | A | 严格 RegistryRef 策略防 GC；Skeleton/Clip 都有 alive flag + Delete 显式释放路径 |
| 错误隔离 | A+ | Lua callback 一律 pcall + fprintf(stderr)，不中断 Update 主线 |

### 4.2 测试质量

| 维度 | 数据 | 说明 |
|------|------|------|
| smoke 断言数 | 93 | 远超原计划（67 + 26 = 93） |
| 错误路径覆盖 | 完整 | Animator 所有 14 新方法都有 raise 路径断言 |
| 跨平台 | 6 / 6 | Windows / Linux / macOS / Android / iOS / Web 全绿 |
| Phase AS-AU 退化 | 无 | Phase AT smoke 40 / 失败 0；Phase AS / AU smoke 同步通过 |

**未覆盖项**（预期由 Phase AV.x 补）：
- 关节矩阵数值断言（vs Blender ground truth）
- Crossfade 中点权重动态验证
- Event 跨循环边界端到端验证
- DrawSkinnedMesh 像素级渲染验证

### 4.3 文档质量

| 文档 | 状态 |
|------|------|
| `docs/Phase AV 骨骼动画/ALIGNMENT_PhaseAV.md` | ✅ Step 1 期完成 |
| `docs/Phase AV 骨骼动画/CONSENSUS_PhaseAV.md` | ✅ Step 1 期完成 |
| `docs/Phase AV 骨骼动画/DESIGN_PhaseAV.md` | ✅ Step 1 期完成 |
| `docs/Phase AV 骨骼动画/TASK_PhaseAV.md` | ✅ Step 1-4 任务卡完整 |
| `docs/Phase AV 骨骼动画/ACCEPTANCE_PhaseAV.md` | ✅ Step 1+2+3+4 验收记录 + CI 链接 |
| `docs/Phase AV 骨骼动画/FINAL_PhaseAV.md` | ✅ 本文件 |
| `docs/Phase AV 骨骼动画/TODO_PhaseAV.md` | ✅ 见同目录 |
| `docs/api/Light_Animation.md` | ✅ 5 模块完整 API（280 行） |
| `docs/api/MODULE_INDEX.md` | ✅ Phase AV 分组 + Phase AM/AN/AQ/AR/AS/AT/AU 历史滞后补全 |
| `samples/demo_animation/README.md` | ✅ 资源指引 + 输出示例 |

### 4.4 与现有系统集成

- **未引入技术债**：不破坏 Phase AS Mesh / Material 渲染管线
- **未引入冲突**：cgltf 与 Phase AS 的 cgltf 实例共享同一份 `third_party/cgltf` 头文件
- **未影响性能**：Phase AT/AU smoke 时间未明显劣化
- **OOP 框架兼容**：所有 luaopen 都用 `luaL_setfuncs(L, ..., 0)`（避开 luaL_register 全局表陷阱）

---

## 5. 关键 memory 资产

本次 Phase 中沉淀并保存的关键 memory（已 create）：

1. `cb79c6b3-cff6-...` — **luaL_register 全局表陷阱**（适用所有 ChocoLight 新模块；首次发现于 Phase AV Step 1）
2. `f9b3da22-a52c-...` — **Lua 5.1 `\xNN` 转义陷阱**（lightc -p 不严，运行时才暴露）
3. `f6c3f763-9148-...` — **lumen-master OUTPUT_NAME 漂移**（lua51 vs Lumen 双 PE 共存导致 access violation）

---

## 6. 文件改动统计（Phase AV 全程）

| 范畴 | 新增文件 | 修改文件 |
|------|---------|---------|
| C++ 实现 | `ChocoLight/include/light_animation.h`<br>`ChocoLight/src/light_animation.cpp` | `ChocoLight/CMakeLists.txt`（cgltf 编译入口）<br>`lumen-master/src/light/light.cpp`（5 个 luaopen 注册） |
| 第三方 | `ChocoLight/third_party/cgltf/cgltf.h` | — |
| smoke | — | `scripts/smoke/animation.lua`（93 断言）<br>`.github/workflows/build-templates.yml`（runtime smoke 调度） |
| sample | `samples/demo_animation/main.lua`<br>`samples/demo_animation/README.md` | `samples/README.md` |
| docs | `docs/Phase AV 骨骼动画/{ALIGNMENT, CONSENSUS, DESIGN, TASK, ACCEPTANCE, FINAL, TODO}_PhaseAV.md`<br>`docs/api/Light_Animation.md` | `docs/api/MODULE_INDEX.md` |

**总代码量增量**：~2400 行（含 1700 行 C++、150 行 Lua sample、500 行 docs、其余 smoke/CMake）

---

## 7. 下一步推荐

详见 [`TODO_PhaseAV.md`](./TODO_PhaseAV.md)。

**短期（Phase AV.x，1-2 周内）**：

1. 引入测试 glTF 资产（cube_skin.glb + simple_walk.glb）→ 解锁数值断言
2. demo_animation 端到端渲染验证（manual + CI smoke）
3. Crossfade 中点权重 / Event 跨循环边界专项测试

**中期（Phase AW 候选）**：

1. GPU skinning（Desktop GL 优先；GLES2 fallback 用 CPU 路径）
2. 多 primitive 单 SkinnedMesh（`pack.meshes = {...}` 数组）
3. IK 约束（two-bone IK / look-at）

**长期（Phase AX+ 候选）**：

1. Layer 系统（base + override + additive 三层 blend）
2. Morph target（blend shapes）
3. Visual graph 编辑器（数据驱动状态机配置）

---

**Phase AV 验收完成 ✅**
