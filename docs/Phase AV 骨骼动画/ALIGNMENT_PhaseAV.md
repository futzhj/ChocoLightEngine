# Phase AV — 骨骼动画 + 状态机 — 对齐文档

> **6A Stage 1: Align**(Phase AV 骨骼动画)
>
> 主题：基于 cgltf 现有集成，给 ChocoLight 引擎补全 3D 骨骼动画与状态机能力，让 Phase AS（3D 渲染 + glTF + PBR）+ Phase AU（3D 物理 + Bullet）形成完整 3D 角色动作游戏闭环。
>
> 风险预警：✅ **0 新第三方依赖**（cgltf 已在 `ChocoLight/third_party/cgltf.h`），shader 改动需兼容 GL 3.3 Core / GLES 3.0 / WebGL 2.0 三平台。

---

## 1. 现状回顾

### 1.1 Phase AS 已就位的 3D 渲染基建

| 能力 | 现状 |
|------|------|
| Mesh / VBO / IBO | ✅ `light_graphics_mesh.cpp` 5 函数 |
| glTF 2.0 加载 | ✅ cgltf 集成，已支持 POSITION/NORMAL/TEXCOORD/COLOR |
| 材质 | ✅ PBR + 法线 + 多光源（Phase AS.4） |
| 自动材质提取 | ✅ Phase AS.4.x（baseColor/metallic/normal/occlusion/emissive 全套） |
| Mat4 数学 | ✅ 2 函数 |
| Backend | ✅ GL 3.3 Core / GL 2.1 fallback / GLES 双路径 |

### 1.2 现有 cgltf 使用情况（缺口分析）

`@e:\jinyiNew\Light\ChocoLight\src\light_graphics_mesh.cpp:165-415` 已用 cgltf 解析：

- ✅ `cgltf_parse_file` / `cgltf_load_buffers`
- ✅ `cgltf_accessor_unpack_floats` / `cgltf_accessor_unpack_indices`
- ✅ POSITION / NORMAL / TEXCOORD_0 / COLOR_0
- ✅ Material（baseColor/metallic/normal/occlusion/emissive）
- ❌ **JOINTS_0 / WEIGHTS_0 完全未提取**
- ❌ **`cgltf_skin` 完全未使用**（`data->skins` 没读）
- ❌ **`cgltf_animation` 完全未使用**（`data->animations` 没读）
- ❌ **`cgltf_node` 层级树完全未使用**（关节变换树）

### 1.3 现有 SpriteAnimation（2D，参考但不复用）

`@e:\jinyiNew\Light\ChocoLight\src\light_graphics_spriteanimation.cpp` 是 2D 帧序列动画，与 3D 骨骼动画机制完全不同（无关节、无 skinning），仅作为命名参考（`Light.Graphics.SpriteAnimation` 与新 `Light.Animation` 共存）。

### 1.4 Phase AU 3D 物理就绪

刚刚收尾的 Bullet 全套（RigidBody / Joint / Character / Vehicle / SoftBody）可与骨骼动画在 ragdoll / 角色物理 上未来联动，但 Phase AV **不**先做联动，避免范围爆炸。

---

## 2. 任务边界

### 2.1 In-Scope

| # | 内容 |
|---|------|
| 1 | `Light.Animation` 顶层模块 + 子模块（Clip / Skeleton / SkinnedMesh / Animator） |
| 2 | glTF 2.0 骨骼数据加载：`cgltf_skin` / `cgltf_node` 层级 / inverseBindMatrices |
| 3 | glTF 2.0 动画数据加载：`cgltf_animation` 通道 / sampler / 插值（LINEAR / STEP / CUBICSPLINE） |
| 4 | AnimationClip 时间轴采样：返回每个关节的 TRS（Translation / Rotation / Scale） |
| 5 | Skeleton 关节变换树前向计算：local TRS → global Mat4 → 蒙皮矩阵 |
| 6 | SkinnedMesh 顶点蒙皮：**GPU skinning 首选**（vertex shader 取 JOINTS_0/WEIGHTS_0 attribute + Mat4 数组 uniform），**CPU skinning fallback**（GLES 2.0 / 老硬件） |
| 7 | Animator 状态机：State + Transition + Crossfade（线性混合两个 clip） |
| 8 | 事件帧：Clip 注册 `OnFrame(time, callback)` 回调 |
| 9 | 单元 smoke：`scripts/smoke/animation.lua`（≥ 30 断言） |
| 10 | 综合 demo：`samples/demo_animation/main.lua`（headless console，可降级；GUI 版本下个 Phase 视情况） |
| 11 | 文档：API 表 + 状态机指南 + glTF 资源准备指引 |

### 2.2 Out-of-Scope（明确不做）

| # | 内容 | 原因 |
|---|------|------|
| 1 | FBX / BVH / DAE 加载 | 仅 glTF（cgltf 已就位，0 依赖） |
| 2 | IK（Inverse Kinematics） | 单独大主题，留给 Phase AV.x 或 AW |
| 3 | Ragdoll（骨骼 → 物理 RigidBody 联动） | 单独主题，与 Phase AU 联动需另设计 |
| 4 | Animation Layer / Sub-state Machine | Unity 风的高级状态机，避免过度设计；当前一层 + Crossfade 可覆盖 90% 场景 |
| 5 | Morph Target / Blend Shape | 表情动画另起，需要 cgltf_morph_target 单独抽取 |
| 6 | 编辑器 / 时间轴 GUI | 引擎不做编辑器 |
| 7 | 网络同步（动画状态广播） | 需要 Net 层支持，不在当前主题 |

### 2.3 验收边界

- 6 平台 CI 全绿（Windows / Linux / macOS / Android / iOS / Web）
- smoke 在 headless 模式可跑（`Light.Graphics` 不可用时优雅跳过 SkinnedMesh 渲染段，但 Skeleton / Clip / Animator 数据层必跑通）
- 演示资源：使用 glTF 2.0 sample assets（公开 CC0/MIT 资源，例如 `RiggedSimple.gltf` 或 `CesiumMan.glb`），但**不入库**，仅写入 `samples/demo_animation/README.md` 引用下载链接

---

## 3. 需求理解

### 3.1 用户期望的典型用法

```lua
-- 加载骨骼角色
local Anim = require 'Light.Animation'
local skinnedMesh = Anim.LoadSkinnedGLTF("character.glb")
-- skinnedMesh.skeleton: Skeleton 对象
-- skinnedMesh.clips:    table<name, AnimationClip>
-- skinnedMesh.mesh:     底层 RenderMesh（含 JOINTS_0/WEIGHTS_0 attribute）

-- 简单播放
local animator = Anim.NewAnimator(skinnedMesh.skeleton)
animator:AddClip("idle", skinnedMesh.clips["Idle"])
animator:AddClip("walk", skinnedMesh.clips["Walk"])
animator:Play("idle")

-- 主循环
animator:Update(dt)                              -- 推进时间轴
animator:GetJointMatrices()                       -- -> array<Mat4> 蒙皮矩阵供 shader 用
Light.Graphics.DrawSkinnedMesh(skinnedMesh, animator, transform, material)

-- 状态机
animator:AddTransition("idle", "walk", function(self) return self:GetParam("speed") > 0.1 end, 0.2)
animator:SetParam("speed", 1.0)
-- Update 时自动检测 transition 并 Crossfade

-- 事件帧
animator:AddEvent("walk", 0.4, function() print("脚踏地") end)
```

### 3.2 关键能力分级

| 能力 | 优先级 | Step |
|------|--------|------|
| glTF 骨骼 + 动画加载 | P0 | Step 1 |
| 关节变换树 + sampler 采样 | P0 | Step 2 |
| GPU skinning shader + 渲染 | P0 | Step 3 |
| CPU skinning fallback | P1 | Step 3 |
| Animator + Crossfade | P0 | Step 4 |
| 状态机（Transition / Param） | P0 | Step 4 |
| 事件帧 | P1 | Step 4 |

### 3.3 性能预期

- **GPU skinning**：4 关节/顶点，每帧 1 次 uniform Mat4 数组上传（关节数 ≤ 64）+ vertex shader 一次 4-mat 乘加 → 60+ FPS / 千顶点角色
- **CPU skinning**：每帧 CPU 端遍历顶点做 4-mat 加权乘法 → 30 FPS / 千顶点（仅低端 fallback）
- **Sampler**：每个关节 3 通道（T/R/S），每通道 ≤ 数百关键帧；线性查找在小数据量下性能足够，无需二分

---

## 4. 智能决策清单（已基于现有项目内容/行业实践决策）

### 4.1 模块布局

**决策**：`Light.Animation` 顶层 + 4 子模块

```
Light.Animation                       (顶层 require 入口)
Light.Animation.Clip                  (静态数据，时间轴)
Light.Animation.Skeleton              (静态数据，关节层级)
Light.Animation.SkinnedMesh           (Mesh 引用 + Skeleton 引用)
Light.Animation.Animator              (运行时状态，状态机 + Crossfade)
```

**理由**：与 `Light.Graphics.SpriteAnimation` 区分（一个是 2D 帧序列，一个是 3D 骨骼）；与 Unity / Godot 风格对齐。

### 4.2 Skinning 路径

**决策**：**GPU skinning 默认 + CPU skinning 作为编译期 fallback**

- GPU skinning shader：JOINTS_0（uvec4）+ WEIGHTS_0（vec4）attribute + `uniform mat4 u_jointMatrices[64]`
- 关节数上限：**64**（Mat4 × 64 = 1024 floats = 16 KB uniform，OpenGL 3.3 / GLES 3.0 / WebGL 2.0 都富余）
- CPU 路径仅用于 GLES 2.0 fallback（动态 vbo upload）
- WASM 默认走 GLES 3.0 / WebGL 2.0，无问题

**理由**：行业通用做法（Unity / Godot / Unreal 默认 GPU skinning）；WebGL 2.0 在 ChocoLight Web 模板已就绪。

### 4.3 状态机粒度

**决策**：**单层状态机**（无 Sub-state / Layer），Transition + 浮点 Param + Crossfade 时长

```lua
animator:AddState("idle", clipIdle, {loop=true})
animator:AddState("walk", clipWalk, {loop=true})
animator:AddTransition("idle", "walk", condition_fn, fadeTime)  -- condition_fn(animator) -> bool
animator:SetParam("speed", v)        -- 任意 string -> number
animator:Update(dt)                  -- 自动检测 transition + 推进 crossfade
animator:GetCurrentState() -> name
```

**理由**：覆盖 90% 角色游戏需求；避免引入 Mecanim 风的 Layer/Sub-state Machine（可后续 Phase AV.x 补）。

### 4.4 插值模式

**决策**：**LINEAR + STEP + CUBICSPLINE 全部支持**（cgltf 都已解析，按 sampler 标记自动选择）

**理由**：glTF 2.0 标准三种全部，cgltf 已无成本支持，没理由阉割。

### 4.5 Step 拆分

| Step | 内容 | 估计 C++ | smoke 断言 |
|------|------|---------|-----------|
| Step 1 | cgltf skin/animation 解析 + AnimationClip + Skeleton 数据结构 + Lua 绑定 | ~600 行 | 8 |
| Step 2 | Animator 时间轴采样 + 关节变换树前向计算 + GetJointMatrices | ~400 行 | 10 |
| Step 3 | SkinnedMesh + GPU skinning vertex shader + DrawSkinnedMesh + CPU fallback | ~600 行 | 8 |
| Step 4 | 状态机 + Transition + Crossfade + 事件帧 + smoke 综合 | ~250 行 | 12 |
| **合计** | | **~1850 行** | **38+** |

每个 step 对应一个 commit，CI 6 平台全绿后再推下一个，与 Phase AU 节奏一致。

### 4.6 文件命名

```
ChocoLight/src/light_animation.cpp                  (~1000 行: Clip/Skeleton/Animator)
ChocoLight/src/light_graphics_skinnedmesh.cpp       (~600 行: 渲染路径 + GPU shader)
ChocoLight/src/light_graphics_mesh.cpp              (改 ~100 行: 导出 GLTF skin 提取 helper)
scripts/smoke/animation.lua                         (~400 行)
samples/demo_animation/main.lua                     (~80 行 headless)
```

注册项（按 [`MEMORY[695f930b]` 5 项规则] 一项不少）：
- `LIGHT_API` 导出 `luaopen_Light_Animation` / `luaopen_Light_Graphics_SkinnedMesh`
- `ChocoLight/CMakeLists.txt` 加 cpp
- `lumen-master/src/light/light.cpp` `g_lightModules[]` 加映射
- `scripts/smoke/animation.lua` 接入 Windows runtime smoke chain
- `.github/workflows/build-templates.yml` Windows runtime smoke step 加 `$animSmoke = ... animation.lua`

### 4.7 CI 风险评估

| 风险点 | 等级 | 缓解 |
|--------|------|------|
| cgltf API 兼容性 | 🟢 低 | 已经在用 |
| GL 3.3 Core skinning shader | 🟢 低 | 标准做法 |
| GLES 3.0 / WebGL 2.0 兼容 | 🟡 中 | 用 `#version 300 es` + `precision highp float`；Web 模板需测 |
| iOS Metal backend | 🟡 中 | 已有 backend 抽象，但需确认 SkinnedMesh 路径在 Metal 也走 |
| 关节数 > 64 | 🟢 低 | 上限断言 + 文档说明；超过则 luaL_error |
| 事件帧重复触发 / 边界 | 🟡 中 | smoke 覆盖：从尾部循环回头部时 |
| Quaternion 插值的最短路径 | 🟢 低 | slerp 标准实现 + dot < 0 翻转 |

CI 一次成功率估计：**75%**（与之前 Phase 持平；shader 改动是主要变量）。

---

## 5. 待用户确认的关键决策点

> **请用户在以下决策点直接确认或修改**，确认后将进入 Stage 2（Architect 架构阶段），生成 CONSENSUS_PhaseAV.md。

### Q1：模块布局

是否同意 `Light.Animation` + 4 子模块的布局（参 §4.1）？或希望改为：
- (a) 集成进 `Light.Graphics.Mesh:LoadSkinnedGLTF()` —— 更紧凑但与 SpriteAnimation 不对称
- (b) 单一 `Light.Skeleton` —— 命名更短但藏不下 Animator/状态机
- (c) **现方案 `Light.Animation` 顶层 + 子模块**（推荐 ✅）

### Q2：Skinning 路径

(参 §4.2) 默认 GPU skinning + CPU fallback ✅；或：
- (a) 仅 GPU skinning（不留 fallback，依赖 GLES 3.0+）
- (b) 仅 CPU skinning（最大兼容，性能差）
- (c) **GPU 默认 + CPU fallback**（推荐 ✅）

### Q3：状态机粒度

(参 §4.3) 单层 + Transition + Crossfade ✅；或：
- (a) 仅 `Play(clip)` / `Crossfade(clipA, clipB, t)`，无状态机（最简）
- (b) **单层状态机 + Transition + Param**（推荐 ✅）
- (c) Mecanim 风（Layer + Sub-state + Trigger / Bool / Float param 全套，工程量翻倍）

### Q4：演示资源

samples/demo_animation 的 glTF 测试资源：
- (a) 不入库，README 注明从 `KhronosGroup/glTF-Sample-Assets` 下载（推荐，**避免仓库膨胀** ✅）
- (b) 内嵌一个最小 procedurally-generated 测试骨骼（无外部资源依赖，但代码量增加 200 行）
- (c) 入库一个小 glb（< 100KB，例如 `RiggedSimple.glb`），代价仓库 +几十 KB

### Q5：是否在 Step 4 顺手加 Ragdoll 雏形

(与 Phase AU Bullet 联动) ：
- (a) 不做，留给 Phase AV.x（推荐 ✅）
- (b) 做最小雏形：Skeleton 关节 → btRigidBody + btConeTwistConstraint，+1 step

---

## 6. 不确定性与假设

| 项 | 假设 | 验证方式 |
|----|------|---------|
| cgltf 的 animation sampler 输出格式 | LINEAR/STEP/CUBICSPLINE 三种均按 cgltf accessor 直读 | Step 1 单测 sampler 取值 |
| Quaternion 与 Mat4 的存储顺序（行/列主序） | 列主序（与 OpenGL 默认 + 当前 mesh.cpp Mat4 一致） | 视觉对照参考资源（标准 RiggedSimple） |
| GLES 2.0 是否仍需支持 | 仅作为编译期 fallback，运行时按 GL 版本检测 | Step 3 在 backend 抽象层加 `IsSkinningGPUSupported()` |
| WebGL 2.0 `usampler` / `uvec4` attribute 支持 | 标准支持 | Web 模板 CI 验证 |
| 关节数 64 上限 | 覆盖典型角色（人形 < 60 关节），超过用 `luaL_error` | smoke 边界用例 |
| 事件帧触发时机 | `Update(dt)` 内、采样完关节后、Crossfade 完成后 | smoke 多帧时间序列断言 |

---

## 7. 项目特性规范对齐

- ✅ Lua 5.1（`Light.Animation` 模块走 `LIGHT_API luaopen_*`）
- ✅ C++17（与现有代码基一致）
- ✅ CI Windows/Linux/macOS/Android/iOS/Web 6 平台
- ✅ 无新第三方依赖（cgltf 已在 third_party）
- ✅ 模块注册 5 项规则（[`MEMORY[695f930b]`]）严格执行
- ✅ smoke `lightc -p` + Windows runtime 两路验证（与 Phase AU 一致）
- ✅ 文档命名 `docs/Phase AV 骨骼动画/{ALIGNMENT,CONSENSUS,DESIGN,TASK,ACCEPTANCE,FINAL,TODO}_PhaseAV.md`

---

## 8. 下一步

待用户对 §5 五个决策点确认后，进入：

- **Stage 2 Architect**：生成 `DESIGN_PhaseAV.md`（架构图 / 数据结构 / API 完整签名 / GPU shader 模板 / 数据流图）
- **Stage 3 Atomize**：生成 `TASK_PhaseAV.md`（4 个 step 的输入/输出契约 + 依赖图）
- **Stage 4 Approve**：用户审批
- **Stage 5 Automate**：按 step 实施 + CI 验证
- **Stage 6 Assess**：FINAL_PhaseAV.md + TODO_PhaseAV.md
