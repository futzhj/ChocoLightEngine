# Phase F.1 TAAU (Temporal Anti-Aliasing Upsampling) — ALIGNMENT 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **目标**：模糊需求 → 精确规范
> **基线**：Phase F.0.11.4 完结（commit `8e7cf25`，main HEAD，截图/录屏 + 多实例后处理 + PBO 异步全量实装）
> **主题**：在 Phase F.0 TAA 基础上引入 **渲染分辨率与显示分辨率解耦**（DLSS / FSR2 风格），让低分辨率渲染 + TAA 时序累积上采样到高分辨率输出
> **创建日期**：2026-05-17

---

## 1. 项目上下文分析

### 1.1 现有 TAA 管线（Phase F.0.x 已交付）

```
Camera Scene (full-res rasterize, jittered projection)
    │
    ▼
HDR FBO (sceneTex RGBA16F + normal RG16F + velocity RG16F/RG8 @ full-res)
    │
    ▼
Bloom → AutoExp → LensDirt → Streak → SSAO → [VelocityDilate]
   → SSR(±Temporal) → LensFlare → MotionBlur
    │
    ▼
TAA::Process(hdrFbo, sceneTex)                        ★ Phase F.0
    │  ├─ FS_TAA: reproject(velocity) + clip(YCoCg/variance) + blend(alpha=0.92)
    │  ├─ Sharpen (Unsharp / CAS / RCAS)
    │  └─ Optional Upscale (Bicubic / Lanczos-2) — 仅 halfResHistory → full-res
    │
    ▼ Blit to sceneTex
Tonemap → Default FBO
```

**关键不变量**：
- **渲染分辨率 = 显示分辨率**（窗口尺寸）。整条管线 RT 全部按窗口尺寸创建。
- jitter 注入到 projection matrix（`±0.5 pixel` Halton-2,3 8-sample），rasterizer 在 **full-res** 下采样多帧不同位置。
- velocity 在 **full-res** 屏幕空间生成（`currentUV − previousUV`）。
- history RT 唯一可选 half-res 缩减（`Phase F.0.5 halfResHistory`，但输入 sceneTex 仍是 full-res）。
- F.0.9 Bicubic / F.0.14 Lanczos-2 upscale 仅服务于 history → sceneTex 同分辨率（`halfRes_history → full-res`）路径，**不是 render-res < output-res 的真正上采样**。

### 1.2 Phase F.0.x 后仍存在的瓶颈

1. **GPU 算力浪费**：低端机 / 移动端 1080p 全分辨率渲染压力大，无法通过降低 render scale 换帧率。
2. **画质 vs 帧率 单点选择**：用户只能改窗口尺寸（牺牲 UI 锐度）或关闭后处理（牺牲画质），缺乏 **render @ 1280×720 + display @ 1920×1080 + TAA 上采样** 这条主流方案。
3. **与现代游戏标准脱节**：DLSS / FSR2 / XeSS / TSR 已成 PC/主机基准，缺失 TAAU 让引擎在桌面与移动端都丢竞争力。
4. **行业事实**：UE4 TemporalAA Upsample / Unity HDRP TAAU / id Software TSSAA 都把 TAA 与 upsample 合二为一，**单 pass** 复用 history reproject 同时处理上采样。

### 1.3 行业参考：TAAU 实现方案对比

| 方案 | 渲染分辨率 | 上采样位置 | history 分辨率 | 优势 | 劣势 |
|---|---|---|---|---|---|
| **UE4 TAA Upsample** | 50%~100% | TAA pass 内（单 pass） | 输出分辨率（高） | 历史保真度高，单 pass 高效 | TAA shader 复杂度增加 |
| **FSR1 / Bicubic** | 任意 | TAA 之后独立 pass | 输入分辨率（低） | 实现简单，TAA 不变 | 上采样无法利用时序信息，等同于空间滤波 |
| **FSR2 / TSR** | 50%~100% | TAA pass 内（单 pass） | 输出分辨率（高） | 业界顶级质量 | 实现复杂度高 |
| **NIS (NVIDIA Image Scaling)** | 任意 | 后置 spatial pass | — | GPU 厂商现成 | 纯空间，无时序累积 |

**ChocoLight 适配选择**：**UE4 风格 TAA-Upsample 单 pass**
- history RT 在输出（高）分辨率，TAA pass 内同时 reproject + 上采样 + clip + blend
- jitter 偏移按 **render 分辨率 pixel** 计算，但映射到输出分辨率 NDC
- velocity buffer 在 render 分辨率生成，shader 内 velocity \* (renderRes / outputRes) 映射
- 完整复用现有 sharpening pipeline（Unsharp / CAS / RCAS 都是 spatial 算子，跑在输出分辨率即可）

### 1.4 当前 backend 接口差距

```cpp
// ─── 现状（@ChocoLight/include/render_backend.h） ───────────
virtual bool CreateTAAHistoryRT(int w, int h,
                                 uint32_t* fbos, uint32_t* texs);
// w, h = sceneTex 全分辨率（== window）
// 缺：(renderW, renderH, outputW, outputH) 四参分离

virtual void DrawTAAPass(uint32_t curHdrTex, uint32_t historyTex, ...,
                         int w, int h, ...);
// 缺：输入纹理 = render-res，输出 FBO = output-res；shader 需要双尺寸 uniform

virtual bool CreateHDRFBO(int w, int h, ...);
// 缺：renderW/renderH 入参 — sceneTex 应在 renderRes，但 dilation/post 仍在 outputRes
```

```cpp
// ─── HDRRenderer State（@ChocoLight/src/hdr_renderer.cpp:66+） ───────────
struct State {
    int width, height;            // 当前 == window；F.1 后需 == renderW/renderH
    // 缺：int outputW, outputH;
    uint32_t fbo;                 // HDR FBO @ render res
    uint32_t sceneTex;            // sceneTex @ render res
    // history / velocity 等 RT 跟随 sceneTex 尺寸
};
```

```cpp
// ─── TAARenderer State（@ChocoLight/src/taa_renderer.cpp:48+） ───────────
struct State {
    int width, height;            // sceneTex 尺寸（全分辨率）
    int historyW, historyH;       // history RT 尺寸（halfResHistory 时减半）
    // 缺：int renderW, renderH;          // 输入分辨率
    // 缺：int outputW, outputH;          // 输出分辨率（== history 尺寸 in TAAU）
    // 缺：float renderScale;             // ratio
    // 缺：bool taauEnabled;              // 区别于 Phase F.0 同分辨率路径
};
```

### 1.5 Phase F.0 TAA 现有 14 项参数

```
blendAlpha (0.92), neighborhoodClip (true), jitterEnabled (true),
sharpness (0.5), antiFlicker (true),
clipMode ("ycocg"), varianceGamma (1.0),
halfResHistory (false), sharpenMode ("unsharp"),
motionGamma (1.5), motionAdaptive (false),
motionSharpness (0.1), motionAdaptiveSharpness (false),
upscaleMode ("bilinear")
```

**Phase F.1 需追加**：
- `bool   taauEnabled`         （TAAU 总开关，默认 false 保兼容）
- `float  renderScale`         （0.5 / 0.667 / 0.75 / 1.0；clamp [0.5, 1.0]）
- `int    upscalePreset`       （0=Performance / 1=Balanced / 2=Quality / 3=Native，影响渲染比例预设）

### 1.6 资源开销估算（输出 1920×1080）

| 资源 | renderScale=1.0（基线 F.0） | renderScale=0.667（Balanced） | renderScale=0.5（Performance） |
|---|---|---|---|
| sceneTex (RGBA16F) | 1920×1080 × 8B = ~16.6 MB | 1280×720 × 8B = ~7.4 MB | 960×540 × 8B = ~4.1 MB |
| velocityTex (RG16F) | 1920×1080 × 4B = ~8.3 MB | 1280×720 × 4B = ~3.7 MB | 960×540 × 4B = ~2.1 MB |
| dilatedVelocityTex | 同上 | 同上（render-res） | 同上 |
| historyTex × 2 (RGBA16F) | 1920×1080 × 8B × 2 = ~33 MB | 1920×1080 × 8B × 2 = ~33 MB（**output-res 不变**） | 1920×1080 × 8B × 2 = ~33 MB |
| **rasterize 像素数** | 2.07M | 0.92M (-55%) | 0.52M (-75%) |
| **GPU 时间预期收益** | 基线 | -30%~40% | -50%~60% |

**核心权衡**：history 依然保留 output-res，只省 raster + post-process 算力，不省 history VRAM。

---

## 2. 用户原始需求

> "🥇 [最高优] Phase F.1 — TAAU (Temporal Anti-Aliasing Upsampling)
> 这是管线目前最核心、难度最大的渲染特性（类 DLSS/FSR 的空间上采样抗锯齿）。
> 背景: 目前已实现 TAA（Phase F.0），但输入分辨率与输出分辨率是 1:1 绑定的。TAAU 需要实现渲染分辨率与显示分辨率的分离。
> 核心需求:
>   1. 分辨率解耦: Render Width/Height (低分辨率) vs Output/Window Width/Height (高分辨率)。
>   2. Jitter 投影矩阵: 摄像机 Projection Matrix 必须在低分辨率下进行 sub-pixel jitter 偏移。
>   3. History Buffer 混合: 在高分辨率空间中进行历史帧的 Reprojection (重投影) 与混合。
>   4. Velocity Buffer 缩放: 运动向量 (Velocity) 需要正确映射到高分辨率空间以获取准确的历史像素。
>   5. 锐化 (RCAS/Unsharp): 必须在 TAAU Resolve 之后的高分辨率空间执行。"

**关键词解析**：

| 用户原文 | 解析 |
|---|---|
| "DLSS/FSR 的空间上采样抗锯齿" | 业界标准方案 — 单 pass TAA + upsample |
| "1. 分辨率解耦" | 核心架构改造 — HDRRenderer / TAARenderer / Backend 都需引入 (renderW/H, outputW/H) 双对 |
| "2. Jitter 投影矩阵在低分辨率下" | jitter offset 仍然按 ±0.5 pixel 但是是 **render res pixel**，NDC 偏移按 render 尺寸折算 |
| "3. History Buffer 混合在高分辨率" | history RT 维持 output-res，每帧 cur(low) → reproject → blend(high) |
| "4. Velocity Buffer 缩放" | velocity 在 render res 生成（`currUV − prevUV` 都是 render UV），但 shader 在 output UV 空间运行，需 `prevUV_output = (vUV_output * (renderRes/outputRes)) − velocity` 之类的映射 |
| "5. 锐化在 TAAU Resolve 之后的高分辨率空间" | Sharpen pipeline 不变（已在 sceneTex 全分辨率工作），TAAU 后 sceneTex 已是 output-res，零改动 |

**核心拆解**：5 项需求归结为 **2 件事**：
1. **采样源分辨率拆分**：sceneTex（render-res）vs historyTex（output-res）
2. **TAA shader 在 output-res 空间运行**：每个 output 像素 → 反查 render-res sceneTex 的 jittered 采样 + reproject 上一帧 output-res history

---

## 3. 边界确认

### 3.1 在范围内（Phase F.1 必交付）

| 项 | 内容 |
|---|---|
| Backend 接口 | `CreateHDRFBO` 增 4 入参（renderW/H, outputW/H 拆分；现有 `w, h` 入参映射到 renderW/H） |
| Backend 接口 | `CreateTAAHistoryRT` 现有 `(w, h)` 解释为 outputW/H（兼容） |
| Backend 接口 | `DrawTAAPass` 增 2 参（renderW, renderH）—— 输出分辨率沿用 `(w, h)` |
| Shader | `FS_TAA` 增 4 uniform（uRenderRes, uOutputRes, uRenderToOutputUV, uOutputToRenderUV）+ jitter 解码到 render-pixel |
| Shader 修改 | shader 内 cur 采样按 `vUV * uRenderToOutputRatio` 映射回 render-res，velocity 同理 |
| HDRRenderer | State 增 outputW/H + renderScale；CreateRT/RebuildRT 双尺寸路径 |
| HDRRenderer | sceneTex / velocityTex / normalTex / depth 在 render-res；history / dilation 在 output-res |
| TAARenderer | State 增 renderW/H + outputW/H + taauEnabled + renderScale + upscalePreset；ApplyJitter 用 render 尺寸折算 NDC |
| TAARenderer | Process 决策路径：taauEnabled=false → 退回 F.0 同分辨率路径；taauEnabled=true → 双尺寸路径 |
| TAARenderer | 7 新参数对（SetTAAUEnabled / SetRenderScale / SetUpscalePreset + getter） |
| Lua API | `Light.Graphics.TAA.SetTAAUEnabled / GetTAAUEnabled` |
| Lua API | `Light.Graphics.TAA.SetRenderScale / GetRenderScale`（clamp [0.5, 1.0]） |
| Lua API | `Light.Graphics.TAA.SetUpscalePreset / GetUpscalePreset`（"performance"/"balanced"/"quality"/"native"） |
| smoke | TAA smoke 增 +6~8 检查点 |
| demo | `demo_taau` 新建（拆自 demo_ssr / demo_taa_split2），按键 R/T/Y 切 renderScale 0.5/0.667/0.75/1.0 |
| docs | 7 件套（ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO） |

### 3.2 不在范围（Phase F.1+ / 后续）

| 项 | 说明 |
|---|---|
| **DLSS / FSR2 算法移植** | F.1 是 UE4 TAAU 风格，不是 NN 重建 / FSR2 多采样 lock；留 Phase F.2+ |
| **MipMap LOD bias 调整** | TAAU 标准做法是 `mipBias = log2(renderScale)`，但 ChocoLight 当前 mesh shader 未暴露 LOD bias，留独立 phase |
| **Variable Rate Shading (VRS)** | 与 TAAU 互补但完全独立；留 Phase F.3 |
| **Dynamic Resolution Scaling (DRS)** | 按帧率自适应改 renderScale，需要 frame time 反馈环；留 Phase F.4 |
| **Output > 4K HDR 性能优化** | 8K render path / large texture handling 不属于 F.1 |
| **TAAU 前的 G-buffer 解析** | 当前是 forward + post-process，无 deferred pass，无需 G-buffer resolve |
| **MSAA 兼容** | TAA 与 MSAA 互斥（业界共识），引擎当前也无 MSAA path |

### 3.3 兼容性

- **向后兼容**：默认 `taauEnabled = false`，所有 F.0 demo 行为完全不变（同分辨率路径）。
- **Legacy backend**：`SupportsTAA = false` 时 `taauEnabled` 永远 false（IsSupported gate）。
- **多 Instance**：每 instance 独立 renderScale，支持 split-screen 不同区域不同 render scale（Player1 高画质 / Player2 低画质）。
- **HDR 联动**：`HDR.Enable(w, h)` 入参解释为 **outputW/H**；`HDR.Resize(w, h)` 同；renderW/H 由 TAA.renderScale 自动推导。
- **MotionBlur / SSR / Bloom / SSAO**：保持工作在 render-res（sceneTex），无任何感知改动。
- **Tonemap**：保持工作在 sceneTex，但 sceneTex 在 TAAU resolve 后已经是 output-res（FS_TAA 输出 写到 output-res sceneTex），所以 Tonemap 自然在 output-res 工作。

### 3.4 性能验收

- 1080p output @ 0.667 renderScale (1280×720 render) 在中端 GPU（GTX 1660 / RTX 3060）相比 F.0 同分辨率应有 **+30%~50% 帧率**。
- 0.5 renderScale (960×540 render) 应有 **+50%~80% 帧率**，但接受 sub-pixel ghosting 略增。
- 上述指标由用户在真机执行（CI 无 GPU 不参与性能验收）。

---

## 4. 需求理解（对现有项目）

### 4.1 TAA 管线插入点（推荐方案：单 pass TAA-Upsample，UE4 风格）

```
Camera (proj 注入 jitter, NDC 偏移按 renderRes 折算)
    │
    ▼
Rasterize @ render-res (e.g. 1280×720)
    │
    ▼
HDR FBO @ render-res:
    sceneTex (RGBA16F 1280×720)
    velocityTex (RG16F 1280×720)
    normalTex (RG16F 1280×720)
    depth (D24 1280×720)
    │
    ▼
Bloom / SSAO / SSR / VelocityDilate / MotionBlur 全部 @ render-res
    │
    ▼ TAAU resolve (单 pass!)
TAARenderer::Process(hdrFbo @ render-res, sceneTex @ render-res):
    │  Input:  sceneTex (render-res), history (output-res), velocity (render-res)
    │  Output: history (output-res, 双 ping-pong)
    │  Shader: FS_TAA
    │      for each output pixel (vUV in output-UV):
    │          renderUV = vUV;  // shader 内全 [0,1] UV 共享，纹理采样自动按尺寸映射
    │          cur = texture(uCurTex, renderUV);  // sceneTex 自动 bilinear filter (render-res)
    │          velocity = texture(uVelocityTex, renderUV);  // 在 render-UV 空间
    │          prevUV = vUV - velocity;  // velocity 是 UV-space delta，directly applicable to output-UV
    │          hist = texture(uHistoryTex, prevUV);  // history 是 output-res
    │          neighborhood clip 按 render-res texel (uRenderTexel)
    │          blend = mix(cur, hist, alpha)
    ▼
Sharpen (Unsharp / CAS / RCAS) @ output-res
    │
    ▼ Blit to sceneTex
sceneTex 重分配为 output-res (or 直接 ResolveToFinalFBO)
    │
    ▼
Tonemap @ output-res → Default FBO
```

**关键洞察**：
- velocity 是 **UV-space delta**（不是 pixel delta），所以 render-res 与 output-res shader 都用同一个 velocity 纹理无需缩放。
- jitter 由 raster 阶段注入到 projection；TAA shader 不再"反向 jitter"——neighborhood clip 自然平滑掉 jitter aliasing。
- cur 采样必须 `texture(uCurTex, vUV)`（bilinear），因为 vUV 在 output-res 但 sceneTex 在 render-res，**bilinear 自动完成 raw upsample**；history reproject 走 8/9-tap clip 抑制 ghosting，最终 blend 完成 supersample 效果。

### 4.2 Jitter 数学（render-res 的 ±0.5 pixel）

```cpp
// Phase F.0 原版 (full-res):
//   jitterX/Y = kHaltonJitter[frame%8];  // ±0.5 pixel
//   ndcOffX = jitterX * 2.0 / width;     // width = output-res
//   jitteredProj[8] += ndcOffX;
//   jitteredProj[9] += ndcOffY;

// Phase F.1 TAAU (render-res):
//   jitterX/Y = kHaltonJitter[frame%8];  // ±0.5 render pixel
//   ndcOffX = jitterX * 2.0 / renderW;   // renderW = render-res
//   ndcOffY = jitterY * 2.0 / renderH;
//   jitteredProj[8] += ndcOffX;
//   jitteredProj[9] += ndcOffY;
//   // raster 在 render-res viewport 下用此 jittered proj
```

**为什么 ±0.5 pixel 是 render-res 而非 output-res**：
- raster 在 render-res 下进行，jitter 必须落在 render 像素 sub-pixel 才能让多帧采样位置错开（标准 TAA 不变量）。
- 折算到 output-res 时，1 个 render pixel = (output/render) 个 output pixel。例如 0.5 render scale 下 1 render pixel = 2 output pixel，jitter ±0.5 render = ±1.0 output。
- 这正是 TAAU 的"信息富集"原理：output 一个像素融合 4-8 帧 render-res sub-pixel 采样，等效于 super-sample。

### 4.3 Velocity 采样数学

```glsl
// FS_TAA shader, Phase F.1 TAAU 路径
in vec2 vUV;  // output-UV [0, 1]

vec2 SampleVelocity(vec2 uv) {
    // velocityTex 是 render-res，但 vUV 是 output-UV
    // bilinear filter 自动处理 [0,1]×[0,1] 采样,
    // 输出仍然是 UV-space delta（无量纲），可直接 prevUV = vUV - velocity
    return DecodeVelocity(texture(uVelocityTex, uv).rg);
}
```

**核心要点**：
- velocity 在 render-res rasterize 时计算 `currUV − prevUV`（都是 render-UV 空间），但 render-UV == output-UV（都是 [0,1] 归一化），所以 **velocity 的数值在 render/output 之间通用**。
- 这是 UV-space velocity 优于 pixel-space velocity 的根本原因（Phase E.13 选 UV-space 是有先见之明）。

### 4.4 Neighborhood Clip 邻域采样（render-res）

```glsl
// Phase F.0 原版（output-res 单尺寸）:
vec2 uTexel;  // 1.0 / outputRes
vec3 s = texture(uCurTex, vUV + uTexel * vec2(-1, -1)).rgb;
// 邻域是 output 像素邻居 == render 像素邻居（同分辨率）

// Phase F.1 TAAU (render-res 邻域):
vec2 uRenderTexel;  // 1.0 / renderRes (render 像素 texel size, output-UV 空间下更大)
vec3 s = texture(uCurTex, vUV + uRenderTexel * vec2(-1, -1)).rgb;
// 邻域必须按 render-res 像素步进, 才覆盖 cur 的实际离散采样点
```

**易错点**：邻域偏移 `uTexel` 在 F.0 既是 cur 采样邻域也是 history 采样邻域，在 F.1 中两者**分离**：
- cur 邻域：`uRenderTexel = 1.0 / renderRes`（覆盖 cur 实际离散像素）
- history 邻域（如 8-tap clip 用 history neighbors）：`uOutputTexel = 1.0 / outputRes`

### 4.5 渲染分辨率计算（避免奇偶不齐导致 1-pixel 偏差）

```cpp
int ComputeRenderRes(int outputRes, float renderScale) {
    int r = (int)std::lround(outputRes * renderScale);
    return std::max(1, r);
}
// 例: output=1080, scale=0.667 → render=720
// 例: output=1920, scale=0.667 → render=1280
// 例: output=1080, scale=0.5  → render=540
```

**对齐**：UE4 / Unity 默认不强制偶数对齐（depth resolve 不依赖偶数尺寸），但 Phase E.18 dilation half-res RT 用了 `(w+1)/2`，TAAU 同样不需要强制偶数。

---

## 5. 用户视角期望

| 期望 | 验收手段 |
|---|---|
| 帧率提升 | demo_taau 真机测试（GTX 1660 / RTX 3060），renderScale 0.667 vs 1.0 帧率对比 |
| 画面 "近似 native" | 默认 renderScale=0.667 + RCAS sharpening，肉眼可接受（业界 "Quality" 档位） |
| 极限性能档可用 | renderScale=0.5 在中低端机仍可用，可见 ghost 但不破坏游戏体验 |
| API 简洁 | 仅 3 对 setter/getter（TAAUEnabled / RenderScale / UpscalePreset），其他 14 项 F.0 参数零改动复用 |
| 多 Instance 独立 | split-screen 各 player 独立 renderScale |
| smoke 全绿 | CI 6/6 green |
| F.0 demo 零回归 | 默认 TAAUEnabled=false，所有 demo_*  画面 / 性能完全一致 |

---

## 6. 算法设计预览

### 6.1 双尺寸 RT 管理

```cpp
// HDRRenderer State (Phase F.1)
struct State {
    int outputW, outputH;       // 窗口尺寸（用户 Enable 入参）
    int renderW, renderH;       // = lround(output * renderScale)
    float renderScale;          // 来自 TAARenderer 的 GetRenderScale (跨模块查询)
    // RT 全部按 renderW × renderH:
    //   sceneTex (RGBA16F)
    //   velocityTex (RG16F)
    //   normalTex (RG16F)
    //   depth (D24)
    // RT 按 outputW × outputH:
    //   dilatedVelocityTex   (output-res, 因为 TAA 在 output 空间用)
    //   dilatedCameraVelocityTex
    //   tonemapped sceneTex (TAA 输出后)
};

// TAARenderer State (Phase F.1)
struct State {
    int outputW, outputH;       // history 尺寸 == output
    int renderW, renderH;       // velocity / sceneTex 尺寸 == render
    float renderScale;          // [0.5, 1.0]
    int upscalePreset;          // 0=Perf / 1=Bal / 2=Qual / 3=Native
    bool taauEnabled;           // 默认 false (兼容)
    // history RT 全部 output-res
    uint32_t historyFbos[2], historyTexs[2];
};
```

### 6.2 渲染分辨率切换流程

```
User: TAA.SetRenderScale(0.667)
    │
    ▼
TAARenderer 重新计算 renderW = lround(outputW * 0.667), renderH 同
    │
    ▼
HDRRenderer.OnTAARenderScaleChanged(renderW, renderH)
    │  Destroy sceneTex / velocityTex / normalTex / depth
    │  Recreate at (renderW, renderH)
    │  保留 dilatedVelocityTex (在 output-res, 但需 dilation 改读 render-res velocity 写 output-res?)
    │  → 简化: dilation 也在 render-res, TAA 时 bilinear 读到 output-res
    ▼
TAARenderer Reset (history 清, hasHistory=false, frameCounter 归零)
    │
    ▼
下一帧 raster 在新 renderW × renderH
```

**Dilation 处理细节**：dilation 在 render-res 还是 output-res？
- 选 A: render-res（dilatedVelocityTex 与 raw 同尺寸） — TAA shader bilinear 读取，接受 1-pixel 误差
- 选 B: output-res — dilation pass 顺便上采样，TAA shader 直接 fetch 同尺寸 — 增加 dilation pass 算力但无误差
- **推荐 A**: 与 raw velocity 同尺寸已是当前架构，改动最小。1-pixel 误差由 history clip 吸收。

### 6.3 sceneTex 双角色

`sceneTex` 在 Phase F.0 是 "TAA 输入 + Tonemap 输入"（同 RT）。Phase F.1 拆分：
- **sceneTex_render** (render-res)：raster + Bloom/SSR/SSAO/MB 写入；TAA 读取作为 cur input
- **sceneTex_output** (output-res)：TAA 写入；Sharpen + Tonemap 读取

**实现方式**：在 HDRRenderer 内增加 `outputSceneTex` (output-res, 仅 TAA 用)：
- TAA disabled (taauEnabled=false): outputSceneTex 不分配，pipeline 同 F.0
- TAA enabled (taauEnabled=true): outputSceneTex 分配，TAA 输出 blit 到 outputSceneTex；Sharpen 改读 outputSceneTex；Tonemap 改读 outputSceneTex

---

## 7. 集成期望

### 7.1 与 F.0 各子 phase 联动

| F.0.x 特性 | F.1 行为 |
|---|---|
| F.0.1 Sharpen (Unsharp) | 跑在 output-res sceneTex（TAAU 之后），无改动 |
| F.0.2/F.0.3 ClipMode YCoCg/Variance | shader 内 cur 邻域用 uRenderTexel，逻辑同 F.0 |
| F.0.4 AntiFlicker (Karis) | luma 加权与分辨率无关，无改动 |
| F.0.5 HalfResHistory | TAAU 模式下 **强制关闭** —— history 必须 output-res，否则 reproject 失意义 |
| F.0.6 CAS Sharpen | 同 F.0.1，跑在 output-res |
| F.0.8 MotionAdaptive Gamma | velocity 长度在 render-res 计算，但传入 shader 后量纲一致，无改动 |
| F.0.9 Bicubic Upscale | TAAU 模式下 **变成 dead code** —— 因为已经是 output-res 输入到 sharpen，不需要再 upscale。F.0.9 路径仅在 halfResHistory=true 时激活，TAAU 关闭它。 |
| F.0.12 RCAS Sharpen | 同 F.0.1, F.0.6 |
| F.0.13 MotionAdaptiveSharpness | 无改动 |
| F.0.14 Lanczos-2 Upscale | 同 F.0.9，TAAU 模式下 dead code |

**冲突**：F.0.5 + F.1 不可同时启用。冲突仲裁规则：`SetTAAUEnabled(true)` 时若 `halfResHistory=true` 则强制改为 false 并 log warning。

### 7.2 与多 Instance 联动

每个 TAA instance 独立持有：
- `renderScale` / `taauEnabled` / `upscalePreset`
- 独立 history RT (output-res，注意：output 仍来自全局 HDRRenderer 的 outputW/H)
- jitter state / frameCounter

split-screen 4 player 各自渲染 1/4 屏区域，每 player 可独立 renderScale：
- Player1 高画质：renderScale=1.0（窗口 1/4 = 480×270 render = 480×270 output）
- Player2 低画质：renderScale=0.5（render 240×135 → upscale 480×270）

### 7.3 性能预算

| 开销项 | 1080p Native (F.0) | 1080p TAAU 0.667 | 1080p TAAU 0.5 |
|---|---|---|---|
| Raster | 100% | 44% | 25% |
| Post-process | 100% | 44% | 25% |
| TAA shader | 100% | 100%（output-res 不变） | 100% |
| Sharpen | 100% | 100% | 100% |
| Tonemap | 100% | 100% | 100% |
| **总 GPU 时间** | 100% | ~60-70% | ~40-55% |
| VRAM | 100% | ~75% | ~70% |

**目标**：renderScale=0.667 节省 30-40% GPU 时间, renderScale=0.5 节省 45-60%，与业界 FSR2 Quality / Performance 基准一致。

---

## 8. 疑问澄清（关键决策点）

### Q1：TAA-Upsample 单 pass 还是分离 TAA + Upsample 两 pass？

| 选项 | 优势 | 劣势 |
|---|---|---|
| **A（推荐）** TAA-Upsample 单 pass（UE4 风格） | history 在 output-res，reproject 直接吃 high-frequency 信息，质量最高 | shader 复杂度增加 |
| B 分离 TAA(render-res) + Upsample(render → output) 两 pass | 实现简单（复用 F.0.9 Bicubic） | history 在 low-res，反复 upsample 损失细节，等同于 FSR1（弱于 FSR2） |

**推荐 A**：与用户原文 "history buffer 混合在高分辨率空间" 完全对齐；UE4/Unity HDRP/TSR 都是单 pass。

### Q2：renderScale 取值范围

| 选项 | 范围 | 备注 |
|---|---|---|
| **A（推荐）** [0.5, 1.0] | 业界标准 50%~100% | 兼容 FSR2 全系预设 |
| B [0.33, 1.0] | 极限性能档（FSR2 Ultra Performance 0.33） | 0.33 下 ghost 显著，引擎质量目标偏高，暂不开 |
| C [0.5, 2.0] | 含 super-sample 倒过来 (DSR) | 与 TAAU 主题不符，留独立 phase |

**推荐 A**：0.5~1.0 覆盖 95% 用例。0.33 留 Phase F.1.x 增量。

### Q3：UpscalePreset 预设与 renderScale 关系

| 选项 | 含义 |
|---|---|
| **A（推荐）** 4 档独立 API: "performance"=0.5 / "balanced"=0.667 / "quality"=0.75 / "native"=1.0；preset 调用时同步 renderScale | 匹配 FSR2/DLSS UI 命名，用户友好 |
| B 仅 renderScale，不暴露 preset | API 极简但用户需自己记忆 0.667 数字 |
| C 4 档外加 "ultra_performance"=0.33 / "custom" | 等 F.1.x 再加 |

**推荐 A**：3 行额外代码即可，UI 友好。`SetUpscalePreset("balanced")` 等价 `SetRenderScale(0.667)`。

### Q4：默认 TAAUEnabled

| 选项 | 推荐场景 |
|---|---|
| **A（推荐）** 默认 false | 与 SSR/Bloom/MotionBlur autoEnable=false 风格一致；保兼容性，老 demo 零回归 |
| B 默认 true | TAAU 是新默认体验，但风险高（多 demo 需重新调参） |

**推荐 A**：F.0 已交付，TAAU 是 opt-in 增量。

### Q5：F.0.5 HalfResHistory 与 TAAU 冲突仲裁

| 选项 | 行为 |
|---|---|
| **A（推荐）** 启用 TAAU 时强制关闭 HalfResHistory + log warning + GetHalfResHistory() 返 false | 自动避免冲突 |
| B 拒绝 SetTAAUEnabled(true) 当 halfResHistory=true | 用户体验差 |
| C 允许同时启用，行为未定义 | 不可接受 |

**推荐 A**：冲突自动消除。

### Q6：F.0.9/F.0.14 Upscale Mode 在 TAAU 路径的角色

| 选项 | 行为 |
|---|---|
| **A（推荐）** TAAU 模式下 upscaleMode 不影响（dead code path） | history 已是 output-res，无需 upscale；保留 API 但效果 = bilinear |
| B TAAU 模式下用 Bicubic / Lanczos 替代 history bilinear filter | shader 内 history 采样改用 Bicubic 9-tap，质量+，性能− |

**推荐 A**：F.1.0 keep simple。B 留 F.1.1 增量。

### Q7：Mipmap LOD bias 调整

| 选项 | 行为 |
|---|---|
| A 自动 `mipBias = log2(renderScale) ≈ -0.585 @ 0.667` | 标准 TAAU 做法，纹理细节锐 |
| **B（推荐）** 不做（留独立 phase F.1.1） | 当前 mesh shader 未暴露 LOD bias uniform；改造影响范围远超 TAAU |

**推荐 B**：F.1.0 不引入 mesh shader 改动，保持核心范围。

### Q8：Resize 时机

| 选项 | 时机 |
|---|---|
| **A（推荐）** outputResize → 重建 sceneTex（render-res）+ history（output-res） | 用户改 window，自动重建全部 |
| B renderScale 改变 → 仅重建 sceneTex；history 保留 | history 与 render 解耦，理论上 history 可不重建。但 sceneTex 重建后 hasHistory=false 强制首帧 cur，等价于重建 |

**推荐 A**：实现路径与 F.0 一致，避免 edge case。

---

## 9. 行业参考

| 引擎 / 论文 | 渲染比例 | history 分辨率 | 上采样位置 | LOD bias |
|---|---|---|---|---|
| UE4 TAA Upsample | 50%~100% | output-res | TAA 单 pass | `log2(scale)` |
| Unity HDRP TAAU | 50%~100% | output-res | TAA 单 pass | `log2(scale)` |
| FSR2 (AMD) | 50%~100% | output-res | TAA 单 pass | `log2(scale) - 0.7` |
| TSR (UE5) | 33%~100% | output-res | TAA 单 pass | 自适应 |
| DLSS (NVIDIA) | 33%~100% | output-res | NN 单 pass | 自适应 |
| FSR1 (AMD) | 任意 | 不参与 | 后置 spatial | 无 |

**ChocoLight Phase F.1**：对齐 UE4/Unity 风格 — 50%~100% + output-res history + TAA 单 pass + 不动 LOD bias（F.1.1 增量）。

---

## 10. 约束

### 10.1 技术约束

- **GLES3 兼容**：所有 shader 双 profile（GL3.3 + GLES3.0），无 compute shader
- **零 mesh shader 改动**：F.1 不动 forward 3D shader（LOD bias 留 F.1.1）
- **零 RT 格式改动**：sceneTex 仍 RGBA16F，velocityTex 仍 RG16F/RG8（不强制 RG16F）
- **后端无关**：所有 GL 操作经 RenderBackend 虚接口
- **Backend 接口**：CreateHDRFBO / CreateTAAHistoryRT / DrawTAAPass 必须保留向后兼容签名（默认参数）

### 10.2 代码约束

- 与 Phase F.0 风格一致：State 字段、setter/getter 模式、Process 内部自检模式、autoEnable 模式
- shader 命名 `FS_TAA` 沿用单文件（增 4 uniform，不分裂为 FS_TAAU）
- Lua API 命名 `SetTAAUEnabled / SetRenderScale / SetUpscalePreset`（与 F.0 SetXxx 风格一致）

### 10.3 测试约束

- smoke 不能依赖真实渲染结果（headless 无图像 GT），仅测 API surface + 默认值 + clamp + round-trip + preset/scale 双向同步
- demo 视觉 + 性能验收由用户在真机执行
- multi-instance 测试：4 instance 各自独立 renderScale 互不影响

---

## 11. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|
| sceneTex / history / velocity 三尺寸混乱导致 1-pixel 偏差 | 中 | 中 | shader 内严格区分 uRenderTexel / uOutputTexel；CPU 端 ComputeRenderRes 用 lround |
| HDRRenderer 重建路径破坏现有 multi-HDR-instance | 中 | 高 | 跨 instance 共用 outputW/H，每 instance 独立 renderW/H + sceneTex；分阶段实现先单 instance |
| velocity 在 render-res 但 dilation 在 output-res 导致映射错误 | 中 | 中 | dilation 锁定与 raw velocity 同尺寸（render-res），TAA shader bilinear 读取 |
| F.0.5 HalfResHistory + TAAU 冲突 | 高 | 低 | Q5 仲裁：自动关 HalfResHistory + warning |
| F.0.9/F.0.14 Upscale dead code 混淆 | 低 | 低 | TAAU 文档明示 upscaleMode 在 TAAU 模式下无效 |
| 多 instance × renderScale 组合复杂度 | 中 | 中 | 默认 instance 不启用 TAAU，user instance 按需启用；smoke 覆盖 4 instance 不同 renderScale |
| 移动端 (GLES3) Mali / Adreni 性能拐点 | 中 | 低 | renderScale=0.5 测试，确认无 GLES 特定 bug |
| Tonemap 误读 sceneTex_render（应读 sceneTex_output） | 中 | 高 | HDRRenderer 内统一 GetSceneTexForOutput() helper, TAAU 路径返 sceneTex_output, 否则 sceneTex |

---

## 12. 验收标准（预案）

### 12.1 功能验收

- [ ] Backend 接口扩展 0 编译 error，向后兼容（无 default 参数破坏）
- [ ] FS_TAA shader 双 profile 增 4 uniform 编译通过（CI 6/6 green）
- [ ] HDRRenderer State 双尺寸 + outputSceneTex 路径，taauEnabled=false 时画面与 main HEAD 完全一致
- [ ] TAARenderer State + 3 setter/getter + ApplyJitter 用 render-res NDC + Process 双路径
- [ ] Lua API 6 新函数注册到 taa_funcs[]
- [ ] smoke 增量 6-8 检查点全过
- [ ] demo_taau 真机：renderScale 0.5/0.667/0.75/1.0 切换无 crash
- [ ] HUD 显示 render-res / output-res / renderScale 实时

### 12.2 质量验收（用户真机）

- [ ] renderScale=0.75 (Quality) 视觉接近 native，肉眼难辨差异
- [ ] renderScale=0.667 (Balanced) 性能-30%~40%，画质轻微 ghost 但可接受
- [ ] renderScale=0.5 (Performance) 性能-50%~60%，明显 ghost 但游戏可玩
- [ ] 镜头快速旋转时 history reject 工作，无残影
- [ ] 静态画面收敛快（8 帧≈140ms @60fps）

### 12.3 兼容性验收

- [ ] 默认 TAAUEnabled=false，所有 demo_*  画面 / 性能完全一致（视觉 0 回归）
- [ ] HalfResHistory + TAAU 冲突自动仲裁（Q5）
- [ ] Multi-instance 4 player 独立 renderScale 工作
- [ ] HDR.Resize → TAA.Resize → renderRes 自动重算

### 12.4 文档验收

- [ ] ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO 全 7 件
- [ ] API_REFERENCE.md TAA 函数数 +6
- [ ] demo_taau/README.md 键位 + renderScale 描述
- [ ] CHANGELOG / 主 README 入口

### 12.5 CI 验收

- [ ] 6 平台 build success（windows / linux / macos / android / ios / web）
- [ ] Windows runtime smoke `[Phase F.1] 通过 N / 失败 0`
- [ ] commit 简洁（5-10 commits 拆分：Backend / HDR / TAA / Lua / Demo / Docs）

---

## 13. 需用户拍板的最终问题

**Q1**：单 pass TAA-Upsample 还是分离两 pass → **推荐 A：单 pass（UE4 风格）**
**Q2**：renderScale 范围 → **推荐 A：[0.5, 1.0]**
**Q3**：UpscalePreset 4 档（performance/balanced/quality/native）→ **推荐 A：暴露 + 同步 renderScale**
**Q4**：默认 TAAUEnabled → **推荐 A：false（兼容优先）**
**Q5**：F.0.5 HalfResHistory + TAAU 冲突仲裁 → **推荐 A：自动关 + warning**
**Q6**：F.0.9/F.0.14 Upscale 在 TAAU 路径 → **推荐 A：dead code（不混合）**
**Q7**：Mipmap LOD bias → **推荐 B：留 F.1.1（不动 mesh shader）**
**Q8**：Resize 路径 → **推荐 A：outputResize 触发全重建**

**全推荐组合 = UE4 风格单 pass TAAU，[0.5, 1.0] 标准范围，4 档预设，默认关闭，最小入侵 mesh shader**。

需用户拍板：**接受全推荐？还是修改某项？**

---

## 14. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v0.1 | 2026-05-17 | 初稿 — 项目上下文 + 8 决策点 + 推荐组合 + 风险评估 |

---

**下一步**：用户拍板 Q1-Q8 → 写 CONSENSUS_PhaseF_1.md 锁定方案 → 进入阶段 2 Architect（DESIGN_PhaseF_1.md 已先行起草，CONSENSUS 后微调即可）。
