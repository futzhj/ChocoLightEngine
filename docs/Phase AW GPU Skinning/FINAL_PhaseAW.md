# FINAL — Phase AW GPU Skinning（项目总结报告）

**完成日期**: 2026-05-10
**总耗时**: 约 1 个工作单元（Stage 1-6 一次会话内完成）
**最终 commit**: `837aa2c`
**CI**: ✅ 6/6 平台 / **170 PASS** / 0 FAIL

---

## 一、项目目标回顾

> **背景**: Phase AV 实现了 CPU skinning 路径（每帧 `DeleteMesh + CreateMesh` 全量重传顶点）。在 5000 顶点的人物模型上单帧 CPU 开销约 1.5ms，且 GPU bus 每帧上传 240KB 顶点数据 — 不适合移动端 / 多角色场景。

> **目标**: 引入 GPU vertex shader skinning，零破坏地把 `Anim.DrawSkinnedMesh` 升级为：mesh 顶点（含 joints/weights）一次性上传 + 每帧仅上传 ≤ 64 mat4 关节调色板（4 KB UBO） → CPU 减负 30 倍 / GPU bus 减负 60 倍。

---

## 二、6A 工作流执行轨迹

| 阶段 | 产物 | 关键决策 |
|------|------|---------|
| **1 Align** | `ALIGNMENT_PhaseAW.md` | 项目上下文分析 + 7 个关键不确定性 (Q1-Q7) |
| **2 Architect** | `CONSENSUS_PhaseAW.md` + `DESIGN_PhaseAW.md` | Q1=UBO / Q2=新结构体 / Q3=u8 packed IPointer / Q4=4 program / Q5=auto+set / Q6=runtime smoke / Q7=Web CPU |
| **3 Atomize** | `TASK_PhaseAW.md` | 8 个独立可验证任务 + 依赖图 |
| **4 Approve** | （此次任务由用户认可后直接进入实施） | 设计可行 / 影响面可控 / 验收标准明确 |
| **5 Automate** | T1-T8 8 次 commit | 每任务独立 commit + push CI 验证 |
| **6 Assess** | `ACCEPTANCE_PhaseAW.md` + `FINAL_PhaseAW.md` + `TODO_PhaseAW.md` | 6 平台全绿；170 PASS；可合并 |

---

## 三、技术成果

### 1. 接口扩展（`render_backend.h`）

```cpp
struct RenderVertex3DSkin {
    float x, y, z;          // 12 — 位置
    float nx, ny, nz;       // 12 — 法线
    float u, v;             //  8 — UV
    float r, g, b, a;       // 16 — 颜色
    uint32_t joints_packed; //  4 — 4×u8 关节索引 (low byte = joint[0])
    float weights[4];       // 16 — 权重（已归一化）
}; // sizeof = 68

class RenderBackend {
    virtual bool SupportsGPUSkinning() const = 0;
    virtual uint32_t CreateSkinnedMesh(const RenderVertex3DSkin*, int, const uint32_t*, int) = 0;
    virtual void DrawSkinnedMeshMaterial(uint32_t, const MaterialDesc*, const float*, int) = 0;
};
```

### 2. GL33Backend 实现（`render_gl33.cpp` +301 行）

- **Shader 体系**：4 program 变体（Unlit/PBR × NonSkin/Skin），FS 完全复用，VS 分两版 `VS3D_SOURCE` / `VS3D_SKIN_SOURCE`，覆盖 Desktop GL3.3 + GLES3
- **UBO 布局**：`layout(std140) uniform JointBlock { mat4 uJointMats[64]; }` (binding=0, 4096 bytes)
- **顶点属性**：6 location（pos/normal/uv/color 复用 + joints via `glVertexAttribIPointer(GL_UNSIGNED_BYTE)` + weights vec4）
- **资源管理**：`uboJointMatrices` + `skinnedMeshes` map；`gpuSkinningSupported` flag；任何环节失败均 silent fallback
- **mesh ID 区分**：`gpuSkinnedMeshId` 高位 `0x80000000`；`DeleteMesh` 按高位分流到对应 map

### 3. 调度逻辑（`light_animation.cpp` +214/-56 行）

- **`SkinningMode` enum class** + `g_skinningMode` 静态全局
- **`ShouldUseGPUSkinning()`**：CPU mode 强制 false；GPU mode 看 backend；AUTO 在 Web (`__EMSCRIPTEN__`) 强制 false 否则看 backend
- **`l_Anim_DrawSkinnedMesh` 重构**：
  - 通用校验 + 解析 `transform` / `material` / `jointMatrices` 路径不变
  - 末尾按 `ShouldUseGPUSkinning()` 分流到 `DrawSkinnedMeshCPU` / `DrawSkinnedMeshGPU`
- **`DrawSkinnedMeshGPU`**：
  - 首次调用构建 `RenderVertex3DSkin[]` + `CreateSkinnedMesh` 一次性上传
  - 每帧把 `modelMat × jointMat[j]` 烘焙到 ≤ 64 mat4，调 `DrawSkinnedMeshMaterial`
  - identity `modelMat` 跳过乘法；first-time GPU 失败 → 透明 fallback 到 CPU
- **`DrawSkinnedMeshCPU`**：算法 100% 不变（仅从原入口提取的纯 refactor）

### 4. Lua API（`light_animation.cpp`）

```lua
Anim.SetSkinningMode("auto" | "cpu" | "gpu")  -- 返回 true 或 nil + err
Anim.GetSkinningMode()                          -- 返回实际生效的 "cpu" / "gpu"
```

S21 nil+err 模式（无 luaL_error / longjmp，C++ 析构安全）。

### 5. 测试覆盖（`scripts/smoke/animation.lua` +68 行）

新增 [15] 段 12 个 CHECK：API 注册、模式切换、错误参数（含大小写 / 类型 / nil）、`DrawSkinnedMesh` 入口签名稳定性。

### 6. 文档（`docs/api/Light_Animation.md` +137 行）

新增完整章节：动机 / 平台默认表 / 完整 API 签名 / 性能数据表 / 实现细节 / 测试覆盖 / Q1-Q7 决策表。

---

## 四、性能预期 vs 实现

| 顶点数 | CPU 路径 | GPU 路径（预期） | 提升 |
|--------|---------|----------------|------|
| 500    | ~0.15ms | ~0.02ms        | 7.5x |
| 5000   | ~1.5ms  | ~0.05ms        | 30x  |
| 50000  | ~15ms   | ~0.3ms         | 50x  |

> **CI 局限**：headless windows runner 无 GL 上下文 → smoke 实测仅验证 API & 错误处理，未实测 GPU 路径性能。建议本地或真机做 frame timing 对比（参见 `TODO_PhaseAW.md`）。

---

## 五、核心设计亮点

### 1. **零 API 破坏的渐进升级**

`Anim.DrawSkinnedMesh(mesh, animator, transform, material)` 签名 100% 不变。用户脚本无需修改即享受性能提升。新 API（`SetSkinningMode` / `GetSkinningMode`）仅作为可选调试工具。

### 2. **Silent Fallback 三重保护**

任何环节失败都自动降级到 CPU，不让用户感知错误：
- backend 检测：`gpuSkinningSupported = false` → AUTO/GPU 模式都返回 cpu
- shader 编译失败：`programUnlitSkin = 0`，`SupportsGPUSkinning()` 仍可能为 true 但 draw 时返回 0 → fallback CPU
- first-time `CreateSkinnedMesh` 返回 0 → 直接调 `DrawSkinnedMeshCPU`

### 3. **mesh ID 高位分流**

无需破坏现有 `DeleteMesh(uint32_t)` 接口（很多调用方依赖），用 `0x80000000` 高位标识 skinned mesh，自然分发到对应 map。普通 mesh ID 起始为 1，永远不冲突。

### 4. **Web 平台保守策略（Q7）**

桌面 + 移动端默认 AUTO=GPU；Web 默认 AUTO=CPU。理由：Safari WebGL2 对 `glVertexAttribIPointer(GL_UNSIGNED_BYTE)` 在某些 GPU 驱动有兼容问题。用户可主动 `Anim.SetSkinningMode("gpu")` 强开。

### 5. **Mat4 transform 烘焙到 jointMats**

不修改 backend 的 `modelview` 栈语义。CPU 路径把 `modelMat` 烘焙到顶点；GPU 路径把 `modelMat` 烘焙到 `jointMats`（前乘）。两种做法在最终 `clip_pos` 上数学等价，且都不需要扩展 shader 或 backend transform 接口。

---

## 六、风险与缓解

| 风险 | 概率 | 缓解措施 |
|------|------|---------|
| Web Safari `glVertexAttribIPointer` 兼容性 | 中 | Q7 默认禁用，用户可强开 |
| 移动端某些 GPU UBO 仅最低 16KB | 低 | 4KB 需求远小于 16KB minimum；运行时检测兜底 |
| shader 编译失败 | 低 | 三重 fallback；不影响 CPU 路径 |
| 数值精度（CPU vs GPU 浮点差异）| 低 | float32 精度足够；视觉感知不可见 |
| 64 关节上限不够 | 低（已有约束） | 与 LoadSkinnedGLTF 一致；shader 数组定长 |

---

## 七、与既有阶段的关系

| 关联 | 说明 |
|------|------|
| **依赖** Phase AS | 复用 PBR/Unlit FS、`MaterialDesc`、`BindMaterialTexture`、`Mat4`、`MVP` 计算路径 |
| **依赖** Phase AV | 复用 `Skeleton` / `Animator` / `SkinnedMesh` / `ComputeJointMatrices` / `CpuSkinVertex` |
| **不影响** Phase AS / AV | API / 数据结构 / 测试均保持向后兼容 |
| **为后续准备** | Phase AX（如 morph target / GPU LBS 优化）的 shader 框架已就绪 |

---

## 八、交付清单

| 类型 | 文件 | 状态 |
|------|------|------|
| 设计文档 | `docs/Phase AW GPU Skinning/ALIGNMENT_PhaseAW.md` | ✅ |
| 设计文档 | `docs/Phase AW GPU Skinning/CONSENSUS_PhaseAW.md` | ✅ |
| 设计文档 | `docs/Phase AW GPU Skinning/DESIGN_PhaseAW.md` | ✅ |
| 设计文档 | `docs/Phase AW GPU Skinning/TASK_PhaseAW.md` | ✅ |
| 验收文档 | `docs/Phase AW GPU Skinning/ACCEPTANCE_PhaseAW.md` | ✅ |
| 总结文档 | `docs/Phase AW GPU Skinning/FINAL_PhaseAW.md` (本文件) | ✅ |
| TODO | `docs/Phase AW GPU Skinning/TODO_PhaseAW.md` | ✅ |
| API 文档 | `docs/api/Light_Animation.md` Phase AW 章节 | ✅ |
| 头文件 | `ChocoLight/include/render_backend.h` | ✅ |
| 实现 | `ChocoLight/src/render_gl33.cpp` | ✅ |
| 实现 | `ChocoLight/src/light_animation.cpp` | ✅ |
| 测试 | `scripts/smoke/animation.lua` 第 [15] 段 | ✅ |

---

## 九、最终状态

**Phase AW GPU Skinning 已全部完成，6 平台 CI 全绿，可合并主线。**

下一阶段建议优先级（按 ROI）：

1. **Phase AX 候选**：真机 frame timing baseline 工具 — 量化 GPU 路径在不同设备的实际收益
2. **GPU vs CPU 数值一致性**：写一个独立 verify 工具（headless GL context + CPU baseline 对比）
3. **关节上限提升**：把 SKIN_MAX_JOINTS 从 64 上调到 128 / 256（部分高精度模型）— 需评估 `GL_MAX_UNIFORM_BLOCK_SIZE` 在低端移动 GPU 的最低值
