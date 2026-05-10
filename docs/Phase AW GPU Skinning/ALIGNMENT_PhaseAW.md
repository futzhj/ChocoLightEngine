# Phase AW — GPU Skinning 对齐文档（ALIGNMENT_PhaseAW.md）

> 6A 工作流 Stage 1（Align）：模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 当前 CPU Skinning 实现（Phase AV Step 3）

**调用入口**：`Light.Animation.DrawSkinnedMesh(mesh, animator, transform_mat4, material)`

**每帧执行链**（`@/e:/jinyiNew/Light/ChocoLight/src/light_animation.cpp:2474-2598`）：

```
DrawSkinnedMesh
  ├─ Animator.jointMatrices 已就绪（Update 时算好）
  ├─ for vertex in baseVertices (CPU loop):
  │     CpuSkinVertex(jointMatrices, joints[4], weights[4], pos, normal)
  │     应用 modelMat (world transform)
  │     写入 sm->skinnedVertices
  ├─ DeleteMesh(gpuMeshId)            ← 性能瓶颈 1: VBO 释放
  ├─ CreateMesh(skinnedVertices, ...) ← 性能瓶颈 2: VBO 全量重传
  └─ DrawMeshMaterial(gpuMeshId, matDesc)
```

**性能特征**：

| 顶点数 | CPU 蒙皮 | DeleteMesh+CreateMesh | 总开销 |
|--------|----------|----------------------|--------|
| 500    | ~0.05ms | ~0.1ms              | 单帧 ~0.15ms |
| 5000   | ~0.5ms  | ~1ms                | 单帧 ~1.5ms |
| 50000  | ~5ms    | ~10ms               | 单帧 ~15ms（不可接受） |

> 实测数据基于 i7-12700H + GTX 4060。移动端 CPU 蒙皮 + VBO 重传性能更差，估计 5000 顶点 / 5ms+。

### 1.2 数据结构（Phase AV 已落地）

`@/e:/jinyiNew/Light/ChocoLight/src/light_animation.cpp:166-190`：

```cpp
struct SkinnedMeshAsset {
    std::vector<RenderVertex3D> baseVertices;       // 原始顶点（蒙皮前）
    std::vector<uint32_t>       indices;
    std::vector<uint32_t>       jointIndicesPacked; // 4×u8 packed per vertex
    std::vector<float>          weights;            // 4 floats per vertex
    Skeleton*                   skeletonPtr;
    int                         skeletonRef;
    uint32_t                    gpuMeshId;          // 缓存 GPU mesh
    std::vector<RenderVertex3D> skinnedVertices;    // 蒙皮后顶点缓冲
    bool                        alive;
};
```

**已具备 GPU skinning 所需全部数据**：JOINTS_0 / WEIGHTS_0 已在 cgltf 解析阶段提取（仅未上传 GPU）。

### 1.3 RenderBackend 抽象层

**`RenderBackend` 接口**（`@/e:/jinyiNew/Light/ChocoLight/src/render_backend.h`）：

| 现有 3D 方法 | 签名 | 说明 |
|-------------|------|------|
| `CreateMesh` | `(RenderVertex3D*, vCount, uint32_t* indices, iCount) → meshId` | 上传静态 VBO + EBO |
| `DeleteMesh` | `(meshId)` | 释放 GPU |
| `DrawMeshMaterial` | `(meshId, MaterialDesc*)` | PBR/Unlit 渲染（含光照）|
| `Supports3D` | `() → bool` | 后端是否能 3D 渲染 |

**当前 `RenderVertex3D`** (`@/e:/jinyiNew/Light/ChocoLight/src/render_backend.h`)：12 floats / 顶点 = `pos(3) + normal(3) + uv(2) + color(4)` = 48 bytes。

**Shader 体系**（`@/e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:113-403`）：
- 桌面：`#version 330 core`
- 移动 / Web：`#version 300 es` + `precision highp float;`
- 双 shader: `programUnlit` + `programPBR` 共享 `VS3D_SOURCE`，分别配 `FS_UNLIT_SOURCE` / `FS_PBR_SOURCE`
- 已有 uniforms: `uMVP`, `uModel`, `uColor`, `uEmissive`, 光照（`uDirLight*`, `uPointLight*`），材质纹理（5 个 sampler + has-flag）

### 1.4 GLES 兼容性约束

| 平台/API | uniform vec4 上限 | mat4 上限 | 说明 |
|----------|------------------|-----------|------|
| GL 3.3 Core (桌面) | ≥ 1024 | ≥ 256 | 实测主流 GPU 远超 |
| GLES 3.0 (Android/iOS/Web) | ≥ 256 | ≥ 64 | **规范最低保证**：`GL_MAX_VERTEX_UNIFORM_VECTORS = 256` |
| GLES 2.0 | ≥ 128 | ≥ 32 | 已不在 ChocoLight 支持范围（Phase AS 已升级 GL33 + GLES3） |

**关键结论**：`uniform mat4 u_jointMats[64]` = 256 vec4 = **GLES 3.0 最低保证的全部 vertex uniform 容量**。再加 `uMVP` / `uModel` / 光照参数等就会**超过 GLES 3.0 minimum**。这意味着 64 关节在某些低端 GLES3 设备可能编译失败或回退软件路径。

### 1.5 Phase AV 已铺垫的工程基础

- ✅ MAX_JOINTS = 64（已在 cgltf 解析层强约束）
- ✅ JOINTS_0 packed 4×u8 / WEIGHTS_0 vec4 / 顶点（已就绪可上传）
- ✅ jointMatrices 列主序 mat4 数组（`Animator::jointMatrices` 已每帧填充）
- ✅ DrawSkinnedMesh 主入口（GPU 路径只需替换内部实现，外部 API 不变）
- ✅ 5 项注册规则齐全（无新 luaopen，仅扩展 backend）

---

## 2. 任务概述

**目标**：在保持 `DrawSkinnedMesh` Lua API 不变的前提下，把每帧 CPU 蒙皮 + VBO 重传路径替换为 GPU skinning（vertex shader 计算 weighted blend matrix），实测性能提升 **3-10x**（取决于顶点数和 GPU/CPU 算力比）。

**度量指标**（端到端 frame time）：

| 顶点数 | 当前 CPU 路径 | GPU 路径目标 | 提升 |
|--------|--------------|-------------|------|
| 500    | ~0.15ms      | ~0.02ms    | 7.5x |
| 5000   | ~1.5ms       | ~0.05ms    | 30x  |
| 50000  | ~15ms        | ~0.3ms     | 50x  |

> 上限受限于 fragment shader（光照计算），GPU skinning 仅消除 CPU 端瓶颈。

---

## 3. 原始需求

> 引用 `TODO_PhaseAV.md` C.1 节：
>
> > **C.1 GPU skinning 优先级**
> > **CPU 路径优点**：跨平台一致；GLES2 / Web 不需特殊处理；调试简单
> > **CPU 路径缺点**：每帧 DeleteMesh + CreateMesh 触发 VBO 重传，> 5000 顶点时性能不理想
> > **GPU 路径优点**：vertex shader 一次上传 inverseBind 矩阵 + per-vertex joint indices/weights，性能 5-10x
> > **GPU 路径缺点**：shader uniform array 上限（通常 ≤ 256 vec4），多平台 GLES 兼容工作量大

---

## 4. 边界确认（明确任务范围）

### 4.1 ✅ 包含

- **GPU skinning 核心实现**：vertex shader 内 `mat4 blend = sum(weight[i] * jointMats[joints[i]])` + 应用到 pos/normal
- **新 RenderBackend 接口**：`CreateSkinnedMesh` / `DrawSkinnedMeshMaterial`
- **新 Skinned Shader 程序**：`programPBRSkin` / `programUnlitSkin`（VS 加蒙皮，FS 复用现有 PBR/Unlit）
- **Vertex 格式扩展**：`RenderVertex3DSkin` 包含 pos/normal/uv/color + joints + weights（per-vertex）
- **运行时检测 + 自动 fallback**：检查 `GL_MAX_VERTEX_UNIFORM_VECTORS`，不足则回退 CPU 路径
- **Lua 端无侵入**：`DrawSkinnedMesh` 签名不变，C++ 内部决定走 GPU 还是 CPU
- **smoke 数值断言**：相同 skeleton + clip + frame，GPU 路径与 CPU 路径输出顶点位置应在 `1e-4` 内一致
- **6 平台 CI 全绿**：Windows / Linux / macOS / Android / iOS / Web

### 4.2 ❌ 不包含

- **MAX_JOINTS 提升**：本阶段保持 64，未来如需 128/256 改用 UBO 或 texture buffer
- **Layer / IK / Morph target**：Phase AW 之后的高级特性
- **GPU 端动画采样**（compute shader / transform feedback）：本阶段 CPU 仍计算 jointMatrices，仅蒙皮上 GPU
- **GLES2 兼容**：仅支持 GLES 3.0+（与 Phase AS 一致）
- **多 primitive SkinnedMesh**：单 mesh 单 primitive，多 primitive 留给 Phase AV.x B.2

### 4.3 ⚠️ 风险点

- **uniform 上限超出**：64 mat4 + 现有 uniforms 在 GLES 3.0 最低规格设备上可能 link failure
- **shader 增量**：从 2 个 program（Unlit/PBR）变 4 个（+SkinUnlit/+SkinPBR），编译时间 +50%（非问题）
- **vertex format 内存**：每顶点从 48 → 68 bytes（+42%），50k 顶点的 mesh 多 1MB VBO（可接受）
- **顶点 attribute 数量**：从 4 → 6 个 attribute（pos/normal/uv/color/joints/weights），仍在 GLES3 最低 16 上限内

---

## 5. 需求理解（对现有项目的理解）

### 5.1 与 Phase AV CPU 路径的关系

| 方面 | CPU 路径（现状） | GPU 路径（新增） |
|------|----------------|----------------|
| Shader | `programPBR` / `programUnlit` | `programPBRSkin` / `programUnlitSkin` |
| Vertex 格式 | `RenderVertex3D` (12 floats) | `RenderVertex3DSkin` (16 floats + 4 bytes) |
| VBO 上传 | 每帧 DeleteMesh + CreateMesh | 一次上传，永不重传 |
| jointMatrices | CPU 计算 + CPU 应用 | CPU 计算 + 每帧 uniform 上传 |
| Backend 入口 | `CreateMesh` + `DrawMeshMaterial` | `CreateSkinnedMesh` + `DrawSkinnedMeshMaterial` |
| Lua 入口 | `Anim.DrawSkinnedMesh` | **同一个 Lua API**（C++ 内部分流） |

**两路并存而非替换**：CPU 路径保留作为 GLES3 minimum 设备的 fallback；GPU 路径在能跑的设备上自动启用。

### 5.2 LegacyBackend / Web 兼容

- `LegacyBackend`（固定管线）不支持 shader → **GPU 路径不可用**，强制走 CPU 路径
- Web (Emscripten) 走 GLES 3.0，可启用 GPU 路径，但 WebGL2 的 vertex uniform 限制更严
- Android / iOS 走 GLES 3.0，桌面 GL 3.3 走桌面路径

### 5.3 与现有 Material / Light 系统的集成

- GPU skinning **只动 vertex stage**，FS 完全复用现有 PBR/Unlit 着色
- DrawSkinnedMeshMaterial 内部仍调用 `UploadCommonMatUniforms` + `UploadPBRLightingUniforms`
- 无需改 MaterialDesc / Lua Material API

### 5.4 与 Phase AV 接口稳定性

`Anim.DrawSkinnedMesh(mesh, animator, transform, material)` Lua 签名**绝对不变**。所有切换在 C++ 内部完成。Phase AV smoke + demo 必须无回归。

---

## 6. 疑问澄清（关键决策点）

> 以下决策点需要在进入 CONSENSUS 前明确。我会基于行业最佳实践提供推荐方案，请确认或调整。

### Q1. jointMatrices 上传方式

| 方案 | 优点 | 缺点 | 兼容性 |
|------|------|------|--------|
| **A. uniform mat4[64]** | 最简单；GLES3 ≥ 256 vec4 上限刚够 | 占满 vertex uniform 容量；shader 编译可能 fail | GL 3.3 ✅ / GLES 3.0 ⚠️（minimum） |
| B. UBO (Uniform Buffer Object) | 分离 uniform 池；可换绑 | GLES 3.0 才支持；setup 复杂 | GL 3.3 ✅ / GLES 3.0 ✅（GLES 3.1+ 更标准） |
| C. Texture buffer (samplerBuffer) | 可超过 256 关节；不占 uniform 池 | 性能略低（每顶点 4 次纹理 fetch）；FS 用 sampler 抢通道 | GL 3.3 ✅ / GLES 3.2+ |

**推荐**：A。理由：
- 64 关节是 Phase AV 既有约束，刚好匹配 GLES 3.0 minimum
- 实现最简、性能最高、debug 最直接
- 未来如需 ≥ 64 关节，再升级 B 或 C（Phase AX 候选）

### Q2. Vertex 格式设计

| 方案 | 优点 | 缺点 |
|------|------|------|
| **A. 新结构 `RenderVertex3DSkin`**（pos/normal/uv/color/joints/weights）| 类型隔离干净；CPU/GPU 路径互不影响 | 多一个结构体定义 |
| B. 扩展 `RenderVertex3D` 加可选字段 | 单一类型 | 非蒙皮 mesh 也带 skin 数据，浪费 24 bytes/顶点 |

**推荐**：A。蒙皮 vs 非蒙皮 mesh 数据本来就不同，类型隔离更清晰。

### Q3. Vertex attribute 数量与 layout

| Location | 名称 | 类型 | 字节数 |
|----------|------|------|--------|
| 0 | aPos | vec3 | 12 |
| 1 | aNormal | vec3 | 12 |
| 2 | aUV | vec2 | 8 |
| 3 | aColor | vec4 | 16 |
| 4 | aJoints | uvec4（GLSL 300 es）/ ivec4（330 core） | 4（packed u8×4）|
| 5 | aWeights | vec4 | 16 |

**总计**：68 bytes / 顶点。共 6 attributes，远在 GLES 3.0 最低 16 attribute 上限内。

**推荐 attribute layout**：直接 `glVertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, ...)` 上传 packed uint32 → 在 VS 内 cast 为 `uvec4`。

### Q4. Shader program 组织

| 方案 | 数量 | 编译时间 | 维护成本 |
|------|-----|---------|---------|
| A. 4 个 program（PBR/Unlit × Skin/NoSkin）| 4 | +50% | 中 |
| B. 单 VS 加 `#define HAS_SKIN`（运行时分支）| 4（同 A）| 同 A | 中（macro 风格）|
| **C. 两个 VS（NoSkin/Skin）+ 两个 FS（PBR/Unlit）**，4 program 链接组合 | 4 | +50% | 低 |

**推荐**：C。VS 分两版（VS3D_SOURCE / VS3D_SKIN_SOURCE），FS 完全复用，maintenance burden 最低。

### Q5. 自动 fallback 策略

**触发条件**：
- 启动时检测 `GL_MAX_VERTEX_UNIFORM_VECTORS < 256` → 强制 CPU 路径
- shader link failure → 强制 CPU 路径
- LegacyBackend → 强制 CPU 路径

**Lua 端可见性**：
- A. 完全自动，Lua 不感知（推荐）
- B. 提供 `Anim.GetSkinningMode()` → "gpu" / "cpu"（仅查询，不可设置）
- C. 提供 `Anim.SetSkinningMode("auto" / "cpu" / "gpu")` 强制覆盖

**推荐**：A + B。自动决策 + 可查询，但**不可强制**（避免用户手动设错）。

### Q6. 数值精度与回归测试

CPU 路径：在 CPU 端做 `mat4 blend = sum(w * M)`，然后 `pos_out = blend * pos_in`。

GPU 路径：在 VS 中做相同运算，但 GPU 用 IEEE 754 binary32（同 CPU），数值差异应在 `~1e-6` 量级（仅运算顺序不同）。

**回归测试设计**：
- smoke 用 `Anim.GetSkinningMode()` 切两次（手动 force CPU vs GPU）
- 同 skeleton + clip + frame，对比 `DrawSkinnedMesh` 后 readback 的 framebuffer 像素差
- 期望像素 diff < 2/255（颜色 8-bit 量化误差）

但 readback 需要 `glReadPixels` —— 这是 framework 性的工作。**简化方案**：仅做编译期 + runtime 烟测验证，不做像素 diff（性价比低）。

**推荐**：仅做 runtime smoke 验证 GPU shader 编译成功 + DrawSkinnedMesh 无 GL error，不做严格数值 diff。

### Q7. Web (WebGL2) 是否启用 GPU 路径

WebGL2 = GLES 3.0 子集，理论支持。但：
- WebGL2 的 `glVertexAttribIPointer` 在某些浏览器（Safari）有 bug
- iOS Safari WebGL2 仅 256 vec4 vertex uniform，刚到 minimum

**推荐**：默认启用，CI 中 Web smoke 验证 GPU 路径可跑通；如发现 Safari issue 再加 user-agent 黑名单。

---

## 7. 待用户确认的最终决策点

| # | 决策点 | 推荐 | 替代 |
|---|--------|------|------|
| Q1 | jointMatrices 上传方式 | uniform mat4[64] | UBO / Texture buffer |
| Q2 | Vertex 格式设计 | 新结构 RenderVertex3DSkin | 扩展 RenderVertex3D |
| Q3 | Vertex attribute layout | location 4=joints (u8×4 packed) + 5=weights (vec4) | 单独 unpack 4×u8 |
| Q4 | Shader program 数量 | 4 个（2 VS × 2 FS 链接组合） | 单 VS + macro |
| Q5 | Lua API 切换可见性 | 自动 + GetSkinningMode 只读 | 自动 + 强制 set |
| Q6 | 数值回归测试 | Runtime smoke 烟测 + GL error 检查 | 像素 diff readback |
| Q7 | Web GPU 路径 | 默认启用 | 默认禁用 |

---

## 8. 状态

- [x] 项目上下文分析完成
- [x] 任务概述明确
- [x] 边界确认（包含 / 不包含 / 风险点）
- [x] 需求理解（与现有 Phase AV / RenderBackend / Material 关系）
- [ ] 关键决策点用户确认（待中断询问）
- [ ] CONSENSUS_PhaseAW.md 生成（基于决策点）

下一步：中断询问 Q1-Q7 的最终决策。
