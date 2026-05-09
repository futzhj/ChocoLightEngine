# Phase AS.2 — 3D Mesh 基础 — 验收文档

> **状态**: ✅ **已完成** — 6 平台 CI 全绿 (一次成功, 无修复)
>
> GitHub Actions run: [25596380171](https://github.com/futzhj/ChocoLightEngine/actions/runs/25596380171) (commit `44b3fe9`)

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| 新增 Lua 模块 | 1 (`Light.Graphics.Mesh`) |
| 新增 Lua 函数 (Mesh) | 6 (New, GetVertexFormat 静态 + Draw, GetVertexCount, GetIndexCount, Delete 实例) |
| 新增 Lua 函数 (Light.Graphics) | 4 (SetPerspective, SetCamera, SetDepthTest, GetDepthTest) |
| 新增 RenderBackend 虚函数 | 8 (Supports3D, CreateMesh, DeleteMesh, DrawMesh, SetDepthTest, SetDepthFunc, LoadView, LoadProjection) |
| 新增 Mat4 工厂方法 | 2 (Perspective, LookAt) |
| 新增 顶点格式 | 1 (`RenderVertex3D` 12 floats: pos3+normal3+uv2+color4) |
| 新增内置 shader | 1 (3D Lambert + diffuse, GLES3 + GL33 双版本) |
| 修改文件 | 6 (render_backend.h/.cpp, render_gl33.cpp, light_graphics.cpp, light.h, lumen/light.cpp, CMakeLists.txt) |
| 新增文件 | 4 (light_graphics_mesh.cpp + 3 docs + smoke) |
| **C++ 总代码** | **~880 行** |

---

## 二、API 详细列表

### 2.1 `Light.Graphics.Mesh` 新模块

```lua
-- 静态
Light.Graphics.Mesh.New(vertices, indices) -> Mesh|nil [, err]
  -- vertices: flat float table, 12 floats/vertex
  --   (pos.x,y,z, normal.x,y,z, uv.x,y, color.r,g,b,a)
  -- indices:  int table (1-indexed; count must be > 0 and multiple of 3)
  -- 软上限: 1M 顶点, 3M 索引
Light.Graphics.Mesh.GetVertexFormat() -> "pos3, normal3, uv2, color4"

-- 实例方法 (Mesh userdata 元表)
mesh:Draw([textureId])              -- textureId 默认 0 (无纹理)
mesh:GetVertexCount() -> int
mesh:GetIndexCount()  -> int
mesh:Delete()                       -- 也通过 __gc 自动调用
mesh:__tostring                     -- "Light.Graphics.Mesh(id=N, verts=M, idx=K)"
```

### 2.2 `Light.Graphics` 3D 控制 (4 fn)

```lua
Light.Graphics.SetPerspective(fovYDeg, aspect, near, far)
Light.Graphics.SetCamera(ex, ey, ez, tx, ty, tz, [ux=0], [uy=1], [uz=0])
Light.Graphics.SetDepthTest(enable)         -- bool
Light.Graphics.GetDepthTest() -> bool
```

### 2.3 RenderBackend C++ 接口扩展 (8 个新虚函数)

```cpp
virtual bool Supports3D() const { return false; }
virtual uint32_t CreateMesh(const RenderVertex3D* verts, int vCount,
                            const uint32_t* indices, int iCount) { return 0; }
virtual void DeleteMesh(uint32_t meshId);
virtual void DrawMesh(uint32_t meshId, uint32_t textureId);
virtual void SetDepthTest(bool enable);
virtual void SetDepthFunc(int func);                 // 0..7
virtual void LoadView(const float* viewMat4);
virtual void LoadProjection(const float* projMat4);  // 替代 LoadOrtho 的通用版
```

GL33 真实现, Legacy 默认 no-op (`Supports3D()` 返回 false)。

### 2.4 Mat4 工厂方法 (2 个)

```cpp
static Mat4 Perspective(float fovYDeg, float aspect, float n, float f);
static Mat4 LookAt(float ex, float ey, float ez,
                   float tx, float ty, float tz,
                   float ux, float uy, float uz);
```

---

## 三、关键设计决策 (与 CONSENSUS_PhaseAS_2 对齐, Q1~Q5 全 A)

| Q | 决策 | 实施 |
|---|---|---|
| Q1 顶点格式 | A — 新 RenderVertex3D 平行 | 不动 RenderVertex 2D |
| Q2 顶点 layout | A — 固定 12 floats/vertex | pos3+normal3+uv2+color4 |
| Q3 深度测试默认 | A — 关闭 | DrawMesh 内部临时开然后恢复 |
| Q4 内置 3D shader | A — 引擎提供默认 | Lambert + diffuse, 用户 Shader:Use 覆盖 |
| Q5 Mesh ID 形式 | A — uint32 + Lua userdata | __gc 自动 DeleteMesh |

---

## 四、跨着色器交互逻辑

### Mesh:Draw 流程

```
if 用户激活了 Shader (userShaderActive == true):
    用用户 shader 绘制, 引擎不切换 program
    if textureId != 0: 仅绑纹理到 slot 0, 用户负责 sampler uniform
else:
    切换到引擎 3D 默认 shader (Lambert + diffuse)
    上传 uMVP, uModel, uLightDir, uLightColor, uAmbient
    if textureId != 0: 绑纹理到 slot 0 + uTexture=0 + uUseTexture=1
    else: uUseTexture=0

绘制前: 临时开启深度测试 (LEqual) 如果未开
绘制: glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT)
绘制后: 恢复深度测试状态; 切回默认 2D shader 和 VAO
```

### 投影 + 视图矩阵流程

```
SetPerspective(fov, aspect, near, far):
    Mat4 proj = Mat4::Perspective(...)
    g_render->LoadProjection(proj.m)        -- backend 内部 projection = proj

SetCamera(eye, target, up):
    Mat4 view = Mat4::LookAt(...)
    g_render->LoadView(view.m)              -- backend 内部 viewMatrix = view, hasView = true

DrawMesh: ComputeMVP3D() = projection * (hasView ? viewMatrix * modelview : modelview)
```

---

## 五、Smoke 测试覆盖 (`scripts/smoke/mesh_3d.lua`, 5 stage)

1. **Light.Graphics 3D 控制**: 4 fns 注册; SetPerspective/SetCamera 调用不崩; SetDepthTest/GetDepthTest round-trip
2. **Light.Graphics.Mesh 模块**: 加载, 2 静态 fn 注册; GetVertexFormat 字符串校验
3. **Mesh.New 边界路径**: 空 table / 错误 vertex 长度 / 错误 index count 都返回 nil + err
4. **合法 mesh**: 3 顶点三角形; 在 GL context active 时验证 GetVertexCount/GetIndexCount/Draw/Delete; headless 时返回 nil + err 不崩
5. **回归兼容**: AS.1 Canvas/Shader + AR Light.Event 仍可用

---

## 六、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `RenderVertex` | **零修改** (新 RenderVertex3D 平行) |
| `BatchRenderer` | **零修改** (Mesh 走独立路径) |
| `Light.Graphics.Canvas/Image/Shader` (AS.1) | **零修改** |
| `Light.Graphics` 函数表 | 新增 4 fn (SetPerspective/SetCamera/SetDepthTest/GetDepthTest) |
| `Mat4` | 加 2 个 static factory, 不破坏现有 |
| `RenderBackend` 接口 | 新增 8 虚函数, 默认 no-op |
| `RenderGL33Backend` | 加 3D shader + Mesh 资源池 + 8 个 override |
| `Render Legacy` | **零修改** (默认 no-op 自动适用) |
| 其他 Light.* 模块 | **零修改** |

---

## 七、未实施项 (留后续 Phase)

- **AS.3** cgltf 集成 + .gltf/.glb 解析: `Light.Graphics.Mesh.LoadGLTF(path)`
- **AS.4** 材质系统 (PBR/Unlit + 多纹理 slot + 完整 lighting)
- **Tangent 顶点属性** (法线贴图需要, 留 AS.4)
- **Skinned mesh / 骨骼动画** (留更后续)
- **Instancing** (留更后续)

---

## 八、CI 验收标准

- [x] `lightc -p scripts/smoke/mesh_3d.lua` Exit=0 (本地)
- [x] GitHub Actions `Build Templates (All Platforms)` **全绿** (run 25596380171):
  - [x] Windows x64: 编译 + Windows runtime smoke (含 mesh_3d.lua) ✅
  - [x] Linux x64: 编译 + 语法检查 ✅
  - [x] macOS Universal: 编译 + 语法检查 ✅
  - [x] Android arm64+x86_64: 编译 ✅
  - [x] iOS arm64: 编译 ✅
  - [x] Web WASM: 编译 ✅

**Phase AS.2 最终交付完成 (一次提交即全平台通过)。**
