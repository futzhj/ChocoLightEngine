# Phase AS.4.1 — Material 资源 + Unlit/Lit 双模式 — 对齐文档

> **6A 工作流 Stage 1**: Align (Phase AS.4.1)
>
> **拆分说明**: AS.4 完整材质系统 (含 PBR + 法线贴图 + 多光源 + glTF 材质提取) 工作量 ~1500+ 行,
> 拆为 AS.4.1 (Material 资源 + Unlit/Lit 基础, ~600 行) 和 AS.4.2 (PBR + 法线贴图 + glTF 材质, 留后续)。

---

## 1. 现状回顾 (AS.2 + AS.3)

- `RenderVertex3D` 已存在 (pos3 + normal3 + uv2 + color4 = 48 bytes)
- 内置 3D shader: Lambert 单方向光 + 单纹理 (uTexture / uUseTexture)
- `mesh:Draw([textureId])` 直接传纹理 ID (无 material 抽象)
- `Light.Graphics.SetCamera/SetPerspective` 存在
- glTF 加载只取顶点数据, **完全忽略材质**

---

## 2. AS.4.1 范围 (In-Scope)

### 2.1 新模块: `Light.Graphics.Material`

```lua
-- 创建材质 (mode = "unlit" 或 "lit")
local mat = Light.Graphics.Material.New("unlit")  -- 或 "lit"
local mat = Light.Graphics.Material.New()         -- 默认 "lit"

-- 颜色与 PBR 标量参数
mat:SetColor(r, g, b, [a=1])                       -- baseColor / albedo
mat:GetColor() -> r, g, b, a

mat:SetEmissive(r, g, b)                          -- 自发光颜色 (Lit 模式有效)
mat:GetEmissive() -> r, g, b

mat:SetMetallic(0..1)                              -- 金属度 (留 AS.4.2 用)
mat:GetMetallic() -> number
mat:SetRoughness(0..1)                             -- 粗糙度 (留 AS.4.2 用)
mat:GetRoughness() -> number

-- 纹理 slot (textureId = 0 表示移除)
mat:SetTexture(slot_name, textureId)
  -- slot_name: "baseColor" | "emissive"
  -- AS.4.2 将加: "normal", "metallicRoughness", "occlusion"
mat:GetTexture(slot_name) -> textureId|0

-- 渲染状态
mat:SetMode("unlit") / mat:GetMode() -> "unlit"|"lit"
mat:SetDoubleSided(bool)                           -- 双面渲染 (默认 false, 单面)
mat:GetDoubleSided() -> bool
mat:SetAlphaMode("opaque" | "blend" | "mask")     -- 默认 "opaque"
mat:GetAlphaMode() -> string
mat:SetAlphaCutoff(0..1)                           -- 仅 mask 模式有效, 默认 0.5
mat:GetAlphaCutoff() -> number

mat:Delete()                                        -- 也通过 __gc
mat:__tostring                                      -- "Material(unlit, color=(...))"
```

### 2.2 修改: `mesh:Draw` 接受 material 参数

```lua
-- 旧 (向后兼容): 仍支持 textureId 参数 (整数)
mesh:Draw([textureId])

-- 新 (AS.4.1):
mesh:Draw(material)                                  -- material userdata
mesh:Draw(material, textureIdOverride)              -- 不常用; material.baseColorTex 优先
```

向后兼容: `mesh:Draw(0)` / `mesh:Draw(123)` / `mesh:Draw(nil)` 仍走老路径 (走默认 lit shader, 用 textureId)。

### 2.3 新内置 shader: Unlit + Lit (替代 AS.2 单 shader)

#### Unlit (无光照, 只显示纹理 + baseColor):
```glsl
FragColor = baseColor * texture(uBaseColorTex, vUV) * vColor;
// alpha mode: opaque (a=1), blend (使用 a), mask (a < cutoff 时 discard)
```

#### Lit (Lambert + ambient + emissive, 简化版 PBR 占位):
```glsl
vec3 N = normalize(vNormalW);
float ndl = max(dot(N, uLightDir), 0.0);
vec3 lit = uAmbient + uLightColor * ndl;
vec4 base = baseColor * texture(uBaseColorTex, vUV) * vColor;
vec3 emissive = uEmissive * texture(uEmissiveTex, vUV).rgb;
FragColor.rgb = base.rgb * lit + emissive;
FragColor.a = base.a;
```

不做完整 Cook-Torrance (留 AS.4.2)。当前 metallic/roughness 仅存储不使用,留接口。

### 2.4 Material 内部布局 (C++ 端)

```cpp
struct Material {
    enum Mode { UNLIT, LIT } mode;
    float color[4]      = {1, 1, 1, 1};   // baseColor
    float emissive[3]   = {0, 0, 0};
    float metallic      = 0.0f;            // AS.4.2 用
    float roughness     = 1.0f;            // AS.4.2 用
    uint32_t baseColorTex = 0;
    uint32_t emissiveTex  = 0;
    bool  doubleSided   = false;
    int   alphaMode     = 0;               // 0=opaque, 1=blend, 2=mask
    float alphaCutoff   = 0.5f;
};
```

Material userdata 直接存 `Material` 副本 (POD, 不需要 GPU 资源, 不需要 __gc 清理 GPU)。
若加 `__gc` 也只是 no-op (POD 自动析构)。

---

## 3. RenderBackend 接口扩展

### 3.1 新增虚函数

```cpp
// 由 light_graphics_mesh.cpp / light_graphics_material.cpp 调用
virtual void DrawMeshMaterial(uint32_t meshId, const MaterialDesc* desc);
```

`MaterialDesc` 是 C++ POD struct (与上面 Material 结构相同), 由调用方填充后传给 backend。
backend 内部根据 `desc->mode` 决定走 Unlit 还是 Lit shader, 上传 uniforms。

### 3.2 兼容性

`DrawMesh(meshId, textureId)` 保留, 内部调 `DrawMeshMaterial` 走默认 lit + textureId 模式 (零行为改变)。

---

## 4. 关键决策 (Q1~Q5)

### Q1 — Material 是否独立 userdata?

**A** (推荐): 独立 userdata, `Light.Graphics.Material.New(mode)` 创建,显式生命周期  
**B**: Lua table 即 material (无元表), 字段直接读  
**C**: 仅在 mesh:Draw 时传一次性 table (不持有)

A 推荐: 与现有 Mesh/Canvas/Shader 模式一致, 易扩展 (后续 PBR 加纹理 slot 多)。

### Q2 — Material 模式枚举

**A** (推荐): "unlit" / "lit" 字符串 + 内部映射枚举  
**B**: 数字常量 (0/1)  
**C**: 多种字符串 ("unlit", "phong", "pbr", ...)

A 推荐: 字符串易读, AS.4.2 加 "pbr" 时无破坏性变更。

### Q3 — 多纹理 slot 命名

**A** (推荐): glTF 标准命名 ("baseColor", "emissive", "normal", "metallicRoughness", "occlusion")  
**B**: 简短命名 ("diffuse", "spec", "normal", ...)  
**C**: 数字索引 (0/1/2...)

A 推荐: 与 glTF / 业界标准对齐, AS.4.2 glTF 自动提取时 0 修改命名。

### Q4 — alphaMode 行为

**A** (推荐): 三种模式 "opaque" (默认禁用 blend) / "blend" (alpha blend) / "mask" (alpha discard)  
**B**: 仅 "opaque" / "blend" (无 mask)  
**C**: bool toggle (透明 yes/no)

A 推荐: 与 glTF spec 完全对齐, 满足游戏常见需求 (树叶/草等用 mask)。

### Q5 — mesh:Draw 向后兼容策略

**A** (推荐): 自动判断参数类型: integer/nil → 走老路径; userdata → 走 material 路径  
**B**: 强制升级 API, mesh:Draw(material) 必须传 material  
**C**: 两套 API: mesh:Draw 老语义 + mesh:DrawMaterial 新语义

A 推荐: 零破坏性, AS.2 用户代码不需要改。

---

## 5. 工作量估算

| 子模块 | C++ 行 |
|---|---|
| `Light.Graphics.Material` 新模块 (light_graphics_material.cpp) | ~280 |
| `MaterialDesc` C struct in render_backend.h | ~25 |
| RenderBackend `DrawMeshMaterial` 虚函数 | ~5 |
| GL33Backend Unlit + Lit shader (替代单 shader) | ~120 |
| GL33Backend `DrawMeshMaterial` 实现 | ~80 |
| Mesh.Draw 改造 (类型判断 + 调 material 路径) | ~30 |
| Smoke `material_3d.lua` (5 阶段) | ~100 |
| 文档 (ALIGNMENT/CONSENSUS/ACCEPTANCE) | - |
| **合计** | **~640 行** |

预期 ~5h, 1 commit, 6 平台 CI 验证。

---

## 6. Out-of-Scope (留 AS.4.2 或后续)

- ❌ **完整 PBR Cook-Torrance** (Schlick Fresnel + GGX D + Smith G)
- ❌ **法线贴图** (需要 Tangent 顶点属性, 修改 RenderVertex3D 破坏性变更, 需先扩展)
- ❌ **多光源** (当前固定 1 个方向光 + ambient)
- ❌ **glTF material 自动提取** (LoadGLTF 不附带 material)
- ❌ **Skybox / IBL 环境光照**
- ❌ **阴影贴图**
- ❌ **后处理** (bloom / tonemapping)

---

## 7. 关键约束

- **零破坏性变更**: AS.2 `mesh:Draw([textureId])` 必须仍可用 (Q5 = A)
- **headless 友好**: Material.New 无 GL context 时仍可创建 (POD, 无 GPU 资源)
- **跨平台**: shader 同时提供 GLES3 (mobile) + GL33 (desktop) 双版本
- **不破坏 RenderVertex3D**: AS.4.2 加 tangent 时再扩展

---

## 8. 决策待确认

请用户确认 Q1~Q5:

1. **全部推荐 (Q1~Q5 全 A)** — 标准方案
2. **拒绝拆分, 一次做完整 AS.4 (含 PBR + 法线贴图 + 多光源)** — 工作量 ~1500+ 行 (~10h)
3. **跳过 AS.4 / AS.4.1, 转其他 phase** — 不做材质系统
4. **分别详细选择每个 Q**
