# Phase AS.4 — 完整材质系统 — 共识文档

> **6A 工作流 Stage 2**: Architect (用户决策: 不拆分, 一次性做完整 AS.4)
>
> **风险控制**: 通过关键技术简化, 将工作量从 ~1500 行压到 **~1050 行**, 不扩展 `RenderVertex3D`。

---

## 1. 关键技术简化决策 (降低风险)

### 简化 1: 不扩展 `RenderVertex3D` (保持 12 floats)

**原方案**: 加 Tangent 顶点属性 (4 floats: tx,ty,tz,handedness), 总 16 floats / 64 bytes
**采用方案**: 法线贴图通过 fragment shader 内 `dFdx`/`dFdy` derivatives 计算 TBN, 不动顶点数据

**理由**:
- 不破坏 AS.2 的 `Mesh.New(vertices, indices)` 12-floats Lua API
- 不破坏 AS.3 cgltf 顶点提取逻辑 (无需新增 Tangent unpack 代码)
- derivatives 方法在 GLES3 + GL33 都原生支持 (不需要扩展)
- 法线贴图视觉质量略低于专业 MikkTSpace 但完全可用 (业界常见简化)

### 简化 2: 简化 PBR (Lambert diffuse + Schlick + GGX specular)

**原方案**: 完整 Cook-Torrance + IBL + Occlusion + 复杂 BRDF
**采用方案**: Lambert diffuse + Schlick Fresnel + GGX (D⋅G) specular, 不做 IBL/occlusion 贴图实际计算

**理由**: 90% 视觉效果 + 50% 代码量, 留高级 PBR 给 AS.4.x 扩展

### 简化 3: 多光源限定 (1 dir + 4 point)

**原方案**: 任意数量光源 + clustered/forward+ 渲染
**采用方案**: 固定 1 方向光 + 4 点光数组 uniform (N=4 软上限)

**理由**: 单 forward pass + 数组 uniform 实现简单, 4 个点光满足绝大多数小场景

### 简化 4: 不做阴影贴图

后续 Phase 处理。当前光源仅 lighting, 不投影阴影。

---

## 2. 完整范围

### 2.1 新模块 `Light.Graphics.Material` (Q1=A 独立 userdata)

```lua
local mat = Light.Graphics.Material.New("pbr")           -- 或 "unlit"
mat:SetColor(r, g, b, [a])              -- baseColor
mat:GetColor() -> r,g,b,a
mat:SetEmissive(r, g, b)                 -- emissive 颜色 (PBR/Unlit 都用)
mat:GetEmissive() -> r,g,b
mat:SetMetallic(0..1)                     -- PBR
mat:SetRoughness(0..1)                    -- PBR (默认 1.0 = 完全粗糙)
mat:SetNormalScale(s)                     -- normal map 强度 (默认 1.0)
mat:SetOcclusionStrength(s)              -- AO 强度 (默认 1.0)
mat:GetMetallic() / GetRoughness() / GetNormalScale() / GetOcclusionStrength()

-- 5 个纹理 slot (Q3=A glTF 标准命名)
mat:SetTexture(name, textureId)          -- 0 = 移除
  -- name: "baseColor" | "metallicRoughness" | "normal" | "emissive" | "occlusion"
mat:GetTexture(name) -> textureId|0

mat:SetMode("pbr"|"unlit")                -- Q2=A 字符串模式
mat:GetMode() -> string
mat:SetDoubleSided(bool)                  -- 默认 false
mat:GetDoubleSided() -> bool
mat:SetAlphaMode("opaque"|"blend"|"mask") -- Q4=A 三模式 (glTF 标准)
mat:GetAlphaMode() -> string
mat:SetAlphaCutoff(0..1)                  -- 仅 mask 模式有效, 默认 0.5
mat:GetAlphaCutoff() -> number

mat:Delete()                               -- 也通过 __gc
mat:__tostring                             -- "Material(pbr, color=(1,0.5,0.2,1))"
```

### 2.2 新增 `Light.Graphics` 多光源 API

```lua
-- 主方向光 (1 个, 可关闭)
Light.Graphics.SetDirectionalLight(dirX, dirY, dirZ, r, g, b, [intensity=1])
Light.Graphics.SetDirectionalLightEnabled(bool)
  -- 默认: dir=(0.41, 0.82, 0.41), color=(0.9, 0.9, 0.85), intensity=1, enabled=true

-- 环境光 (单色)
Light.Graphics.SetAmbientLight(r, g, b)
  -- 默认: (0.2, 0.2, 0.25)

-- 点光 (最多 4 个, 超出忽略)
local id = Light.Graphics.AddPointLight(x, y, z, r, g, b, range, [intensity=1])
  -- 返回 id (1..4) 用于后续 Remove; 满了返回 0
Light.Graphics.RemovePointLight(id)
Light.Graphics.ClearPointLights()
Light.Graphics.GetPointLightCount() -> int (0..4)

Light.Graphics.GetMaxPointLights() -> 4   -- 常量查询
```

### 2.3 改造 `mesh:Draw` (Q5=A 自动判断参数类型)

```lua
mesh:Draw()                                -- 默认 lit shader, 无纹理
mesh:Draw(textureId)                       -- 老路径, integer 参数
mesh:Draw(material)                        -- 新路径, userdata 参数
mesh:Draw(nil)                             -- 等同 Draw(), 显式无材质
```

C++ 内部:
```cpp
if (lua_isnumber(L, 2)) {
    // 老路径: 走默认 lit shader + textureId
} else if (lua_isuserdata(L, 2)) {
    Material* m = luaL_checkudata(L, 2, MATERIAL_MT);
    // 新路径: 调 DrawMeshMaterial(meshId, m)
} else {
    // 默认 lit shader, 无纹理
}
```

### 2.4 cgltf 材质提取

```lua
-- 单 primitive 加载 + 同时取 material
local mesh, material = Light.Graphics.Mesh.LoadGLTF("model.glb", 0, true)
  -- 第三参数 with_material=true 时返回 (Mesh, Material)
  -- 第三参数省略或 false 时只返回 Mesh (向后兼容 AS.3)

-- 失败时返回 nil + err 不变
```

提取规则 (cgltf v1.14):
```
glTF                          → Material
─────────────────────────────────────────────
material.has_pbr_metallic_roughness:
  pbr_metallic_roughness.base_color_factor      → mat:SetColor
  pbr_metallic_roughness.metallic_factor         → mat:SetMetallic
  pbr_metallic_roughness.roughness_factor        → mat:SetRoughness
  pbr_metallic_roughness.base_color_texture     → 加载贴图 + mat:SetTexture("baseColor", id)
  pbr_metallic_roughness.metallic_roughness_tex  → 加载 + SetTexture("metallicRoughness", id)

material.normal_texture                          → SetTexture("normal", id)
material.normal_texture.scale                    → SetNormalScale
material.emissive_factor                         → SetEmissive
material.emissive_texture                        → SetTexture("emissive", id)
material.occlusion_texture                       → SetTexture("occlusion", id)
material.occlusion_texture.strength              → SetOcclusionStrength
material.alpha_mode (cgltf_alpha_mode)           → SetAlphaMode
material.alpha_cutoff                            → SetAlphaCutoff
material.double_sided                            → SetDoubleSided
material.unlit                                    → SetMode("unlit") else "pbr"
```

纹理加载: cgltf 提供 image URI (相对路径), 我们调 stb_image 加载, 然后 g_render->CreateTexture 返回 texId。
注意: glTF embedded base64 image (data URI) 也支持 (cgltf 自动 decode 到 buffer_view)。

### 2.5 新内置 shader (替代 AS.2 单 shader)

#### Unlit shader
```glsl
// VS: 标准 mvp 变换 + 传 vUV/vColor
// FS:
vec4 base = uColor * vColor;
if (uHasBaseColorTex == 1) base *= texture(uBaseColorTex, vUV);
vec3 emissive = uEmissive;
if (uHasEmissiveTex == 1) emissive *= texture(uEmissiveTex, vUV).rgb;
vec3 result = base.rgb + emissive;

// alpha 处理
if (uAlphaMode == 2 && base.a < uAlphaCutoff) discard;  // mask
FragColor = vec4(result, base.a);
```

#### PBR shader (简化)
```glsl
// VS: 增加 vWorldPos out (用于点光距离计算)
// FS:
vec4 base = uColor * vColor;
if (uHasBaseColorTex == 1) base *= texture(uBaseColorTex, vUV);

float metallic = uMetallic;
float roughness = uRoughness;
if (uHasMetallicRoughnessTex == 1) {
    vec3 mr = texture(uMetallicRoughnessTex, vUV).rgb;
    metallic *= mr.b;       // glTF spec: metalness in B
    roughness *= mr.g;       // roughness in G
}

// 法线: 默认顶点法线, 有 normal map 时用 derivatives 算 TBN
vec3 N = normalize(vNormalW);
if (uHasNormalTex == 1) {
    vec3 mappedN = texture(uNormalTex, vUV).rgb * 2.0 - 1.0;
    mappedN.xy *= uNormalScale;
    // TBN 通过 derivatives 计算
    vec3 dp1 = dFdx(vWorldPos);
    vec3 dp2 = dFdy(vWorldPos);
    vec2 duv1 = dFdx(vUV);
    vec2 duv2 = dFdy(vUV);
    vec3 dp2perp = cross(dp2, N);
    vec3 dp1perp = cross(N, dp1);
    vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
    vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
    float invmax = inversesqrt(max(dot(T,T), dot(B,B)));
    mat3 TBN = mat3(T * invmax, B * invmax, N);
    N = normalize(TBN * mappedN);
}

// 累积光照 (1 dir + N point)
vec3 V = normalize(uCameraPos - vWorldPos);
vec3 F0 = mix(vec3(0.04), base.rgb, metallic);
vec3 lightSum = vec3(0.0);

if (uDirLightEnabled == 1) {
    vec3 L = uDirLightDir;
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    lightSum += BRDF(N, V, L, H, base.rgb, metallic, roughness, F0) * uDirLightColor * NdotL;
}
for (int i = 0; i < uPointLightCount; i++) {
    vec3 toLight = uPointLightPos[i] - vWorldPos;
    float d = length(toLight);
    if (d > uPointLightRange[i]) continue;
    vec3 L = toLight / d;
    vec3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);
    float atten = pow(1.0 - d / uPointLightRange[i], 2.0);
    lightSum += BRDF(...) * uPointLightColor[i] * NdotL * atten;
}

// AO + emissive
float ao = 1.0;
if (uHasOcclusionTex == 1) ao = mix(1.0, texture(uOcclusionTex, vUV).r, uOcclusionStrength);
vec3 ambient = uAmbient * base.rgb * ao;

vec3 emissive = uEmissive;
if (uHasEmissiveTex == 1) emissive *= texture(uEmissiveTex, vUV).rgb;

FragColor = vec4(ambient + lightSum + emissive, base.a);

// alpha 处理同 Unlit
```

#### BRDF (在 shader 中作为 helper):
```glsl
vec3 BRDF(N, V, L, H, baseColor, metallic, roughness, F0) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // GGX D
    float denom = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159 * denom * denom);

    // Smith G (Schlick approx)
    float k = (roughness + 1.0); k = k * k / 8.0;
    float G1V = NdotV / (NdotV * (1.0 - k) + k);
    float G1L = NdotL / (NdotL * (1.0 - k) + k);
    float G = G1V * G1L;

    // Schlick Fresnel
    vec3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);

    // Specular + Diffuse
    vec3 spec = (D * G * F) / max(4.0 * NdotV * NdotL, 0.001);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 diff = kD * baseColor / 3.14159;

    return diff + spec;
}
```

### 2.6 RenderBackend 接口扩展

```cpp
// 新结构 (POD)
struct MaterialDesc {
    int  mode;                    // 0=Unlit, 1=PBR
    float color[4];                // baseColor
    float emissive[3];
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    uint32_t texBaseColor;
    uint32_t texMetallicRoughness;
    uint32_t texNormal;
    uint32_t texEmissive;
    uint32_t texOcclusion;
    int   alphaMode;              // 0=opaque, 1=blend, 2=mask
    float alphaCutoff;
    int   doubleSided;
};

struct PointLight {
    float pos[3];
    float color[3];
    float range;
    float intensity;
};

// 新虚函数
virtual void DrawMeshMaterial(uint32_t meshId, const MaterialDesc* desc);
virtual void SetDirectionalLight(const float* dir, const float* color, float intensity, bool enabled);
virtual void SetAmbientLight(const float* color);
virtual void SetCameraPos(const float* pos);  // PBR 需要 view dir
virtual int  AddPointLight(const PointLight* light);  // 返回 id 1..4, 满返回 0
virtual void RemovePointLight(int id);
virtual void ClearPointLights();
virtual int  GetPointLightCount() const;
```

`DrawMesh(meshId, textureId)` 保留, 内部转 DrawMeshMaterial 走默认 PBR + textureId 模式。

---

## 3. 工作量估算 (修订版)

| 子模块 | C++ 行 |
|---|---|
| `MaterialDesc` + `PointLight` struct in render_backend.h | ~40 |
| `Light.Graphics.Material` 模块 (light_graphics_material.cpp) | ~340 |
| 多光源 API in light_graphics.cpp | ~150 |
| RenderBackend 接口扩展 (8 虚函数 + no-op 默认) | ~30 |
| GL33 Unlit + PBR shader (双 GLES3+GL33 = 4 source) | ~280 |
| GL33 DrawMeshMaterial + SetDirectionalLight 等实现 | ~200 |
| Mesh.Draw 改造 (类型判断, with_material) | ~50 |
| LoadGLTF 材质提取 (with_material=true) + 纹理 URI 加载 | ~150 |
| Smoke `material_3d.lua` (6 阶段) | ~150 |
| 文档 (ALIGNMENT/CONSENSUS/ACCEPTANCE) | - |
| **合计** | **~1390 行** |

预期 ~10h, 1 commit, 6 平台 CI 验证。

**比 ALIGNMENT 中的 1500+ 行少, 因为不扩展 RenderVertex3D 节省了 ~300 行**。

---

## 4. 实施顺序 (单 commit)

1. `render_backend.h`: `MaterialDesc` + `PointLight` struct + 8 新虚函数声明
2. `render_backend.cpp` / `render_gl33.cpp`: 实现 8 虚函数, 替换 AS.2 单 shader 为 Unlit/PBR 双 shader
3. `light_graphics_material.cpp`: 新模块 + 12 方法
4. `light_graphics.cpp`: 加多光源 API (8 fn)
5. `light_graphics_mesh.cpp`: Mesh.Draw 类型判断 + LoadGLTF with_material 参数 + cgltf 材质提取
6. `light.h`: 加 `luaopen_Light_Graphics_Material` 声明
7. `lumen-master/src/light/light.cpp`: 注册 `Light.Graphics.Material`
8. `CMakeLists.txt`: 加 `light_graphics_material.cpp`
9. `scripts/smoke/material_3d.lua`: 新 smoke (6 阶段)
10. `.github/workflows/build-templates.yml`: 加 material_3d 到 Windows runtime smoke

---

## 5. 验收标准

- [ ] `Light.Graphics.Material.New("pbr"|"unlit")` 创建 userdata
- [ ] 12 个 Material 方法都可用 (SetColor/SetTexture/SetMetallic/...)
- [ ] mesh:Draw 自动判断 textureId vs material userdata (向后兼容 AS.2)
- [ ] `Light.Graphics.SetDirectionalLight/AddPointLight/...` 8 个 fn 可用
- [ ] LoadGLTF with_material=true 返回 (mesh, material) 二元组
- [ ] `lightc -p material_3d.lua` Exit=0
- [ ] 6 平台 CI 全绿
- [ ] AS.2 / AS.3 旧 smoke (mesh_3d.lua) 不破坏 (向后兼容验证)

---

## 6. 风险

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| GLES3 / GL33 shader 不一致编译错 | 中 | 中 | 双版本 source + lightc 本地预检 |
| derivatives 在 GLES3 需 `#extension GL_OES_standard_derivatives` (老 GPU) | 低 | 低 | GLES 3.0+ 默认支持, AS 系列需求 GL 3.3+ 已限定 |
| cgltf material 提取在 embedded vs URI 两种模式行为不同 | 低 | 中 | 优先做 URI, embedded 留 fallback |
| Material userdata 没有 GPU 资源, __gc 是否需要? | 低 | 低 | __gc no-op (POD) |
| 多光源 4 上限不够某些场景 | 低 | 低 | AS.4.x 可扩展到更高 N |
| PBR 数学错误 (Cook-Torrance 公式) | 中 | 中 | 参考 LearnOpenGL PBR 章节复制经过验证的实现 |
