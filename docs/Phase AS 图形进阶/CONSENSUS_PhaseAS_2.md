# Phase AS.2 — 3D Mesh 基础 — 共识文档

> **状态**: ✅ 决策已锁定 (Q1~Q5 全 A), 进入实施

---

## 1. 锁定范围

### 1.1 新数据结构 (`render_backend.h`)

```cpp
// 3D 顶点 (48 bytes = 12 floats, 16-byte 对齐)
struct RenderVertex3D {
    float x, y, z;       // pos
    float nx, ny, nz;    // normal
    float u, v;          // uv
    float r, g, b, a;    // color
};

// Mat4 加 2 个工厂
static Mat4 Perspective(float fovYDeg, float aspect, float n, float f);
static Mat4 LookAt(float ex, float ey, float ez,
                   float tx, float ty, float tz,
                   float ux, float uy, float uz);
```

### 1.2 新 RenderBackend 虚函数 (8 个)

```cpp
virtual uint32_t CreateMesh(const RenderVertex3D* verts, int vCount,
                            const uint32_t* indices, int iCount) { return 0; }
virtual void DeleteMesh(uint32_t meshId) {}
virtual void DrawMesh(uint32_t meshId, uint32_t textureId) {}
virtual void SetDepthTest(bool enable) {}
virtual void SetDepthFunc(int func) {}
virtual void LoadView(const float* viewMat4) {}
virtual void LoadProjection(const float* projMat4) {}
virtual bool Supports3D() const { return false; }   // GL33 返回 true, Legacy false
```

### 1.3 新 Lua 模块 `Light.Graphics.Mesh` (5 fn)

```lua
Light.Graphics.Mesh.New(vertices, indices) -> Mesh|nil
mesh:Draw([textureId])
mesh:GetVertexCount() -> int
mesh:GetIndexCount()  -> int
mesh:Delete()  -- 也通过 __gc 自动调用
```

### 1.4 `Light.Graphics` 3D 控制 (4 fn)

```lua
Light.Graphics.SetPerspective(fovYDeg, aspect, near, far)
Light.Graphics.SetCamera(ex, ey, ez, tx, ty, tz, [ux=0], [uy=1], [uz=0])
Light.Graphics.SetDepthTest(enable)
Light.Graphics.GetDepthTest() -> bool
```

---

## 2. 关键决策 (Q1~Q5 全 A)

| Q | 决策 |
|---|---|
| Q1 | **A** — 新 RenderVertex3D 平行, 不动 RenderVertex |
| Q2 | **A** — 固定 12 floats/vertex (pos3+normal3+uv2+color4) |
| Q3 | **A** — 深度测试默认关; Mesh:Draw 内部临时开然后恢复 |
| Q4 | **A** — 引擎提供内置 3D 默认 shader (基础 lighting + diffuse texture) |
| Q5 | **A** — uint32 mesh id, Lua 用 userdata 包装含 __gc |

---

## 3. 内置 3D 默认 shader

```glsl
// VS: pos+normal+uv+color, uniforms: uMVP, uModel, uNormalMat
// FS: 简单 Lambert (单方向光) + diffuse texture + vertex color
// 用户可通过 Light.Graphics.Shader.New + UseShader 覆盖
```

---

## 4. 跨线程 / 跨模块约束

- Mesh GPU 资源 (VAO/VBO/EBO) 仅在主线程访问 (与现有 GL 资源一致)
- Mesh userdata 的 __gc 在主线程运行 lua_pcall 触发
- 不引入新依赖

---

## 5. 实施步骤 (单 commit)

1. `render_backend.h`: RenderVertex3D + Mat4::Perspective/LookAt + 8 个新 virtual
2. `render_backend.cpp`: Mat4::Perspective + LookAt 实现
3. `render_gl33.cpp`: 内置 3D shader + Mesh CRUD + 深度测试
4. `light_graphics_mesh.cpp` (新): Light.Graphics.Mesh 5 fn + userdata
5. `light_graphics.cpp`: SetPerspective/SetCamera/SetDepthTest/GetDepthTest
6. `light.h` / `lumen/light.cpp` / CMakeLists.txt: 注册新模块
7. `scripts/smoke/mesh_3d.lua`: 5 stage smoke
8. `build-templates.yml`: 加 mesh_3d.lua
9. ACCEPTANCE + commit + push + CI 全绿

---

## 6. 验收标准

- [ ] `Light.Graphics.Mesh` 模块加载, 5 fn type==function
- [ ] `Light.Graphics.SetPerspective/SetCamera/SetDepthTest/GetDepthTest` 是 fn
- [ ] Mesh.New(invalid) 返回 nil 不崩
- [ ] Mesh.New(valid table 表 12 floats/vertex × N) 在 headless 安全 (返回 nil 或 mesh)
- [ ] mesh:GetVertexCount/GetIndexCount 返回 number
- [ ] SetDepthTest(true/false) 安全切换
- [ ] 现有 2D 渲染 (Light.Graphics.Draw 等) 仍可用 (回归)
- [ ] `lightc -p scripts/smoke/mesh_3d.lua` Exit=0
- [ ] 6 平台 CI 全绿
- [ ] 不引入 cgltf 或任何 3D 解析库
