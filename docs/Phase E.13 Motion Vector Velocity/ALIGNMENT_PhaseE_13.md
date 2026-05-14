# Phase E.13 Motion Vector Velocity — ALIGNMENT 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **目标**：将“完整 velocity buffer 改善 Temporal SSR 动态物体 ghosting”转为可执行范围
> **当前状态**：规划草案，尚未进入实现

---

## 1. 核心结论

Phase E.13 采用 **混合路线**：在 HDR G-buffer 中新增 motion vector / velocity attachment，并让默认 3D、GPU skinning、GPU skin+morph shader 在主场景绘制时直接写入每像素速度。

通俗说：现在 Temporal SSR 像“看着上一张照片猜这个像素上帧在哪里”。E.13 要给每个像素贴一张“移动小箭头”，让 SSR 直接按箭头回到上一帧对应位置，减少动态物体拖影。

---

## 2. 已确认决策

| 决策项 | 结论 |
|---|---|
| E.13 范围 | 完整 velocity buffer，而不是只做 camera-only reprojection |
| 架构路线 | 混合路线：inline MRT + 显式/自动 prev-state |
| HDR 集成 | 扩展现有 HDR MRT：color + normal + velocity |
| 普通 mesh | 支持显式 previous model matrix；未提供时退化为 camera-only velocity |
| GPU skinning | Animator 保存上一帧 joint matrices，自动传入 backend |
| GPU morph | Animator 保存上一帧 morph weights，自动传入 backend |
| SSR 接入 | Temporal pass 优先使用 velocityTex；缺失时保留 E.12 reprojectMat4 fallback |
| CPU skin/morph | 第一实现优先保证 GL33 GPU 路径；CPU fallback 明确退化，不伪装为完整动画 velocity |
| 本地验证约束 | 遵循前序约束：不做本地 CMake build / runtime smoke，优先静态检查与 CI |

---

## 3. 现有项目上下文

### 3.1 HDR / G-buffer 状态

当前 HDR FBO 由 `HDRRenderer::Enable` 创建，调用 `RenderBackend::CreateHDRFBO(w, h, &tex, &normalTex)`。

GL33 当前资源布局：

| Attachment | 格式 | 用途 |
|---|---|---|
| COLOR_ATTACHMENT0 | RGBA16F | HDR scene color |
| COLOR_ATTACHMENT1 | RG16F | view-space normal，供 SSAO / SSR 使用 |
| DEPTH_ATTACHMENT | Depth24 RBO | 场景深度；SSAO/SSR 通过 blit 旁路复制 |

关键文件：

| 文件 | 作用 |
|---|---|
| `ChocoLight/include/hdr_renderer.h` | HDRRenderer 对外生命周期与查询接口 |
| `ChocoLight/src/hdr_renderer.cpp` | HDR RT 创建、BeginScene/EndScene、后处理顺序 |
| `ChocoLight/include/render_backend.h` | `CreateHDRFBO` / `GetHDRNormalTex` 虚接口 |
| `ChocoLight/src/render_gl33.cpp` | GL33 HDR FBO 创建、MRT draw buffers、默认 3D shader |

### 3.2 默认 3D shader 状态

当前默认 3D 顶点 shader 已输出：

| 数据 | 来源 | 用途 |
|---|---|---|
| `gl_Position` | `uMVP * vertex` | 当前帧裁剪空间位置 |
| `vWorldPos` | `uModel * vertex` | PBR normal map TBN / lighting |
| `vNormalW` | `mat3(uModel) * normal` | normal MRT / lighting |

GPU skin / morph shader 当前只接收当前帧状态：

| 路径 | 当前输入 | 缺失项 |
|---|---|---|
| static mesh | `uMVP`, `uModel` | `uPrevMVP` 或 prev model |
| GPU skin | `uJointMats[64]` | `uPrevJointMats[64]` |
| GPU skin+morph | `uJointMats`, `uMorphWeights` | `uPrevJointMats`, `uPrevMorphWeights` |

### 3.3 SSR Temporal 状态

Phase E.12 Temporal SSR 已有：

| 能力 | 当前实现 |
|---|---|
| 历史 RT | full-res RGBA16F ping-pong |
| reprojection | `prevViewProj * invCurViewProj` |
| jitter | Halton 2,3 8-sample |
| rejection | `0=current-depth threshold`, `1=neighborhood clip` |
| fallback | `hasHistory=0` 时直接输出 current |

E.13 的 velocityTex 会作为更准确的 `prevUV` 来源，保留 E.12 矩阵重投影作为兼容路径。

---

## 4. 需求边界

### 4.1 本阶段必须覆盖

- HDR FBO 能创建并暴露 velocity attachment。
- 默认 GL33 3D PBR / Unlit shader 写入 velocity。
- GPU skinning shader 写入骨骼动画 velocity。
- GPU skin+morph shader 写入骨骼 + morph 组合 velocity。
- SSR Temporal pass 能读取 velocityTex 并用 `prevUV = uv - velocity` 采样 history。
- 首帧、resize、enable/disable、缺 prev-state 时不会产生大面积错误拖影。

### 4.2 本阶段不强行覆盖

- Legacy / 非 GL33 后端的真实 velocity 输出。
- 用户自定义 shader 自动写 velocity。
- `SetCanvas(userFbo)` 用户画布中的 velocity。
- CPU skin/morph fallback 的精确逐顶点动画 velocity。
- 需要额外 material property buffer 的 roughness-aware temporal weighting 真实实现。

### 4.3 Roughness-aware 说明

粗糙度感知 temporal filtering 的核心是：粗糙表面和光滑表面对历史反射的信任程度不同。但当前 G-buffer 只有 normal，没有 roughness attachment。

因此 E.13 主线先完成 motion vector 正确性；roughness-aware 作为 E.13.x 设计项保留，后续可在以下方案中选择：

| 方案 | 优点 | 缺点 |
|---|---|---|
| 新增 material/roughness attachment | 语义清晰 | 增加 MRT 带宽与内存 |
| velocity 改为 RGBA16F，BA 存 roughness/validity | 少一个 attachment | velocity buffer 从 8MB/1080p 增至约 16MB/1080p |
| SSR 内用 material uniform 近似 | 改动小 | 屏幕空间无法区分同一 draw 内不同 roughness texel |

---

## 5. 验收标准

| 类别 | 标准 |
|---|---|
| 静态接口 | `render_backend.h` 新旧调用点一致，无签名遗漏 |
| 资源生命周期 | HDR enable/resize/disable 后 velocityTex 不泄漏、不悬挂 |
| shader 覆盖 | static / skin / skin+morph 双 profile 都有 velocity 输出策略 |
| SSR 兼容 | velocityTex=0 时 E.12 Temporal SSR 行为保持可用 |
| 动态物体 | 有 prev-state 的移动物体使用 velocity reproject，不再只靠 depth matrix 猜测 |
| 历史安全 | 首帧、prev invalid、越界 prevUV、过大 velocity 均拒绝 history 或降权 |
| 文档 | Phase E.13 DESIGN/TASK 明确接口、数据流、风险与回退策略 |

---

## 6. 关键风险

| 风险 | 影响 | 缓解 |
|---|---|---|
| prev-state 不完整 | 动态物体 velocity 错误，产生反向拖影 | 显式 `hasPrev` / validity，首帧清零 |
| draw-order 追踪错配 | 同 mesh 多实例或可见性变化时错配 | 不采用纯后端 draw-call 自动追踪 |
| shader 覆盖不全 | 部分物体写不到 velocity | 默认 shader 全覆盖；用户 shader 明确 fallback |
| MRT 成本增加 | 带宽和显存上升 | 首版使用 RG16F velocity；roughness 另行评估 |
| CPU skin 精确 velocity 成本高 | 需要上传 previous baked vertex | 首版只声明退化，不伪装完整支持 |

---

## 7. 下一步

进入 DESIGN：定义 velocity buffer 格式、接口扩展、shader 数据流、SSR Temporal 接入点与资源生命周期。
