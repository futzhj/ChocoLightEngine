# Phase AS.2 — 3D Mesh 基础 — 对齐文档

> **6A 工作流 Stage 1**: Align (Phase AS.2)

---

## 1. 现状调研结论

### 1.1 现有顶点格式与渲染管线

| 项 | 现状 |
|---|---|
| `RenderVertex` (`render_backend.h:20`) | 9 floats: `(x,y,z, u,v, r,g,b,a)` — **已有 z**, 但实际仅作 2D layer 排序用 |
| 顶点属性 layout (GL33) | 3 个属性: aPos(3f) + aTexCoord(2f) + aColor(4f) |
| `BatchRenderer` | 仅 2D quad/triangle 批渲染, 完全围绕 RenderVertex 设计 |
| 现有 GL 后端 shader | 单一通用 shader (uMVP + uTexture + uUseTexture), 假定 2D |
| 现有用户 Shader | Phase 3 已有 `Light.Graphics.Shader`, 共用 RenderVertex VAO |
| 投影矩阵 | 只有 `Mat4::Ortho`, **没有 `Perspective`** |
| Camera | 无显式 view matrix; modelview = identity * 用户 transform |
| 深度测试 | `glClear(... GL_DEPTH_BUFFER_BIT)` 已清, 但 **`glEnable(GL_DEPTH_TEST)` 没启用** |
| FBO 创建时 | `glRenderbufferStorage(... GL_DEPTH_COMPONENT16, ...)` — 深度 RB 已分配 |

**结论**: 引擎已为 3D 留了一些钩子 (depth RB, RenderVertex 有 z), 但没有完整 3D 管线。

### 1.2 已有模块对 RenderVertex 的依赖

| 模块 | 用法 |
|---|---|
| `light_graphics.cpp` (Draw/Quad/Line/Polygon/Circle 等) | 直接构造 RenderVertex 数组, 走 BatchRenderer |
| `light_graphics_particles.cpp` | 同上 |
| `light_graphics_tilemap.cpp` | 同上 |
| `light_graphics_canvas.cpp` | 不直接用顶点 |
| `batch_renderer.cpp` | 核心数据结构 |

如果引入新 `RenderVertex3D`, 必须**保持 RenderVertex 不变** 以避免 2D 模块 ABI 破坏。

---

## 2. Phase AS.2 范围

### 2.1 In-Scope (本子 Phase 必做)

#### A. 新 3D 顶点格式 + Mat4 扩展

```cpp
// render_backend.h
struct RenderVertex3D {
    float x, y, z;       // pos (12 bytes)
    float nx, ny, nz;    // normal (12 bytes)
    float u, v;          // uv (8 bytes)
    float r, g, b, a;    // color (16 bytes)
};  // 共 48 bytes (16-byte aligned)

// Mat4 加 2 个工厂
static Mat4 Perspective(float fovYDeg, float aspect, float near, float far);
static Mat4 LookAt(float ex, float ey, float ez,
                   float tx, float ty, float tz,
                   float ux, float uy, float uz);
```

不在 RenderVertex3D 加 tangent (留 AS.4 PBR 时再加, 那时同时引入 RenderVertex3DTangent 或别的)。

#### B. RenderBackend 3D 接口扩展

```cpp
// 深度测试控制 (默认关闭以兼容现有 2D 渲染)
virtual void SetDepthTest(bool enable) {}
virtual void SetDepthFunc(int func) {}    // 0=Less, 1=LEqual, 2=Greater 等

// 视图矩阵 (separate from modelview)
virtual void LoadView(const float* viewMat4) {}
virtual void LoadProjection(const float* projMat4) {}  // 替代 LoadOrtho 的通用版

// Mesh 接口 (3D 顶点 + 索引)
virtual uint32_t CreateMesh(const RenderVertex3D* verts, int vertexCount,
                            const uint32_t* indices, int indexCount) { return 0; }
virtual void DeleteMesh(uint32_t meshId) {}
virtual void DrawMesh(uint32_t meshId, uint32_t textureId) {}
```

GL33 真实现 (额外 VAO + VBO + EBO 为 mesh), Legacy 默认 no-op。

#### C. `Light.Graphics.Mesh` Lua 模块

```lua
-- 静态方法
Light.Graphics.Mesh.New(vertices, indices) -> Mesh   -- vertices: flat float table; indices: int table; 失败返回 nil
Light.Graphics.Mesh.GetVertexFormat()      -> string -- "pos3, normal3, uv2, color4" (固定 12 floats/vertex)

-- Mesh 实例方法 (元表)
mesh:Draw([textureId])                              -- 默认 0 (无纹理), 用户传 texId 启用纹理
mesh:GetVertexCount() -> int
mesh:GetIndexCount()  -> int
mesh:Delete()
mesh:__gc()                                         -- 自动释放 GPU 资源
```

`vertices` 表布局: 12 floats 每顶点 (pos.x,y,z, normal.x,y,z, uv.x,y, color.r,g,b,a)。`indices` 表是 1-indexed 整数。

#### D. `Light.Graphics` 3D 投影/视图 API

```lua
Light.Graphics.SetPerspective(fovYDeg, aspect, near, far)  -- 替换 Ortho 投影
Light.Graphics.SetCamera(ex, ey, ez, tx, ty, tz, [ux], [uy], [uz])  -- LookAt
Light.Graphics.SetDepthTest(enable)         -- bool
Light.Graphics.GetDepthTest() -> bool

-- Ortho 仍可用 (现有 SetupOrthoProjection 内部继续调 LoadOrtho)
```

#### E. Smoke + 文档 + CI

`scripts/smoke/mesh_3d.lua` 5 stage:
1. Light.Graphics.Mesh 模块加载, fns 注册
2. SetPerspective / SetCamera / SetDepthTest 是 fn
3. Mesh.New 在 headless 安全失败 (返回 nil 不崩)
4. Mesh.New 提供 vertex/index table 路径不崩 (即使无 GL 上下文)
5. 兼容性: 现有 2D 渲染 (Light.Graphics.Draw) 仍可用

### 2.2 Out-of-Scope (留 AS.3/4)

- **glTF/glb 加载** (留 AS.3)
- **PBR / Unlit 双 shader 系统** (留 AS.4)
- **Light, 阴影, 环境贴图** (留 AS.4 或更后)
- **Tangent 顶点属性** (留 AS.4 法线贴图时加)
- **Skinned mesh / 骨骼动画** (留更后续 Phase)
- **Instancing / GPU 实例化** (留更后续)

---

## 3. 关键决策预案 (Q1~Q5)

### Q1 — 复用 RenderVertex 还是新 RenderVertex3D?

**方案 A** (推荐): 新 `RenderVertex3D` (48 bytes, 含 normal); 现有 RenderVertex 不动  
**方案 B**: 扩展 RenderVertex 加 normal 字段 — 破坏 ABI, 影响所有 2D 模块  
**方案 C**: 单一 RenderVertex 但 normal 复用 color 字段 — 太 hacky

**A**: 平行路径, 不影响 2D, 风险最低。

### Q2 — Mesh 顶点表是否支持多种 layout?

**方案 A** (推荐): 固定 12 floats/vertex (pos3+normal3+uv2+color4) — 简单稳定  
**方案 B**: 用户传 layout descriptor + flexible struct  
**方案 C**: 多个工厂 (NewPosOnly / NewPosUV / NewFull)

**A**: 简单清晰, 性能可预期。AS.4 PBR 时按需扩展。

### Q3 — 深度测试默认值?

**方案 A** (推荐): 默认 **关闭** (兼容现有 2D 引擎); Mesh:Draw 内部临时开启然后恢复  
**方案 B**: 默认开启 + 2D 自动用 depth ALWAYS  
**方案 C**: 用户决定 (Mesh:Draw 不动 depth state)

**A**: 与现有 2D 行为完全一致, 无回归风险; Mesh:Draw 内部 push/pop depth state。

### Q4 — Mesh:Draw 是否使用当前 g_render shader?

**方案 A** (推荐): 引擎内置一个 3D 默认 shader (基础 lighting + diffuse texture); 如果用户激活了自定义 shader (Shader:Use), Mesh:Draw 用用户的  
**方案 B**: Mesh:Draw 必须配合用户 shader, 引擎不提供默认 3D shader  
**方案 C**: 复用现有 2D shader (无 normal 用法)

**A**: 用户友好, 提供基础 3D 默认; Phase AS.4 引入更完善的 PBR/Unlit。

### Q5 — Mesh ID 是 uint32 还是 lightuserdata 包装?

**方案 A** (推荐): C++ 层用 uint32 (与现有 textureId/shaderProgram 一致); Lua 暴露 userdata 含 mesh_id, 通过 __gc 自动释放  
**方案 B**: lightuserdata 直接当 ID

**A**: 与现有模式一致, lifecycle 安全。

---

## 4. 工作量估算

| 子模块 | 新 fns | C++ 行 | Lua hook | smoke 行 |
|---|---|---|---|---|
| Mat4 Perspective + LookAt | (内部) | ~80 | - | - |
| RenderBackend 3D 接口 + GL33 实现 | - | ~280 (Mesh VAO/VBO/EBO) | - | - |
| 3D 默认 shader (VS+FS) | - | ~80 | - | - |
| Light.Graphics.Mesh 模块 | 5 fns | ~200 | - | ~80 |
| Light.Graphics 3D 控制 (SetPerspective/SetCamera/SetDepthTest) | 4 fns | ~120 | - | ~30 |
| 文档 + ACCEPTANCE | - | - | - | - |
| **合计** | **9 fns + 5 method** | **~760** | - | **~110** |

预期 1 commit, ~700 行 C++, ~5h 工作量, 6 平台 CI 全绿后交付。

---

## 5. 关键约束

- **不破坏 RenderVertex** — 现有 2D 模块零修改
- **不破坏 BatchRenderer** — Mesh 走独立路径 (不进 batch)
- **不破坏现有 Canvas/Shader 行为** — 深度测试默认关
- **headless 友好** — Mesh.New 在无 GL 时返回 nil 不崩
- **跨平台** — GL33 真实现, Legacy 默认 no-op (不支持 mesh)

---

## 6. 关键决策待确认

5 个 Q 都给出推荐 (A)。请用户确认:

1. **全部采用推荐 (Q1~Q5 全 A)** — 推荐, 工作量 ~700 行
2. **去掉 3D 默认 shader (Q4 选 B)** — 减 80 行, 用户必须配合 Shader:Use
3. **去掉深度测试控制 (Q3 简化)** — 不暴露 SetDepthTest, Mesh:Draw 内部固定行为
4. **分别详细选择**
