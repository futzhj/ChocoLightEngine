# ALIGNMENT — Phase E.7 · Lens Flare (Ghost + Halo + Chromatic Aberration)

> 6A 工作流 · 阶段 1 · Align
> Phase E.7 目标：在 HDR + Bloom + AE + LensDirt + Streak 链路之上接入 **Lens Flare**（屏幕空间镜头光晕：ghost + halo + chromatic aberration），完成"电影感后处理 6 剑客"全套。

---

## 1. 原始需求

延续 Phase E.6 TODO：

> **Phase E.7** Lens Flare（鬼影 ghost）—— 需要光源主方向采样，沿画面中心方向反投若干个不同尺寸 / 颜色的光圈。独立 `LensFlareRenderer` namespace。

---

## 2. 项目上下文分析

### 2.1 复用基础（Phase E.6 已建立模式）

| 资产 | 复用方式 |
|------|---------|
| `RenderBackend` 虚接口风格 | 6 个新虚接口：1 Supports + 2 RT 生命周期 + 3 Draw |
| GL33 后端 InitLensFx 套路 | 同函数追加 1 个 program 编译（ghost shader）+ composite 复用 |
| `BloomRenderer::DrawBloomBrightPass` | **直接复用**做 threshold 提取（避免重复 shader） |
| `BloomRenderer::DrawBloomComposite` | **直接复用**做最终 additive blend |
| Ping-pong RT 风格 | 同 Streak（半分辨率 RGBA16F，min 32×32） |
| HDR 5 联动点（OnHDREnabled/Disabled/Resized） | 同 Streak |
| Lua 子表 + ASCII smoke + headless demo | 同 LensDirt/Streak |
| CI workflow phaseE6Smoke 模式 | 追加 phaseE7Smoke |

### 2.2 既有架构对齐

Phase E 链路层次（**Phase E.7 接入位置**）：

```
HDR.EndScene:
  Bloom.Process       ──►  HDR RT 加亮
  AE.Process           ──►  exposure
  LensDirt.Process     ──►  bloom × dirt → HDR RT
  Streak.Process       ──►  bright + blur + comp → HDR RT
  LensFlare.Process    ──►  bright + ghost+halo+aberration → HDR RT   [新增]
  Tonemap → backbuffer
```

### 2.3 命名空间一致性

| 现有 | 新增 |
|------|------|
| `BloomRenderer::` | — |
| `AutoExposureRenderer::` | — |
| `LensDirtRenderer::` | — |
| `StreakRenderer::` | — |
| | `LensFlareRenderer::` 新建 |

Lua 端：
- 现有：`Light.Graphics.Bloom` / `.AutoExposure` / `.LensDirt` / `.Streak`
- 新增：`Light.Graphics.LensFlare`

---

## 3. 需求理解

### 3.1 视觉算法（业界标准 ghost-halo 屏幕空间方案）

**Ghost generation**：

```text
ghost(uv):
  vec2 centerVec = (0.5, 0.5) - uv                    # 朝中心方向
  vec2 ghostVec  = centerVec * ghostDispersal          # 步长缩放
  vec3 result = 0
  for i in 1..ghostCount:
    vec2 sampleUV = uv + ghostVec * float(i)
    sampleUV = clamp(sampleUV, 0.0, 1.0)              # 或 wrap
    float weight = pow(1.0 - length(centerVec) * 2.0, 4.0)  # 中心衰减
    weight = max(weight, 0.0)
    # Chromatic aberration: RGB 分量分别采样（径向偏移）
    vec2 ca = normalize(ghostVec) * chromaticAberration
    float r = sample(brightTex, sampleUV + ca).r
    float g = sample(brightTex, sampleUV).g
    float b = sample(brightTex, sampleUV - ca).b
    result += vec3(r, g, b) * weight
  return result
```

**Halo generation**（环状光晕，沿径向方向偏移采样）：

```text
halo(uv):
  vec2 centerVec = (0.5, 0.5) - uv
  vec2 haloVec   = normalize(centerVec) * haloWidth    # 固定径向距离
  vec2 sampleUV  = uv + haloVec
  float distFromRing = abs(length(centerVec) - haloWidth)
  float weight = smoothstep(0.5, 0.0, distFromRing)
  # 同 chromatic aberration
  return sample_with_chroma(sampleUV) * weight
```

**Composite**（additive 合到 HDR RT）：

```text
hdr += (ghost + halo) * intensity
```

### 3.2 参数集（10 个）

| 参数 | 默认值 | 范围 | 含义 |
|------|--------|------|------|
| **Threshold** | 1.0 | [0, +∞) | 亮度阈值（同 Bloom/Streak） |
| **Intensity** | 0.4 | [0, +∞) | 最终合成强度 |
| **GhostCount** | 4 | [0, 8] | ghost 数量；0 关 ghost |
| **GhostDispersal** | 0.4 | [0, 2.0] | 径向缩放因子（步长） |
| **HaloWidth** | 0.5 | [0, 1.0] | halo 环形半径（UV 空间） |
| **ChromaticAberration** | 0.005 | [0, 0.02] | RGB 分量径向偏移 |
| **DistortionEnabled** | true | bool | 是否启用色差 |

7 实用 + 3 Set/Get 自动 → **共 10 个 Lua API**（外加生命周期 6 + 联动 2 = **18 Lua fn**）。

### 3.3 联动策略

| 触发 | LensFlare 行为 |
|------|----------------|
| `HDR.Enable(W, H)` + autoEnable=true | `LensFlare.Enable(W, H)` |
| `HDR.Disable()` | `Disable()`（释放 ping-pong RT） |
| `HDR.Resize(W, H)` | `Resize(W, H)` |
| `Bloom` 未启用 | LensFlare.Process 仍可工作（bright 用自己的 pass 或复用 Bloom 后端 shader） |
| autoEnable | **默认 false**（同 LensDirt/Streak/AE） |

### 3.4 Legacy 后端降级

- `SupportsLensFlare() = false` → 所有 API no-op
- Lua API 静默返回 false / 0

---

## 4. 边界确认

### 4.1 范围内

- ✅ Ghost generation（多层径向缩放采样）
- ✅ Halo generation（环状径向偏移）
- ✅ Chromatic Aberration（RGB 分量分别采样）
- ✅ Threshold pass 提取亮像素源
- ✅ HDR 自动联动（Enable/Disable/Resize 三件套）
- ✅ Lua 子表 + smoke + demo + 6 平台 CI
- ✅ Ping-pong RT 半分辨率（同 Streak 节流策略）

### 4.2 范围外

- ❌ **光源主方向采样**：原 TODO 提到的"沿画面中心方向反投"已通过 `ghostDispersal` 实现，但真正的 **3D 光源 → 屏幕方向** 投影需要相机矩阵，超出 2D/HDR 后处理范畴 → 后续 Phase F.x
- ❌ **Lens dirt 纹理合成**：dirt 是 LensDirt 的职责，flare 不重复
- ❌ **Anamorphic streak in flare**：streak 与 flare 视觉互补但独立
- ❌ **Animated flare**（动画 ghost 数量/颜色变化）→ 后续可选
- ❌ **多光源 flare 排序**：v1 单一全屏 ghost-halo 算法

### 4.3 平台/兼容

- 仅 GL33Backend 支持（与 Bloom/Streak 一致；Legacy 后端 no-op）
- 不引入新 GLSL extension（fragment shader 标准操作）
- 不依赖 compute shader（保持 GLES3 兼容）

---

## 5. 疑问澄清

| 疑问 | 决策 | 依据 |
|------|------|------|
| Q1: ghost 算法是否走单独 RT，还是和 Streak 共用？ | **独立 ping-pong RT 对** | 解耦；避免 Streak/LensFlare 同开时互相覆盖 |
| Q2: 是否要 PBO / readback 优化？ | **不需要** | Lens flare 不涉及 CPU readback |
| Q3: 是否需要可选 lens flare 贴图（rays 星芒）？ | **v1 不做** | Procedural ghost 已足够；外部贴图留 v2 |
| Q4: Halo 数量是否可调？ | **v1 固定 1 个 halo** | UE 实践证明 1 环 + N ghost 已足够 |
| Q5: 是否要 SetAutoEnable 默认 true？ | **false** | 电影感强烈不应默认接管，同 LensDirt/Streak/AE |
| Q6: ghostCount=0 是否合法？ | **是，等于关 ghost 只留 halo** | 用户可单独开 halo |
| Q7: 是否提供 GhostColor 数组（每个 ghost 独立颜色）？ | **v1 不做** | 仅依赖采样源颜色 + chromatic aberration；颜色数组 → v2 |
| Q8: chromatic aberration 是否硬关闭通道？ | **DistortionEnabled = false 时整体跳过偏移采样** | 节省 fragment |

---

## 6. 验收标准

### 6.1 功能验收

| # | 准则 |
|---|------|
| AC-1 | RenderBackend 6 个新虚接口签名稳定，Legacy 后端默认 no-op |
| AC-2 | GL33 后端编译 ghost shader 成功；composite 复用 Bloom composite shader |
| AC-3 | `LensFlareRenderer` namespace API 14 函数（Init/Shutdown + lifecycle 5 + autoEnable 2 + HDR hook 3 + params + Process） |
| AC-4 | `Light.Graphics.LensFlare` 子表挂在 `Light.Graphics` 之下，**18 个** Lua 函数全可调用 |
| AC-5 | smoke `scripts/smoke/lens_flare.lua` 在 6 平台 CI Windows runtime smoke 阶段 PASS |
| AC-6 | demo_lens_flare 可独立运行（headless 也有 API 探测分支） |
| AC-7 | HDR.Enable/Disable/Resize 触发 LensFlare 联动正确（autoEnable=true 时拉起；关闭顺序在 Streak 之后） |
| AC-8 | 各参数 clamp 正确（Threshold ≥ 0、GhostCount [0, 8] 整数、GhostDispersal [0, 2]、HaloWidth [0, 1]、ChromaticAberration [0, 0.02]） |
| AC-9 | 6 平台 CI 全绿（Web / Linux / Android / macOS / iOS / Windows） |
| AC-10 | ACCEPTANCE + FINAL + TODO 文档完整 |

### 6.2 视觉验收（人工）

- demo_lens_flare 启动后，HDR 亮点周围可见：
  - **Ghost**：朝画面中心反投的多个光圈（径向排列）
  - **Halo**：围绕中心的环状光晕
  - **Chromatic Aberration**：边缘 RGB 色散
- 切换 ghostCount=0 → ghost 消失只剩 halo
- 切换 haloWidth=0 → halo 消失只剩 ghost
- 切换 distortionEnabled=false → 色差消失

---

## 7. 项目特性规范对齐

| 维度 | 对齐项 |
|------|--------|
| 命名风格 | `LensFlareRenderer` 全大驼峰，匹配 `BloomRenderer` / `StreakRenderer` |
| 文件命名 | `lens_flare_renderer.h` / `.cpp`（snake_case，匹配 `lens_dirt_renderer.h` / `streak_renderer.h`） |
| Lua subtable | `Light.Graphics.LensFlare`（PascalCase）匹配 `LensDirt` / `Streak` |
| Smoke 命名 | `scripts/smoke/lens_flare.lua` |
| Demo 命名 | `samples/demo_lens_flare/main.lua` + `README.md` |
| Backend phase 注释 | `// ==================== Phase E.7 — Lens Flare ====================` |
| Commit prefix | `feat(phase-e7.1)` / `feat(phase-e7.2)` / `feat(phase-e7.3)` |
| 文档目录 | `docs/Phase E 渲染管线升级/{ALIGNMENT,DESIGN,TASK,ACCEPTANCE,FINAL,TODO}_PhaseE_7.md` |

---

## 8. 与 Phase E.6 关键差异

| 项 | Phase E.6 (LensDirt + Streak) | Phase E.7 (LensFlare) |
|----|-------------------------------|----------------------|
| 模块数 | 2 (LensDirt + Streak) | 1 (LensFlare) |
| Backend 虚接口 | 8 (2 + 6) | 6 (1 supports + 2 RT + 3 draw, **Bright/Composite 复用 Bloom**) |
| Lua API 总数 | 23 (10 + 13) | 18 |
| RT 持有 | LensDirt 无 / Streak ping-pong | ping-pong |
| 算法依赖 | LensDirt 需 Bloom 输出 | 独立（自有 bright pass） |
| Shader 编译 | 3 个新（lens_dirt_composite, streak_blur, streak_composite） | **1 个新**（lens_flare_ghost；bright/composite 复用 Bloom） |
| EndScene 顺序位置 | Bloom → AE → LD → ST → Tonemap | Bloom → AE → LD → ST → **LF** → Tonemap |

→ **复用度更高、shader 更少、单模块更紧凑**。

---

## 9. 风险与缓解

| 风险 | 缓解 |
|------|------|
| Bloom bright pass 复用造成耦合 | LensFlareRenderer.Process 显式检查 `Bloom.IsSupported()`；Legacy backend 后端不支持时整体降级 |
| ghostCount 太大导致 fragment 爆炸 | Lua/C++ 双层 clamp [0, 8]；半分辨率 RT |
| Wrap mode 选择（fract vs clamp） | v1 用 `clamp(0, 1)` + 中心衰减权重 → 避免 fract 引入硬边 |
| autoEnable 默认值 | 与 LensDirt/Streak/AE 一致 = **false** |
| HDR.Disable 关闭顺序 | LensFlare 先于 Streak 关（与 LensFlare 在管线末端的位置一致） |

---

**Phase E.7 需求边界清晰，技术方案与 Phase E.6 高度对齐，所有不确定性已澄清。准入 DESIGN 阶段。**
