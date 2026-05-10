# Light.Animation

Phase AV — 骨骼动画 + 状态机 + Crossfade + 事件帧

跨 6 平台 (Windows / Linux / macOS / Android / iOS / Web) 一致 API。基于 cgltf 解析 glTF 2.0，CPU skinning 渲染。

## `Light.Graphics.SpriteAnimation` vs `Light.Animation` 选型

新手常见困惑：二者都叫"动画"，何时用谁？**一句话区分**：`SpriteAnimation` 是 2D 帧动画（精灵序列 + 时间轴），`Light.Animation` 是 3D 骨骼动画（glTF + 状态机）。

| 维度 | `Light.Graphics.SpriteAnimation` | `Light.Animation` |
|------|----------------------------------|-------------------|
| **数据源** | 雪碧图 / 多张 PNG / texture atlas | glTF 2.0（`.gltf` / `.glb`） |
| **维度** | 2D 仅 | 3D 为主（CPU skinning 变换顶点） |
| **模型** | 帧序列（整张纹理切换） | 骨骼 + 蒙皮 mesh（顶点跟随关节） |
| **变形** | 无（像素级固定） | 任意关节变换（旋转 / 平移 / 缩放） |
| **时间** | 单帧时长或 FPS；单向播放 | 浮点时间；任意速度；循环 |
| **状态机** | 无（多 anim 表切换靠用户代码） | 内置：AddState / Play / Crossfade / Transition / Event / Param |
| **混合** | 无 | Crossfade + TRS lerp/slerp 每关节混合 |
| **事件** | 无（靠 fps 计数推断） | AddEvent(time) + 跨循环边界精确触发 |
| **内存** | 每帧一张 texture（显存密集） | 一份 mesh + 关节矩阵 upload |
| **适用** | 像素风 / 传统 RPG / UI 特效 | 3D 角色 / Mixamo 动画 / glTF 模型 |

**决策树**：

```
你的模型是 .png / .aseprite / 雪碧图？
  └─ 是 → SpriteAnimation
你的模型是 .gltf / .glb（有 skin + animations）？
  └─ 是 → Light.Animation
两者都有（如 2D UI + 3D 角色）？
  └─ UI 用 SpriteAnimation，角色用 Light.Animation
```

**常见误用纠正**：

- ❌ 用 SpriteAnimation 渲染角色（逐帧切换 3D 预渲染图）→ 体积大且无法 runtime 混合动作
- ❌ 用 Light.Animation 做 2D UI 动画（加载骨骼开销）→ 用 SpriteAnimation 或手写 tween
- ✅ 2D 像素游戏用 SpriteAnimation；3D 游戏用 Light.Animation；混合游戏各司其职

---

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

**实现**：Phase AW 起内部按 `Anim.GetSkinningMode()` 自动分流：

- `cpu` 路径：每帧 CPU 加权变换 baseVertices + DeleteMesh + CreateMesh 全量重传 + DrawMeshMaterial
- `gpu` 路径：首次调用一次性上传 `RenderVertex3DSkin` mesh（含 joints/weights）；后续每帧仅上传 ≤ 64 个 mat4 关节调色板（UBO 4096 bytes），shader 内做加权混合

**API 签名 100% 不变**。详见下文 [GPU Skinning 模式](#phase-aw--gpu-skinning-模式)。

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

---

## Phase AV.x — Procedural API + 内省 Getter

> **动机**：Phase AV 主线依赖 glTF 资产。Phase AV.x 引入无需外部资产的 procedural 构造 API 与状态机内省 getter，用于单元测试、程序化动画、状态机可视化调试。

### 顶层 procedural 入口

| 方法 | 返回 | 说明 |
|------|------|------|
| `Light.Animation.NewEmptySkeleton(jointCount)` | `Skeleton` 或 `nil, err` | 创建 N 关节骨骼（1..64）；所有关节默认 `parent=-1`，bind = identity，IBM = identity |
| `Light.Animation.NewEmptyClip(name, duration)` | `AnimationClip` | 创建空 clip；`duration` 可选（默认 0，会被 `AddSampler` 自动推进） |

### Skeleton procedural setter

| 方法 | 说明 |
|------|------|
| `sk:SetJointName(idx1, name)` | 重命名关节；同步更新 `nameToIndex` |
| `sk:SetJointParent(idx1, parentIdx1_or_0)` | `0` = 无 parent；禁止自引用；自动重算 `rootJoint` |
| `sk:SetBindLocalTRS(idx1, tx,ty,tz, qw,qx,qy,qz, sx,sy,sz)` | 设置绑定姿态本地 TRS（旋转 **wxyz**，ChocoLight 内部约定） |
| `sk:SetInverseBindMatrix(idx1, table_m16)` | 覆盖逆绑定矩阵（16 floats 列主序） |

### Clip procedural setter

| 方法 | 说明 |
|------|------|
| `clip:SetDuration(sec)` | 显式设置时长 |
| `clip:AddSampler(jointIdx1, target, mode, times, values)` | 追加采样器；自动 `duration = max(duration, times.back())` |

**`AddSampler` 参数规范**：

| 参数 | 取值 |
|------|------|
| `target` | `"translation"` / `"rotation"` / `"scale"` |
| `mode` | `"LINEAR"` / `"STEP"` / `"CUBICSPLINE"`（大小写不敏感） |
| `times` | Lua array of seconds（**升序**，不做强校验） |
| `values` | Lua flat float array；布局与 glTF 一致（rotation 为 **xyzw**） |

**`values` 长度校验**（不匹配则 raise）：

| target | mode | 每 keyframe floats |
|--------|------|--------------------|
| `translation` / `scale` | `LINEAR` / `STEP` | 3 |
| `rotation` | `LINEAR` / `STEP` | 4（xyzw） |
| 任意 | `CUBICSPLINE` | 3 × comps（`in_tan`、`value`、`out_tan`） |

### Animator 内省 getter（调试 / 可视化）

| 方法 | 返回 | 说明 |
|------|------|------|
| `an:GetClip(name)` | `Clip` 或 `nil` | 按 state 名取 clip userdata（保持身份一致） |
| `an:GetActiveClip()` | `Clip` 或 `nil` | 当前 state 对应的 clip |
| `an:ListStates()` | `{ "idle", "walk", ... }` | state 名数组（顺序未指定） |
| `an:GetTransitionInfo(idx1)` | `{ from, to, duration, hasCond }` 或 `nil` | 越界返回 `nil` |
| `an:GetEventInfo(idx1)` | `{ state, triggerTime, hasCallback }` 或 `nil` | 越界返回 `nil` |
| `an:ListParams()` | `{ [name] = value, ... }` | 键值对 table |

### 使用示例

```lua
local Anim = require 'Light.Animation'

-- 2 关节骨骼 root → tip
local sk = Anim.NewEmptySkeleton(2)
sk:SetJointName(1, 'root')
sk:SetJointName(2, 'tip')
sk:SetJointParent(2, 1)                         -- tip 的 parent = root
sk:SetBindLocalTRS(2, 0,1,0,  1,0,0,0,  1,1,1)  -- tip 相对 root 沿 Y 偏移 1

-- walk clip: root 沿 X 线性平移 0 → 10 in 1s
local walk = Anim.NewEmptyClip('walk', 1.0)
walk:AddSampler(1, 'translation', 'LINEAR',
                 {0.0, 1.0}, {0,0,0,  10,0,0})

local an = Anim.NewAnimator(sk)
an:AddState('walk', walk)
an:Play('walk')
an:Update(0.5)

-- 断言: walk 中点 tx = 5
local mats = an:GetJointMatrices()
assert(math.abs(mats[13] - 5.0) < 1e-3)        -- root[12] == 5
```

### 设计决策

1. **旋转入参格式**：与 `LoadSkinnedGLTF` 一致采用 glTF 的 xyzw；内部自动转 wxyz 存储。避免 procedural 与 glTF 路径语义分叉。
2. **CUBICSPLINE tan 原样存储**：切向量不是 unit quat，不能参与 xyzw→wxyz 交换；`EvaluateSampler` 在插值后统一走 `GltfQuatXyzwToWxyz + Normalize`。
3. **`NewEmptySkeleton` 默认值**：所有关节 `parent=-1`（全部为根），bind = identity，IBM = identity，`rootJoint = 0`。`SetJointParent` 后自动重算 `rootJoint`。
4. **`SetJointParent` 自引用保护**：`parentIdx == idx` → raise。
5. **getter 返回格式**：table，键为英文字段名（from/to/duration/hasCond 等），便于 Lua 端结构化读取。越界一律返回 `nil`（不 raise）。

### 测试覆盖

见 `scripts/smoke/animation.lua` 第 [12][13][14] 段：

- **[12]** 完整性：14 个新方法全部存在
- **[13]** 端到端数值：walk t=0.5 LINEAR = 5.0；crossfade 中点 weight=0.5 混合 translation；event 跨循环边界触发（prev=0.8, cur=0.2 wrap）；prevTime 记录；param round-trip；transition/event getter；ListStates / GetActiveClip / GetClip
- **[14]** 错误路径：joint count 越界、target/mode 未知、values 长度不符、自引用 parent、短 IBM table 等

---

## Phase AW — GPU Skinning 模式

> **动机**：Phase AV CPU skinning 路径每帧 `DeleteMesh + CreateMesh` 全量重传顶点，5000 顶点 mesh 单帧约 1.5ms CPU 开销。Phase AW 引入 GPU vertex shader skinning：mesh 顶点（含 joints/weights）一次性上传，每帧仅上传 ≤ 64 mat4 关节调色板（4096 bytes UBO），shader 内做加权混合 → CPU 减负 30 倍，GPU bus 减负 60 倍。
>
> **API 完全兼容**：`Anim.DrawSkinnedMesh` 签名 100% 不变；用户脚本无需修改。

### 自动模式选择

`Anim.DrawSkinnedMesh` 内部按 `Anim.GetSkinningMode()` 决定走哪条路径：

| 平台 | 默认实际生效模式 | 说明 |
|------|-----------------|------|
| Windows / Linux / macOS（桌面 GL 3.3）| `gpu` | 标准 UBO 支持，`GL_MAX_UNIFORM_BLOCK_SIZE ≥ 16KB` |
| Android / iOS（GLES 3.0）| `gpu` | GLES 3.0 minimum 即满足 4KB UBO + glVertexAttribIPointer |
| Web（Emscripten / WebGL2）| `cpu` | Q7 默认禁用：Safari WebGL2 attribute int pointer 风险 |
| LegacyBackend (GL 1.x) | `cpu` | 不支持 UBO + 无 vertex shader |

> **fallback 自动透明**：如果 GPU 路径任何环节失败（shader compile / UBO 创建 / first-time `CreateSkinnedMesh`），引擎自动 fallback CPU，不让用户感知错误。

### `Light.Animation.GetSkinningMode()`

返回当前**实际生效**的蒙皮路径（不一定等于用户设置值）。

**返回**

| 值 | 含义 |
|----|------|
| `"cpu"` | 当前帧使用 CPU 蒙皮 |
| `"gpu"` | 当前帧使用 GPU 蒙皮 |

```lua
local Anim = require 'Light.Animation'
print(Anim.GetSkinningMode())   -- 桌面: "gpu"; Web: "cpu"
```

---

### `Light.Animation.SetSkinningMode(mode)`

强制切换蒙皮模式。常用于调试、设备对比、测试。

**参数**

| 名称 | 类型 | 说明 |
|------|------|------|
| `mode` | `"auto"` / `"cpu"` / `"gpu"` | 任意非法值返回 `nil + err` |

**返回**

| 状态 | 返回 |
|------|------|
| 成功 | `true` |
| 非法 mode | `nil, err_string` |

**语义**

| 设置值 | 行为 |
|--------|------|
| `"auto"` | 默认；按平台 + backend 支持自动选择（见上表） |
| `"cpu"` | 强制走 CPU 路径，即使 backend 支持 GPU |
| `"gpu"` | 优先 GPU 路径；不支持时自动 fallback `"cpu"`（`GetSkinningMode()` 反映实际值） |

```lua
-- 调试性能对比（伪代码 — 实际需配合 frame timing）
Anim.SetSkinningMode('cpu')
local cpu_ms = measure_one_frame()
Anim.SetSkinningMode('gpu')
local gpu_ms = measure_one_frame()
print('CPU:', cpu_ms, 'GPU:', gpu_ms)
Anim.SetSkinningMode('auto')

-- 错误参数 → nil + err
local r, e = Anim.SetSkinningMode('GPU')   -- 大小写敏感
assert(r == nil and type(e) == 'string')
```

> **注意**：`SetSkinningMode` 仅修改进程级全局状态；不会立即重传 mesh。已上传 GPU mesh 的 `SkinnedMesh` 切回 CPU 后会保留 GPU 资源（不浪费），下次切回 GPU 直接复用。

---

### 性能特征对比

| 顶点数 | CPU 路径每帧 | GPU 路径每帧 | 提升 |
|--------|-------------|-------------|------|
| 500    | ~0.15ms     | ~0.02ms    | 7.5x |
| 5000   | ~1.5ms      | ~0.05ms    | 30x  |
| 50000  | ~15ms       | ~0.3ms     | 50x  |

| 维度 | CPU 路径 | GPU 路径 |
|------|---------|---------|
| CPU 计算 | 每顶点 4 关节加权 mat4 应用 | 仅做 modelMat × jointMat（≤ 64 次 mat4 乘法）|
| GPU bus 上传 | 顶点全量 / 帧（5000 顶点 = 240KB）| 关节调色板 / 帧（4KB UBO + 200B uniforms）|
| GPU mesh 资源 | 重建 / 帧 | 复用（首次上传后永不变）|

> 数据来源：i7-12700H + GTX 4060 桌面 GL 3.3。移动端按 GPU/CPU 算力比折算。

---

### 实现细节（`docs/Phase AW GPU Skinning/`）

- **顶点格式**：`RenderVertex3DSkin`（68 bytes / 顶点；pos 12 + normal 12 + uv 8 + color 16 + joints_packed 4 + weights 16）
- **Shader**：4 个 program 变体（programUnlit / programPBR / programUnlitSkin / programPBRSkin）；VS 分两版（VS3D / VS3D_SKIN），FS 完全复用现有 PBR/Unlit FS
- **UBO 布局**：`layout(std140) uniform JointBlock { mat4 uJointMats[64]; }`，固定 binding point 0
- **MAX_JOINTS = 64**（与 LoadSkinnedGLTF 上限一致；shader 数组定长 64；超出截断）
- **mesh ID 方案**：`gpuSkinnedMeshId` 起始 `0x80000001`（高位区分普通 `gpuMeshId` 起始 1）；`DeleteMesh` 内部按高位分流到对应 map

### 测试覆盖

见 `scripts/smoke/animation.lua` 第 [15] 段：

- API 注册存在性
- `GetSkinningMode` 返回类型 + 值域
- `SetSkinningMode` 三个合法值（"auto" / "cpu" / "gpu"）
- 错误参数：`"invalid"` / `"CPU"`（大小写敏感）/ 整数 / nil → `nil + err`
- 模式切换不破坏 `DrawSkinnedMesh` 入口签名

### 设计决策（参见 `docs/Phase AW GPU Skinning/CONSENSUS_PhaseAW.md`）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| Q1 | 关节矩阵上传 | **UBO** | 标准做法，预留 ≥ 64 关节扩展 |
| Q2 | Vertex 格式 | 新结构 `RenderVertex3DSkin` | 类型隔离 |
| Q3 | Joints attrib | u8×4 packed via `glVertexAttribIPointer` | 节省内存 |
| Q4 | Shader 数 | 4 program | FS 复用 |
| Q5 | Lua 切换 | 自动 + 可强制 set | 调试灵活 |
| Q6 | 测试方式 | runtime smoke + GL error | 性价比 |
| Q7 | Web 默认 | 禁用（CPU 路径）| Safari WebGL2 风险规避 |

### 真机验证 GPU Skinning 收益（Phase AW.x）

桌面 GPU 机器（Windows / Linux / macOS）上运行 `samples/demo_skinning_perf/` 即可量化 CPU vs GPU 的实际帧时间差异：

#### Windows

```powershell
cd <ChocoLight 根目录>
.\samples\demo_skinning_perf\setup.ps1                                   # 下载默认资产 (~80KB)
.\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua  # 启动
```

#### Linux / macOS

```bash
cd <ChocoLight 根目录>
chmod +x samples/demo_skinning_perf/setup.sh
./samples/demo_skinning_perf/setup.sh
./Light-0.2.3/<platform>/light samples/demo_skinning_perf/main.lua
```

启动后会自动跑 60 帧 CPU + 60 帧 GPU baseline 并打印对比表（典型 5000 顶点模型在桌面 GL3.3 上能看到 **20-30x** 提升）；之后按 **G/C/A** 键运行时切换 GPU/CPU/AUTO 模式，OSD 实时显示 frame ms。

详见 `samples/demo_skinning_perf/README.md` 与 `docs/Phase AW.x/`。

> 也可以通过 `Light.Graphics.GetBackendName()`（Phase AW.x 新增）查询当前 backend 名称（`GL33Core` / `LegacyGL` 等），用于性能日志与诊断。

---

## Phase AX — Morph Target（表情/形状变形，CPU + GPU 双路径）

完整支持 glTF 2.0 morph target（blend shapes）：

- **POSITION / NORMAL / TANGENT delta**（CPU 路径用 POSITION + NORMAL；GPU shader 用 POSITION + 可选 NORMAL）
- **数量上限 8 morph target**（`Light.Animation.MORPH_TARGET_MAX = 8`）
- **动画通道驱动**：自动解析 glTF `animation.channels[].target_path == "weights"`，`Animator:Update(dt)` 内每帧评估
- **手动覆盖**：`SetMorphWeight(idx, val)` 即时写入并保留，直到 `ClearMorphWeights` 清除（NaN sentinel 区分动画值与手动值）
- **CPU + GPU 双路径**：与 Phase AW 共存（启 GPU skinning 时自动用 `VS3D_SKIN_MORPH` shader）

### 模块常量

| 名称 | 值 | 说明 |
|------|----|------|
| `Light.Animation.MORPH_TARGET_MAX` | `8` | morph target 数量上限（与 GPU shader uniform array 一致）|

### Animator 实例方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `SetMorphWeight` | `animator:SetMorphWeight(idx, value) -> bool / nil + err` | `idx`：1-based [1, 8]；`value`：number。即时生效（无需等下一帧 Update）|
| `GetMorphWeight` | `animator:GetMorphWeight(idx) -> number` | 返回当前生效权重（动画值或手动值）；越界返回 0；`idx < 1` 返回 nil + err |
| `ClearMorphWeights` | `animator:ClearMorphWeights()` | 清除所有手动覆盖，恢复动画驱动 |
| `GetMorphTargetCount` | `animator:GetMorphTargetCount() -> integer` | 当前 Animator 已分配的 weight 槽数（最近一次 Update 评估到的 N）|
| `GetMorphWeights` | `animator:GetMorphWeights() -> table` | 返回 `{w1, w2, ..., wN}` 副本数组 |
| `HasManualMorphOverride` | `animator:HasManualMorphOverride(idx) -> bool` | 该槽是否被手动覆盖过（NaN 表示未覆盖）|

### SkinnedMesh 实例方法

| 方法 | 签名 | 说明 |
|------|------|------|
| `HasMorphTargets` | `mesh:HasMorphTargets() -> bool` | 是否含 morph target |
| `GetMorphTargetCount` | `mesh:GetMorphTargetCount() -> integer` | morph target 数量（0 = 无）|
| `GetMorphTargetName` | `mesh:GetMorphTargetName(idx) -> string \| nil` | 1-based；越界返回 nil（不报错）|

### 典型使用流程

```lua
local Anim = require('Light.Animation')

-- 1. 加载含 morph 的 glTF
local pack = Anim.LoadSkinnedGLTF('character.glb')
local mesh, skel = pack.mesh, pack.skeleton

-- 2. 检视 morph 信息
if mesh:HasMorphTargets() then
    print('morph targets:', mesh:GetMorphTargetCount())
    for i = 1, mesh:GetMorphTargetCount() do
        print(string.format('  [%d] %s', i, mesh:GetMorphTargetName(i)))
    end
end

-- 3. 创建 animator + 启动动画 clip (动画通道自动驱动 weights)
local an = Anim.NewAnimator(skel)
for n, c in pairs(pack.clips) do an:AddState(n, c); break end
an:Play(pack.clipNames[1])

-- 4. 主循环
while running do
    an:Update(dt)                  -- 自动评估动画 + 应用手动覆盖

    -- 可选: 手动覆盖某个 slot (例如 UI 滑条调整表情)
    if userMovedSlider then
        an:SetMorphWeight(slotIdx, sliderValue)
    end
    if userPressedReset then
        an:ClearMorphWeights()     -- 恢复纯动画驱动
    end

    Anim.DrawSkinnedMesh(mesh, an, transform, material)
end
```

### 路径分流

| mesh 状态 | `useGPU` | `SupportsMorphTargets()` | 实际路径 |
|-----------|---------|--------------------------|---------|
| 无 morph | true | - | `DrawSkinnedMeshGPU`（Phase AW）|
| 无 morph | false | - | `DrawSkinnedMeshCPU`（Phase AV）|
| 有 morph | true | true | `DrawSkinnedMorphMeshGPU`（Phase AX 新增 GPU）|
| 有 morph | true | false | `DrawSkinnedMorphMeshCPU`（fallback CPU）|
| 有 morph | false | - | `DrawSkinnedMorphMeshCPU`（CPU 路径）|

### 实现细节（`docs/Phase AX/`）

- **CPU 算法**：`out = base + Σ(weight[i] · delta_i[v])` → skinning → modelMat。weight=0 时短路跳过该 target
- **GPU shader**：`VS3D_SKIN_MORPH` 内先对 `gl_VertexID` 做 morph，再做 4-joint skin。`uniform float uMorphWeights[8]` + `sampler2D uMorphPosDelta / uMorphNrmDelta` (RGB32F texture, width=vCount, height=morphCount)
- **数据传输**：morph delta 用 2D texture 而非 uniform array（vCount × N 通常远超 uniform 上限）
- **顺序约定**：morph 在 skin 之前应用（与 glTF 2.0 spec §6.16 一致；CPU/GPU 路径数学等价）
- **8 个 program**：原 4 (Unlit/PBR × Plain/Skin) + 新 2 (UnlitSkinMorph + PBRSkinMorph)；FS_UNLIT / FS_PBR 完全复用
- **Texture unit 分配**：0=baseColor, 1=metallicRoughness, 2=normal, 3=emissive, 4=occlusion, **5=morphPosDelta, 6=morphNrmDelta**

### 测试覆盖

见 `scripts/smoke/animation.lua` 第 [16] 段（26 PASS）：

- 模块常量 `MORPH_TARGET_MAX = 8`
- 9 个 metatable 方法存在性
- `SetMorphWeight` / `GetMorphWeight` round-trip
- 多 slot + 边界 (idx=1, 8)
- `GetMorphTargetCount` / `GetMorphWeights` 数组完整性
- `HasManualMorphOverride` set / unset 状态
- 越界错误处理（idx=0, idx=9, idx=100）
- `ClearMorphWeights` 清除手动覆盖
- `Update(dt)` 在无 morph clip 时不崩溃

### 视觉演示（Phase AX sample）

`samples/demo_morph_target/` 提供完整交互演示：

#### Windows

```powershell
.\samples\demo_morph_target\setup.ps1                                   # 下载 AnimatedMorphCube
.\Light-0.2.3\windows-x64\light.exe samples\demo_morph_target\main.lua  # 启动
```

#### Linux / macOS

```bash
chmod +x samples/demo_morph_target/setup.sh
./samples/demo_morph_target/setup.sh
./Light-0.2.3/<platform>/light samples/demo_morph_target/main.lua
```

启动后键盘控制：
- `1-8`：选择激活 slot
- `↑/↓`：调整当前 slot weight (±0.1, clamp [0,1])
- `Q/W`：快捷设 0/1
- `C`：清除所有手动覆盖
- `G/N`：切换 GPU/CPU 路径观察视觉一致性
- `Space`：暂停/恢复
- `Esc`：退出

详见 `samples/demo_morph_target/README.md`（含 Blender 导出 SKIN+MORPH 资产指引）。

### 设计决策（参见 `docs/Phase AX/CONSENSUS_PhaseAX.md`）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| Q1 | 范围 | 完整（POS+NRM+TAN, CPU+GPU）| 商业级 glTF spec 对齐 |
| Q2 | 权重来源 | 动画 + 手动覆盖 | 动画师手 K + 代码运行时调整都需要 |
| Q3 | GPU 数据 | 限 N≤8 + uniform array (weights) + 2D texture (delta) | 平衡 GLES 3.0 / WebGL 2 兼容性 |
| Q4 | 与 Skinning 共存 | 全 GPU 新 shader (`VS3D_SKIN_MORPH`) | 不损失 Phase AW 收益 |

---

## 相关

- 工作流文档：`docs/Phase AV 骨骼动画/`、`docs/Phase AW GPU Skinning/`、`docs/Phase AW.x/`、`docs/Phase AX/`
- 示例：`samples/demo_animation/`、`samples/demo_skinning_perf/`、`samples/demo_morph_target/`
- Smoke：`scripts/smoke/animation.lua`、`scripts/smoke/graphics.lua`
