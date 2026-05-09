# Phase AS — 图形进阶 — 对齐文档

> **6A 工作流 Stage 1**: Align (Phase AS)

---

## 1. 现状调研结论 (重要发现)

经审视 `light_graphics_canvas.cpp`、`light_graphics_shader.cpp`、`render_backend.h`、`render_gl33.cpp`、`render_legacy.cpp`,发现:

### 1.1 RenderTarget/FBO — **大部分已存在!**

| 能力 | 状态 |
|---|---|
| `Light.Graphics.Canvas` Lua 模块 | ✅ 已有 (Phase 早期) |
| `Light.Graphics.SetCanvas(canvas\|nil)` | ✅ 已有 (`light_graphics.cpp:212`) |
| `Light.Graphics.GetCanvas()` | ✅ 已有 |
| `RenderBackend::CreateFBO/DeleteFBO/BindFBO/UnbindFBO` | ✅ 已有 (GL33 + Legacy 双后端实现) |
| `Light.dll` IDA 还原的 `luaopen_Light_Graphics_Canvas` | ✅ 完整 |

但**缺少**:
- `canvas:GetTexture()` — Canvas 不能作为纹理被 shader 采样 (**严重能力缺口**)
- `canvas:GetWidth()` / `canvas:GetHeight()` — 查询尺寸
- `canvas:Clear([r], [g], [b], [a])` — 清空 canvas (用户必须 SetCanvas + Graphics.Clear 两步)
- 多目标栈支持: `PushCanvas` / `PopCanvas` (替代单一 SetCanvas, 支持嵌套渲染)

### 1.2 Shader uniform — 部分已存在,缺关键能力

| 能力 | 状态 |
|---|---|
| `Light.Graphics.Shader.New(vs, fs)` | ✅ 已有 |
| `shader:Use()` / `Shader.UseDefault()` / `Shader.IsSupported()` | ✅ 已有 |
| `shader:SetFloat / SetVec2 / SetVec3 / SetVec4 / SetInt / SetMat4` | ✅ 已有 |
| `shader:Delete()` / `__gc` | ✅ 已有 |

**缺少 (Phase AS 核心)**:
- `shader:SetTexture(name, image, slot)` — **绑定纹理 uniform**, 这是 shader 玩自定义采样的基础
- `shader:SetMat3(name, table)` — 3x3 矩阵 (法线变换)
- `shader:SetIVec2 / SetIVec3 / SetIVec4` — 整数向量 (像素坐标采样、模式标志)
- `shader:SetFloatArray(name, table)` — float 数组 (用于 SSAO 采样核、模糊核)
- `shader:SetVec2Array(name, table)` — vec2 数组
- `shader:SetMat4Array(name, table)` — mat4 数组 (instance 渲染、骨骼)

### 1.3 glTF 加载 — **完全没有,且工程量巨大**

引擎是**纯 2D**:
- 没有 mesh 几何概念 (只有 quad/sprite)
- 没有 vertex layout 抽象 (RenderVertex 只有 pos+uv+color, 没法线没切线)
- 深度缓冲存在但实际未启用 3D 渲染
- 没有 lighting model / 材质系统
- 没有任何 .gltf / .glb 解析代码 (没引入 cgltf/tinygltf)

引入 glTF 需要先建:
1. 3D mesh userdata + vertex format 抽象 (~400 行)
2. cgltf 库集成 + .gltf/.glb 解析 (~600 行)
3. 3D 渲染管线 (depth test 真启用, 3D camera, mesh.Draw) (~500 行)
4. 材质系统 (PBR / unlit, 多 texture slot) (~400 行)
5. Lua API: `Light.Graphics.Mesh` 模块 (~300 行)

**总计 ~2200 行 C++ + 大幅修改 render_backend / render_gl33,远超单个 Phase 工作量。**

---

## 2. Phase AS 范围调整建议

基于上述调研,**强烈建议范围调整**:

### 2.1 In-Scope (本 Phase)

#### A. Canvas/RenderTarget 增强 (4 个新方法 + 内部支持)

```lua
canvas:GetTexture() -> Image          -- 把 canvas 包装成 Image 让 Shader 采样
canvas:GetWidth() -> int
canvas:GetHeight() -> int
canvas:Clear([r], [g], [b], [a])      -- 默认 (0,0,0,0)
```

(可选)
```lua
Light.Graphics.PushCanvas(canvas) -> ()    -- 入栈, 切换到 canvas
Light.Graphics.PopCanvas()        -> ()    -- 出栈, 恢复上一目标
```

#### B. Shader uniform 扩展 (6 个新方法)

```lua
shader:SetTexture(name, image, [slot])    -- 关键: 让 shader 采样自定义纹理
shader:SetMat3(name, {m00, ..., m22})     -- 3x3 矩阵 (9 floats)
shader:SetIVec2(name, x, y)
shader:SetIVec3(name, x, y, z)
shader:SetIVec4(name, x, y, z, w)
shader:SetFloatArray(name, {v1, v2, ...})  -- 1D float 数组
```

#### C. RenderBackend 接口扩展 (内部)

```cpp
// 新增 virtual 函数
virtual uint32_t GetFBOTexture(uint32_t fbo) const { return 0; }   // 已有 outTex 参数, 但缺 query API
virtual void ClearCurrent(float r, float g, float b, float a) { }  // 清空当前 FBO/默认目标
virtual void SetUniform1iv(int loc, int count, const int* values) { }
virtual void SetUniformMat3(int loc, const float* m) { }
virtual void SetUniform1fv(int loc, int count, const float* values) { }
virtual void SetUniformSampler(int loc, int slot, uint32_t texId) { }  // 绑定纹理到 slot, 设 sampler uniform
```

### 2.2 Out-of-Scope (本 Phase 不做, 留独立 Phase)

**glTF + 3D mesh** — 单独"Phase AS+ 3D 渲染" (估计 1 周, 与 Phase AS 并行的话需独立分支):
- cgltf 集成 + glTF/glb 解析
- Light.Graphics.Mesh 模块
- 3D 渲染管线 (depth test, 3D camera)
- 材质系统 / PBR shader

理由:
- 本 Phase 聚焦 2D 引擎的 shader/canvas 能力提升
- 3D 是新方向, 工作量比单个 Phase 大 3x
- 加 3D 影响渲染管线全局, 风险较高

### 2.3 工程量重估

| 子模块 | 新 fns | C++ 行 | smoke 行 |
|---|---|---|---|
| Canvas:GetTexture / GetWidth / GetHeight / Clear | 4 | ~150 | ~40 |
| Canvas Push/PopCanvas (栈机制) | 2 | ~80 | ~30 |
| Shader: SetTexture (含 sampler 绑定 + slot 管理) | 1 | ~120 | ~30 |
| Shader: SetMat3 | 1 | ~30 | ~10 |
| Shader: SetIVec2/3/4 | 3 | ~70 | ~20 |
| Shader: SetFloatArray + SetVec2Array | 2 | ~70 | ~20 |
| RenderBackend GL33 实现 | (内部) | ~200 | - |
| RenderBackend Legacy 桩 / 不支持 shader array | (内部) | ~50 | - |
| **合计** | **13 fns** | **~770** | **~150** |

总工作量与 Phase AR 相当 (~770 行 C++, 13 fns), 风险可控。

---

## 3. 关键决策预案 (Q1~Q5)

### Q1 — Canvas:GetTexture() 返回什么类型?

**方案 A** (推荐): 返回**新建的 Image userdata** — 用 canvas 的 GL texture ID 包成 Image, 与 `Light.Graphics.Image` 通用 API 兼容 (可以 Image:Draw / 传入 SetTexture)
**方案 B**: 返回原生 GL texture ID (number) — 需要用户额外调用底层 API 包装
**方案 C**: 返回 Canvas 自身 (有 __index 让 Image API 可用) — 复杂的元表继承

A 简洁兼容现有 Image, 推荐。

### Q2 — Push/PopCanvas 栈深度上限?

**方案 A** (推荐): 软限制 8 (足够多嵌套场景), 超过报警告但不中断  
**方案 B**: 不限 (动态 vector)  
**方案 C**: 不实现 PushCanvas (用户自己用 GetCanvas + SetCanvas 包装栈)

A: 限制 8 防止误用导致 GPU 资源泄漏, 实用。

### Q3 — SetTexture(name, image, slot) 中 slot 是否必传?

**方案 A** (推荐): slot 可选, 默认 slot=1 (slot=0 留给引擎默认纹理)  
**方案 B**: slot 必传, 用户必须管理  
**方案 C**: 不暴露 slot (引擎自动分配)

A: 简化常见用例(单纹理), 高级用法(多纹理)还可以指定。

### Q4 — Mat3 如何传?

**方案 A** (推荐): 9 个 float 的 table — `{m00, m01, m02, m10, m11, m12, m20, m21, m22}`  
**方案 B**: 9 个独立参数 — `SetMat3(name, m00, ..., m22)`  
**方案 C**: 接受任何能 `__index[1..9]` 的 table-like

A: 与现有 SetMat4 一致, 用户友好。

### Q5 — IVec / FloatArray 在 Legacy GL 后端的行为?

**方案 A** (推荐): Legacy 不支持 shader, 直接 silent no-op (返回 false)  
**方案 B**: Legacy 抛 Lua 错误  
**方案 C**: 在 Legacy 上模拟为 SetFloat/SetInt 调用

A: 与现有 IsSupported() 保持一致, 调用方应先查询。

---

## 4. 现有项目模式对齐

### 4.1 复用模式

- **Image userdata 包装** — 沿用 `light_graphics_image.cpp` 的模式 (luaL_newuserdata + 元表), Canvas:GetTexture 返回这种结构
- **Shader 元表方法** — 沿用 `light_graphics_shader.cpp` 现有模式, 新增 setter 直接加在 methods[] 即可
- **RenderBackend 双后端** — GL33 真实现, Legacy 用 `SupportsShaders() = false` 路径自然 silent fail
- **smoke 风格** — 沿用 `pen_event_timer.lua`/`textinput.lua`

### 4.2 关键约束

- **不破坏现有 Canvas 元表** — 新方法追加, 不动 __call/__tostring/__gc
- **不破坏 RenderVertex layout** — 本 Phase 不涉及 3D mesh, 保持 2D 顶点格式
- **headless 友好** — 无 GL context 时所有 fn 安全失败, 与现有 Canvas 行为一致

---

## 5. 验收标准

- [ ] `Light.Graphics.Canvas` 元表新增 4 个方法 (GetTexture/GetWidth/GetHeight/Clear), 都是 function
- [ ] `Light.Graphics.PushCanvas / PopCanvas` 是 function
- [ ] `Light.Graphics.Shader` 元表新增 6 个 setter (SetTexture/SetMat3/SetIVec2/3/4/SetFloatArray)
- [ ] `Canvas:GetTexture()` 返回 Image userdata (兼容现有 Image API)
- [ ] Shader:SetTexture 支持单纹理 (slot=1) 和多纹理 (slot=N) 两种用法
- [ ] `scripts/smoke/canvas_shader_ext.lua` 通过 lightc -p
- [ ] **Windows runtime smoke 通过** — 关键验收, 因 Canvas/Shader 测试需要 GL 上下文
- [ ] 6 平台 CI 全绿
- [ ] 不引入 cgltf / 任何 3D mesh 依赖

---

## 6. 用户确认结果 (锁定)

用户选择: **包含 glTF, 拆为 4 个子 Phase 连续推进** (~1 周工作量)

| 子 Phase | 范围 | 工作量估计 | 文档前缀 |
|---|---|---|---|
| **AS.1** (当前) | Canvas 增强 + Shader uniform 扩展 (含 SetTexture) | ~770 行, 12 fns | `*_PhaseAS_1.md` |
| **AS.2** | 3D Mesh 基础 — `Light.Graphics.Mesh`, vertex layout 抽象, 深度测试启用 | ~700 行, 8-10 fns | `*_PhaseAS_2.md` |
| **AS.3** | cgltf 集成 + .gltf/.glb 解析, `Light.Graphics.Mesh.LoadGLTF()` | ~600-800 行 | `*_PhaseAS_3.md` |
| **AS.4** | 材质系统 (PBR/Unlit 双 shader, 多 texture slot, lighting) | ~400-500 行 | `*_PhaseAS_4.md` |

**执行顺序**: 严格依次 AS.1 → AS.2 → AS.3 → AS.4, 每个 6 平台 CI 全绿后才进入下一个。

Q1~Q5 全部 **A 方案** 采纳 (适用于 AS.1)。
