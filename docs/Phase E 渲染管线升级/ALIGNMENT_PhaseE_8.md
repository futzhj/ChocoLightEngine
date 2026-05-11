# ALIGNMENT — Phase E.8 · SSAO (Screen-Space Ambient Occlusion)

> 6A 工作流 · 阶段 1 · Align
> Phase E.8 目标：在 HDR + Bloom + AE + LensDirt + Streak + LensFlare 链路之上接入 **SSAO**（屏幕空间环境光遮蔽），为 3D mesh 场景提供角落/缝隙的自然阴影，显著提升几何质感。

---

## 1. 原始需求

延续 `@e:\jinyiNew\Light\docs\Phase E 渲染管线升级\FINAL_PhaseE_7.md:182` §7 后续阶段建议：

> **Phase E.8** — SSAO（屏幕空间环境光遮蔽） → 3D 场景质感大幅提升

用户确认（2026-05-12）：

> **SSAO 适用范围**：仅 3D mesh / 显式 `SetDepthTest(true)` 场景。纯 2D 场景 SSAO 输出全为 1（无遮蔽），存在但不影响画面。

---

## 2. 项目上下文分析

### 2.1 关键基础设施调研结果

| 维度 | 现状 | SSAO 兼容性 |
|------|------|------------|
| **HDR RT depth** | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:2398` GL_DEPTH_COMPONENT24 **renderbuffer** | ⚠️ 不可采样，需升级为 depth texture |
| **3D PBR 渲染** | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:507-623` 完整 PBR forward（含 normal/worldPos） | ✅ 已有 3D 用例；但无 G-buffer normal |
| **Lit2D 渲染** | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:626-813` 2D forward lighting | ⚠️ 主要 2D，z=0 场景 SSAO 无效果（符合用户确认范围） |
| **透视相机** | `Light.Graphics.SetPerspective(fovY, aspect, near, far)` `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp:297` | ✅ 已有 Lua API |
| **视图矩阵** | `Light.Graphics.SetCamera(ex,ey,ez, tx,ty,tz)` LookAt | ✅ 已有 |
| **Depth test 开关** | `Light.Graphics.SetDepthTest(bool)` 默认关 | ✅ 已有 |
| **Mesh API** | `Light.Graphics.Mesh.New(vertices, indices)` 12 floats/vertex `@e:\jinyiNew\Light\ChocoLight\src\light_graphics_mesh.cpp:59` | ✅ demo 可 Lua 端代码生成几何体 |
| **BatchRenderer Flush** | HDR 启用时 `Pause/Resume` 对 user RT 的切换已做 | ✅ 可复用模式 |
| **Legacy backend** | GL 1.x 无 depth texture + FBO 能力有限 | ❌ SSAO 直接 no-op |

### 2.2 复用基础（Phase E.4~E.7 已建立的模式）

| 资产 | 复用方式 |
|------|---------|
| `RenderBackend` 虚接口风格 | ~7 个新虚接口：1 Supports + 2 RT 生命周期 + 2 Depth-tex + 2 Draw |
| GL33 后端 `InitLensFx` 套路 | 同文件追加 SSAO shader 编译（2 shader: SSAO + Blur） |
| Ping-pong RT 风格 | 同 Streak/LensFlare（半分辨率单通道 R8/R16F，min 32×32） |
| HDR 5 联动点（OnHDREnabled/Disabled/Resized） | 同 Streak/LensFlare |
| Lua 子表 + ASCII smoke + headless demo | 同 LensDirt/Streak/LensFlare |
| CI workflow smoke 注册 | 追加 `phaseE8Smoke` |

### 2.3 既有架构对齐

Phase E 链路层次（**Phase E.8 SSAO 插入位置**）：

```
HDR.BeginScene → HDR RT (with DEPTH TEXTURE, 新)  ← 依赖 depth 升级

Lua Draw (用户开 SetDepthTest(true), SetPerspective + SetCamera, mesh:Draw):
   - 3D mesh 写 HDR color RT + depth texture

HDR.EndScene:
   SSAO.Process    ──► 读 depth tex → 生成 AO → blur → 调制 HDR RT   [★ 新增]
   Bloom.Process           ──► HDR 加亮
   AE.Process              ──► exposure
   LensDirt.Process        ──► bloom × dirt → HDR
   Streak.Process          ──► anamorphic flare → HDR
   LensFlare.Process       ──► ghost + halo + aberration → HDR
   Tonemap → backbuffer
```

**关键设计**：SSAO 必须**在 Bloom 之前**执行 — AO 是"暗部加深"，应在 bright pass 前完成，否则被 bloom 提亮抵消。

### 2.4 命名空间一致性

| 现有 | 新增 |
|------|------|
| `BloomRenderer::` / `AutoExposureRenderer::` / `LensDirtRenderer::` / `StreakRenderer::` / `LensFlareRenderer::` | — |
| | `SSAORenderer::` 新建 |

Lua 端：

- 现有：`Light.Graphics.Bloom` / `.AutoExposure` / `.LensDirt` / `.Streak` / `.LensFlare`
- 新增：`Light.Graphics.SSAO`

---

## 3. 需求理解

### 3.1 视觉算法（业界标准 — 经典 SSAO + 双边滤波）

**Pass 1: SSAO raw**（半分辨率 R8/R16F RT）：

```glsl
// 输入: uDepthTex (full-res depth), uNoiseTex (4x4 rotation), uKernel[16] (半球采样方向)
// 输出: AO value ∈ [0, 1]

1. 由 screen UV + depth 重建 view-space position P
2. ddx/ddy 重建 view-space normal N
3. 取 noise tex 采样 4x4 旋转向量 R (每 pixel 旋转 kernel, tile 方式)
4. 构造 TBN 矩阵 (R + N)
5. for i in 0..kernelSize (8 / 16):
     samplePos = P + TBN * kernel[i] * radius
     projPos   = projection * samplePos
     sampleUV  = projPos.xy / projPos.w * 0.5 + 0.5
     sampleDepth = sample(uDepthTex, sampleUV)
     sampleViewZ = reconstructZ(sampleDepth)
     rangeCheck = smoothstep(0, 1, radius / abs(P.z - sampleViewZ))
     occlusion += (sampleViewZ >= samplePos.z + bias ? 1.0 : 0.0) * rangeCheck
6. ao = 1.0 - occlusion / kernelSize
7. ao = pow(ao, uPower)   // 调整对比度
```

**Pass 2: Bilateral Blur**（2-pass separable, 水平 + 垂直）：

```glsl
// 输入: uSSAOTex, uDepthTex
// 目的: 去 noise + 保留 depth 边缘（不跨物体边界模糊）

for i in -2..+2:
    offsetUV = vUV + vec2(i*texel, 0)    // 水平 pass
    sampleAO    = sample(uSSAOTex, offsetUV)
    sampleDepth = sample(uDepthTex, offsetUV)
    w = exp(-abs(centerDepth - sampleDepth) / depthFalloff)
    sum += sampleAO * w
    wsum += w
result = sum / wsum
```

**Pass 3: Composite**（full-res modulate）：

```glsl
// 输入: uHDRColor (主 RT), uSSAOTex (blurred)
// 输出: HDR RT *= AO（覆盖写 HDR RT）

vec3 hdr = sample(uHDRColor, vUV).rgb
float ao = sample(uSSAOTex, vUV).r   // blurred, full-res 过滤
ao = mix(1.0, ao, uIntensity)   // 用户强度控制
FragColor = vec4(hdr * ao, 1.0)
```

### 3.2 可调参数（Public Lua API）

| 参数 | 类型 | 默认 | 范围 | 含义 |
|------|------|------|------|------|
| `Radius` | float | 0.5 | [0.05, 5.0] | 采样半径（world/view space 单位） |
| `Bias` | float | 0.025 | [0.0, 0.2] | 防自遮蔽偏移 |
| `Intensity` | float | 1.0 | [0, 4.0] | AO 强度乘子（composite 时 mix） |
| `KernelSize` | int | 16 | {8, 16} | 采样数（性能/质量权衡）|
| `Power` | float | 2.0 | [0.5, 8.0] | AO 对比度 `pow(ao, power)` |
| `BlurEnabled` | bool | true | — | 是否过双边 blur（false = raw AO）|
| `AutoEnable` | bool | false | — | HDR.Enable 时自动开 SSAO |

### 3.3 API Surface（Lua 端 23 fn，对齐 LensFlare）

```
Lifecycle (5):
  Enable(w, h) / Disable / IsEnabled / IsSupported / Resize(w, h)

AutoEnable (2):
  SetAutoEnable / GetAutoEnable

Params (14, 7 pairs):
  SetRadius / GetRadius
  SetBias / GetBias
  SetIntensity / GetIntensity
  SetKernelSize / GetKernelSize
  SetPower / GetPower
  SetBlurEnabled / GetBlurEnabled
  SetAutoEnable / GetAutoEnable         ← 属 AutoEnable 组

总 = 5 + 2 + 12 = 19 fn 子表
```

### 3.4 关键技术决策（已自决）

| 决策 | 选择 | 理由 |
|------|------|------|
| **HDR depth 升级** | RB → texture（`glTexImage2D(GL_DEPTH_COMPONENT24)`）| GLES3 核心支持；行为零变化；现有 `hdrFboDepthRB` map 仅改 1 处 |
| **算法** | 经典 SSAO + 16 kernel + 4×4 noise | GL3.3/GLES3 通杀；性能/质量均衡 |
| **Normal 来源** | depth 重建（`ddx/ddy + cross`）| 零 G-buffer 改造；纯 depth pass |
| **采样 kernel 生成** | CPU 生成半球 Hammersley 序列 + noise 4×4 RGB | 质量稳定；一次生成 lifetime 复用 |
| **RT 规格** | raw AO: `R16F` 半分辨率；blur 后: 同 `R16F`；后续 full-res composite 直接采样插值 | 带宽最优 |
| **Composite 策略** | `HDR *= mix(1.0, ao, intensity)` 覆盖写 HDR RT | 作用在 bloom 之前，AO 暗部不被 bloom 抹平 |
| **适用范围** | 仅 `depthTex != 0` 且 depth 有变化时有效；纯 2D / 无 depth = SSAO 输出 1.0 | 用户确认 |
| **AutoEnable** | 默认 false | 与 LensDirt/Streak/LensFlare/AE 一致 |
| **Legacy backend** | `SupportsSSAO() = false`，全链路静默 | 同其他 Phase E 模块 |

---

## 4. 边界确认（任务范围）

### 4.1 ✅ 本 Phase 范围内

- GL33 后端：HDR depth RB → texture 升级（含 `hdrFboDepthRB` → `hdrFboDepthTex` map 重命名）
- GL33 后端：2 新 shader（SSAO + Bilateral Blur），1 Composite（复用 Bloom composite? 不可复用 — AO 是乘法，Bloom 是加法；需独立 1 shader）
- GL33 后端：半分辨率 ping-pong RT（raw AO + blur temp）
- GL33 后端：4×4 noise texture（RGB 随机旋转向量）+ 16 kernel uniform
- `SSAORenderer` namespace 模块 + HDR 5 联动点
- Lua 子表 `Light.Graphics.SSAO` 19 fn
- `scripts/smoke/ssao.lua` smoke（headless tolerant ≥50 断言）
- `samples/demo_ssao/` Lua 代码生成 cube+plane 场景 + T 键 toggle + 参数调节
- CI workflow smoke 注册
- ACCEPTANCE / FINAL / TODO 文档

### 4.2 ❌ 本 Phase 范围外（留后续 / TODO）

- **G-buffer normal attachment**（MRT normal RT）— 需大改 PBR/Lit2D shader；当前 depth 重建足够
- **HBAO / GTAO 算法升级**— 留 Phase E.8.1+ 或 Phase F（compute shader）
- **Temporal filtering**（TAA-style）— 需历史帧 RT + velocity vector
- **SSAO 对 2D 精灵的 fake depth**（把 2D 层转成伪 z 让 SSAO 生效）— 需 2D API 大改
- **GLES 2.0 / WebGL1 支持**— Legacy backend 永 no-op
- **Legacy backend SSAO**—  同上

### 4.3 向后兼容保证

1. **HDR RT depth 升级必须零行为变化**：
   - 所有现有 `glClear(GL_DEPTH_BUFFER_BIT)` / `glEnable(GL_DEPTH_TEST)` 行为不变
   - 现有 PBR/3D demo 运行效果 100% 相同
   - 仅内部存储从 renderbuffer 变为 texture
2. **SSAO.Enable 默认 off**：不影响现有所有 demo
3. **SSAO 对无 depth 场景零副作用**：纯 2D 场景 AO = 1.0 全屏乘 1 = no-op

---

## 5. 疑问澄清

### 5.1 已确认（用户回答 or 自决）

| # | 问题 | 决策 | 方式 |
|---|------|------|------|
| Q1 | SSAO 适用范围：2D + 3D 都跑 vs 仅 3D？ | 仅 3D + 显式 DepthTest 场景 | 用户确认 2026-05-12 |
| Q2 | HDR depth 升级路径？ | 直接升级 `CreateHDRFBO` 的 depth 为 texture（路径 1） | 自决（见 §3.4） |
| Q3 | 算法选型？ | 经典 SSAO + 16 kernel + noise | 自决 |
| Q4 | Normal 来源？ | depth 重建（ddx/ddy） | 自决 |
| Q5 | demo 场景？ | Lua 端用 `Mesh.New` 代码生成 cube + plane + 旋转相机 | 自决（查明 Mesh API 可用） |
| Q6 | API 函数数？ | 19 fn（比 LensFlare 的 23 少 2 对 Params；少 Distortion/FlareTex）| 自决 |

### 5.2 待运行期发现（保留）

以下风险点放入 TODO 用于 E.8 交付后验证：

- **GLES 3.0 depth texture 支持检测**：部分旧 Android 驱动可能需要 `GL_OES_depth_texture` 扩展；计划中 `InitSSAO()` 时检测并 `supported = false` 降级
- **性能实测**：SSAO 半分辨率 + 16 kernel + 2-pass blur 在低端设备可能过重；计划 TODO 加 SetKernelSize(8) 路径

---

## 6. 最终共识（摘要，正文见 `CONSENSUS_PhaseE_8.md` 如需）

- **目标**：Phase E 第 7 剑客 — SSAO，对齐 6A + 既有 Phase E.3~E.7 风格
- **交付物**：2 新 shader + 1 新后端虚接口套件（~7 虚方法）+ 1 新 renderer 模块 + 1 Lua 子表 19 fn + smoke + demo + 6 文档
- **不做**：G-buffer / HBAO / Temporal / Legacy
- **CI 要求**：6/6 绿
- **拆分**：沿 E.7 模式：**E.8.1 backend / E.8.2 module / E.8.3 Lua+smoke+demo / docs**

---

**Align 阶段完成**。下一步：进入 **Architect** 阶段，输出 `DESIGN_PhaseE_8.md`（含架构图、模块依赖、接口契约、数据流向图）。
