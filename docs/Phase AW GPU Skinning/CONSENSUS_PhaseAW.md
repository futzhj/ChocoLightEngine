# Phase AW — GPU Skinning 共识文档（CONSENSUS_PhaseAW.md）

> 6A 工作流 Stage 1（Align）输出：基于 `ALIGNMENT_PhaseAW.md` 的 Q1-Q7 决策点，用户已确认最终方案。

---

## 1. 明确的需求描述

### 1.1 核心目标

在保持 `Light.Animation.DrawSkinnedMesh(mesh, animator, transform, material)` Lua API 100% 不变的前提下，把每帧 CPU 蒙皮 + VBO 重传路径替换为 **GPU vertex shader skinning**：

- **Vertex stage**：每顶点用 `mat4 blend = sum(weight[i] * jointMats[joints[i]])` 计算 weighted blend matrix，应用到 pos/normal
- **Joint matrices 上传**：每帧通过 **UBO (Uniform Buffer Object)** 上传 64×mat4 调色板（共 4096 bytes）
- **VBO 上传**：mesh 加载时**一次性上传**（含 JOINTS_0/WEIGHTS_0 attributes），后续永不重传

### 1.2 性能目标

| 顶点数 | 当前 CPU 路径 | GPU 路径目标 | 提升 |
|--------|--------------|-------------|------|
| 500    | ~0.15ms      | ~0.02ms    | 7.5x |
| 5000   | ~1.5ms       | ~0.05ms    | 30x  |
| 50000  | ~15ms        | ~0.3ms     | 50x  |

> 实测于 i7-12700H + GTX 4060 桌面 GL 3.3，移动端按 GPU/CPU 算力比折算。

### 1.3 验收标准

| 验收项 | 标准 |
|--------|------|
| Lua API 不变 | `Anim.DrawSkinnedMesh(mesh, animator, transform, material)` 签名完全保留 |
| 6 平台 CI 编译通过 | win / lin / mac / and / ios / web 全绿 |
| Phase AV 现有 smoke 无回归 | `[Phase AV Step 1+2+3+4 + Phase AV.x]` 仍 157/0 |
| GPU/CPU 路径自动选择 | 启动时检测 GLES 上限 + LegacyBackend 回退 + Web 默认 CPU |
| 用户可强制覆盖 | `Anim.SetSkinningMode("auto"\|"cpu"\|"gpu")` |
| 用户可查询当前模式 | `Anim.GetSkinningMode() → "cpu"\|"gpu"` |
| 数值一致性（runtime smoke）| GPU 路径不报 GL error；DrawSkinnedMesh 调用成功；jointMatrices 上传无 buffer overflow |

---

## 2. 技术实现方案

### 2.1 决策点最终方案（参考 `ALIGNMENT_PhaseAW.md` Q1-Q7）

| # | 决策 | **最终方案** | 备注 |
|---|------|------------|------|
| Q1 | jointMatrices 上传方式 | **UBO（Uniform Buffer Object）** | 比 uniform array 更标准，预留 ≥ 64 关节扩展，需 GLES 3.0+ |
| Q2 | Vertex 格式 | **新结构 `RenderVertex3DSkin`** | 蒙皮/非蒙皮类型隔离 |
| Q3 | Attribute layout | **location 4 = joints (u8×4 packed) + 5 = weights (vec4)** | 6 attributes 在 GLES3 16 上限内 |
| Q4 | Shader program 数量 | **4 个**：programUnlit / programPBR / programUnlitSkin / programPBRSkin | VS 分两版（VS3D / VS3D_SKIN），FS 完全复用 |
| Q5 | Lua 切换可见性 | **自动 + `GetSkinningMode()` 只读 + `SetSkinningMode()` 可强制** | 默认 auto；用户可设 cpu/gpu/auto |
| Q6 | 回归测试 | **Runtime smoke + GL error 检查** | 不做像素 diff readback |
| Q7 | Web GPU 路径 | **默认禁用**（强制 CPU） | Safari WebGL2 attrib int pointer 风险；用户可 SetSkinningMode("gpu") 强开 |

### 2.2 数据结构

**新顶点格式**（`@/e:/jinyiNew/Light/ChocoLight/src/render_backend.h` 新增）：

```cpp
struct RenderVertex3DSkin {
    float    x, y, z;            // POSITION (vec3)
    float    nx, ny, nz;          // NORMAL (vec3)
    float    u, v;                // UV (vec2)
    float    r, g, b, a;          // COLOR (vec4)
    uint32_t joints_packed;       // JOINTS_0 (4 × uint8 packed, little-endian)
    float    weights[4];          // WEIGHTS_0 (vec4)
};
// sizeof = 48 + 4 + 16 = 68 bytes / vertex
```

**RenderBackend 接口扩展**（`@/e:/jinyiNew/Light/ChocoLight/src/render_backend.h`）：

```cpp
class RenderBackend {
public:
    // ... 现有 3D 方法 ...

    // ---- Phase AW: GPU Skinning ----
    virtual bool     SupportsGPUSkinning() const { return false; }
    virtual uint32_t CreateSkinnedMesh(const RenderVertex3DSkin* verts, int vCount,
                                        const uint32_t* indices, int iCount) { return 0; }
    virtual void     DrawSkinnedMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                              const float* jointMatrices, int jointCount) {}
    // DeleteMesh 复用现有接口（GPU mesh 资源释放统一）
};
```

**LegacyBackend 不重写** → 默认实现返回 `false` / `0`，自动 fallback 到 CPU 路径。

### 2.3 GL33Backend GPU Skinning 实现

#### 2.3.1 新增成员

```cpp
class GL33Backend : public RenderBackend {
    // ... 现有成员 ...

    // ---- Phase AW: GPU Skinning ----
    GLuint  programUnlitSkin = 0;
    GLuint  programPBRSkin   = 0;
    GLuint  uboJointMatrices = 0;     // UBO 句柄（每帧 glBufferSubData 更新）
    GLint   uniformBlockBindingPoint = 0;  // UBO binding point（固定 0）
    bool    gpuSkinningSupported = false;  // 启动时检测后设置

    // SkinnedMesh 资源池（与 meshes 分开，不同 vertex 格式）
    std::unordered_map<uint32_t, MeshGPU> skinnedMeshes;
    uint32_t                              nextSkinnedMeshId = 0x80000001;  // 高位区分
};
```

> `nextSkinnedMeshId` 起始值 `0x80000001` 让 skinned mesh ID 与普通 mesh ID（从 1 开始）天然区分；`DeleteMesh` 内部按 ID 高位决定查哪个 map。

#### 2.3.2 启动检测

```cpp
bool Init() override {
    // ... 现有 shader 编译 ...

    // GPU Skinning 支持检测
    GLint maxUniformBlockSize = 0, maxVertexUniformBlocks = 0;
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUniformBlockSize);
    glGetIntegerv(GL_MAX_VERTEX_UNIFORM_BLOCKS, &maxVertexUniformBlocks);
    gpuSkinningSupported = (maxUniformBlockSize >= 4096) && (maxVertexUniformBlocks >= 1);

    if (gpuSkinningSupported) {
        // 编译 Skin shader
        GLuint vsSkin   = CompileShader(GL_VERTEX_SHADER, VS3D_SKIN_SOURCE);
        if (vsSkin && fsUnlit) programUnlitSkin = LinkProgram(vsSkin, fsUnlit);
        if (vsSkin && fsPBR)   programPBRSkin   = LinkProgram(vsSkin, fsPBR);
        if (vsSkin) glDeleteShader(vsSkin);
        // 创建 UBO
        glGenBuffers(1, &uboJointMatrices);
        glBindBuffer(GL_UNIFORM_BUFFER, uboJointMatrices);
        glBufferData(GL_UNIFORM_BUFFER, 64 * 16 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_UNIFORM_BUFFER, 0);
        glBindBufferBase(GL_UNIFORM_BUFFER, uniformBlockBindingPoint, uboJointMatrices);
        // 绑定 UBO block index
        if (programUnlitSkin) {
            GLuint blockIdx = glGetUniformBlockIndex(programUnlitSkin, "JointBlock");
            if (blockIdx != GL_INVALID_INDEX) glUniformBlockBinding(programUnlitSkin, blockIdx, uniformBlockBindingPoint);
        }
        if (programPBRSkin) { /* 同上 */ }
    }
    return true;
}

bool SupportsGPUSkinning() const override { return gpuSkinningSupported; }
```

#### 2.3.3 Vertex Shader (VS3D_SKIN_SOURCE)

```glsl
#version 330 core    // 桌面
// 或 #version 300 es + precision highp float; (移动)

layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aColor;
layout(location=4) in uvec4 aJoints;     // u8×4 packed → glVertexAttribIPointer
layout(location=5) in vec4  aWeights;

uniform mat4 uMVP;
uniform mat4 uModel;

layout(std140) uniform JointBlock {
    mat4 uJointMats[64];                  // 64 × 64 bytes = 4096 bytes
};

out vec3 vNormalW;
out vec3 vWorldPos;
out vec2 vTexCoord;
out vec4 vColor;

void main() {
    // Weighted blend matrix
    mat4 blend = aWeights.x * uJointMats[aJoints.x]
               + aWeights.y * uJointMats[aJoints.y]
               + aWeights.z * uJointMats[aJoints.z]
               + aWeights.w * uJointMats[aJoints.w];

    vec4 skinnedPos    = blend * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(blend) * aNormal;

    vec4 worldPos = uModel * skinnedPos;
    vWorldPos = worldPos.xyz;
    vNormalW  = mat3(uModel) * skinnedNormal;
    vTexCoord = aUV;
    vColor    = aColor;
    gl_Position = uMVP * skinnedPos;
}
```

#### 2.3.4 DrawSkinnedMeshMaterial 实现要点

```cpp
void DrawSkinnedMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                              const float* jointMatrices, int jointCount) override {
    auto it = skinnedMeshes.find(meshId);
    if (it == skinnedMeshes.end()) return;
    GLuint program3D = (desc->mode == 0) ? programUnlitSkin : programPBRSkin;
    if (!program3D) return;

    glUseProgram(program3D);
    // 上传 jointMatrices 到 UBO（每帧更新）
    glBindBuffer(GL_UNIFORM_BUFFER, uboJointMatrices);
    int n = (jointCount > 64) ? 64 : jointCount;
    glBufferSubData(GL_UNIFORM_BUFFER, 0, n * 16 * sizeof(float), jointMatrices);
    glBindBuffer(GL_UNIFORM_BUFFER, 0);

    // 上传 MVP / Model / Material uniforms / 光照（复用现有 helper）
    // ...

    glBindVertexArray(it->second.vao);
    glDrawElements(GL_TRIANGLES, it->second.indexCount, GL_UNSIGNED_INT, (void*)0);
    glBindVertexArray(0);
    glUseProgram(program);     // 恢复 2D shader
    glBindVertexArray(vao);
}
```

### 2.4 light_animation.cpp 调度逻辑

```cpp
// 全局 skinning 模式（CPU/GPU/Auto）
enum class SkinningMode : uint8_t { AUTO = 0, CPU = 1, GPU = 2 };
static SkinningMode g_skinningMode = SkinningMode::AUTO;

static bool ShouldUseGPUSkinning() {
    switch (g_skinningMode) {
        case SkinningMode::CPU: return false;
        case SkinningMode::GPU: return g_render && g_render->SupportsGPUSkinning();
        case SkinningMode::AUTO:
        default:
#if defined(__EMSCRIPTEN__)
            return false;     // Web 默认 CPU（Q7）
#else
            return g_render && g_render->SupportsGPUSkinning();
#endif
    }
}

static int l_Anim_DrawSkinnedMesh(lua_State* L) {
    SkinnedMeshAsset* sm = CheckSkinnedMesh(L, 1);
    Animator* an = CheckAnimator(L, 2);
    // ... 现有参数检查 ...

    if (ShouldUseGPUSkinning()) {
        return DrawSkinnedMeshGPU(L, sm, an, modelMat, matDesc);
    } else {
        return DrawSkinnedMeshCPU(L, sm, an, modelMat, matDesc);    // 现有路径
    }
}
```

新 Lua API：

```cpp
// Anim.SetSkinningMode("auto" | "cpu" | "gpu")  → returns nil + err on bad arg
// Anim.GetSkinningMode()                        → "cpu" | "gpu" (实际生效模式, 不是设置值)
```

> 注意 `GetSkinningMode` 返回**实际生效**而非用户设置值：例如用户设了 "gpu" 但 `SupportsGPUSkinning()=false`，返回 `"cpu"`（auto fallback）。

### 2.5 SkinnedMeshAsset 扩展

```cpp
struct SkinnedMeshAsset {
    // ... 现有字段 ...

    // Phase AW: GPU 路径 mesh 句柄（CPU 路径用 gpuMeshId）
    uint32_t gpuSkinnedMeshId = 0;     // GPU 路径专用，含 JOINTS/WEIGHTS attribs
    bool     gpuMeshUploaded  = false; // 首次 DrawSkinnedMesh 时上传

    // 注：CPU 路径继续用 gpuMeshId（每帧 DeleteMesh+CreateMesh）
    //     GPU 路径用 gpuSkinnedMeshId（一次上传，永不重传）
    //     两路并存，按 ShouldUseGPUSkinning() 选用
};
```

### 2.6 LegacyBackend 行为

不重写 `SupportsGPUSkinning()`（默认 `false`），不重写 `CreateSkinnedMesh` / `DrawSkinnedMeshMaterial`（默认空实现）。

`l_Anim_DrawSkinnedMesh` 内 `ShouldUseGPUSkinning()` 自动返回 `false` → 走 CPU 路径。

---

## 3. 任务边界限制

### 3.1 ✅ 包含

- `RenderVertex3DSkin` 结构定义 + `RenderBackend` 接口扩展
- `GL33Backend` UBO + Skin shader 实现（桌面 GL 3.3 + GLES 3.0）
- `light_animation.cpp` 中 `ShouldUseGPUSkinning` + `DrawSkinnedMeshGPU` 路径
- Lua API：`Anim.SetSkinningMode` / `Anim.GetSkinningMode`
- smoke：runtime 验证 + GL error 检查 + 模式切换断言
- 6 平台 CI 全绿
- API 文档更新（`docs/api/Light_Animation.md`）

### 3.2 ❌ 不包含

- MAX_JOINTS 提升（保持 64，Phase AX 候选）
- Layer / IK / Morph target（Phase AV.x B 项后续）
- GPU 端动画采样（compute shader / transform feedback）
- GLES 2.0 兼容（已在 Phase AS 排除）
- 多 primitive 单 SkinnedMesh（Phase AV.x B.2 后续）
- 像素 diff readback 验证（Q6 决策不做）

### 3.3 ⚠️ 风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| GLES3 设备 UBO 不支持（罕见）| GPU 路径不可用 | `SupportsGPUSkinning()` 返回 false → 自动 fallback CPU |
| Web (Safari WebGL2) attrib int pointer bug | GPU 路径渲染异常 | Q7 默认禁用 Web GPU |
| UBO 与 std140 布局对齐 mismatch | Shader 读到错误数据 | 严格用 `mat4` + `std140` 标准布局，C++ 端一次上传 4096 bytes 连续 float |
| 现有 CPU 路径回归 | Phase AV smoke 失败 | CPU 路径**完全不动**，仅在 `l_Anim_DrawSkinnedMesh` 入口分流 |
| Material 系统集成 | uniform 名冲突 | Skin shader 完全复用现有 PBR/Unlit FS 的 uniforms |

---

## 4. 验收标准（具体可测试）

### 4.1 编译/链接级

- [ ] 6 平台 CI 编译通过
- [ ] `programUnlitSkin` / `programPBRSkin` 在桌面 + GLES3 平台均成功 link
- [ ] UBO 创建成功（`glGetError() == GL_NO_ERROR` after `glBufferData`）
- [ ] LegacyBackend 编译通过（不需重写新接口）

### 4.2 运行时级

- [ ] `Anim.GetSkinningMode()` 在 Win/Lin/Mac 桌面返回 `"gpu"`
- [ ] `Anim.GetSkinningMode()` 在 Web 默认返回 `"cpu"`
- [ ] `Anim.SetSkinningMode("cpu")` 后 `GetSkinningMode()` 返回 `"cpu"`
- [ ] `Anim.SetSkinningMode("gpu")` 在不支持设备上自动 fallback 返回 `"cpu"`
- [ ] `Anim.SetSkinningMode("invalid")` 返回 `nil + err` 字符串
- [ ] GPU 路径下 `DrawSkinnedMesh` 调用无 `glGetError` 报错
- [ ] CPU 路径切到 GPU 路径再切回 CPU，渲染结果应不出现明显几何错误（视觉验证）

### 4.3 回归保护

- [ ] `[Phase AV Step 1+2+3+4 + Phase AV.x] 通过 157 / 失败 0` 仍通过
- [ ] Phase AS / AT / AU smoke 全绿
- [ ] `demo_animation` Lua 脚本无修改即可运行（API 不变）

### 4.4 性能验证（可选）

- [ ] 5000 顶点 SkinnedMesh 在 GPU 路径下 frame time < 0.5ms（桌面 GL 3.3）
- [ ] 50000 顶点 SkinnedMesh 在 GPU 路径下 frame time < 2ms（桌面）

---

## 5. 集成方案

### 5.1 与现有 Phase AV CPU 路径的集成

- **完全并存**：CPU 路径源码（`DrawSkinnedMeshCPU`）零改动
- **入口分流**：`l_Anim_DrawSkinnedMesh` 内一行 `if (ShouldUseGPUSkinning())` 分流
- **mesh 数据共享**：`SkinnedMeshAsset` 同时持有 CPU 路径的 `gpuMeshId` + GPU 路径的 `gpuSkinnedMeshId`

### 5.2 与 RenderBackend 抽象的集成

- 新接口加默认实现（returns false / 0 / no-op），不破坏二进制兼容
- 仅 `GL33Backend` 重写新接口，`LegacyBackend` 不动

### 5.3 与 Material / Light 系统的集成

- Skin shader **完全复用** PBR/Unlit FS（uniforms 不变，texture binding 不变）
- 新增 vertex stage 仅多 6 个 uniform（UBO + 现有 uMVP/uModel 已够）
- `MaterialDesc` 不变

---

## 6. 关键假设确认（用户已选择）

- [x] Q1 = UBO（Uniform Buffer Object）—— 用户选择 "调整 Q1：换 UBO 路径"
- [x] Q2 = 新结构 RenderVertex3DSkin —— 用户接受推荐
- [x] Q3 = location 4=joints / 5=weights —— 用户接受推荐
- [x] Q4 = 4 个 program —— 用户接受推荐
- [x] Q5 = 自动 + GetSkinningMode 只读 + SetSkinningMode 可写 —— 用户选择 "Q5 改为允许强制 set"
- [x] Q6 = Runtime smoke + GL error —— 用户接受推荐
- [x] Q7 = Web 默认禁用 —— 用户选择 "Q7 默认禁用 Web"

---

## 7. 下一步

- **Stage 2 Architect**：生成 `DESIGN_PhaseAW.md`，包含整体架构图（mermaid）、模块依赖图、接口契约定义、数据流向图、异常处理策略
- **Stage 3 Atomize**：生成 `TASK_PhaseAW.md`，拆分为可独立验收的原子任务（vertex format / backend api / shader / Lua API / smoke / docs）
- **Stage 4 Approve**：人工审查 → 用户批准后 Stage 5 Automate 实施

**预估工作量**：

| 阶段 | 工作量 | 依赖 |
|------|-------|------|
| Architect | 1-2h（写 design + mermaid 图）| 当前 |
| Atomize | 1h（拆 6-8 个任务）| Architect |
| Implement（vertex format + backend）| 3-4h | Atomize |
| Implement（shader + GL33 wiring）| 4-5h | vertex format |
| Implement（Lua API + dispatch）| 1-2h | shader |
| Smoke + CI | 2-3h | All |
| Docs | 1h | All |
| **总计** | **~14-17h** | |
