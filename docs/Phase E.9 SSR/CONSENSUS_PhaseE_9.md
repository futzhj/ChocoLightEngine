# CONSENSUS — Phase E.9 · Screen Space Reflection (SSR)

> 6A 工作流 · 阶段 1 · Align (对齐阶段) · 最终共识
> 关键决策已与用户确认 → 进入 Architect 阶段

---

## 0. 用户拍板的方案

**高质量方案** — 用户于 2026-05-12 12:13 选定：

> full-res 反射 + 64 步 ray march + SSR 插在 Bloom 之前 + GetReflectionTexId 调试 API
> FPS 影响较大，适合高端 PC

---

## 1. 明确的需求描述

### 1.1 核心需求

实现 **Screen Space Reflection (SSR)** 后处理通道，作为 Phase E.8.x G-buffer normal 升级的自然延续：

- **复用 Phase E.8.x 的 G-buffer normal MRT** → 无需新增 G-buffer
- **沿用 SSAO/Bloom 的模块模板** → namespace + State + 标准生命周期
- **Lua API `Light.Graphics.SSR.*`** → 与 SSAO 同风格
- **集成到 HDR EndScene 管线** → SSR 在 Bloom 之前执行

### 1.2 验收标准（具体可测试）

| 验收项 | 度量 | 通过标准 |
|--------|------|----------|
| **编译** | MSVC 2022 Release | 0 error, 0 new warning |
| **Lua API 完整性** | smoke 检查 | 13 个 fn 全部存在 |
| **参数 round-trip** | smoke 检查 | 7 对 Set/Get 全部值匹配 (epsilon 1e-4) |
| **参数 clamp 边界** | smoke 检查 | 越界值被正确截断 |
| **Lifecycle 幂等性** | smoke 检查 | Disable() 重复调用不 crash |
| **HDR 联动** | smoke 检查 | autoEnable=true + HDR.Enable 自动拉起 SSR |
| **Silent fallback** | smoke 检查 | 后端不支持 / G-buffer normal 缺失时 SSR.Enable 返回 false |
| **GetReflectionTexId** | smoke 检查 | HDR off 返回 0；HDR on 返回 RT id |
| **回归测试** | scripts/smoke 运行 | hdr/bloom/ssao/mesh_3d/graphics 全 PASS |
| **demo 场景** | samples/demo_ssr/main.lua | 金属球反射地面纹理可见 |
| **性能** | demo 1080p | full-res 64 步在中端 GPU (GTX 1660) ≥ 30 FPS |

---

## 2. 技术实现方案

### 2.1 算法选择

**Linear ray march in view space**：

```glsl
// FS_SSR 核心算法 (双 profile: GL33 + GLES3)
void main() {
    vec2 uv = gl_FragCoord.xy * uTexelSize;          // [0, 1] x [0, 1]
    float depth = texture(uDepthTex, uv).r;
    if (depth >= 1.0) discard;                        // 天空盒不反射

    // 重建 view-space pos
    vec3 viewPos = ReconstructViewPos(uv, depth, uInvProj);

    // 解码 view-space normal
    vec2 nEnc = texture(uNormalTex, uv).rg;
    vec3 viewN = DecodeViewNormal(nEnc);

    // 反射方向
    vec3 viewV = normalize(-viewPos);
    vec3 viewR = reflect(-viewV, viewN);

    // 自反射剔除: 法线背向相机时跳过
    if (dot(viewN, viewV) < 0.05) discard;

    // ray march
    vec3 reflectColor = vec3(0.0);
    float reflectAlpha = 0.0;

    for (int i = 1; i <= uMaxSteps; ++i) {
        vec3 samplePosVS = viewPos + viewR * (uStepSize * float(i));
        if (-samplePosVS.z > uMaxDistance) break;     // 距离上限

        vec4 sampleClip = uProj * vec4(samplePosVS, 1.0);
        vec2 sampleUV = sampleClip.xy / sampleClip.w * 0.5 + 0.5;

        // 屏外 -> 边缘 fade + break
        if (sampleUV.x < 0.0 || sampleUV.x > 1.0 || sampleUV.y < 0.0 || sampleUV.y > 1.0) {
            break;
        }

        float sampleDepth = texture(uDepthTex, sampleUV).r;
        vec3 sampleVS = ReconstructViewPos(sampleUV, sampleDepth, uInvProj);

        float depthDiff = sampleVS.z - samplePosVS.z;
        if (depthDiff > 0.0 && depthDiff < uThickness) {
            // 命中: 采样 HDR + 边缘 fade
            float edgeFade = smoothstep(0.0, uEdgeFade, sampleUV.x)
                           * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.x)
                           * smoothstep(0.0, uEdgeFade, sampleUV.y)
                           * smoothstep(0.0, uEdgeFade, 1.0 - sampleUV.y);
            reflectColor = texture(uHDRTex, sampleUV).rgb;
            reflectAlpha = edgeFade;
            break;
        }
    }

    FragColor = vec4(reflectColor, reflectAlpha);
}
```

### 2.2 模块结构（沿用 SSAO 模板）

```cpp
// include/ssr_renderer.h
namespace SSRRenderer {
    // 生命周期
    void Init(RenderBackend*);
    void Shutdown();
    bool Enable(int w, int h);
    void Disable();
    bool IsEnabled() / IsSupported();
    bool Resize(int w, int h);

    // HDR 联动
    void OnHDREnabled(int w, int h);
    void OnHDRDisabled();
    void OnHDRResized(int w, int h);
    void SetAutoEnable(bool flag);
    bool GetAutoEnable();

    // 参数 (7 对 setter/getter)
    void  SetMaxSteps(int n);       int   GetMaxSteps();          // [8, 128], 默认 64
    void  SetStepSize(float v);     float GetStepSize();          // [0.01, 1.0], 默认 0.1
    void  SetThickness(float v);    float GetThickness();         // [0.01, 5.0], 默认 0.5
    void  SetMaxDistance(float v);  float GetMaxDistance();       // [1.0, 1000.0], 默认 50.0
    void  SetIntensity(float v);    float GetIntensity();         // [0.0, 2.0], 默认 0.7
    void  SetEdgeFade(float v);     float GetEdgeFade();          // [0.0, 0.5], 默认 0.1

    // 调试
    uint32_t GetReflectionTexId();   // 返回 RGBA16F 反射 RT id (0 = 未启用)

    // 管线调用
    void Process(uint32_t hdrFbo, uint32_t hdrTex);
}
```

### 2.3 Backend 接口扩展（5 个 virtual fn）

```cpp
// include/render_backend.h (附加在 SSAO section 之后)
virtual bool SupportsSSR() const { return false; }

/// SSR 反射 RT: full-res RGBA16F + linear filter, no depth
virtual bool CreateSSRTargets(int /*w*/, int /*h*/,
                               uint32_t* /*outFbo*/, uint32_t* /*outTex*/) {
    return false;
}
virtual void DeleteSSRTargets(uint32_t* /*fbo*/, uint32_t* /*tex*/) {}

/// SSR raw pass: depthTex + normalTex + hdrTex -> dstFbo
/// @param maxSteps  [8, 128] linear march iterations
/// @param stepSize  view-space units per step
/// @param thickness depth hit tolerance
/// @param maxDist   ray march distance cap
/// @param edgeFade  UV space fade region width
virtual void DrawSSR(uint32_t /*depthTex*/, uint32_t /*normalTex*/, uint32_t /*hdrTex*/,
                     uint32_t /*dstFbo*/,
                     int /*w*/, int /*h*/,
                     const float* /*projMat4*/, const float* /*invProjMat4*/,
                     int /*maxSteps*/, float /*stepSize*/, float /*thickness*/,
                     float /*maxDist*/, float /*edgeFade*/) {}

/// SSR composite: hdr += reflectTex.rgb * reflectTex.a * intensity
/// 用 临时 RT 解 feedback loop (读 HDR + 写 HDR)
virtual void DrawSSRComposite(uint32_t /*reflectTex*/, uint32_t /*hdrFbo*/,
                               int /*w*/, int /*h*/, float /*intensity*/) {}
```

### 2.4 EndScene 管线插入位置

```cpp
// src/hdr_renderer.cpp::EndScene
void EndScene() {
    // ...

    // Phase E.9 — SSR (在 Bloom 之前, 让反射高光参与 bloom 提亮)
    SSRRenderer::Process(g.fbo, g.sceneTex);

    // Phase E.4.2 — Bloom
    BloomRenderer::Process(g.fbo, g.sceneTex);

    // ... AE / LensDirt / Streak / SSAO / LensFlare 保持原序
}
```

**注意**：原 SSAO 在 Bloom 之后；Phase E.9 后 SSR 移到 Bloom 之前；SSAO 维持原位。最终顺序：

```
Bloom (含 SSR 反射) → AE → LensDirt → Streak → SSAO → LensFlare → Tonemap
SSR  ↗                                                             ↑
```

### 2.5 GL33 实现要点

| 资源 | 类型 | 说明 |
|------|------|------|
| `programSSR` | GLSL program | VS = vsTonemap (复用)，FS = FS_SSR |
| `programSSRComposite` | GLSL program | VS = vsTonemap，FS = FS_SSR_COMPOSITE（hdr += reflect） |
| `ssrFbo / ssrTex` | RGBA16F + GL_LINEAR + GL_CLAMP_TO_EDGE | full-res，无 depth |
| `ssrCompositeTempFbo / Tex` | RGBA16F | 解 feedback loop，临时 RT |

---

## 3. 任务边界与限制

### 3.1 包含项（必做）

- [x] `include/ssr_renderer.h` 新建
- [x] `src/ssr_renderer.cpp` 新建（沿用 SSAO 模板）
- [x] `include/render_backend.h` 加 5 个 virtual fn
- [x] `src/render_gl33.cpp` 加 SSR 实现（FS_SSR + FS_SSR_COMPOSITE 双 profile）
- [x] `src/hdr_renderer.cpp::EndScene` 加入 SSR Process
- [x] `src/light_ui.cpp` 加 Init/Shutdown 注册
- [x] `src/light_graphics.cpp` 加 Lua binding（13 函数：5 lifecycle + 2 autoEnable + 14 params + 1 debug = 22）
- [x] `scripts/smoke/ssr.lua` 新建（与 ssao.lua 同结构）
- [x] `samples/demo_ssr/main.lua` + README.md
- [x] `docs/Phase E.9 SSR/` 7 份 6A 文档

### 3.2 不包含项（明确不做）

- ❌ Hierarchical Depth (HiZ) — Phase E.10+ 视情况
- ❌ TAA-style 时序滤波 — 需要 motion vector
- ❌ Cone tracing 模糊反射 — 需要 roughness G-buffer
- ❌ Cubemap fallback — 等 Phase E.10 IBL
- ❌ 透明物体反射 — SSR 固有限制
- ❌ Roughness-based reflection — 默认 mirror

---

## 4. 决策记录（Q1~Q8）

| Q | 问题 | 决策 | 来源 |
|---|------|------|------|
| Q1 | half-res vs full-res？ | **full-res**（用户选高质量方案）| 用户拍板 |
| Q2 | 默认步数？ | **64**（用户选高质量方案）| 用户拍板 |
| Q3 | ray miss fallback？ | **alpha=0**（保持 HDR 原色）| 业界经验 |
| Q4 | 边缘 fade？ | **smoothstep**, 默认 0.1 | 业界经验 |
| Q5 | SSR vs Bloom 顺序？ | **SSR 在 Bloom 之前**（反射高光参与 bloom）| 用户拍板 |
| Q6 | Lua 命名空间？ | **`Light.Graphics.SSR.*`** | 项目惯例 |
| Q7 | 调试 API？ | **加 `GetReflectionTexId`** | 用户拍板 |
| Q8 | 默认参数集？ | 见 §2.2 表格 | 业界经验 |

---

## 5. 默认参数最终确认

| 参数 | 默认 | 范围 | clamp 策略 |
|------|------|------|------------|
| MaxSteps | **64** | [8, 128] | int clamp |
| StepSize | 0.1 | [0.01, 1.0] | float clamp |
| Thickness | 0.5 | [0.01, 5.0] | float clamp |
| MaxDistance | 50.0 | [1.0, 1000.0] | float clamp |
| Intensity | 0.7 | [0.0, 2.0] | float clamp |
| EdgeFade | 0.1 | [0.0, 0.5] | float clamp |
| BlurEnabled | false | bool | (Phase E.9 不实现，保留 API) |

---

## 6. 与现有 Phase E 的兼容性

| Phase | 影响 | 验证 |
|-------|------|------|
| E.3 HDR | 无变更 | hdr.lua smoke |
| E.4 Bloom | EndScene 顺序前移到 SSR 之后；Bloom 自身不变 | bloom.lua smoke |
| E.5 AutoExposure | 无变更 | ae.lua smoke |
| E.6 LensDirt/Streak | 无变更 | lens_dirt.lua smoke |
| E.7 LensFlare | 无变更 | lens_flare.lua smoke |
| E.8 SSAO | 无变更 | ssao.lua smoke |
| E.8.x G-buffer normal | 复用 normalTex 通路 | 已就位，无新需求 |

---

## 7. 实施风险（确认与对策）

| 风险 | 概率 | 对策 |
|------|------|------|
| ray march 64 步性能在中端 GPU 不达 30 FPS | 中 | 提供 `SetMaxSteps` 让用户降步数；demo README 注明硬件要求 |
| GLES3 RGBA16F filter 不支持 | 低 | 检测 + fallback RGBA8 |
| 自反射 / 内反射伪影 | 中 | viewN·viewV < 0.05 时 discard |
| ray miss 边缘锯齿 | 中 | smoothstep edge fade 已有，必要时加 jitter |
| Bloom 顺序调换破坏现有 demo | 低 | SSR 默认 disabled，现有 demo 顺序变化但视觉等价（SSR=off） |

---

## 8. 完成度门控

- [x] 需求边界清晰无歧义
- [x] 技术方案与现有架构对齐（沿用 SSAO 模板 + 复用 G-buffer）
- [x] 验收标准具体可测试（11 项）
- [x] 所有关键假设已确认（Q1~Q8 八个决策点）
- [x] 项目特性规范已对齐（renderer 模板 / Lua API 命名 / 双 profile shader）

---

**进入 6A 阶段 2: Architect**（系统分层设计 → 模块设计 → 接口规范）

下一文档：`docs/Phase E.9 SSR/DESIGN_PhaseE_9.md`
