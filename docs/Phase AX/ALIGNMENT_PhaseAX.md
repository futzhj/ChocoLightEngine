# ALIGNMENT — Phase AX（Morph Target 表情/形状变形）

> **6A 工作流 Stage 1 — Align 阶段产物**
> 目的：将"加 morph target 支持"这一模糊需求转化为精确的、与 ChocoLight 现有架构对齐的规范。

---

## 一、项目和任务特性规范

### 1.1 工作空间

- **项目**: `futzhj/ChocoLightEngine` (`e:\jinyiNew\Light`)
- **引擎**: ChocoLight (Lumen + SDL3 + OpenGL/GLES + Lua 5.1)
- **本阶段**: Phase AX，紧接 Phase AW.x 之后（GPU Skinning 工具链已完成）

### 1.2 范围声明

**本阶段做**：
- 在 ChocoLight 中实现 glTF 2.0 morph target 完整规范（POSITION + NORMAL + TANGENT delta）
- CPU 路径 + GPU 路径双实现
- glTF animation channel `target_path == "weights"` 自动驱动
- Lua 手动 API（`SetMorphWeight` / `GetMorphWeight` / `ClearMorphWeights` 等）
- 与 Phase AW GPU skinning 完全互兼（同时启用时使用新 shader `VS3D_SKIN_MORPH`）
- smoke + sample + 文档闭环

**本阶段不做**（明确排除）：
- ❌ Sparse accessor（cgltf 已支持但 ChocoLight 主路径未用）— 后续如有需要再做
- ❌ 自定义 attribute 名称（如 `_TARGET_POSITION_2`）— 仅按 glTF spec 路径
- ❌ Morph target 与 Layer 混合策略（Q2 选 B 不选 C）— 仅动画 + 手动覆盖
- ❌ Web Safari WebGL2 性能调优 — 沿用 Phase AW 规则（Web 默认 CPU）

---

## 二、原始需求

> "Phase AX: morph target 表情 — 加 glTF morph target / blend shape（人物表情动画、口型同步）"

来源：用户在 Phase AW.x 完成后的 Stage 6 §20 询问中明确选择"启动新阶段"。

业务驱动：
- Phase AV/AW 已交付**骨骼动画 + GPU skinning**，但人物表情、嘴型、面部细节仅靠骨骼无法表达
- glTF 2.0 标准 morph target 是商业引擎（Unity/Unreal/Godot）通用方案
- 在 `docs/Phase AV 骨骼动画/TODO_PhaseAV.md` §C.2 已被列为下阶段候选

---

## 三、边界确认

### 3.1 任务范围（基于 Q1 答案 = A 完整）

| 维度 | 决策 |
|------|------|
| Attribute 覆盖 | POSITION + NORMAL + TANGENT delta（完整 glTF spec）|
| Morph target 数量上限 | 8（与 Q3 = B 一致；典型角色 6-8 个 blend shape 已足够）|
| CPU 路径 | ✅ 必做（兼容所有 backend）|
| GPU 路径 | ✅ 必做（uniform array 方案 + 限 N ≤ 8）|
| 动画通道驱动 | ✅ 解析 glTF animation channel `target_path=="weights"` |
| 手动权重 API | ✅ `SetMorphWeight` / `GetMorphWeight` / `ClearMorphWeights` |
| 与 Skinning 共存 | ✅ 新 shader `VS3D_SKIN_MORPH` 全路径 GPU |

### 3.2 不在本阶段范围

- glTF morph target sparse encoding（cgltf 支持但低优先级）
- 自定义 morph attribute（仅 glTF 标准路径）
- Web Safari morph 性能调优
- IK / Layer / Additive blend（Phase AV TODO 其他候选）

---

## 四、对现有项目的需求理解

### 4.1 关键代码勘察结论

| 项目 | 当前状态 | 引用 |
|------|---------|------|
| **glTF 解析** | cgltf v1.13 单文件，已支持 morph target API（`cgltf_morph_target`/`prim.targets[]`/`mesh.weights[]`/`target_names[]`/`cgltf_animation_path_type_weights`）| `ChocoLight/third_party/cgltf.h:560-583` |
| **`ChannelTarget` enum** | 仅 TRANSLATION / ROTATION / SCALE，明确注释 `UNSUPPORTED = 255 // morph weights 等本 Phase 不支持` | `light_animation.cpp:64-69` |
| **`SkinnedMeshAsset` 结构** | 仅存 `baseVertices` + `jointIndicesPacked` + `weights` + `skinnedVertices`；无 morph 字段 | `light_animation.cpp:171-193` |
| **`Animator` 结构** | 含 `params: unordered_map<string,float>`（Phase AV Step 4），但无 `morphWeights` 数组 | `light_animation.cpp:122-164` |
| **`LoadSkinnedGLTF`** | 调用 `BuildSkeleton` / `BuildClip` / `ExtractSkinMesh`；未提取 mesh.targets | `light_animation.cpp:1061-1169` |
| **`ConvertChannelTarget`** | weights 路径直接落到 UNSUPPORTED | `light_animation.cpp:988-995` |
| **shader VS3D / VS3D_SKIN** | location 0-3 (pos/nrm/uv/color) + 4-5 (joints/weights)；无 morph delta attribs | `render_gl33.cpp:113-166` |
| **`CreateSkinnedMesh` / `DrawSkinnedMeshMaterial`** | 已实现 GPU 蒙皮全链路，可作为 morph GPU 路径模板 | `render_gl33.cpp:1617-1740` |
| **`ShouldUseGPUSkinning` / `DrawSkinnedMeshCPU` / `DrawSkinnedMeshGPU`** | Phase AW 已建立 CPU/GPU 双路径模式，可直接复用结构 | `light_animation.cpp:2491-2727` |

### 4.2 现有代码模式与约定

- **类型隔离**：`RenderVertex3D`（普通）vs `RenderVertex3DSkin`（蒙皮，含 joints/weights）— 模式一致，扩展时新建 `RenderVertex3DMorph` / `RenderVertex3DSkinMorph` 即可
- **userdata 元表命名**：`Light.Animation.<ClassName>` 风格（Skeleton/Clip/Animator/SkinnedMesh）— 不需要新增 userdata 类，扩展现有 SkinnedMesh + Animator
- **CPU/GPU 双路径**：`ShouldUseGPUSkinning()` + `Draw*MeshCPU/GPU` 分流 — Phase AX 沿用此模式，加 `ShouldUseGPUMorph()` 或合并到现有判定
- **关节矩阵 UBO**：64 关节 × 16 floats = 4 KB，远低于 OpenGL 16 KB UBO 上限 — morph 也用 uniform array 是安全的
- **Silent fallback**：GPU 失败自动 fallback CPU（Phase AW 已实现）— 沿用

### 4.3 集成点（Phase AV/AW → Phase AX）

| 集成点 | 改动方向 |
|--------|---------|
| `enum class ChannelTarget` | 增加 `MORPH_WEIGHTS = 3`（不再是 UNSUPPORTED）|
| `Sampler` | `components` 字段含义扩展（weights 时 = morph target 数）|
| `SkinnedMeshAsset` | 新增 `morphTargets`/`morphTargetCount`/`morphDefaultWeights`/`morphTargetNames` 字段 |
| `Animator` | 新增 `morphWeights`/`morphWeightsManual` 数组 + 辅助 API |
| `LoadSkinnedGLTF` | 调用 `ExtractMorphTargets` + 解析 `mesh.weights[]` 默认值 |
| `BuildClip` | 处理 `target_path == weights` channel（components = N，sampler 输出 N×float）|
| `ComputeJointMatrices` | 不变（与 morph 正交）|
| `DrawSkinnedMeshCPU` | 新增 morph delta 应用步骤（在蒙皮变换前）|
| `DrawSkinnedMeshGPU` | 新增 UBO morphWeights 上传 + 选择 `VS3D_SKIN_MORPH` shader |
| `RenderBackend` 接口 | 新增 `CreateMorphMesh` / `DrawMorphMesh*` 4 个虚函数 |
| `render_gl33.cpp` | 新建 4 个 program（VS3D_MORPH / VS3D_SKIN_MORPH × FS_UNLIT/FS_PBR）|
| `render_legacy.cpp` | morph 在 LegacyBackend 下 fallback CPU（不实现 GPU 路径）|

---

## 五、智能决策（依据现有项目和行业知识）

### 5.1 已自动决策（无须用户参与）

| 决策点 | 选择 | 依据 |
|--------|------|------|
| `morphTargetCount` 上限 | **8** | glTF 实际典型 6-8；与 Q3 选项 B 一致；uniform array 长度可控 |
| Morph delta 数据布局 | **per-vertex AoS（POS/NRM/TAN 各 vec3 array）** | 与 cgltf 解析顺序一致；shader 取数容易 |
| 默认权重来源 | **`mesh.weights[]`**（glTF 标准）| 用户未调 SetMorphWeight 时使用 |
| Morph target 名称来源 | **`mesh.target_names[]`** + fallback `"target_<idx>"` | 名称式 SetMorphWeight 友好 |
| 动画通道 sampler `components` | **= N**（target 数）| glTF spec 规定每帧 N 个 weight |
| 手动覆盖 sentinel | **NaN 表示"未覆盖, 用动画值"** | 与 0 区分，不破坏正常 0 权重 |
| 与 Phase AW 共享 backend 接口 | **复用 `MaterialDesc`** + 新增 `morphWeights, morphTargetCount` 参数 | 不破坏既有 API |
| Backend 名称扩展 | **不变**（仍 GL33Core/LegacyGL）| 仅内部 program 增加，外部识别符不变 |
| Lua API 模块挂载位置 | **复用 `Light.Animation`** + `Animator:SetMorphWeight` 等方法 | 与既有 Skeleton/Clip/Animator API 风格一致 |
| smoke 文件命名 | **复用 `scripts/smoke/animation.lua`**（添加 morph 段落）| 避免 file 增多；与 Phase AV/AW 一致 |
| sample 命名 | **`samples/demo_morph_target/`** 独立 demo | 与 demo_skinning_perf 平行 |

### 5.2 用户已决策的关键点（Q1-Q4）

| ID | 问题 | 决策 |
|----|------|------|
| **Q1** | Phase AX 范围边界 | **A. 完整**（POS+NORMAL+TANGENT, CPU+GPU）|
| **Q2** | weights 来源 | **B. 动画 + 手动覆盖**（推荐）|
| **Q3** | GPU 路径数据传输 | **B. 限 N≤8 + uniform array**（推荐；非 SSBO/TBO，平衡兼容性）|
| **Q4** | 与 Phase AW 共存 | **A. 全路径 GPU**（新 shader `VS3D_SKIN_MORPH`，4→6 个 program）|

---

## 六、待澄清的疑问（Stage 1 §1.4 中断询问产物）

> 已在 Stage 1 §1.4 通过 `ask_user_question` 4 次中断询问全部解决。无遗留疑问。

---

## 七、最终统一理解

### 7.1 Phase AX 一句话目标

> 在 ChocoLight 中实现完整的 glTF 2.0 morph target 支持（POS+NORMAL+TANGENT，最多 8 个 target），同时支持动画通道驱动和手动权重覆盖，提供 CPU 路径（兼容所有 backend）和 GPU 路径（限 GL33Core，新 shader `VS3D_MORPH` / `VS3D_SKIN_MORPH`），与 Phase AW GPU skinning 完全协作（同时启用时走全 GPU），交付 smoke + sample + 完整 6A 文档。

### 7.2 工作量预估

| 维度 | 预估 |
|------|------|
| C++ 数据结构 + 解析 | ~250 行 |
| C++ CPU 路径 | ~80 行 |
| C++ GPU 路径 + backend 接口 | ~250 行 |
| Shader（VS3D_MORPH + VS3D_SKIN_MORPH）| ~80 行 |
| Lua API 绑定 | ~120 行 |
| smoke 增量（animation.lua）| ~120 行 |
| Sample (demo_morph_target/) | ~250 行 |
| 文档（6A 7 个文档 + 主文档同步）| ~700 行 |
| **总计** | **~1850 行**，5-6 个原子任务 |

### 7.3 Stage 1 完成判据

- [x] 项目上下文已剖析（4.1 表格）
- [x] 任务范围明确（3.1 / 3.2）
- [x] 现有代码集成点列出（4.3）
- [x] 4 个关键决策点（Q1-Q4）已用户确认
- [x] 已自动决策的次级问题（5.1）有依据
- [x] 工作量与原子任务划分初步勾勒（7.2）

---

## 八、下一步（Stage 2 Architect）

进入 6A Stage 2：
1. 写 `CONSENSUS_PhaseAX.md` — 锁定决策表 + 验收标准
2. 写 `DESIGN_PhaseAX.md` — 系统架构图 + 接口契约 + 数据流
3. （Stage 3）写 `TASK_PhaseAX.md` — 拆分原子任务 + 依赖 DAG

> Stage 2-3 文档完成后再进入 Stage 5 实施代码。
