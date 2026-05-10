# CONSENSUS — Phase AX（Morph Target 表情/形状变形）

> **6A 工作流 Stage 2 — Architect §共识阶段产物**
> 锁定 Phase AX 的所有决策、接口契约、验收标准。一切实现以本文件为准。

---

## 一、明确的需求描述

### 1.1 核心目标

在 ChocoLight Animation 模块中加入完整 glTF 2.0 morph target 支持：
- **结构层**：扩展 `SkinnedMeshAsset` / `Animator` 数据结构，存储 morph delta 与权重
- **解析层**：`LoadSkinnedGLTF` 自动提取 `prim.targets[]` / `mesh.weights[]` / `mesh.target_names[]`
- **驱动层**：动画通道 `target_path == "weights"` 自动驱动 + 手动 `SetMorphWeight` 覆盖
- **渲染层**：CPU 路径（兼容所有 backend）+ GPU 路径（限 GL33Core，新 shader）
- **协作层**：与 Phase AW GPU skinning 完全协作，同时启用时走 `VS3D_SKIN_MORPH` 全 GPU 路径

### 1.2 不在范围

- ❌ Sparse accessor / 自定义 attribute 名称
- ❌ Layer / Additive blend / IK
- ❌ Web Safari WebGL2 性能调优（沿用 Phase AW Q7：Web 默认 CPU）

---

## 二、关键决策表（已锁定）

### 2.1 用户决策（Stage 1 §1.4 已确认）

| ID | 问题 | 选择 | 理由 |
|----|------|------|------|
| **Q1** | Phase AX 范围 | **A. 完整**（POS+NORMAL+TANGENT, CPU+GPU）| 商业级能力；与 glTF 2.0 spec 对齐 |
| **Q2** | 权重来源 | **B. 动画 + 手动覆盖** | 动画师手 K + 代码运行时调整都需要 |
| **Q3** | GPU 数据传输 | **B. 限 N≤8 + uniform array** | 平衡 GLES 3.0 / WebGL 2 / Legacy GL 兼容性 |
| **Q4** | 与 Skinning 共存 | **A. 全路径 GPU**（新 shader）| 不损失 Phase AW 收益；最自然 |

### 2.2 自动决策（Stage 1 §5.1）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| AD1 | morph target 数量上限 | **8** | glTF 实际 6-8；uniform array 长度可控 |
| AD2 | morph delta 内存布局 | **per-target × per-vertex AoS（POS/NRM/TAN 三独立 vector\<float\>）** | 与 cgltf accessor 一致；shader 取数容易 |
| AD3 | 默认权重来源 | `mesh.weights[]` | glTF spec 标准 |
| AD4 | morph target 名称 | `mesh.target_names[]` + fallback `target_<idx>` | 友好名称；找不到时也 robust |
| AD5 | sampler `components`（weights 路径）| `= morphTargetCount` | glTF spec 规定每帧 N 个 weight |
| AD6 | 手动覆盖 sentinel | **NaN** 表示"未覆盖, 用动画值" | 与 0 区分（0 是合法权重）|
| AD7 | Lua 模块挂载 | 复用 `Light.Animation` + `Animator:*` 方法 | 与 Skeleton/Clip/Animator 风格一致 |
| AD8 | smoke 文件 | 复用 `scripts/smoke/animation.lua`（添加段落）| 避免 file 增多 |
| AD9 | sample 命名 | `samples/demo_morph_target/` | 与 demo_skinning_perf 平行 |
| AD10 | backend 名称扩展 | **不变**（GL33Core/LegacyGL）| 仅内部 program 增加 |
| AD11 | morph delta 上传频率 | **一次性上传**（与 base verts 一同 CreateMorphMesh）| 与 Phase AW skinning 模式一致 |
| AD12 | 每帧权重上传 | **uniform array，每帧 glUniform1fv** | 8 floats 上传开销可忽略 |

---

## 三、技术实现方案

### 3.1 C++ 数据结构扩展

#### `enum ChannelTarget`

```cpp
enum class ChannelTarget : uint8_t {
    TRANSLATION   = 0,
    ROTATION      = 1,
    SCALE         = 2,
    MORPH_WEIGHTS = 3,    // ← Phase AX 新增
    UNSUPPORTED   = 255,
};
```

#### `struct Sampler`（扩展，不破坏既有字段）

```cpp
struct Sampler {
    int            jointIndex = -1;       // ChannelTarget != MORPH_WEIGHTS 时使用
    int            meshNodeIdx = -1;      // ChannelTarget == MORPH_WEIGHTS 时使用 (cgltf node 索引)
    ChannelTarget  target     = ChannelTarget::UNSUPPORTED;
    InterpMode     mode       = InterpMode::LINEAR;
    int            components = 3;        // 3 (T/S) 或 4 (R) 或 N (MORPH_WEIGHTS)
    std::vector<float> times;
    std::vector<float> values;
};
```

#### `struct MorphTarget`（新增）

```cpp
struct MorphTarget {
    std::vector<float> posDelta;   // vCount × 3 floats
    std::vector<float> nrmDelta;   // vCount × 3 floats（可空）
    std::vector<float> tanDelta;   // vCount × 3 floats（可空，glTF spec 要求 vec3 not vec4）
};
```

#### `struct SkinnedMeshAsset`（扩展）

```cpp
struct SkinnedMeshAsset {
    // ... 既有字段 ...

    // Phase AX：morph target 数据
    std::vector<MorphTarget>      morphTargets;          // size <= 8
    int                           morphTargetCount = 0;
    std::vector<float>            morphDefaultWeights;   // size = morphTargetCount
    std::vector<std::string>      morphTargetNames;      // size = morphTargetCount

    // Phase AX：GPU 路径专用 mesh ID（与 gpuSkinnedMeshId 互斥；同时启动 morph+skin 时使用）
    uint32_t gpuMorphMeshId       = 0;
    uint32_t gpuSkinnedMorphMeshId = 0;
    bool     gpuMorphMeshUploaded = false;
    bool     gpuSkinnedMorphMeshUploaded = false;
};
```

#### `struct Animator`（扩展）

```cpp
struct Animator {
    // ... 既有字段 ...

    // Phase AX：morph 权重运行时状态
    std::vector<float> morphWeights;          // 当前生效权重（动画+手动覆盖）
    std::vector<float> morphWeightsManual;    // 手动覆盖（NaN = 用动画值）
};
```

### 3.2 Backend 接口扩展（`render_backend.h`）

```cpp
class RenderBackend {
public:
    // ... 既有 ...

    // Phase AX：morph target mesh 创建（4 种 mesh 类型矩阵）
    // 1. 普通 mesh + morph
    virtual uint32_t CreateMorphMesh(const RenderVertex3D* verts, int vCount,
                                       const uint32_t* indices, int iCount,
                                       const float* posDeltas,    // size = vCount × 3 × N
                                       const float* nrmDeltas,    // size = vCount × 3 × N (可 nullptr)
                                       const float* tanDeltas,    // size = vCount × 3 × N (可 nullptr)
                                       int morphTargetCount) { return 0; }
    virtual void DrawMorphMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                         const float* morphWeights, int morphTargetCount) {}

    // 2. 蒙皮 mesh + morph
    virtual uint32_t CreateSkinnedMorphMesh(const RenderVertex3DSkin* verts, int vCount,
                                              const uint32_t* indices, int iCount,
                                              const float* posDeltas,
                                              const float* nrmDeltas,
                                              const float* tanDeltas,
                                              int morphTargetCount) { return 0; }
    virtual void DrawSkinnedMorphMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                                 const float* jointMatrices, int jointCount,
                                                 const float* morphWeights, int morphTargetCount) {}

    // Phase AX：能力查询
    virtual bool SupportsMorphTargets() const { return false; }
};
```

### 3.3 Shader 设计

#### `VS3D_MORPH`（新增 — base + morph）

```glsl
#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;

uniform mat4 uMVP;
uniform mat4 uModel;

const int MORPH_MAX = 8;
uniform float uMorphWeights[MORPH_MAX];
uniform int   uMorphCount;
uniform sampler2D uMorphPosDelta;   // RGBA32F texture: width=vCount, height=N
uniform sampler2D uMorphNrmDelta;
uniform int   uHasMorphNormal;

out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;

void main() {
    vec3 morphedPos    = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        if (w == 0.0) continue;
        vec3 dPos = texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        morphedPos += w * dPos;
        if (uHasMorphNormal == 1) {
            vec3 dNrm = texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
            morphedNormal += w * dNrm;
        }
    }
    gl_Position = uMVP * vec4(morphedPos, 1.0);
    vNormalW    = mat3(uModel) * morphedNormal;
    vWorldPos   = (uModel * vec4(morphedPos, 1.0)).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
}
```

> **关键技术选择**：morph delta 数据放 **2D texture**（width=vCount, height=N），shader 用 `texelFetch(idx, target_idx)` 取数。**优于 uniform array 的原因**：vCount 可能很大（5000+），uniform array 上限 4096 vec4 不够用；texture 无此限制，且符合 GLES 3.0 / WebGL 2 兼容性。

> **修正 Q3 决策**：经过详细设计，发现 uniform array 仅适合 **weights**（N ≤ 8 floats），但 **delta 数据** 必须用 texture。Stage 2 锁定为：
>
> - **uMorphWeights**: uniform array (N ≤ 8 floats)
> - **uMorphPosDelta / NrmDelta / TanDelta**: 2D texture（vCount × N）

#### `VS3D_SKIN_MORPH`（新增 — base + morph + skin）

```glsl
#version 300 es
precision highp float;
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;
layout(location=5) in vec4  aWeights;

uniform mat4 uMVP;
uniform mat4 uModel;
layout(std140) uniform JointBlock { mat4 uJointMats[64]; };

const int MORPH_MAX = 8;
uniform float uMorphWeights[MORPH_MAX];
uniform int   uMorphCount;
uniform sampler2D uMorphPosDelta;
uniform sampler2D uMorphNrmDelta;
uniform int   uHasMorphNormal;

out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;

void main() {
    // 1. 应用 morph delta
    vec3 morphedPos    = aPos;
    vec3 morphedNormal = aNormal;
    for (int i = 0; i < MORPH_MAX; ++i) {
        if (i >= uMorphCount) break;
        float w = uMorphWeights[i];
        if (w == 0.0) continue;
        morphedPos    += w * texelFetch(uMorphPosDelta, ivec2(gl_VertexID, i), 0).xyz;
        if (uHasMorphNormal == 1) {
            morphedNormal += w * texelFetch(uMorphNrmDelta, ivec2(gl_VertexID, i), 0).xyz;
        }
    }

    // 2. 蒙皮
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];
    vec4 skinnedPos    = blend * vec4(morphedPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * morphedNormal;

    gl_Position = uMVP * skinnedPos;
    vNormalW    = mat3(uModel) * skinnedNormal;
    vWorldPos   = (uModel * skinnedPos).xyz;
    vTexCoord   = aUV;
    vColor      = aColor;
}
```

> **6 个 program 矩阵**（VS × FS）：
>
> | Program | VS | FS | 用途 |
> |---------|----|----|------|
> | UnlitPlain | VS3D | FS_UNLIT | 普通 mesh + Unlit |
> | PBRPlain | VS3D | FS_PBR | 普通 mesh + PBR |
> | UnlitSkin | VS3D_SKIN | FS_UNLIT | 蒙皮 + Unlit |
> | PBRSkin | VS3D_SKIN | FS_PBR | 蒙皮 + PBR |
> | **UnlitMorph** | VS3D_MORPH | FS_UNLIT | morph + Unlit (新)|
> | **PBRMorph** | VS3D_MORPH | FS_PBR | morph + PBR (新)|
> | **UnlitSkinMorph** | VS3D_SKIN_MORPH | FS_UNLIT | 蒙皮 + morph + Unlit (新)|
> | **PBRSkinMorph** | VS3D_SKIN_MORPH | FS_PBR | 蒙皮 + morph + PBR (新)|
>
> 共 8 个 program（4 + 4 新增）。

### 3.4 Lua API

```
-- 顶层模块（无新增；复用 Light.Animation）
Light.Animation.LoadSkinnedGLTF(path)         -- 自动提取 morph
Light.Animation.NewAnimator(skeleton)          -- 关联 mesh 后自动初始化 morphWeights
Light.Animation.DrawSkinnedMesh(mesh, animator, transform, material)  -- 内部分流 morph 路径

-- Animator 实例方法（新增）
animator:GetMorphTargetCount()                 -- 返回 number (0~8)
animator:GetMorphTargetName(idx)               -- 返回 string 或 nil（idx 1-based）
animator:SetMorphWeight(idx_or_name, value)    -- idx number(1-based) 或 string name；value number
animator:GetMorphWeight(idx_or_name)           -- 返回当前生效 weight
animator:ClearMorphWeights()                   -- 清除所有手动覆盖（恢复动画驱动）
animator:GetMorphWeights()                     -- 返回 array { w1, w2, ..., wN }（current effective）

-- Mesh 实例方法（新增）
mesh:HasMorphTargets()                          -- 返回 bool
mesh:GetMorphTargetCount()                      -- 返回 number
mesh:GetMorphTargetName(idx)                    -- 返回 string

-- 模块函数（新增）
Light.Animation.MORPH_TARGET_MAX                -- 常量 = 8
```

### 3.5 数据流

```
glTF 文件
  ↓ cgltf_parse_file + cgltf_load_buffers
cgltf_data
  ↓ ExtractMorphTargets(prim, &skMesh)
SkinnedMeshAsset { morphTargets[], morphTargetCount, morphDefaultWeights[], morphTargetNames[] }
  ↓ NewAnimator(skeleton) → Animator { morphWeights[N] = morphDefaultWeights, morphWeightsManual[N] = NaN }

每帧 Animator:Update(dt):
  ├─ ComputeJointMatrices (既有)
  └─ EvaluateMorphWeights:
        for each weights sampler in activeClip:
            morphWeights[i] = sampler 值 (按 currentTime 插值)
        for i in 0..N:
            if not isnan(morphWeightsManual[i]):
                morphWeights[i] = morphWeightsManual[i]   ← 手动覆盖

DrawSkinnedMesh:
  ├─ ShouldUseGPUSkinning() && skMesh.HasMorph()
  │     → DrawSkinnedMorphMeshGPU (使用 VS3D_SKIN_MORPH)
  ├─ ShouldUseGPUSkinning() && !skMesh.HasMorph()
  │     → DrawSkinnedMeshGPU (Phase AW 既有)
  ├─ !ShouldUseGPUSkinning() && skMesh.HasMorph()
  │     → DrawSkinnedMorphMeshCPU (CPU morph + CPU skin)
  └─ !ShouldUseGPUSkinning() && !skMesh.HasMorph()
        → DrawSkinnedMeshCPU (Phase AV/AW 既有)
```

---

## 四、验收标准（精确可测试）

| # | 标准 | 验证方法 |
|---|------|---------|
| **A1** | `LoadSkinnedGLTF` 加载带 morph target 的 glTF 时填充 `mesh.morphTargets` / `mesh.morphTargetCount` 等字段 | smoke 加载已知 glTF（如 AnimatedMorphCube）后调 `mesh:GetMorphTargetCount()` 返回正确值 |
| **A2** | morph target N 上限 = 8（超过截断到前 8 个）| smoke 加载 N=10 的合成 glTF，`mesh:GetMorphTargetCount()` 返回 8 |
| **A3** | `Animator:SetMorphWeight(idx, val)` 设置后 `GetMorphWeight(idx)` 返回 val | smoke 直接 set/get pair |
| **A4** | `SetMorphWeight(name, val)` 通过名称设置 | smoke 用 `mesh:GetMorphTargetName(1)` + set + get 闭环 |
| **A5** | `ClearMorphWeights()` 清除所有手动覆盖 | smoke set + clear + get == 默认 weight |
| **A6** | 越界 idx / 不存在 name → 返回 `nil + err` 不 raise | smoke pcall + 检查 |
| **A7** | 动画通道 weights 路径 sampler 加载成功（components = N）| smoke 加载有 weights animation 的 glTF，检查 `clip:GetSampler(i)` 类型 |
| **A8** | `Animator:Update(dt)` 自动推进 morph weights | smoke set t0 → Update(0.5) → 检查 weight 变化 |
| **A9** | 手动覆盖优先级：手动 > 动画 | smoke set + Update + get == manual value |
| **A10** | `DrawSkinnedMesh` 在有 morph target 的 mesh 上不抛异常 | smoke headless DrawSkinnedMesh return false 但不 raise |
| **A11** | GPU 路径选择正确 backend（GL33Core 走 GPU morph；其他走 CPU）| smoke 检查 `Anim.GetSkinningMode()` 与 `mesh:GetMorphTargetCount() > 0` 组合 |
| **A12** | 6 平台 CI 全绿（Windows / Linux / macOS / Android / iOS / Web）| GitHub Actions |
| **A13** | 既有 smoke 不退化（181 PASS 维持）| `[Light.Animation smoke]` 既有段计数不变 |
| **A14** | 新 sample `samples/demo_morph_target/` 在桌面 GPU 上可视觉演示 morph | 用户手动验证（README 含截图说明）|
| **A15** | 文档同步：`samples/README.md` / `docs/api/Light_Animation.md` / `Light_Graphics.md` 更新 | 文档审查 |

---

## 五、技术约束

### 5.1 必须遵守

- **零既有 API 破坏**：所有 morph 字段都是 `SkinnedMeshAsset` / `Animator` 的扩展；既有方法签名不变
- **Lua 5.1 兼容**：所有新 Lua API 用 `lua_*` 而非 `lua5.3+` 特性
- **lightc -p 友好**：所有 smoke 脚本用 `safe_require` + 早 return 模式
- **6 平台编译**：Web 走 CPU 路径（不实例化 morph shader）
- **MAX_JOINTS = 64 不变**：morph 与 skin UBO 互不影响

### 5.2 推荐遵守

- 复用 Phase AV 的 `EvaluateSampler` 函数（components 改为 N 时也能工作）
- 复用 `MaterialDesc`（不新增 Material 字段）
- shader 全用 `gl_VertexID` + `texelFetch`（GLES 3.0+ 支持，避免 WebGL 1 兼容性）

---

## 六、风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| GPU shader 编译失败（某些 GPU 不支持 ivec2 texelFetch） | 低 | 高 | shader 编译失败时 fallback CPU；编译错误捕获 + 日志 |
| Web Safari morph delta texture 上传失败 | 中 | 中 | Web 走 CPU 路径（与 Phase AW 一致策略）|
| 多 mesh / 多 animator 共享导致 weights 状态污染 | 中 | 中 | 每个 Animator 独立持 `morphWeights` 数组（不共享）|
| morph + skin 的关节矩阵与 morph delta 数学交换性 | 低 | 高 | 严格按 glTF spec：先 morph 后 skin（shader 与 CPU 路径一致）|
| glTF 中 morph target 数量超过 8 | 中 | 中 | 截断到前 8 个 + warning print |

---

## 七、Stage 2 完成判据

- [x] 所有 Q1-Q4 决策在 §2.1 锁定
- [x] 所有自动决策（AD1-AD12）在 §2.2 列出 + 给出依据
- [x] C++ 数据结构扩展完整定义（§3.1）
- [x] Backend 接口契约完整定义（§3.2）
- [x] shader 完整设计（§3.3，含修正 Q3 → texture）
- [x] Lua API 完整定义（§3.4）
- [x] 数据流图（§3.5）
- [x] 15 个验收标准 A1-A15（§四）
- [x] 技术约束 + 风险缓解（§五 + §六）

下一步：写 `DESIGN_PhaseAX.md`（系统架构图 + 模块依赖 + 接口实现细节）。
