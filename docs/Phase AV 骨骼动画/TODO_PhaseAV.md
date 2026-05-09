# Phase AV 骨骼动画 — 待办事项

> 6A 工作流阶段 6（Assess）的精简 TODO 清单
>
> **核心原则**：本文档只列出 **当前阻塞或近期可推动** 的事项。已完成项见 `ACCEPTANCE_PhaseAV.md` §3 / `FINAL_PhaseAV.md` §2。

---

## A. 资源类（你提供，我接入）

> 这一组事项需要你（用户）提供资源；我无法自己生成测试 glTF。提供后我可以立即接入并触发 CI。

### A.1 ⭐ 提供测试用 glTF 资产

**用途**：解锁 Phase AV.x 数值断言 + demo 端到端渲染。

**需要文件**（放到 `samples/demo_animation/assets/` 即可）：

| 文件名 | 来源建议 | 用途 |
|--------|---------|------|
| `cube_skin.glb` | Blender / glTF-Sample-Models 仓库 [BoxAnimated](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/BoxAnimated) | 1 关节最简 case，验证基本采样 |
| `simple_walk.glb` | Mixamo 任意行走动画（可付费免费均可） | 多关节 + 多 clip 状态机验证 |

**约束**：
- ≤ 64 关节（Phase AV 上限）
- 至少 2 个 clip（idle + walk）便于 transition 演示
- ≤ 5 MB 文件大小（避免 git 仓库膨胀）

**操作指引**：

```powershell
# 选项 1：从 KhronosGroup 拉简单样本
curl -L "https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Models/master/2.0/BoxAnimated/glTF-Binary/BoxAnimated.glb" -o samples/demo_animation/assets/cube_skin.glb

# 选项 2：Blender 自己导出（推荐 Khronos glTF Exporter 默认设置）
# - File → Export → glTF 2.0 (.glb/.gltf)
# - Format: glTF Binary (.glb)
# - Animation: ✓ Include Animations
# - Skinning: ✓ Include Skinning
```

提供完成后告诉我，我会：
1. 接入到 demo_animation 主路径（删降级分支）
2. 加 Phase AV.x smoke 数值断言（vs Blender 计算的 ground truth 矩阵）
3. 推 CI 验证

---

### A.2 ⭐ 提供 PBR 材质贴图（可选）

**用途**：让 DrawSkinnedMesh 渲染更真实。

**需要**：
- baseColor.png（diffuse / albedo）
- metallic_roughness.png（蓝通道 metallic + 绿通道 roughness）
- normal.png（normal map）

**约束**：≤ 1024×1024，PNG/JPG 均可。

可选（无贴图也能跑，会用纯白 baseColor + 默认 PBR 参数）。

---

## B. 代码类（我可独立完成）

> 这一组事项不需要你提供资源，我可以直接做。等你确认优先级后开工。

### B.1 智能 fallback：cgltf 不存在 mesh 时优雅退化

**现状**：`LoadSkinnedGLTF(non_skin.glb)` 会返回 `nil, "no skin found"`。
**改进**：可选参数 `requireSkin=false` 时退化为 `Light.Graphics.Mesh` 路径。
**工作量**：~30 行 + 4 行 smoke。

### B.2 多 primitive 单 SkinnedMesh

**现状**：每个 primitive 创建一个 SkinnedMesh。
**改进**：`pack.meshes = {mesh1, mesh2, ...}` 数组，便于多材质模型。
**工作量**：~50 行 C++ + 8 行 smoke + API 文档更新。

### B.3 GetActiveClip / GetTransitions / GetEvents 读取接口

**现状**：状态机内部数据无法 Lua 侧检视，调试困难。
**改进**：加只读 getter（返回 table）。
**工作量**：~80 行 + 12 行 smoke。

### B.4 Crossfade 中点权重数值 smoke 测试

**现状**：smoke 只验证 metatable 方法存在，未数值验证 progress 推进。
**改进**：用模拟 Skeleton/Clip（不需要 glTF 资源）做 Update 时序断言。
**工作量**：~50 行 smoke + 设计辅助 mock helper。
**前置**：可独立做（不依赖 A.1）。

---

## C. 配置类（你需要决策）

> 这一组事项需要你（用户）做技术选型或优先级决策。

### C.1 GPU skinning 优先级

**问题**：Phase AV.x 是否启动 GPU skinning 实现？

**权衡**：
- **CPU 路径优点**：跨平台一致；GLES2 / Web 不需特殊处理；调试简单
- **CPU 路径缺点**：每帧 DeleteMesh + CreateMesh 触发 VBO 重传，> 5000 顶点时性能不理想
- **GPU 路径优点**：vertex shader 一次上传 inverseBind 矩阵 + per-vertex joint indices/weights，性能 5-10x
- **GPU 路径缺点**：shader uniform array 上限（通常 ≤ 256 vec4），多平台 GLES 兼容工作量大

**推荐**：先做 B.1 / B.2 / B.3 / B.4 巩固功能，等业务真用到大模型再做 C.1。

**等你确认**：是否在下个 Phase（AW 候选）启动 GPU skinning？

### C.2 Layer / IK / Morph target 路线图

**问题**：哪个先做？

| 选项 | 业务价值 | 实现复杂度 |
|------|---------|---------|
| Layer（base + override + additive） | 中 | 中 |
| IK（two-bone / look-at） | 高（脚踩地形 / 手抓物体） | 高 |
| Morph target（blend shapes） | 中（表情系统） | 中 |

**等你确认**：业务中下一项需求是哪个？

---

## D. 文档类（我可独立完成）

### D.1 Phase AV 与现有 SpriteAnimation 的对比指引

**改进**：在 `docs/api/Light_Animation.md` 加 "When to use SpriteAnimation vs Animation" 节，避免新手混淆。
**工作量**：~50 行 markdown。

### D.2 Animator 状态机 cookbook

**改进**：在 `samples/demo_animation/README.md` 加常见模式：
- "idle ↔ walk ↔ run" 标准三态
- "Any state → death" 任意状态转换
- "Attack with combo" 计时器 + Param 组合
- "Ragdoll-on-death" 物理混合

**工作量**：~120 行 markdown + Lua 片段。

---

## 立即可推进的下一步建议

按优先级（你做选择）：

1. **A.1 + A.2 提供资产** → 我接入 + 加数值 smoke + 推 CI（~半天工作量）
2. **B.4 中点权重数值 smoke**（无依赖，独立可推）→ 加 50 行 smoke + 推 CI（~1 小时）
3. **B.3 getter 接口**（无依赖）→ 加 ~80 行 + smoke + 推 CI（~2 小时）
4. **D.1 / D.2 文档完善**（无依赖）→ ~半天

或者：直接开 Phase AW（跨 Phase 推进，先关闭 Phase AV）？

请告诉我你想先做哪一项，或者你想我自动推进的次序。
