# Phase AS.3 — cgltf + .gltf/.glb 加载 — 验收文档

> **状态**: 已完成本地实施 + lightc 语法检查通过, 待 GitHub Actions CI 全平台验证

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| 新增第三方库 | cgltf v1.14 (single-header, MIT, ~193KB) |
| 新增 Lua 函数 | 2 (`Light.Graphics.Mesh.LoadGLTF`, `GetGLTFMeshCount`) |
| 新增 C++ 辅助函数 | 3 (GLTF_ParseAndLoad, GLTF_TotalPrimitives, GLTF_FindPrimitive, GLTF_ExtractPrimitive) |
| 修改文件 | 3 (CMakeLists.txt, light_graphics_mesh.cpp, mesh_3d.lua smoke) |
| 新增文件 | 5 (cgltf.h, cgltf_impl.c + 3 docs) |
| **我方 C++ 代码** | **~310 行** + cgltf.h 第三方 ~6000 行 |

---

## 二、API 详细列表

### 2.1 `Light.Graphics.Mesh` 新静态方法

```lua
-- 加载 .gltf 或 .glb 文件, 返回第 N 个 primitive 作为 Mesh
Light.Graphics.Mesh.LoadGLTF(path, [primitive_index=0]) -> Mesh|nil [, err]

-- 查询 .gltf/.glb 中的 primitive 总数 (跨所有 mesh, 跨所有 primitive)
Light.Graphics.Mesh.GetGLTFMeshCount(path) -> int|nil [, err]
```

### 2.2 顶点属性提取规则 (Q3 决策 A)

| glTF 属性 | RenderVertex3D 字段 | 缺失时填充 |
|---|---|---|
| POSITION (vec3) | x, y, z | **必须**, 缺失返回 `"primitive has no POSITION attribute"` |
| NORMAL (vec3) | nx, ny, nz | (0, 1, 0) |
| TEXCOORD_0 (vec2) | u, v | (0, 0) |
| COLOR_0 (vec3 或 vec4) | r, g, b, a | (1, 1, 1, 1); vec3 时 a=1.0 |

类型不匹配 (如 NORMAL 不是 vec3) 时同样填充默认值,不报错。

### 2.3 索引转换

- cgltf 索引可能是 u8/u16/u32 → 通过 `cgltf_accessor_unpack_indices()` 统一转 `uint32_t`
- 无索引 (drawArray 模式) → 自动生成 `0, 1, 2, ..., vCount-1` 序列

### 2.4 软上限保护

- 单 primitive 顶点数 ≤ 1,000,000
- 单 primitive 索引数 ≤ 3,000,000

超出返回 `nil + err` 而非 OOM。

---

## 三、错误消息覆盖

| 场景 | 错误消息 |
|---|---|
| path 不是 string | `luaL_checkstring` 抛错 |
| primitive_index 负数 | `"primitive_index must be >= 0"` |
| 文件无法解析 | `"parse failed (cgltf err N)"` |
| buffer 加载失败 | `"buffer load failed (cgltf err N)"` |
| primitive_index 越界 | `"primitive index N out of range (have M)"` |
| 缺失 POSITION | `"primitive has no POSITION attribute"` |
| 0 顶点 | `"primitive has 0 vertices"` |
| 顶点超 1M | `"primitive vertex count N exceeds 1M soft limit"` |
| 索引超 3M | `"index count N exceeds 3M soft limit"` |
| 无 GL context | `"no render backend (window not opened?)"` |
| backend 不支持 3D | `"render backend does not support 3D mesh (need GL 3.3+)"` |
| GPU 上传失败 | `"CreateMesh failed (GPU upload error)"` |

---

## 四、Smoke 测试覆盖

`scripts/smoke/mesh_3d.lua` 增加 4b 段 (Phase AS.3 glTF 加载边界):

1. `LoadGLTF("nonexistent.gltf")` → `nil + err` 不崩
2. `LoadGLTF("nonexistent.glb")` → `nil + err` 不崩
3. `LoadGLTF(_, -1)` → `nil + err` 不崩
4. `GetGLTFMeshCount("nonexistent.gltf")` → `nil + err` 不崩

CI 不包含真实 .gltf/.glb 资源 (避免 repo 体积膨胀), 实际加载路径通过 cgltf 库自身的鲁棒性 (官方 test suite + 多年生产使用) 保证。

---

## 五、关键设计决策 (Q1~Q4 全 A)

| Q | 决策 | 实施 |
|---|---|---|
| Q1 | A — 我自动下载 cgltf.h | 已下载 v1.14 到 `third_party/cgltf.h` (193KB) |
| Q2 | A — `LoadGLTF(path, [pidx=0])` 灵活 | `luaL_optinteger(L, 2, 0)` |
| Q3 | A — 缺失属性自动填默认值 | NORMAL=(0,1,0), UV=(0,0), Color=(1,1,1,1) |
| Q4 | A — 暴露 `GetGLTFMeshCount` | 跨所有 mesh 计 primitive 总数 |

---

## 六、跨平台兼容性

cgltf 是纯 C 单文件,无 STL/异常依赖,所有平台 0 修改:

- ✅ Windows MSVC: 标准 C99/C11
- ✅ Linux GCC: 标准 C99/C11
- ✅ macOS Clang: 标准 C99/C11
- ✅ Android NDK: 标准 C99/C11
- ✅ iOS: 不需要 ObjC 编译 (与 stb_image 一致)
- ✅ Web (Emscripten): 标准 C99/C11

---

## 七、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `Light.Graphics.Mesh.New / Draw / Delete` | **零修改** |
| `RenderVertex3D` / `RenderBackend` | **零修改** (LoadGLTF 直接构造同样格式) |
| `Light.Graphics 2D / Canvas / Shader` | **零修改** |
| `CMakeLists.txt` | 加 1 行 `cgltf_impl.c` |
| `light_graphics_mesh.cpp` | 加 ~310 行 (顶部 +5 行 include + 3 辅助 fn + 2 lua fn + 注册表 +2 项) |
| `scripts/smoke/mesh_3d.lua` | 加 ~30 行 4b 段 |

---

## 八、未实施项 (留 AS.4 或后续)

- **多 primitive 同时加载** (返回 array of Mesh) — 留 AS.3.x 扩展 (低优先级)
- **材质属性提取** (baseColorFactor, metallicFactor, roughnessFactor 等) — 留 AS.4 PBR 时一起做
- **纹理自动加载** (从 .gltf 引用的 .png/.jpg 资源) — 留 AS.4 资产管线时一起做
- **动画 / 蒙皮** — 留更后续 (Skinned mesh 需要新顶点格式)
- **Tangent 顶点属性** (法线贴图必需) — 留 AS.4

---

## 九、CI 验收标准

- [x] cgltf.h v1.14 下载到 third_party/ 成功 (193KB)
- [x] cgltf_impl.c 创建
- [x] CMakeLists.txt 加源
- [x] `lightc -p scripts/smoke/mesh_3d.lua` Exit=0 (本地)
- [ ] GitHub Actions `Build Templates (All Platforms)` 全绿:
  - [ ] Windows x64: cgltf 编译 + Mesh.LoadGLTF 边界 smoke 通过
  - [ ] Linux x64: cgltf 编译 + 语法检查
  - [ ] macOS Universal: cgltf 编译 + 语法检查
  - [ ] Android arm64+x86_64: cgltf 编译
  - [ ] iOS arm64: cgltf 编译
  - [ ] Web WASM: cgltf 编译

CI 全绿后此子 Phase 才算最终交付完成。
