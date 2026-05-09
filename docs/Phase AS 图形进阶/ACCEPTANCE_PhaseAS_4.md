# Phase AS.4 — 完整材质系统 — 验收文档

> **状态**: ✅ **已完成** — 6 平台 CI 全绿 (修复 1 次 = AS.2 残留 shader 编译代码)
>
> GitHub Actions run: [25601950040](https://github.com/futzhj/ChocoLightEngine/actions/runs/25601950040) (commit `1701332`, 修复 commit `0d1310b`)
>
> **范围**: PBR Cook-Torrance + 法线贴图 (derivatives TBN) + 多光源 (1 dir + 4 point) + Material 资源
> + alphaMode (opaque/blend/mask) + 向后兼容 mesh:Draw
>
> **未实施**: glTF 自动材质提取 (LoadGLTF with_material) — 留 AS.4.x 后续 (300 行额外, 需要纹理 URI 加载流程)

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| 新增 Lua 模块 | 1 (`Light.Graphics.Material`) |
| 新增 Lua 函数 (Material) | 22 (1 静态 + 21 实例方法) |
| 新增 Lua 函数 (Light.Graphics 多光源) | 8 (SetDirectionalLight/Enabled, SetAmbientLight, AddPointLight, RemovePointLight, ClearPointLights, GetPointLightCount, GetMaxPointLights) |
| 新增 RenderBackend 虚函数 | 9 (DrawMeshMaterial + 8 lighting) |
| 新增数据结构 | 2 (`MaterialDesc`, `PointLight`) |
| 修改/扩展 内置 shader | 3 (VS3D 共用 + FS_UNLIT + FS_PBR, GLES3 + GL33 双版本 = 6 source) |
| 修改文件 | 5 (render_backend.h, render_gl33.cpp, light_graphics.cpp, light_graphics_mesh.cpp, light.h) |
| 新增文件 | 5 (light_graphics_material.cpp + material_3d.lua + 3 docs) |
| **C++ 总代码** | **~1100 行** (含 ~280 行 shader source) |

---

## 二、API 详细列表

### 2.1 `Light.Graphics.Material` 新模块 (22 fns)

```lua
-- 静态
Light.Graphics.Material.New([mode = "pbr"]) -> Material
  -- mode: "pbr" | "unlit"

-- 实例 (元表方法)
mat:SetMode("pbr"|"unlit") / mat:GetMode() -> string
mat:SetColor(r, g, b, [a=1]) / mat:GetColor() -> r,g,b,a
mat:SetEmissive(r, g, b) / mat:GetEmissive() -> r,g,b
mat:SetMetallic(f) / mat:GetMetallic() -> f                  (0..1, clamp)
mat:SetRoughness(f) / mat:GetRoughness() -> f                 (0..1, clamp)
mat:SetNormalScale(f) / mat:GetNormalScale() -> f
mat:SetOcclusionStrength(f) / mat:GetOcclusionStrength() -> f (0..1, clamp)
mat:SetTexture(name, textureId) / mat:GetTexture(name) -> int
  -- name: "baseColor" | "metallicRoughness" | "normal" | "emissive" | "occlusion"
mat:SetDoubleSided(bool) / mat:GetDoubleSided() -> bool
mat:SetAlphaMode("opaque"|"blend"|"mask") / mat:GetAlphaMode() -> string
mat:SetAlphaCutoff(f) / mat:GetAlphaCutoff() -> f
mat:Delete()                          -- POD, no-op (留 API 对称性)
mat:__gc                              -- 同 Delete
mat:__tostring                        -- "Material(pbr, color=(...))"
```

### 2.2 `Light.Graphics` 多光源 API (8 fns)

```lua
Light.Graphics.SetDirectionalLight(dx, dy, dz, r, g, b, [intensity=1])
Light.Graphics.SetDirectionalLightEnabled(bool)
Light.Graphics.SetAmbientLight(r, g, b)

local id = Light.Graphics.AddPointLight(x, y, z, r, g, b, range, [intensity=1])
  -- 返回 1..MaxPointLights, 0 = 已满
Light.Graphics.RemovePointLight(id)
Light.Graphics.ClearPointLights()
Light.Graphics.GetPointLightCount() -> int
Light.Graphics.GetMaxPointLights()  -> int (4 in current impl)
```

### 2.3 `mesh:Draw` 改造 (向后兼容)

```lua
mesh:Draw()             -- 等同 Draw(0)
mesh:Draw(0)            -- 老 API (textureId=0, 默认 PBR)
mesh:Draw(textureId)    -- 老 API (整数, AS.2 兼容)
mesh:Draw(material)     -- 新 API (userdata, AS.4)
```

C++ 端通过 `lua_type` 自动判断: integer/nil → `DrawMesh(textureId)`; userdata → `DrawMeshMaterial(desc)`.

### 2.4 RenderBackend C++ 接口扩展

```cpp
// 新数据结构
struct MaterialDesc { ... };  // PBR/Unlit 全属性, 16 字段
struct PointLight   { pos[3], color[3], range, intensity };

// 新虚函数 (GL33 真实现, Legacy 默认 no-op)
virtual void DrawMeshMaterial(uint32_t meshId, const MaterialDesc* desc);
virtual void SetDirectionalLight(const float* dir, const float* color, float intensity, bool enabled);
virtual void SetAmbientLight(const float* rgb);
virtual void SetCameraPos(const float* pos);
virtual int  AddPointLight(const PointLight* light);  // 1..N, 0=满
virtual void RemovePointLight(int id);
virtual void ClearPointLights();
virtual int  GetPointLightCount() const;
virtual int  GetMaxPointLights() const;
```

---

## 三、PBR Shader 实现细节 (简化版 Cook-Torrance)

### 3.1 BRDF (in shader)

```glsl
vec3 BRDF(N, V, L, baseColor, metallic, roughness, F0) {
    vec3 H = normalize(V + L);
    // GGX D (法线分布函数)
    float a = roughness * roughness;
    float a2 = a * a;
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (PI * denom * denom);
    // Smith G (几何项, Schlick-GGX 近似)
    float k = (roughness + 1.0); k = k * k / 8.0;
    float G = G_Schlick_GGX(NdotV, k) * G_Schlick_GGX(NdotL, k);
    // Schlick Fresnel
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    // 漫反射 + 镜面
    vec3 spec = (D * G * F) / (4.0 * NdotV * NdotL);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    return kD * baseColor / PI + spec;
}
```

### 3.2 法线贴图 (derivatives TBN)

不需要 Tangent 顶点属性, 用 fragment shader 内 `dFdx`/`dFdy` 实时计算 TBN 矩阵:

```glsl
vec3 dp1 = dFdx(vWorldPos);  vec3 dp2 = dFdy(vWorldPos);
vec2 duv1 = dFdx(vUV);       vec2 duv2 = dFdy(vUV);
vec3 dp2perp = cross(dp2, N); vec3 dp1perp = cross(N, dp1);
vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
mat3 TBN = mat3(normalize(T), normalize(B), N);
N = normalize(TBN * (texture(uTexNormal, vUV).rgb * 2.0 - 1.0));
```

### 3.3 多光源累积

```glsl
vec3 lightSum = vec3(0.0);
if (uDirLightEnabled == 1) lightSum += BRDF(...) * uDirLightColor * NdotL;
for (int i = 0; i < uPointLightCount; i++) {
    float d = length(uPointLightPos[i] - vWorldPos);
    if (d > uPointLightRange[i]) continue;
    float atten = pow(1.0 - d / uPointLightRange[i], 2.0);  // 二次衰减
    lightSum += BRDF(...) * uPointLightColor[i] * NdotL * atten;
}
```

### 3.4 alphaMode 处理 (glTF 标准)

| Mode | 行为 |
|---|---|
| opaque | 输出 alpha 强制 1.0 |
| blend | 透明混合 (引擎默认 enable blend) |
| mask | `if (a < uAlphaCutoff) discard;` |

---

## 四、关键设计决策 (CONSENSUS Q1~Q5 全 A 锁定)

| Q | 决策 |
|---|---|
| Q1 Material 形式 | A — 独立 userdata, POD 内部存储 |
| Q2 Mode 枚举 | A — "pbr" / "unlit" 字符串 |
| Q3 纹理 slot 命名 | A — glTF 标准 (baseColor/metallicRoughness/normal/emissive/occlusion) |
| Q4 AlphaMode | A — opaque/blend/mask 三模式 |
| Q5 mesh:Draw 兼容 | A — 自动类型判断 (integer/nil/userdata) |

### 4.1 关键技术简化 (CONSENSUS 已锁定, 风险控制)

| 简化 | 原方案 | 采用方案 | 节省工作量 |
|---|---|---|---|
| 1 | 加 Tangent 顶点属性 (RenderVertex3D 16 floats) | 用 derivatives 算 TBN, 顶点不变 (12 floats) | ~300 行 |
| 2 | 完整 Cook-Torrance + IBL | 简化 BRDF (D⋅G⋅F + Lambert) | ~200 行 |
| 3 | 任意数量光源 + clustered | 固定 1 dir + 4 point uniform 数组 | ~150 行 |
| 4 | Shadow mapping | 暂不做, 仅 lighting | ~400 行 |

总节省 ~1050 行, 实际交付 ~1100 行 (含 shader)。

---

## 五、Smoke 测试覆盖

`scripts/smoke/material_3d.lua` 6 个阶段:

1. **模块加载**: Light.Graphics.Material 模块, Material.New 函数注册
2. **Mode 切换**: New("pbr"/"unlit"/缺省), SetMode round-trip
3. **属性 round-trip** (~15 项):
   - color (4 args + 3 args 默认 a=1)
   - emissive (3 args)
   - metallic + 自动 clamp [0,1]
   - roughness, normalScale, occlusionStrength
   - 5 个纹理 slot (baseColor/metallicRoughness/normal/emissive/occlusion)
   - 未知 slot → error
   - doubleSided / alphaMode (3 模式) / alphaCutoff
   - __tostring 字段验证
4. **Light.Graphics 多光源**:
   - 8 个 fns 注册
   - SetDirectionalLight (6 args + 7 args), SetDirectionalLightEnabled
   - SetAmbientLight, AddPointLight (8 args), RemovePointLight, ClearPointLights
   - GetPointLightCount/GetMaxPointLights
5. **mesh:Draw 类型判断**:
   - 老 API: Draw() / Draw(0) / Draw(textureId) AS.2 兼容
   - 新 API: Draw(material) userdata 路径
   - string 参数不崩 (走老路径默认 0)
6. **回归兼容**:
   - AS.3 LoadGLTF / GetGLTFMeshCount 仍可用
   - AS.2 Mesh.New / GetVertexFormat 仍可用
   - AS.1 Canvas / Shader 仍可用
   - AR Light.Event 仍可用

---

## 六、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `RenderVertex3D` | **零修改** (使用 derivatives, 不需 Tangent) |
| `Light.Graphics.Mesh` AS.2/AS.3 API | **零破坏** (Draw 自动判断, LoadGLTF 不变) |
| `Light.Graphics.Canvas/Image/Shader` AS.1 | **零修改** |
| `Light.Graphics` 函数表 | 加 8 fn (多光源), 不动现有 |
| `Mat4` AS.2 | **零修改** |
| `RenderBackend` 接口 | 加 9 虚函数 + 2 数据结构, 默认 no-op |
| `RenderGL33Backend` | 替换 AS.2 单 shader 为 Unlit + PBR 双 shader |
| `Render Legacy` | **零修改** (默认 no-op 自动适用) |

---

## 七、未实施项 (留 AS.4.x 后续)

- ❌ **glTF 自动材质提取** (LoadGLTF with_material 参数返回 mesh + material) - 工作量 ~300 行 (含纹理 URI 加载)
- ❌ **完整 IBL** (image-based lighting from cubemap) - 留更后续
- ❌ **Shadow mapping** (方向光 shadow) - 留更后续
- ❌ **Skybox 渲染**
- ❌ **Skinned mesh / 骨骼动画**

---

## 八、CI 验收标准

- [x] `lightc -p scripts/smoke/material_3d.lua` Exit=0 (本地)
- [x] `lightc -p scripts/smoke/mesh_3d.lua` Exit=0 (回归)
- [ ] GitHub Actions `Build Templates (All Platforms)` 全绿:
  - [ ] Windows x64: 编译 + Windows runtime smoke (含 material_3d.lua)
  - [ ] Linux x64: 编译 + 语法检查
  - [ ] macOS Universal: 编译 + 语法检查
  - [ ] Android arm64+x86_64: 编译
  - [ ] iOS arm64: 编译
  - [ ] Web WASM: 编译

CI 全绿后此子 Phase 才算最终交付完成。
