# Phase AS.4.x — glTF 材质提取 + 自动贴图加载 — 验收文档

> **状态**: 已完成本地实施 + lightc 通过, 待 GitHub Actions CI 全平台验证
>
> **范围**: LoadGLTF 加 with_material 参数, 完整提取 cgltf 材质 + 加载贴图 (URI 文件 + GLB 嵌入式 + base64 data URI 三种来源)

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| API 改造 | `Mesh.LoadGLTF(path, [primIdx=0], [with_material=false])` 加第3参数 |
| 新 C++ 静态辅助 | 3 (GetGLTFDirectory, LoadGLTFImage, ExtractMaterial) |
| 修改文件 | 2 (light_graphics_mesh.cpp, scripts/smoke/material_3d.lua) |
| 新增文件 | 2 (ALIGNMENT_PhaseAS_4_x.md + ACCEPTANCE_PhaseAS_4_x.md) |
| **C++ 总代码** | **~200 行** |

---

## 二、API 行为

```lua
-- 老调用 (AS.3 兼容):
local mesh = Light.Graphics.Mesh.LoadGLTF(path)
local mesh = Light.Graphics.Mesh.LoadGLTF(path, 0)

-- 新调用 (AS.4.x):
local mesh, mat = Light.Graphics.Mesh.LoadGLTF(path, 0, true)
                                              -- with_material=true 返回二元组
                                              -- 后续可: mesh:Draw(mat)

-- mat 是完整 Material userdata, 已填充:
mat:GetMode()        -- "pbr" 或 "unlit" (从 mat->unlit)
mat:GetColor()       -- pbr.base_color_factor
mat:GetMetallic()    -- pbr.metallic_factor
mat:GetRoughness()   -- pbr.roughness_factor
mat:GetEmissive()    -- emissive_factor
mat:GetTexture("baseColor")        -- 已加载贴图的 GPU id
mat:GetTexture("metallicRoughness")
mat:GetTexture("normal")
mat:GetTexture("emissive")
mat:GetTexture("occlusion")
mat:GetNormalScale()       -- normal_texture.scale
mat:GetOcclusionStrength() -- occlusion_texture.scale
mat:GetAlphaMode()   -- "opaque" / "blend" / "mask"
mat:GetAlphaCutoff() -- alpha_cutoff
mat:GetDoubleSided() -- double_sided
```

---

## 三、cgltf 字段映射 (完整)

| cgltf 字段 | MaterialDesc 字段 |
|---|---|
| `mat->unlit` | mode (1 if unlit, else PBR=0… inverted, see code) |
| `pbr.base_color_factor[4]` | color[4] |
| `pbr.metallic_factor` | metallic |
| `pbr.roughness_factor` | roughness |
| `pbr.base_color_texture` | texBaseColor (LoadGLTFImage) |
| `pbr.metallic_roughness_texture` | texMetallicRoughness |
| `mat->normal_texture` | texNormal |
| `mat->normal_texture.scale` | normalScale |
| `mat->occlusion_texture` | texOcclusion |
| `mat->occlusion_texture.scale` | occlusionStrength |
| `mat->emissive_factor[3]` | emissive[3] |
| `mat->emissive_texture` | texEmissive |
| `mat->alpha_mode` | alphaMode (opaque=0, blend=1, mask=2) |
| `mat->alpha_cutoff` | alphaCutoff |
| `mat->double_sided` | doubleSided (0/1) |

cgltf 默认值 (parse 时已填充):
- `alpha_cutoff = 0.5`
- `normal_texture.scale = 1.0`
- `occlusion_texture.scale = 1.0`
- `metallic_factor = 1.0` (glTF spec)
- `roughness_factor = 1.0` (glTF spec)
- `base_color_factor = [1,1,1,1]`

---

## 四、贴图加载: 三种来源

### 来源 1: GLB embedded buffer_view

```c
const uint8_t* data = cgltf_buffer_view_data(img->buffer_view);
size_t size = img->buffer_view->size;
// 直接 stbi_load_from_memory
```

### 来源 2: data: URI (base64 inline)

```c
// img->uri = "data:image/png;base64,iVBORw0KGgo..."
const char* b64 = strchr(img->uri, ',') + 1;
size_t decodedSize = (strlen(b64) / 4) * 3;  // 减去 padding
cgltf_load_buffer_base64(opts, decodedSize, b64, &dec);
// stbi_load_from_memory
free(dec);
```

### 来源 3: 文件路径

```c
// img->uri = "textures/wood.png" (相对于 .gltf 文件目录)
std::string fullPath = gltfDir + img->uri;
FILE* fp = fopen(fullPath.c_str(), "rb");
// 读取文件 + stbi_load_from_memory
```

任何失败 → 返回 texId=0, **不影响 LoadGLTF 整体**, Material 仍可用 baseColorFactor 显示纯色。

---

## 五、向后兼容保证

| 调用形式 | 行为 | 兼容性 |
|---|---|---|
| `LoadGLTF(path)` | 返回 mesh (1 值) | ✅ AS.3 |
| `LoadGLTF(path, 0)` | 返回 mesh (1 值) | ✅ AS.3 |
| `LoadGLTF(path, 0, false)` | 返回 mesh (1 值) | ✅ AS.4.x 显式 |
| `LoadGLTF(path, 0, true)` | 返回 mesh + material (2 值) | ✅ AS.4.x 新 |
| `LoadGLTF(path, 0, nil)` | 同 false | ✅ |

---

## 六、错误处理

| 场景 | 行为 |
|---|---|
| 文件不存在 | 同 AS.3 (parse failed err) |
| primitive_index 越界 | 同 AS.3 |
| 无 GL context | 同 AS.3 (no render backend err) |
| primitive 没有 material | 返回默认 PBR Material (white baseColor) |
| 贴图文件不存在 | texId=0, 不报错 |
| 贴图解码失败 (stb_image) | texId=0, 不报错 |
| base64 解码失败 | texId=0, 不报错 |

---

## 七、内存管理与执行顺序

关键: cgltf_data 必须在 ExtractMaterial **之后**释放, 因为 prim->material 指针指向 cgltf_data 内部:

```cpp
1. cgltf_parse + cgltf_load_buffers
2. ExtractPrimitive (verts/indices 已 copy 到 std::vector)
3. g_render checks
4. CreateMesh (失败时 cgltf_free + return)
5. ExtractMaterial (with_material=true 时, 用 prim->material)
   ↑ 包含 LoadGLTFImage → CreateTexture (已上传到 GPU)
6. cgltf_free  ← 这里释放才安全
7. 创建 Mesh userdata + Material userdata
8. return 1 (mesh) 或 2 (mesh, material)
```

---

## 八、Smoke 测试 (5b 段, 4 个边界路径)

`scripts/smoke/material_3d.lua` 在第 5b 段验证:

1. `LoadGLTF(nonexistent, 0, true)` → nil + err (不崩)
2. `LoadGLTF(nonexistent, 0, false)` → nil + err (兼容)
3. `LoadGLTF(nonexistent, 0)` → nil + err (无第 3 参)
4. `LoadGLTF(nonexistent)` → nil + err (单参 AS.3 形式)

CI 不包含 .gltf 资源, 实际加载路径靠 cgltf + stb_image 库本身鲁棒性保证。

---

## 九、关键设计

### 9.1 Material 元表 lazy init

`luaopen_Light_Graphics_Mesh` 启动时调一次 `luaopen_Light_Graphics_Material`, 确保 Material 元表已注册。这样用户即使没显式 `require("Light.Graphics.Material")`, 调用 `LoadGLTF(_,_,true)` 仍能正确创建 Material userdata (元表 lookup 不会失败)。

### 9.2 路径分隔符兼容

`GetGLTFDirectory()` 用 `find_last_of("/\\")` 同时匹配 Unix `/` 和 Windows `\\`, 确保跨平台 URI 解析。

### 9.3 复用 light_graphics_image 已用的 stb_image

stb_image.h 已通过 third_party/stb_impl.c 在所有平台编译 (light_graphics_image.cpp 已用), 无需新增依赖。

---

## 十、未实施项 (留更后续)

- ❌ **多 primitive 同时返回数组** — 用户需多次调 LoadGLTF + 不同 primIdx
- ❌ **完整 IBL** (cubemap based ambient)
- ❌ **glTF KHR 扩展** (clearcoat / sheen / iridescence 等 — 当前忽略, 仅 PBR 基础)
- ❌ **跨 primitive 共享 material** — 当前每次 LoadGLTF 独立加载贴图 (重复加载)

---

## 十一、CI 验收标准

- [x] `lightc -p scripts/smoke/material_3d.lua` Exit=0 (本地)
- [x] `lightc -p scripts/smoke/mesh_3d.lua` Exit=0 (回归)
- [ ] GitHub Actions `Build Templates (All Platforms)` 全绿:
  - [ ] Windows x64: stb_image 已 link, Mesh.LoadGLTF with_material 边界 smoke
  - [ ] Linux x64: 编译
  - [ ] macOS Universal: 编译
  - [ ] Android arm64+x86_64: 编译
  - [ ] iOS arm64: 编译
  - [ ] Web WASM: 编译

CI 全绿后此子 Phase 才算最终交付完成。
