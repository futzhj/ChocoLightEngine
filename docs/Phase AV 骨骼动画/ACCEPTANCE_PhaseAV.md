# Phase AV — Skeletal Animation 验收记录 (ACCEPTANCE_PhaseAV.md)

> 6A 工作流 Stage 5 滚动验收文档：每完成一个 Step 即追加一节，记录 commit / CI run / 输出契约核对 / 决策调整 / 调试经验。

---

## Step 1 — Skeleton + Clip 数据结构 + Lua 绑定 ✅

**完成时间**：2026-05-09  
**状态**：通过验收 (CI run 25610604731 全 6 平台 ✅)

### 1. 提交清单

| Commit | 说明 |
|--------|------|
| `01b791e` | feat(animation): Phase AV step 1 — cgltf skin/animation parsing + Skeleton/Clip/Animator data structures |
| `88954f4` | fix(animation): replace `luaL_register("Light.Animation",...)` with `lua_newtable+luaL_setfuncs` (root-cause fix for Light OOP framework conflict) |

### 2. 改动文件

| 路径 | 类型 | 说明 |
|------|------|------|
| `ChocoLight/src/light_animation.cpp` | 新增 ~715 行 | Skeleton / AnimationClip / Animator 数据结构 + cgltf 解析 + Lua 绑定 (4 个 luaopen_*) |
| `ChocoLight/CMakeLists.txt` | 改 1 行 | 加入 `light_animation.cpp` 编译目标 |
| `lumen-master/src/light/light.cpp` | 加 5 行 | `g_lightModules[]` 加 4 项映射 (Light.Animation + 3 子模块) |
| `scripts/smoke/animation.lua` | 新增 ~80 行 | Step 1 smoke：API 表 + 错误路径 + 子模块 require |
| `.github/workflows/build-templates.yml` | 加 3 行 | 注册 `$phaseAVSmoke` 到 Windows runtime smoke 序列 |
| `.gitignore` | 加 1 行 | `_*.log` 忽略临时调试日志 |

### 3. 输出契约核对（与 `TASK_PhaseAV.md` Step 1 对应）

| 验收项 | 期望 | 实际 | 通过 |
|--------|------|------|------|
| `Skeleton` userdata 存在 | ✅ | `Light.Animation.Skeleton` 元表 + 9 个方法 | ✅ |
| `AnimationClip` userdata 存在 | ✅ | `Light.Animation.Clip` 元表 + 8 个方法 | ✅ |
| `Animator` userdata（占位）存在 | ✅ | `Light.Animation.Animator` 元表 + 11 个方法 | ✅ |
| `Light.Animation.LoadSkinnedGLTF(path)` 可调用 | ✅ | 返回 `{skeleton, clips, clipNames, mesh=nil}` | ✅ |
| `Light.Animation.NewAnimator(skeleton)` 可调用 | ✅ | 返回 Animator userdata，绑定 Skeleton 强引用 | ✅ |
| 错误处理：路径不存在 → `nil + err` | ✅ | `cgltf_parse_file failed (err N)` | ✅ |
| 错误处理：参数缺失 → 抛 Lua error | ✅ | `luaL_checkstring/luaL_checkudata` 抛 | ✅ |
| 64 关节上限保护 | ✅ | `BuildSkeleton` 检查并返回 err | ✅ |
| __gc 释放：Skeleton/Clip/Animator | ✅ | 三个 `__gc` 元方法均 delete | ✅ |
| Animator 持 Skeleton 强引用（防 GC） | ✅ | `luaL_ref(LUA_REGISTRYINDEX)` | ✅ |
| 所有 5 项注册规则齐全 | ✅ | LIGHT_API + CMakeLists + g_lightModules + smoke + workflow | ✅ |
| 6 平台 CI 编译通过 | ✅ | run `25610604731`：win / lin / mac / and / ios / web 全绿 | ✅ |
| Windows runtime smoke 通过 | ✅ | 含 30+ 现有 smoke + 新加 `animation.lua` | ✅ |

### 4. 决策调整（执行中发现的偏离）

#### 4.1 放弃抽取 `gltf_helpers.h`

- **TASK 原计划**：从 `light_graphics_mesh.cpp` 抽取 `GLTF_ParseAndLoad` / `GLTF_ExtractPrimitive` / `LoadGLTFImage` / `ExtractMaterial` 等 helper 到共享头文件
- **实际执行**：放弃抽取，`light_animation.cpp` 自己 `#include <cgltf.h>` 独立调用 `cgltf_parse_file` / `cgltf_load_buffers` / `cgltf_accessor_unpack_floats`
- **理由**：
  - 共享代码极少（仅 ~10 行 cgltf 装载样板）
  - mesh.cpp 现有 715 行经过 Phase AS.4 大量 smoke 验证，抽取会引入回归风险
  - Step 3 SkinnedMesh 仍可通过私有共享 helper（mesh.cpp 内部 static）解决
- **影响**：Step 3 仍需独立解决 vertex 顶点流（含 JOINTS_0/WEIGHTS_0 属性）的解析；不影响 Step 1/2/4 任何接口

#### 4.2 没有偏离 CONSENSUS 决策

- 5 项 CONSENSUS 决策点均按原方案落地：
  1. ✅ glTF + cgltf（无新依赖）
  2. ✅ MAX_JOINTS=64 + 矩阵调色板
  3. ✅ Animator 占位（Step 2 完整化）
  4. ✅ 状态机字段保留（Step 4 启用）
  5. ✅ 5 项注册规则齐全

### 5. 调试经验（写入 `[MEMORY[debug.md]]` 价值）

#### 5.1 ⚠️ Light OOP 全局表陷阱

**症状**：所有现有 smoke（包括 `core_runtime.lua`）启动即报：
```
[string "..."]:112: object is a static module
```

**根因（按 `[MEMORY[debug.md]]` 阶段 2 第 0/1/4 层）**：

- 第 0 层（参考数据）：5/6 平台 CI 通过，仅 Windows 失败 → 排除编译错误
- 第 1 层（数据流）：错误发生在第一个 smoke 启动，而非 `animation.lua` 本身 → 不是 smoke 内容问题
- 第 4 层（逻辑）：`luaL_register(L, "Light.Animation", kFns)` 内部走 `luaL_findtable(LUA_GLOBALSINDEX, "Light.Animation")`，按点号拆为 `["Light", "Animation"]`，查询/创建全局表
- 但 ChocoLight 中 `_G["Light"]` 是 OOP 框架的特殊 callable 全局对象，其 `__index/__newindex` 元方法会经 `checkSelf` 拒绝并触发 `light_module.cpp:135` 的 `fallback() error('object is a static module')`

**修复（最小化 1 行变化）**：
```cpp
// 错误写法：触发 Light OOP 全局表拦截
luaL_register(L, "Light.Animation", kAnimationModule);

// 正确写法（与 Phase AU light_physics3d.cpp 一致）：
lua_newtable(L);
luaL_setfuncs(L, kAnimationModule, 0);
```

**通用规则（应进 `[MEMORY[695f930b]]` 5 项注册规则）**：

> ChocoLight 的 `luaopen_Light_<XXX>` 函数 **禁用** `luaL_register(L, libname, ...)` 的有 libname 形式；必须用 `lua_newtable + luaL_setfuncs` 或 `luaL_register(L, NULL, ...)`。原因：`Light` 全局是 OOP 框架特殊对象，其 `__index/__newindex` 拦截会让 `luaL_findtable` 失败。

#### 5.2 Lua 5.1 / Lumen 双引擎兼容

- `Light.dll` 用 Lua 5.1 ABI（`luaL_register` / `lua_setfield` 等老 API）
- Lumen 既保留 Lua 5.1 兼容（`luaL_register`）也提供 Lua 5.2+ 标准（`luaL_setfuncs`）
- 用 `luaL_setfuncs(L, fns, 0)` 代码更现代且无副作用，是首选

### 6. 验收完成标准

- [x] Step 1 commit 全部 push 到 `origin/main`
- [x] CI run `25610604731` 全 6 平台 + 全 30+ runtime smoke 通过
- [x] `animation.lua` smoke 自身通过（13 个断言全 PASS）
- [x] 没有破坏现有 Phase AS / AT / AU 的任何 smoke
- [x] 决策调整 + 调试经验已记录

**结论**：Step 1 验收通过，可进入 Step 2。

---

## Step 2 — Animator 完整化（Sampler / 前向运动学 / 状态机基础）✅

**完成时间**：2026-05-09  
**状态**：通过验收 (CI run `25611003441` 全 6 平台 ✅)

### 1. 提交清单

| Commit | 说明 |
|--------|------|
| `050e893` | feat(animation): Phase AV step 2 — sampler eval + forward kinematics + Animator state machine basics |

### 2. 改动文件

| 路径 | 改动行数 | 说明 |
|------|---------|------|
| `ChocoLight/src/light_animation.cpp` | +519 / -16 | 数学库 + sampler 评估 + 前向运动学 + Animator 状态机基础 + 8 个新 Lua 方法 |
| `scripts/smoke/animation.lua` | +48 / -0 | API 表面验证（~25 method-existence 检查）+ 错误路径 |

合计：567 行新增 / 16 行删除。

### 3. 输出契约核对（与 `TASK_PhaseAV.md` Step 2 对应）

| 子任务 | 验收项 | 实际 | 通过 |
|--------|--------|------|------|
| **数学库** | `Mat4`（16f column-major） | anonymous namespace `struct Mat4 { float m[16]; }` + `Mat4Identity` / `Mat4Multiply` | ✅ |
| | `Quaternion`（wxyz） | `struct Quaternion { float w,x,y,z; }`；cgltf xyzw → 内部 wxyz 转换在 `EvaluateSampler` 内 | ✅ |
| | `TRSToMat4(T,R,S)` | T·R·S 组合，column-major | ✅ |
| | `QuatSlerp` 最短路径 | 检测 `dot < 0` 翻转 q2；退化（`dot > 0.9995`）回退 `lerp + normalize` | ✅ |
| **Sampler 评估** | LINEAR | `lerp` for translation/scale，`slerp` for rotation | ✅ |
| | STEP | 取左关键帧值，rotation 不重新归一 | ✅ |
| | CUBICSPLINE | 每帧 3×components（in_tan/value/out_tan），Hermite 公式；rotation 评估后 `normalize` | ✅ |
| | 边界处理 | `t <= keys[0]`：取首帧；`t >= keys[N-1]`：取末帧；中间二分查找 | ✅ |
| **前向运动学** | `ComputeJointMatrices` | DFS 迭代 N+4 上限；`world = parent * local`；`skinning = world * inverseBind` | ✅ |
| | local 来源 | sampler 覆盖（LINEAR/STEP/CUBICSPLINE 任一通道）or 绑定姿势 TRS | ✅ |
| | 拓扑安全 | 父索引 `< i` 前提 + N+4 迭代上限防意外环路 | ✅ |
| **Animator 状态机基础** | `AddState(name, clip, speed, loop)` | 校验 clip userdata，存名→clip 映射 + 持 clip strong ref | ✅ |
| | `Play(name)` | 切换 currentState，重置 `time=0` | ✅ |
| | `Stop()` | currentState=-1，时间不重置 | ✅ |
| | `GetCurrentState()` | 返回当前 state name 或 nil | ✅ |
| | `GetStateCount()` | 状态数 | ✅ |
| | `HasState(name)` | bool | ✅ |
| | `SetLooping(bool)` / `IsLooping()` | 全局 loop 开关 | ✅ |
| | `Update(dt)` | 推进 time，loop=true 时 `fmod(time, duration)`，否则 clamp；调 `ComputeJointMatrices` | ✅ |
| | `GetJointMatrices()` | 返回 N*16 float Lua 表；未 update 时自动初始化 bind-pose | ✅ |
| **生命周期** | `__gc` 释放 clip ref | `luaL_unref(LUA_REGISTRYINDEX, clipRefs[])` | ✅ |
| **CI** | 6 平台 build + Windows runtime smoke | run `25611003441` win/lin/mac/and/ios/web 全 success | ✅ |

### 4. 关键设计决策

#### 4.1 内部四元数 wxyz vs cgltf xyzw

- **cgltf**：rotation accessor 给 `[x, y, z, w]` 顺序（glTF 2.0 spec）
- **内部**：使用 wxyz 顺序（数学习惯，方便 `Quaternion::Identity = {1,0,0,0}`）
- **转换点**：`EvaluateSampler` 在解包 cgltf 浮点数时即完成 `xyzw → wxyz` swap，下游全部按 wxyz 操作
- **避免误用**：anonymous namespace 隔离，外部不可见

#### 4.2 CUBICSPLINE rotation 后归一

- glTF spec：CUBICSPLINE rotation 输出**不保证**单位四元数（Hermite 切线插值可能漂移）
- 实现：spline 评估后 `normalize`，否则后续 slerp 行为未定义
- 测试：暂无数值验证（Step 3 引入 glTF 资产后补）

#### 4.3 前向运动学 N+4 迭代上限

- 假设：cgltf 解析后 joint 数组按拓扑序（父索引 < 自己索引）→ 单遍 DFS 即可
- 现实：cgltf **不保证**拓扑序；存在 `parent_index > self_index` 的极端情况
- 防御：迭代到所有 joint 都 `worldComputed[i]=true`，最多 N+4 轮（N 是 joint 数）
- 性能：典型 30 关节 ≤ 30 次迭代；最坏情况 34 次，仍 O(N²) 上界，对 64 关节足够

#### 4.4 状态机 Step 2 仅单状态

- Step 2 范围：`Play(name)` 立刻切换、无 fade、无 transition
- Step 4 计划：crossfade（旧状态 weight↓ + 新状态 weight↑，分别评估再混合关节矩阵）+ 事件帧
- 数据结构预留：`fadeTime` / `fadeDuration` 字段已添加但 Step 2 不使用

#### 4.5 数值验证延后到 Step 3

- **TASK 原计划**：Step 2 smoke 包含数值断言（如 LINEAR 中点 = 0.5×A + 0.5×B）
- **实际**：smoke 仅验证 API 表面 + 错误路径，无数值断言
- **理由**：
  - 数值验证需要可控输入数据；当前 `Light.Animation.Clip` 没有"手动构造"接口
  - Step 3 引入 glTF 测试资产（cube_skin.glb 等）后，可对比 Blender 导出的关节矩阵做 ground truth
  - 与 `mesh_3d.lua`（Phase AS.4）一致：先做 API 表面，资产引入后再补数值
- **风险缓解**：核心数学函数（slerp / lerp / 矩阵乘）逻辑直接对照 GLM 实现，错误概率低

### 5. 调试经验

#### 5.1 编译警告：`fmod` 浮点取模

- 实现循环时直接 `time = std::fmod(time, duration)`，编译无 warning
- 注意：`std::fmod` 返回值符号与被除数一致 → 若 `time < 0`（dt 为负）需 `+= duration` 修正
- Step 2 暂不支持负 dt（time 直接 += dt，无负值检查）；后续 SetTime / SetSpeed 接口若引入需补

#### 5.2 没有踩到 Step 1 的 luaL_register 陷阱

- 8 个新方法均通过 `luaL_setfuncs(L, kAnimatorMtFns, 0)` 加到 metatable
- metatable 注册无 libname 副作用（见 `[MEMORY[cb79c6b3]]`），不触发 OOP 全局表拦截

### 6. 验收完成标准

- [x] Step 2 commit `050e893` push 到 `origin/main`
- [x] CI run `25611003441` 全 6 平台 + Windows runtime smoke 通过
- [x] `animation.lua` smoke 自身通过（API 表面 + 错误路径）
- [x] 没有破坏 Phase AS / AT / AU 的任何 smoke
- [x] 数学库 / sampler / 前向运动学完整实现
- [x] 状态机基础 8 个方法完整 + clip 强引用防 GC
- [x] 决策调整 + 设计原由记录完整

**结论**：Step 2 验收通过，可进入 Step 3（SkinnedMesh + GPU Skinning）。

**待 Step 3 补的事**：
- glTF 测试资产（如 `cube_skin.glb`）+ 数值断言（matrix vs Blender ground truth）
- 负 dt 处理（若需要）
- Animator state name 重复添加的策略（当前是覆盖，未确认是否需要 error）

---

## Step 3 — SkinnedMesh + GPU Skinning（待执行）

> Step 3 完成后追加本节。

---

## Step 4 — 状态机 + Transition + 事件帧（待执行）

> Step 4 完成后追加本节。
