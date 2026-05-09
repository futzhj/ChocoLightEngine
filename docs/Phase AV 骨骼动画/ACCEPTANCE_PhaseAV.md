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

## Step 2 — Animator 完整化（待执行）

> Step 2 完成后追加本节。

---

## Step 3 — SkinnedMesh + GPU Skinning（待执行）

> Step 3 完成后追加本节。

---

## Step 4 — 状态机 + Transition + 事件帧（待执行）

> Step 4 完成后追加本节。
