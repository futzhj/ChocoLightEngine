# TODO — Phase AW GPU Skinning（待办与建议）

> Phase AW 主体已完整交付（详见 `ACCEPTANCE_PhaseAW.md`、`FINAL_PhaseAW.md`）。本文件列出**不阻塞当前合并**但建议后续跟进的事项。

---

## 一、强烈建议跟进（P0）

### ✅ 1.1 真机性能 baseline 测量

**已完成 (Phase AW.x)**：见 `samples/demo_skinning_perf/`。启动后自动跑 60 帧 CPU + 60 帧 GPU baseline 并打印对比表（含 speedup ratio）；OSD 实时显示 frame ms。详见 `docs/Phase AW.x/` 与 `samples/demo_skinning_perf/README.md`。

**原文（保留以便回溯）**：

**为什么**：CI 是 headless 环境（无 GL 上下文），实际 windows runner 跑的是 LegacyBackend → CPU 路径。GPU 路径**编译通过 ✓** 但**未在真实 GPU 上端到端运行验证过**。

**建议操作**：
```lua
-- 用户在本地（有 GL3.3 GPU 的桌面机器）跑：
-- 1. 启动 light.exe + 任意 demo_animation 场景
-- 2. 加载 5000+ 顶点骨骼模型
-- 3. 添加 frame timing 输出
local Anim = require 'Light.Animation'

Anim.SetSkinningMode('cpu')
local cpu_avg = run_n_frames(60)   -- 用户实现的 frame timing helper
Anim.SetSkinningMode('gpu')
local gpu_avg = run_n_frames(60)
print(string.format('CPU: %.3fms / frame', cpu_avg))
print(string.format('GPU: %.3fms / frame', gpu_avg))
print(string.format('Speedup: %.1fx', cpu_avg / gpu_avg))
```

**预期**：5000 顶点模型 GPU 路径应至少 5x 提升。如低于 2x，说明 UBO 上传或 mat4 烘焙有性能瓶颈，需 profile。

**所需支持**：用户自行运行；如需要我可以补一个 `Light.Util.FrameTime` API（单独小阶段）。

---

### ✅ 1.2 GPU vs CPU 数值一致性验证

**已完成 (Phase AW.x，方案 B - 视觉对比)**：在 `samples/demo_skinning_perf/` 中按 G/C 键运行时切换 GPU/CPU 模式，肉眼对比同一帧渲染结果。Phase AW.x 提供了切换工具但仍未做像素级数值对比（CONSENSUS Q6 决策维持，工程量过大）。

**原文（保留以便回溯）**：

**为什么**：CONSENSUS Q6 已决策"用 runtime smoke + GL error 替代离线 baseline 数值对比"。但 smoke 只验证了 API 表面契约，未验证**两路径产出的实际像素**等价性。

**风险点**：
- shader 浮点精度（mediump 在 GLES 可能 fp16）→ 与 CPU fp32 可能有可见差异
- 列主序 vs 行主序 mat4 × vec4 实现细节
- transform 烘焙顺序（CPU = `M × Σ(w × J × v)`；GPU = `Σ(w × M × J × v)`）— 数学上等价但浮点累加顺序不同

**建议操作**（任一即可）：
- **A. 单元测试**：headless GL context + 比较 framebuffer pixel diff（工程量较大）
- **B. 视觉对比**：用 demo_animation 切换两种 mode，肉眼对比同一帧渲染
- **C. 数值对比**：在 GL 路径里增加可选 readback（`glReadPixels`），对比第一帧 fragment 颜色 (debug-only)

**优先 B**：性价比最高，5 分钟内可完成。

---

## 二、建议优化（P1）

### 2.1 关节上限可配置

**当前**：`SKIN_MAX_JOINTS = 64` 硬编码，与 `LoadSkinnedGLTF` 上限一致。

**问题**：部分高精度角色模型超过 64 关节（典型如手指 + 表情 blend）。

**方案选项**：
- **方案 A（推荐）**：编译期常量提升到 128，UBO 增到 8192 bytes。GLES 3.0 minimum `GL_MAX_UNIFORM_BLOCK_SIZE = 16KB`，绝对安全
- **方案 B**：运行时检测，按 `GL_MAX_UNIFORM_BLOCK_SIZE` 动态选择 64/128/256（shader 需多版本）
- **方案 C**：用 SSBO 替代 UBO（需 GLES 3.1+，移动端兼容性下降）

**优先级**：低（当前 64 关节够 95% 场景）

---

### 2.2 Web 平台 GPU 路径打开实测

**当前**：Q7 决策 — Web 平台 AUTO 默认 cpu。

**前提假设**：Safari WebGL2 对 `glVertexAttribIPointer(GL_UNSIGNED_BYTE)` 兼容性可疑。

**验证方法**：
1. 在 Web build 用 Chrome / Firefox / Safari 跑一遍
2. 在每个浏览器手动 `Anim.SetSkinningMode('gpu')` 验证渲染正确
3. 如全部正常，把 Q7 决策从 "默认 cpu" 改为 "默认 gpu"，简化用户体验

**优先级**：中（Web 用户群体决定 ROI）

---

### 2.3 jointMats 上传增量优化

**当前**：每帧 `glBufferSubData` 上传完整 N 个 mat4（最多 4096 bytes）。

**优化空间**：如果用户只动了 5 个关节（手指动画），其他 50 个关节其实和上一帧相同。

**方案**：
- 在 `Animator` 内部跟踪 `dirty_joints` bitmap
- `DrawSkinnedMeshGPU` 用 `glBufferSubData(offset, size, ptr)` 增量上传

**收益评估**：现代 GPU UBO 上传 4KB 通常 < 0.05ms，优化收益可能很小。需 profile 实测。

**优先级**：低（除非有 profile 证据）

---

### 2.4 多 mesh 共享同一 animator 时 jointMats 重复烘焙

**当前**：每个 `DrawSkinnedMeshGPU` 调用独立计算 `modelMat × jointMat[j]` 并上传。如果 N 个 mesh 共享同一 animator + 不同 modelMat，就有 N 次烘焙。

**优化方案**：
- 缓存 "本帧 animator.jointMats"（modelMat=identity 版本）已上传的 UBO
- 用户调用 `DrawSkinnedMesh(mesh, anim, modelMat)` 时，shader 读 UBO + 用 modelMat uniform 二次乘法

**代价**：shader 多一步 mat4 乘法 / 顶点（虽然多但 GPU 并行）；逻辑复杂度增加。

**优先级**：低（除非用户明确反馈多 mesh 场景）

---

## 三、文档与维护（P2）

### 3.1 同步 `docs/SKILLS.md`

如果有 SKILLS.md 索引，需登记 Phase AW 已完成（参考 Phase AV 的处理）。

**建议**：在 `docs/SKILLS.md` 找 Phase AV 章节，紧邻处加 Phase AW 一行链接。

---

### 3.2 同步 `docs/PROJECT_SUMMARY.md`

如果项目级总结文档维护"按阶段完成度"表格，登记 Phase AW = 完成 ✓。

---

### ✅ 3.3 demo 示例补充

**已完成 (Phase AW.x)**：新建 `samples/demo_skinning_perf/main.lua`（独立 OOP Window demo）+ `setup.ps1`/`setup.sh` 一键下载 Khronos RiggedSimple.glb。比下面建议的伪代码更完整。

**原建议（保留以便回溯）**：

`samples/demo_animation/` 当前可能没有 GPU skinning 演示。建议补一个：

```lua
-- samples/demo_animation/main.lua（伪代码）
local Anim = require 'Light.Animation'
print('Skinning mode:', Anim.GetSkinningMode())   -- 桌面应该看到 "gpu"

-- 可选：演示运行时切换
function love.keypressed(k)
    if k == 'g' then Anim.SetSkinningMode('gpu')
    elseif k == 'c' then Anim.SetSkinningMode('cpu')
    elseif k == 'a' then Anim.SetSkinningMode('auto')
    end
    print('Mode -> ' .. Anim.GetSkinningMode())
end
```

**优先级**：低（不影响 API 完整性）

---

## 四、需用户提供 / 决策的事项

无 — 当前实现完全自洽，所有 Q1-Q7 已在 CONSENSUS 文档中决策完成。

---

## 五、操作指引（如何处理本 TODO）

### A. 立即可做（5-10 分钟）

```powershell
# 本地启动 demo + 视觉对比 GPU/CPU
cd e:\jinyiNew\Light\Light-0.2.3\demo_animation  # 或任一 demo
.\light.exe                                       # 启动后用 Lua console 切换 mode

# Lua console
> require('Light.Animation').SetSkinningMode('cpu')
> require('Light.Animation').SetSkinningMode('gpu')
# 视觉无差异 -> §1.2 通过；如有差异 -> 报告 issue
```

### B. 短期优化（1-2 小时）

实现 §1.1 的 frame timing helper，补一个 demo / smoke。

### C. 长期规划

§2.1 / §2.2 / §2.3 / §2.4 视项目优先级决定。建议至少完成 §2.2（Web 平台默认值校验），其他按用户反馈决定。

---

## 六、TODO 跟踪

如果后续完成本文件中的事项，请：
1. 把对应章节标题加 ✅ 前缀
2. 在文末"完成日志"加一行（日期 + commit）

### 完成日志

- **2026-05-10** Phase AW.x 完成 §1.1 / §1.2 / §3.3：交付 `samples/demo_skinning_perf/` 工具链（main.lua + setup 脚本 + README）+ `Light.Graphics.GetBackendName` 新 API + `scripts/smoke/graphics.lua` smoke 验证 + 主文档同步。详见 `docs/Phase AW.x/`。
