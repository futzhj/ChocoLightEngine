# Phase AS.3 — cgltf 集成 + .gltf/.glb 加载 — 共识文档

> **状态**: ✅ 决策已锁定 (Q1~Q4 全 A + 我自动下载 cgltf.h), 进入实施

---

## 1. 锁定范围

### 1.1 第三方库

- **cgltf v1.14** (single-header, MIT)
- 文件: `ChocoLight/third_party/cgltf.h` (~280KB) + `cgltf_impl.c` (~3 行)
- 来源: https://github.com/jkuhlmann/cgltf v1.14 tag

### 1.2 新 Lua API (`Light.Graphics.Mesh`)

```lua
Light.Graphics.Mesh.LoadGLTF(path, [primitive_index=0]) -> Mesh|nil [, err]
Light.Graphics.Mesh.GetGLTFMeshCount(path) -> int|nil [, err]
```

### 1.3 顶点属性提取规则

| glTF 属性 | RenderVertex3D 字段 | 缺失时 |
|---|---|---|
| POSITION (vec3) | x, y, z | **必须**, 缺失 → 失败 |
| NORMAL (vec3) | nx, ny, nz | 填 (0, 1, 0) |
| TEXCOORD_0 (vec2) | u, v | 填 (0, 0) |
| COLOR_0 (vec4 或 vec3) | r, g, b, a | 填 (1, 1, 1, 1); vec3 时 a=1 |

### 1.4 索引转换

- cgltf 索引可能是 u8/u16/u32 → 统一转 uint32 给 backend
- 无索引 (drawArray 模式) → 自动生成 (0, 1, 2, ...) 序列

---

## 2. 关键决策 (Q1~Q4 全 A)

| Q | 决策 |
|---|---|
| Q1 | 我自动下载 cgltf.h v1.14 |
| Q2 | `LoadGLTF(path, [primitive_index=0])` 灵活选择 |
| Q3 | 缺失属性自动填默认值 (不失败) |
| Q4 | 暴露 `GetGLTFMeshCount(path)` |

---

## 3. 实施步骤 (单 commit)

1. 下载 cgltf.h → `third_party/cgltf.h`
2. 创建 `cgltf_impl.c`: `#define CGLTF_IMPLEMENTATION + #include`
3. CMakeLists.txt 加 `cgltf_impl.c` 到源列表
4. `light_graphics_mesh.cpp` 加 `LoadGLTF` + `GetGLTFMeshCount` (~380 行)
5. smoke `mesh_3d.lua` 加 LoadGLTF 错误路径测试
6. ACCEPTANCE + commit + push + 6 平台 CI 全绿

---

## 4. 错误消息表

| 场景 | 错误消息 |
|---|---|
| path 不是 string | luaL_checkstring 错 |
| 文件不存在 | "cannot open: <path>" |
| 解析失败 | "parse failed (cgltf err <N>)" |
| buffer 加载失败 | "buffer load failed (cgltf err <N>)" |
| primitive_index 越界 | "primitive index <N> out of range (have <M>)" |
| 缺失 POSITION | "primitive has no POSITION attribute" |
| 0 顶点 | "primitive has 0 vertices" |
| 无 GL 上下文 | "no render backend (window not opened?)" |
| backend 不支持 3D | "render backend does not support 3D mesh" |
| GPU 上传失败 | "CreateMesh failed" |

---

## 5. 验收标准

- [ ] cgltf.h v1.14 下载到 third_party/ 成功
- [ ] cgltf_impl.c 编译通过 (6 平台)
- [ ] `Light.Graphics.Mesh.LoadGLTF/GetGLTFMeshCount` 是 function
- [ ] LoadGLTF("nonexistent.gltf") 返回 nil + err 不崩
- [ ] LoadGLTF(no path) 返回 nil + err
- [ ] GetGLTFMeshCount 同上 boundary 路径
- [ ] 现有 Mesh.New / 2D 渲染 / Canvas 仍可用 (回归)
- [ ] `lightc -p mesh_3d.lua` Exit=0
- [ ] 6 平台 CI 全绿
