# Phase AV — 骨骼动画 + 状态机 — 共识文档

> **6A Stage 1 收官**：所有决策已对齐，进入 Stage 2 Architect。
>
> 来源：`@e:\jinyiNew\Light\docs\Phase AV 骨骼动画\ALIGNMENT_PhaseAV.md`，§5 五个决策点用户**全部接受推荐方案** ✅。

---

## 1. 锁定的需求描述

ChocoLight 引擎新增 `Light.Animation` 模块系列，提供基于 glTF 2.0 的 3D 骨骼动画与状态机能力。复用已集成的 cgltf（无新第三方依赖），与 Phase AS（3D 渲染 + glTF + PBR）+ Phase AU（3D 物理 + Bullet）形成完整 3D 角色动作游戏闭环。

---

## 2. 锁定的技术方案

### 2.1 模块布局（Q1 锁定）

```
Light.Animation                       (顶层 require 入口 + LoadSkinnedGLTF)
Light.Animation.Clip                  (静态: 时间轴 + sampler)
Light.Animation.Skeleton              (静态: 关节层级 + 反向绑定矩阵)
Light.Animation.SkinnedMesh           (Mesh + Skeleton 引用 + JOINTS/WEIGHTS attrib)
Light.Animation.Animator              (运行时: 状态机 + Crossfade + Param + 事件)
```

### 2.2 Skinning 路径（Q2 锁定）

- **GPU skinning 默认**：vertex shader `JOINTS_0`/`WEIGHTS_0` attribute + `uniform mat4 u_jointMatrices[64]`
- **关节数上限 64**（uniform 16 KB，主流 GPU 富余）
- **CPU skinning fallback**：仅在 backend 报告 GLES 2.0 时启用（动态 VBO upload）
- **WASM**：默认走 WebGL 2.0 / GLES 3.0，无问题

### 2.3 状态机粒度（Q3 锁定）

**单层状态机** + Transition + 浮点 Param + Crossfade（线性混合两 clip）；**不**做 Layer / Sub-state Machine（留 Phase AV.x）。

```lua
animator:AddState(name, clip, {loop=true})
animator:AddTransition(from, to, condition_fn, fadeTime)
animator:SetParam(name, value)
animator:Update(dt)
animator:GetCurrentState() -> name
animator:Crossfade(targetClip, fadeTime)   -- 直接调用版（绕过 transition）
```

### 2.4 演示资源（Q4 锁定）

`samples/demo_animation/` **不入库** glTF 资源；`README.md` 指引从 `KhronosGroup/glTF-Sample-Assets` 下载（CC0 / 公开许可），路径 `samples/demo_animation/assets/`（已在 `.gitignore` 排除模式下）。Demo 自动检测资源缺失时优雅 print + 退出 0。

### 2.5 Ragdoll（Q5 锁定）

**不做**，留 Phase AV.x（Skeleton ↔ Bullet RigidBody 联动需独立设计）。

### 2.6 插值模式（已决策）

**LINEAR / STEP / CUBICSPLINE 全部支持**，按 cgltf sampler 标签自动选择。

### 2.7 文件结构

```
ChocoLight/src/light_animation.cpp                  (~1000 行)
ChocoLight/src/light_graphics_skinnedmesh.cpp       (~600 行)
ChocoLight/src/light_graphics_mesh.cpp              (改 ~100 行)
scripts/smoke/animation.lua                          (~400 行)
samples/demo_animation/main.lua                      (~80 行)
samples/demo_animation/README.md                     (~40 行 - 资源下载指引)
docs/api/Light_Animation.md                          (~200 行)
```

### 2.8 模块注册（5 项规则严格执行）

每个新 `Light.Animation*` 模块：

1. `extern "C" LIGHT_API int luaopen_Light_Animation(lua_State* L)` 导出
2. `ChocoLight/CMakeLists.txt` 加新 cpp
3. `lumen-master/src/light/light.cpp` `g_lightModules[]` 加映射（5 个模块）
4. `scripts/smoke/animation.lua` 含 API 表 + nil 边界
5. `.github/workflows/build-templates.yml` Windows runtime smoke 加 `$animSmoke = ...`

---

## 3. 任务边界（锁定）

### 3.1 In-Scope ✅

- glTF 2.0 skin/animation/node 解析
- AnimationClip 时间轴 + 三种插值
- Skeleton 关节变换树 + 反向绑定矩阵
- SkinnedMesh GPU + CPU 双路径
- Animator 状态机 + Crossfade + Param + 事件帧
- 6 平台 CI 验证

### 3.2 Out-of-Scope ❌

| 项 | 留给 |
|---|---|
| FBX / BVH / DAE 加载 | 永不 |
| IK | Phase AV.x 或 AW |
| Ragdoll | Phase AV.x |
| Animation Layer / Sub-state | Phase AV.x |
| Morph Target / Blend Shape | Phase AV.x |
| 编辑器 / 时间轴 GUI | 永不 |
| 网络同步 | 与 Net Phase 联动时 |

---

## 4. 验收标准（锁定）

### 4.1 功能验收

| # | 指标 | 阈值 |
|---|------|------|
| 1 | 加载 `RiggedSimple.glb`（KhronosGroup 标准样本） | Skeleton 关节数正确，Clip 数量正确 |
| 2 | Clip 三种插值（LINEAR/STEP/CUBICSPLINE）采样 | 数值与 cgltf 直读对照误差 < 1e-5 |
| 3 | Skeleton 前向变换树 | 子关节 global = parent global × local，与 reference 一致 |
| 4 | GPU skinning 绘制 | 1k 顶点角色 60 FPS（中端硬件） |
| 5 | CPU skinning fallback | 1k 顶点 30 FPS |
| 6 | Animator Crossfade(idle, walk, 0.3) | 中点 t=0.15 时混合权重 0.5/0.5 |
| 7 | Transition 触发 | `SetParam("speed", 1.5)` → 自动从 idle 进 walk |
| 8 | 事件帧 | clip 一次播放期间触发次数 = 注册次数 |
| 9 | 关节数 > 64 | `luaL_error` 友好报错，不崩 |

### 4.2 质量验收

- 6 平台 CI 全绿（一次成功率目标 75%，与 Phase AU 持平）
- smoke `lightc -p` 全部通过
- Windows runtime smoke `animation.lua` 通过
- ≥ 38 断言（按 ALIGNMENT §4.5 step 拆分）
- 代码规范：与现有 light_graphics_mesh.cpp 风格一致（snake_case / brace 风格 / 错误处理范式）
- 无 `printf` / `fprintf` 探针残留（commit 前清理，详见 `[MEMORY[debug.md]]` 阶段 5）

### 4.3 文档验收

- `docs/Phase AV 骨骼动画/{ALIGNMENT,CONSENSUS,DESIGN,TASK,ACCEPTANCE,FINAL,TODO}_PhaseAV.md` 全部齐备
- `docs/api/Light_Animation.md` 完整 API 表
- `docs/api/MODULE_INDEX.md` 增加 Animation 模块条目（同时**附带**补 Phase AM/AN/AQ/AR/AS/AT/AU 历史滞后条目）
- `samples/demo_animation/README.md` 资源下载指引

---

## 5. 不确定性已解决

| 原不确定项 | 解决 |
|-----------|------|
| Q1 模块布局 | ✅ Light.Animation 顶层 + 4 子模块 |
| Q2 Skinning 路径 | ✅ GPU 默认 + CPU fallback |
| Q3 状态机粒度 | ✅ 单层 + Transition + Param + Crossfade |
| Q4 演示资源 | ✅ 不入库，README 引用 KhronosGroup |
| Q5 Ragdoll | ✅ 不做，留 Phase AV.x |
| 插值模式 | ✅ LINEAR/STEP/CUBICSPLINE 全支持 |
| 关节数上限 | ✅ 64 |
| 行/列主序 | ✅ 列主序（与现有 Mat4 一致） |
| GLES 2.0 是否支持 | ✅ 编译期 fallback，运行时按 backend 检测 |

---

## 6. 项目特性规范对齐

- ✅ Lua 5.1 兼容（`unpack`/`string.char(0)`/无 `\xNN`）
- ✅ C++17（与现有代码基一致）
- ✅ 6 平台 CI（Windows/Linux/macOS/Android/iOS/Web）
- ✅ 0 新第三方依赖
- ✅ 模块注册 5 项规则
- ✅ 推送 `origin` 远程（[`MEMORY[d0cbe313]`]）
- ✅ 编译验证走 GitHub Actions（[`MEMORY[f16c3d53]`]）

---

## 7. 共识达成

**所有不确定性已确认，需求与现有架构对齐，验收标准具体可测。Stage 1 收官，进入 Stage 2 Architect 生成 DESIGN_PhaseAV.md。**
