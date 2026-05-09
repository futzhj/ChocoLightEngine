# Light.Animation

Phase AV — 骨骼动画 + 状态机 + Crossfade + 事件帧

跨 6 平台 (Windows / Linux / macOS / Android / iOS / Web) 一致 API。基于 cgltf 解析 glTF 2.0，CPU skinning 渲染。

## 模块结构

| 模块 | 用途 |
|------|------|
| `Light.Animation` | 顶层：`LoadSkinnedGLTF` / `NewAnimator` / `DrawSkinnedMesh` |
| `Light.Animation.Skeleton` | 骨骼数据 userdata |
| `Light.Animation.Clip` | AnimationClip userdata（采样器集合） |
| `Light.Animation.Animator` | 播放控制 + 状态机 + Transition / Crossfade / Event / Param |
| `Light.Animation.SkinnedMesh` | 蒙皮网格 userdata（CPU skinning） |

## 顶层 API

### `Light.Animation.LoadSkinnedGLTF(path)`

加载 glTF 2.0 文件并解析骨骼/动画/蒙皮 mesh。

**参数**

| 名称 | 类型 | 说明 |
|------|------|------|
| `path` | `string` | glTF 文件路径（`.gltf` 或 `.glb`） |

**返回**

成功：返回 `pack` table：

| 字段 | 类型 | 说明 |
|------|------|------|
| `skeleton` | `Skeleton` userdata | 骨骼（关节数 ≤ 64） |
| `clips` | `table<string, Clip>` | 名 → AnimationClip 映射 |
| `clipNames` | `string[]` | 1-based 顺序的 clip 名数组 |
| `mesh` | `SkinnedMesh` 或 `nil` | 第一个含 JOINTS_0/WEIGHTS_0 的 mesh primitive；无则 nil |

失败：返回 `nil, err_string`

**示例**

```lua
local Anim = require 'Light.Animation'
local pack, err = Anim.LoadSkinnedGLTF('character.glb')
assert(pack, err)
print(pack.skeleton:GetJointCount(), #pack.clipNames)
```

---

### `Light.Animation.NewAnimator(skeleton)`

创建一个 Animator 绑定到指定 Skeleton。

**参数**

| 名称 | 类型 | 说明 |
|------|------|------|
| `skeleton` | `Skeleton` | 已加载的骨骼 |

**返回**：`Animator` userdata。Animator 持 Skeleton 的强引用，防止 GC。

---

### `Light.Animation.DrawSkinnedMesh(mesh, animator, transform_mat4, material)`

每帧调用，把蒙皮网格渲染到当前 3D 场景。

**参数**

| 名称 | 类型 | 说明 |
|------|------|------|
| `mesh` | `SkinnedMesh` | LoadSkinnedGLTF 返回的 mesh |
| `animator` | `Animator` | 提供 jointMatrices |
| `transform_mat4` | `table[16]` 或 `nil` | 模型矩阵列主序；nil = identity |
| `material` | `Material` userdata 或 `nil` | Phase AS 材质；nil = 默认 |

**返回**

| 状态 | 返回 |
|------|------|
| 成功 | `true` |
| 渲染上下文未就绪 / mesh dead | `false, err_string` |

**说明**：headless 环境 / 引擎未初始化时返回 `false + err`，**不崩**。

**实现**：CPU skinning（每帧 DeleteMesh + CreateMesh），跨平台一致。GPU skinning 性能优化留 Phase AV.x。

---

## Skeleton 方法

| 方法 | 返回 | 说明 |
|------|------|------|
| `:GetJointCount()` | `int` | 关节总数（≤ 64） |
| `:GetJointName(idx_1based)` | `string` 或 `nil` | 1-based 关节名 |
| `:FindJoint(name)` | `int` 或 `nil` | 找不到返回 nil |
| `:GetJointParent(idx)` | `int` | 父关节索引；root 返回 0 |
| `:GetRootJoint()` | `int` | 根关节索引（1-based） |
| `:GetBindLocalTRS(idx)` | `T,R,S` | 各 3/4/3 floats（R 为 wxyz） |
| `:GetInverseBindMatrix(idx)` | `float[16]` | 列主序 mat4 |
| `:IsAlive()` | `bool` | __gc 后 false |
| `:Delete()` | — | 显式释放 |

---

## Clip 方法

| 方法 | 返回 | 说明 |
|------|------|------|
| `:GetName()` | `string` | clip 名 |
| `:GetDuration()` | `number` | 秒 |
| `:GetSamplerCount()` | `int` | 采样器数量 |
| `:GetSamplerInfo(idx)` | `table` | `{joint, target, interp, keyCount}` |
| `:Sample(t, jointIdx, target)` | floats | target ∈ `'translation'/'rotation'/'scale'` |
| `:IsAlive()` / `:Delete()` | — | 同 Skeleton |

---

## Animator 方法

### 时间 / 速率

| 方法 | 说明 |
|------|------|
| `:Update(dt)` | 推进 currentTime；自动处理 looping / crossfade / transitions / events / 重新计算关节矩阵 |
| `:GetCurrentTime()` / `:SetCurrentTime(t)` | 当前 active clip 上的时间 |
| `:GetPrevTime()` | 上一帧 currentTime（事件帧跨边界判定用） |
| `:SetSpeed(s)` | 时间倍率（1.0 = 正常；负值倒放） |
| `:Pause()` / `:Resume()` / `:IsPaused()` | 暂停 |

### 关节矩阵

| 方法 | 返回 | 说明 |
|------|------|------|
| `:GetJointMatrices()` | `table` of N×16 floats | 列主序蒙皮矩阵；空时自动计算 bind pose |
| `:GetSkeleton()` | `Skeleton` | 关联骨骼 |

### 状态机基础（Step 2）

| 方法 | 说明 |
|------|------|
| `:AddState(name, clip)` | 注册 state；持 clip 强引用 |
| `:Play(name)` | 立即切换 state；currentTime = 0 |
| `:Stop()` | 清空 currentState 和 crossfade |
| `:GetCurrentState()` | string 或 nil |
| `:GetStateCount()` / `:HasState(name)` | — |
| `:SetLooping(bool)` / `:IsLooping()` | 全局 loop 开关 |

### Transition（Step 4）

`Transition` = `(fromState → toState, condFn, duration)`。Update 每帧检查首个 `condFn` 返回 `true` 的；启动 crossfade；同帧最多触发一次。

| 方法 | 说明 |
|------|------|
| `:AddTransition(fromState, toState, condFn, duration)` | `fromState=""` 表示 Any state；`condFn(animator) -> bool`；`duration=0` 立即切换 |
| `:ClearTransitions()` | 释放所有 condFn 引用 |
| `:GetTransitionCount()` | 当前 transition 数 |

**示例**

```lua
animator:AddTransition("Idle", "Walk", function(a)
    return (a:GetParam("speed") or 0) > 0.5
end, 0.3)

animator:Play("Idle")
animator:SetParam("speed", 1.0)
animator:Update(1/60)   -- 触发 Idle→Walk crossfade (300ms)
```

### Crossfade（Step 4，手动）

| 方法 | 说明 |
|------|------|
| `:Crossfade(targetState, duration)` | 立即启动；`duration=0` 立即切换；返回 true / `nil+err` |
| `:IsCrossfading()` | bool |
| `:GetCrossfadeProgress()` | `number ∈ [0, 1]` |
| `:GetCrossfadeTarget()` | string 或 nil |

**说明**：crossfade 期间，`activeClip` 与 `crossfadeClip` 各自推进 time，关节矩阵按 (T lerp / R slerp / S lerp) 混合后再做 forward kinematics。

### Event（Step 4）

事件帧：在 active clip 的 `prevTime → currentTime` 区间跨过 `triggerTime` 时调 `callbackFn(animator)`。**自动处理 looping 跨边界**。

| 方法 | 说明 |
|------|------|
| `:AddEvent(state, triggerTime, callbackFn)` | state 必须已 AddState |
| `:ClearEvents()` | 释放所有 callback 引用 |
| `:GetEventCount()` | — |

**循环边界示例**

```lua
animator:AddEvent("Walk", 0.4, function() print("footstep!") end)
animator:Play("Walk")
animator:SetCurrentTime(0.95)
animator:Update(0.1)   -- 跨过 1.0 → 0.05; trigger 0.4 不在 [0.95, 0.05+1] 内 → 不触发
animator:Update(0.4)   -- 累计到 0.45; trigger 0.4 ∈ [0.05, 0.45] → 触发一次
```

### Param（Step 4）

| 方法 | 说明 |
|------|------|
| `:SetParam(name, value)` | number 类型；其他类型抛错 |
| `:GetParam(name)` | number 或 nil |
| `:HasParam(name)` | bool |

**说明**：Param 仅 number 类型。Transition condFn 通常读 Param 做条件判断（如 `speed > 0.5`）。

### 生命周期

| 方法 | 说明 |
|------|------|
| `:IsAlive()` | __gc 后 false |
| `:Delete()` | 显式释放：clip refs / transition condFn refs / event callback refs / Skeleton ref / params |

---

## SkinnedMesh 方法

| 方法 | 返回 | 说明 |
|------|------|------|
| `:GetVertexCount()` | `int` | 顶点数 |
| `:GetIndexCount()` | `int` | 索引数 |
| `:GetSkeleton()` | `Skeleton` | 关联骨骼（与 Animator 共享） |
| `:IsAlive()` / `:Delete()` | — | __gc 时自动释放 GPU mesh |

---

## 错误处理约定

| 调用 | 失败行为 |
|------|---------|
| 缺参数 / 类型错 | `luaL_checkXxx` 抛 Lua error（pcall 包裹） |
| 文件 / 资源不存在 | 返回 `nil, err_string`（**不**抛错） |
| dead userdata | 返回 `false, "dead"` 或 nil |
| Lua callback 抛错（condFn / event） | 写 `stderr` 日志，**不**中断 Update |

## 实现细节

- **关节上限**：64（超出 LoadSkinnedGLTF 返回 `nil + err`）
- **矩阵格式**：列主序 mat4（与 OpenGL/glm/cgltf 一致）
- **四元数**：内部 wxyz；cgltf 输入 xyzw 自动转换
- **插值**：LINEAR / STEP / CUBICSPLINE 自动按 cgltf 标签分发
- **CUBICSPLINE rotation**：评估后归一化（spec 不保证单位）
- **Slerp**：含最短路径翻转（`dot < 0` 翻转 b）
- **Cross-frame events**：基于 prevTime/currentTime 区间，含 looping wrap 处理
- **Skinning**：CPU 路径（`DeleteMesh + CreateMesh` 每帧）；GPU skinning 留 Phase AV.x

## 相关

- 工作流文档：`docs/Phase AV 骨骼动画/`
- 示例：`samples/demo_animation/`
- Smoke：`scripts/smoke/animation.lua`
