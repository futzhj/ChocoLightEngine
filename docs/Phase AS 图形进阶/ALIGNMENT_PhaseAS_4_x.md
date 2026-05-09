# Phase AS.4.x — glTF 材质提取 + 自动贴图加载 — 对齐文档

> **6A Stage 1**: AS.4 未实施项补完。LoadGLTF 加 with_material 参数, 完整提取 cgltf 材质 + 加载贴图。

---

## 1. API 扩展

```lua
-- 旧 (向后兼容):
local mesh = Light.Graphics.Mesh.LoadGLTF(path)
local mesh = Light.Graphics.Mesh.LoadGLTF(path, primIdx)

-- 新 (AS.4.x):
local mesh, material = Light.Graphics.Mesh.LoadGLTF(path, primIdx, true)
                                                    -- with_material=true
```

---

## 2. 材质提取规则 (cgltf v1.14 → MaterialDesc)

| cgltf 字段 | MaterialDesc 字段 |
|---|---|
| `mat->unlit` | mode = 0 (Unlit), 否则 1 (PBR) |
| `pbr.base_color_factor` | color[4] |
| `pbr.metallic_factor` | metallic |
| `pbr.roughness_factor` | roughness |
| `pbr.base_color_texture` | texBaseColor (加载贴图) |
| `pbr.metallic_roughness_texture` | texMetallicRoughness |
| `mat->normal_texture` | texNormal |
| `mat->normal_texture.scale` | normalScale |
| `mat->occlusion_texture` | texOcclusion |
| `mat->occlusion_texture.scale` | occlusionStrength |
| `mat->emissive_factor[3]` | emissive[3] |
| `mat->emissive_texture` | texEmissive |
| `mat->alpha_mode` | alphaMode (opaque=0, blend=1, mask=2) |
| `mat->alpha_cutoff` | alphaCutoff |
| `mat->double_sided` | doubleSided |

---

## 3. 贴图加载策略

### 来源优先级 (按 cgltf_image)

1. **image->buffer_view** 非空 (GLB embedded chunk): 用 `cgltf_buffer_view_data()` 拿数据
2. **image->uri** 是 "data:" URI (base64): 用 `cgltf_load_buffer_base64()` 解码
3. **image->uri** 是文件路径: 拼接 gltfDir + uri, fopen 读取

### 解码

`stbi_load_from_memory(data, size, &w, &h, &ch, 4)` → RGBA → `g_render->CreateTexture(w, h, 4, pixels)`

### 错误处理

任何失败 (file IO / base64 / stb_image 解码) 都返回 texId=0, **不让整个 LoadGLTF 失败**。Material 仍可用 baseColorFactor 等标量参数, 用户能看到颜色。

---

## 4. 实施步骤

1. `light_graphics_mesh.cpp` 顶部加 `#include "stb_image.h"` (extern "C" block 内)
2. 加 `LoadGLTFTexture(data, texture_view, gltfDir)` 静态辅助 (~80 行)
3. 加 `ExtractMaterial(MaterialDesc&, data, material, gltfDir)` 静态辅助 (~50 行)
4. `l_Mesh_LoadGLTF` 改造: 第 3 参数 with_material=true 时, 创建 Material userdata + 填充 + 返回 (Mesh, Material) 二元组
5. smoke `material_3d.lua` 加 with_material 边界路径 (无文件返回 nil + err, 不崩)

---

## 5. 边界 / 风险

| 项 | 处理 |
|---|---|
| primitive 没有 material 引用 (`prim->material == nullptr`) | 返回默认 PBR Material (white baseColor) |
| 文件没有 material section | 同上 |
| 贴图加载失败 | texId=0, 材质继续, 用 factor 显示 |
| primitive_index 越界 | 已有 (parse 阶段检测) |
| 文件路径含中文 | fopen 在 Windows 用 ANSI, 需要测; 暂时不做 unicode 路径转换 |
| 路径分隔符 (Windows '\\' vs glTF '/') | 拼路径时统一用 '/', cgltf URI 也是 '/' |

---

## 6. 工作量

| 子模块 | C++ 行 |
|---|---|
| `LoadGLTFTexture` (含 file IO + base64) | ~80 |
| `ExtractMaterial` | ~50 |
| `l_Mesh_LoadGLTF` 改造 (返回元组) | ~30 |
| smoke 加 with_material 边界 | ~30 |
| **合计** | **~190 行** |

预期 ~3h, 1 commit, 6 平台 CI 验证。

---

## 7. 关键决策

| Q | 决策 |
|---|---|
| Q1 API 形式 | 第 3 参数 with_material=true 返回二元组 (向后兼容) |
| Q2 贴图失败处理 | 静默 texId=0, 不让 LoadGLTF 失败 |
| Q3 路径处理 | 简单拼接 gltfDir + image.uri (不做 URL decode 已 cgltf 做了) |
| Q4 没有 material 时 | 返回默认 PBR Material |

直接实施 (无歧义, 全 A 推荐)。
