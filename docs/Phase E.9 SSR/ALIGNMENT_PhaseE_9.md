# ALIGNMENT — Phase E.9 · Screen Space Reflection (SSR)

> 6A 工作流 · 阶段 1 · Align (对齐阶段)
> 模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 现有项目特性

**ChocoLight Engine 渲染管线现状（截至 Phase E.8.x）**：

| 子系统 | 命名空间 | API 数 | 关键 RT |
|--------|----------|--------|---------|
| HDR 离屏 | `HDRRenderer` / `Light.Graphics.HDR` | 12 | RGBA16F color + RG16F normal (MRT, Phase E.8.x) + Depth |
| Bloom | `BloomRenderer` / `Light.Graphics.Bloom` | 13 | RGBA16F pyramid (2~8 级) |
| Auto Exposure | `AutoExposureRenderer` / `Light.Graphics.AutoExposure` | 18 | luminance R16F |
| Lens Dirt | `LensDirtRenderer` / `Light.Graphics.LensDirt` | 10 | 复用 Bloom top tex |
| Streak | `StreakRenderer` / `Light.Graphics.Streak` | ~10 | RGBA16F ping-pong |
| Lens Flare | `LensFlareRenderer` / `Light.Graphics.LensFlare` | ~12 | RGBA16F ping-pong |
| SSAO | `SSAORenderer` / `Light.Graphics.SSAO` | 20 | R16F ping-pong + depth blit + RG16F normal (Phase E.8.x) |

**统一的模块模板**（Bloom/SSAO/LensDirt/Streak 同构）：

```
namespace XxxRenderer {
    void Init(RenderBackend*);
    void Shutdown();
    bool Enable(int w, int h);          // 失败 silent fallback
    void Disable();                      // 幂等
    bool IsEnabled() / IsSupported();
    bool Resize(int w, int h);
    void OnHDREnabled(int w, int h);    // HDR 联动: autoEnable=true 时拉起
    void OnHDRDisabled();                // 强制关闭
    void OnHDRResized(int w, int h);
    void SetAutoEnable(bool) / GetAutoEnable();
    void SetParamX(...) / GetParamX();   // 6~10 对参数, 全 clamp
    void Process(uint32_t hdrFbo, uint32_t hdrTex);   // HDR EndScene 内调用
}
```

**HDR EndScene 管线顺序**（`src/hdr_renderer.cpp::EndScene`）：

```
HDR scene rendered → BeginScene 已 BindFBO(HDR_RT)
  ↓
Bloom        (Process: bright + downsample + upsample + composite)
AutoExposure (Process: luminance measure → currentExposure)
LensDirt     (Process: bloomTex composite to HDR)
Streak       (Process: anamorphic blur to HDR)
SSAO         (Process: depth blit + raw + blur + composite)
LensFlare    (Process: ghost + halo to HDR)
  ↓
Tonemap → default fb (sRGB encode, 多 operator)
```

### 1.2 Backend 接口模式

`include/render_backend.h` 现有 SSAO/Bloom 接口：

```cpp
// SSAO (Phase E.8 + E.8.x)
virtual bool SupportsSSAO() const;
virtual bool CreateSSAOTargets(int w, int h, uint32_t* fbos, uint32_t* texs,
                                int* outW, int* outH);
virtual void DeleteSSAOTargets(uint32_t* fbos, uint32_t* texs);
virtual uint32_t CreateSSAONoiseTex();
virtual void DrawSSAO(uint32_t depthTex, uint32_t noiseTex, uint32_t normalTex,
                       uint32_t dstFbo, ...);
virtual uint32_t GetHDRNormalTex(uint32_t fbo) const;        // ★ Phase E.8.x 新增
virtual void BlitHDRDepthToSSAO(uint32_t hdrFbo, uint32_t dstFbo, ...);
```

**Phase E.9 的 backend 接口将沿用 SSAO 同样的命名规范**。

### 1.3 SSR 与现有 G-buffer 的天然契合

**Phase E.8.x 已铺好的 G-buffer 通路**：
- HDR FBO 现有 MRT: `colorTex (RGBA16F) + normalTex (RG16F)` + Depth
- `GetHDRNormalTex(fbo)` 提供 view-space normal 访问
- `BlitHDRDepthToSSAO` 提供 depth blit 范式

**SSR 完全复用这套 G-buffer**：
- View-space normal → 计算反射方向
- Depth → ray march 命中检测
- HDR color → 反射结果采样源
- 不需要新的 G-buffer RT，**Phase E.9 是 Phase E.8.x 的自然延续**

---

## 2. 原始需求（用户输入）

> 推进 Phase E.9 SSR — Screen Space Reflection 复用 G-buffer normal RT，作为 Phase E.8.x 的自然延续。

---

## 3. 任务边界确认

### 3.1 任务范围（包含）

| 范围 | 说明 |
|------|------|
| **SSR 算法实现** | 屏幕空间 ray march 反射，linear march（不上 HiZ） |
| **Backend 接口扩展** | `SupportsSSR / CreateSSRTargets / DeleteSSRTargets / DrawSSR / DrawSSRComposite` |
| **GL33 后端实现** | RGBA16F 反射 RT (half-res) + linear ray march FS shader（双 profile：GL33 + GLES3） |
| **SSRRenderer 模块** | 与 SSAO 同模板的 namespace 模块 |
| **Lua API** | `Light.Graphics.SSR.*` 子表（生命周期 5 + autoEnable 2 + 参数 ~12） |
| **HDR 联动** | OnHDREnabled/Disabled/Resized + 加入 EndScene 管线 |
| **smoke 测试** | `scripts/smoke/ssr.lua`（headless tolerant，参数 round-trip + clamp） |
| **demo** | `samples/demo_ssr/main.lua`（金属球反射地面/天空盒） |
| **6A 文档** | ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO 完整 7 份 |

### 3.2 任务范围（不包含）

| 不做 | 原因 |
|------|------|
| Hierarchical Depth (HiZ) | 复杂度高（mip chain + binary search），起步用 linear march 已足 |
| 时序滤波 (TAA-like) | 需要 motion vector + history RT，Phase E.10+ 再考虑 |
| Cone tracing 模糊反射 | 需要 roughness G-buffer，本 phase 默认 mirror reflection |
| 透明物体反射 | G-buffer 不含透明几何，是 SSR 固有限制 |
| Cubemap fallback (ray miss → IBL) | 要等 Phase E.10 IBL，本 phase ray miss = 透明（保留 HDR 原色） |
| Roughness-based reflection | 同上，本 phase 默认全 mirror |

---

## 4. 需求理解（对现有项目的理解）

### 4.1 SSR 算法核心（linear ray march）

```glsl
// 伪代码
fragment shader main:
    1. uv = gl_FragCoord.xy / screenSize
    2. depth = texture(uDepthTex, uv).r          // [0, 1]
    3. viewPos  = ReconstructViewPos(uv, depth, uInvProj)
    4. viewN    = DecodeViewNormal(texture(uNormalTex, uv).xy)
    5. viewV    = normalize(-viewPos)            // 视点方向
    6. viewR    = reflect(-viewV, viewN)         // 反射方向

    7. ray march in view space:
        for i in 1..uMaxSteps:
            samplePosVS  = viewPos + viewR * (uStepSize * i)
            sampleClip   = uProj * vec4(samplePosVS, 1.0)
            sampleUV     = sampleClip.xy / sampleClip.w * 0.5 + 0.5
            if sampleUV out of [0,1]^2: break + edge fade
            sampleDepth  = texture(uDepthTex, sampleUV).r
            sampleZ_VS   = ReconstructViewZ(sampleDepth, uInvProj)
            if (samplePosVS.z < sampleZ_VS - uThickness):
                continue                          // 还没到深度面前
            if (samplePosVS.z >= sampleZ_VS - uThickness && samplePosVS.z <= sampleZ_VS):
                return texture(uHDRTex, sampleUV).rgb * uIntensity * fade
        return vec3(0.0)                          // miss
```

### 4.2 与现有 SSAO 算法的对比

| 维度 | SSAO | SSR |
|------|------|------|
| 输入 | depth + normal + noise | depth + normal + HDR color |
| 输出 | R16F (单通道遮蔽) | RGBA16F (反射颜色 + alpha) |
| 算法 | 半球采样统计 | view-ray reflect + march |
| 分辨率 | half-res | half-res（性能考虑） |
| Composite | hdr * mix(1, ao, intensity) | hdr += reflect * intensity |
| Fallback | 静默 skip | ray miss → alpha=0, hdr 不变 |

### 4.3 集成点（HDR EndScene）

**SSR 应在 SSAO 之后、Bloom 之前**？分析：

- **SSAO 之后**：✅ SSAO 减弱反射区的环境光，反射不应被 SSAO 影响（反射是光泽，AO 是漫反射阴调）
- **Bloom 之前**：❌ Bloom 应该把反射的高光也参与亮部提取（金属反射的太阳要 bloom）

**最终决定**：SSR 在 **Bloom 之前** 执行，让反射结果参与 bloom 提亮。具体顺序：

```
旧顺序                        新顺序 (Phase E.9)
SSAO    [Phase E.8]          SSAO    [Phase E.8]
Bloom   [Phase E.4]          SSR     [Phase E.9] ★
                              Bloom   [Phase E.4]   ← 现在能 bloom 反射高光
AE                            AE
LensDirt                      LensDirt
Streak                        Streak
LensFlare                     LensFlare
```

这是对 EndScene 顺序的**重要修正**（现有顺序中 Bloom 在 SSAO 之前，SSR 加进去会促使重新审视）。

---

## 5. 疑问澄清（关键决策点）

### Q1：SSR 半分辨率 vs 全分辨率？

**默认建议**：half-res（与 SSAO 对齐）。理由：
- SSR 性能消耗为 O(N_steps) per pixel，full-res 性能可能下降 30%+
- 半分辨率 + 双线性 upsample 在视觉上对 mirror 反射可接受
- 高质量需求可在 Lua API 提供 `SetFullResolution(bool)` 切换（**待用户决策是否暴露**）

### Q2：ray march 步数（uMaxSteps）默认值？

**业界经验**：32~64 步起步，参考 Frostbite SSR、Unreal SSR：
- 16 步：性能优 / 质量差（明显锯齿、远距离 miss）
- 32 步：性能/质量平衡（推荐默认）
- 64 步：高质量 / 移动端勉强
- 128 步：基本只用于 PC 高画质模式

**默认 32**，clamp [8, 128]，Lua API `SetMaxSteps`。

### Q3：ray miss 时 fallback？

**选项**：
- A. 透明（alpha=0），HDR 颜色不受影响（**推荐**，简单 + 与未来 IBL 兼容）
- B. 用屏幕边缘最后一次有效采样的颜色 fade out（视觉好但实现复杂）
- C. 直接黑色（视觉差，会让金属物体局部变黑）

**推荐 A**，`composite shader` 用 `hdr.rgb += reflect.rgb * reflect.a * intensity`，alpha 即 fade weight。

### Q4：边缘 fade 策略？

需要边缘 fade 防止反射在屏幕边界突兀截断。**默认开**：

```glsl
float edgeFade = smoothstep(0.0, 0.1, uv.x) * smoothstep(0.0, 0.1, 1.0 - uv.x)
                * smoothstep(0.0, 0.1, uv.y) * smoothstep(0.0, 0.1, 1.0 - uv.y);
reflectColor *= edgeFade;
```

参数化为 `SetEdgeFade(float)`（默认 0.1，clamp [0, 0.5]）。

### Q5：是否做 SSR 与 SSAO 的强制顺序保护？

SSR 需要 G-buffer normal（Phase E.8.x），SSAO 也需要。**它们都依赖 HDR FBO 的 MRT normal RT**。

如果用户：
1. `HDR.Enable(...)` → MRT normal tex 创建
2. `SSAO.Enable(...)` → ok
3. `SSR.Enable(...)` → ok

**正常情况无冲突**，因为两个都是只读 normalTex。但要确保：
- **DrawSSAO 和 DrawSSR 在 backend 内部都正确取 normalTex**
- **在 OnHDRDisabled 时两者都 silent fallback**

### Q6：Lua API 命名空间是 `Light.Graphics.SSR` 还是其他？

**沿用 SSAO 同模式**：`Light.Graphics.SSR.*`。

### Q7：是否要可视化调试接口（类似 SSAO.GetNormalTexId）？

**推荐**：加 `Light.Graphics.SSR.GetReflectionTexId()`，返回 SSR 反射 RT 的 GL id（0 = 未启用），与 SSAO.GetNormalTexId 风格一致。

### Q8：默认参数集合？

| 参数 | 默认 | clamp 范围 | 说明 |
|------|------|------------|------|
| `MaxSteps` | 32 | [8, 128] | ray march 步数 |
| `StepSize` | 0.1 | [0.01, 1.0] | 每步 view-space 距离 (units) |
| `Thickness` | 0.5 | [0.01, 5.0] | 深度命中容差 (view-space units) |
| `MaxDistance` | 50.0 | [1.0, 1000.0] | ray march 最大距离 (view-space units) |
| `Intensity` | 0.7 | [0.0, 2.0] | 反射强度 (multiplier) |
| `EdgeFade` | 0.1 | [0.0, 0.5] | 屏幕边缘 fade 区域宽度 (UV space) |
| `BlurEnabled` | false | bool | 反射模糊 (本 phase 不实现) |

---

## 6. 技术方案预览（与现有架构对齐）

### 6.1 文件清单（预计）

```
include/ssr_renderer.h                      新增 (~80 行)
src/ssr_renderer.cpp                        新增 (~250 行, 沿用 SSAO 模板)
include/render_backend.h                    +30 行 (5 个 virtual fn)
src/render_gl33.cpp                         +250 行 (FS_SSR + bind 函数 + RT 创建)
src/hdr_renderer.cpp                        +5 行  (EndScene 加入 SSR Process)
src/light_ui.cpp                            +5 行  (Init/Shutdown 注册)
src/light_graphics.cpp                      +200 行 (Lua binding 13 fn)
scripts/smoke/ssr.lua                       新增 (~250 行, 与 ssao.lua 同结构)
samples/demo_ssr/main.lua                   新增 (~150 行, 金属球 + 地面)
samples/demo_ssr/README.md                  新增 (~30 行)
docs/Phase E.9 SSR/                         新增 (7 份 6A 文档, ~3000 行)
```

**预计代码总量**：~1000 行（不含 docs）+ ~3000 行 docs。

### 6.2 与现有 6A 文档复用度

- **CONSENSUS_PhaseE_8x.md** 的 backend 抽象决策可直接复用（默认 MRT + silent fallback）
- **DESIGN_PhaseE_8x.md** 的 view-space normal decode helper 可复用
- **TASK_PhaseE_8x.md** 的依赖图模板可改写为 SSR 任务图

---

## 7. 风险与对策

| 风险 | 概率 | 影响 | 对策 |
|------|------|------|------|
| ray march 边缘自反射 | 高 | 视觉错误 | 跳过 viewN 与 viewR 夹角 < 0 的反射（背面） |
| half-res 反射严重锯齿 | 中 | 视觉差 | 提供 `SetFullResolution(bool)` Lua API |
| Bloom-SSR 顺序调换破坏现有 demo | 低 | 视觉回归 | 现有 demo 无 SSR，顺序调换对 SSR=off 完全无影响 |
| ray march 性能 | 中 | FPS 下降 | 默认 32 步 + half-res + early break（屏外/距离超限） |
| GLES3 设备 RGBA16F filter 不支持 | 低 | 反射有锯齿 | 检测 + fallback 到 RGBA8（精度损失但可接受） |

---

## 8. 完成度门控（必须全 ✅ 才能进入 CONSENSUS）

- [x] 项目上下文已分析（renderer 模板 / backend 模式 / HDR EndScene 管线已 mapping）
- [x] 任务边界已圈定（包含 / 不包含项 P0 决策）
- [x] 需求理解已与现有项目对齐（复用 G-buffer 通路）
- [x] 关键疑问已识别（Q1~Q8 共 8 项）
- [ ] **关键疑问已澄清**（待用户确认 Q1/Q2/Q5/Q7 的默认决策；其他可基于业界经验决策）
- [ ] CONSENSUS 文档已生成

---

**下一步**：基于业界经验为 Q1~Q8 提出决策默认值，关键决策点询问用户后生成 CONSENSUS 文档。
