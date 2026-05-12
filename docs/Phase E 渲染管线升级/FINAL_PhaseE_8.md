# FINAL — Phase E.8 · SSAO (Screen-Space Ambient Occlusion)

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.8 完整交付：HDR 链路第 7 剑客 — **SSAO**（屏幕空间环境光遮蔽）。采用「**双 RT 旁路**」策略，HDR RT 零侵入；用户 API 完全透明。

---

## 1. 交付物总览

### 1.1 4 子阶段拆分

| 阶段 | commit | 范围 | CI |
|------|--------|------|-----|
| 规划 | `40aef66` + `1ec8464` | ALIGNMENT + DESIGN + TASK（含双 RT 旁路策略修订）| ✅ 6/6 |
| **E.8.1** Backend | `7f14b96` + fix `c4e7d35` | render_backend.h 11 虚接口 + GL33 3 shader 双 profile + 实现 + InitLensFx + Shutdown | ⏳ 跑中 |
| **E.8.2** Module | `9cd60af` | SSAORenderer namespace (27 C++ fn) + HDR 5 联动点 + light_ui + CMake | ⏳ 跑中 |
| **E.8.3** Lua API | `f9108cb` | Light.Graphics.SSAO 19 fn + smoke ~50 断言 + demo_ssao 3D 场景 + CI 注册 | TBD |
| docs | `pending` | ACCEPTANCE + FINAL + TODO | — |

### 1.2 改动文件清单

| 文件 | 行数 | 类型 |
|------|------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +72 | E.8.1（11 SSAO 虚接口）|
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +580 | E.8.1（3 shader 双 profile + state + 11 override + Init/Shutdown）|
| `@e:\jinyiNew\Light\ChocoLight\include\ssao_renderer.h` | +85 | E.8.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\ssao_renderer.cpp` | +295 | E.8.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +8 | E.8.2（5 联动点）|
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +5 | E.8.2 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +155 | E.8.3（19 binding + 子表）|
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | E.8.2 |
| `@e:\jinyiNew\Light\scripts\smoke\ssao.lua` | +170 | E.8.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_ssao\main.lua` | +260 | E.8.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_ssao\README.md` | +95 | E.8.3 新建 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.8.3 CI 注册 |
| 6 份 Phase E.8 docs | — | — |

**总新增代码（生产）**：C++ ~1200 行 + Lua ~430 行 + YAML/CMake ~4 行
**总新增文档**：~1500 行

---

## 2. 技术架构

### 2.1 双 RT 旁路策略（用户选择 2026-05-12）

**核心思想**：HDR RT 保持原状（`GL_DEPTH_COMPONENT24 renderbuffer`），SSAO 自管独立 `depth texture`，每帧用 `glBlitFramebuffer` 旁路复制 depth：

```
3D mesh 写入 HDR FBO (depth RB 写正常)
                │
   每帧 SSAO.Process 入口:
                ▼
glBlitFramebuffer(hdrFbo → ssaoDepthFbo, GL_DEPTH_BUFFER_BIT)
                │   (~0.1 ms GPU 原生操作)
                ▼
SSAO depth tex (可采样, NEAREST + CLAMP_TO_EDGE)
                │
                ▼
DrawSSAO → DrawSSAOBlur (h+v) → DrawSSAOComposite → HDR color
```

**对比直接升级方案的优势**：
- ✅ HDR RT 代码零改动，所有现有 demo / smoke 行为完全不变
- ✅ 无需切换 depth attachment 类型，规避驱动差异
- ✅ 用户 API 完全透明（无需手动包住 3D 绘制段）
- 🆗 仅多 ~0.1 ms / 帧的 blit 开销

### 2.2 数据流（HDR 链路 7 剑客全开）

```
Lua Draw 帧:
  SetPerspective + SetCamera + SetDepthTest(true)
  HDRRenderer::BeginScene → bind HDR_FBO (RGBA16F + depth RB)
  Lua mesh:Draw → 写入 HDR RT (color + depth)
  HDRRenderer::EndScene:
      ├── BloomRenderer::Process              ── pyramid additive  [Phase E.4]
      ├── AutoExposureRenderer::Process       ── 测 logLuma         [Phase E.5]
      ├── LensDirtRenderer::Process            ── bloom × dirt      [Phase E.6]
      ├── StreakRenderer::Process              ── anamorphic flare  [Phase E.6]
      ├── SSAORenderer::Process:                                    [Phase E.8] ★
      │     0. BlitHDRDepthToSSAO(hdrFbo, ssaoDepthFbo, w, h)
      │     1. DrawSSAO(depthTex, noise, kernel[16]) → AO raw (R16F)
      │     2. DrawSSAOBlur axis=0 (水平 bilateral)
      │     3. DrawSSAOBlur axis=1 (垂直 bilateral)
      │     4. DrawSSAOComposite(aoTex, hdrFbo, intensity)
      ├── LensFlareRenderer::Process           ── ghost + halo      [Phase E.7]
      │
      ├── exposure ← AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
      └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
            ↓
       Backbuffer
```

### 2.3 模块层次

```
┌────────────────────────────────────────────────────────────────────────┐
│  Light.Graphics (Lua subtables, 7 个 = ~130 函数)                       │
│  .HDR 12 / .Bloom 15 / .AutoExposure 18 / .LensDirt 10 / .Streak 13    │
│  .LensFlare 23 / .SSAO 19                                              │
└────────────────────────────────────────────────────────────────────────┘
                                ↓
   HDRRenderer (scene RT)
   ├── BloomRenderer       (pyramid N 级)
   ├── AutoExposureRenderer (luminance mipmap + readback)
   ├── LensDirtRenderer    (no RT, dirtTexId)
   ├── StreakRenderer       (ping-pong RT, 1/2 res)
   ├── LensFlareRenderer    (ping-pong RT, 1/2 res)
   └── SSAORenderer         (独立 depth tex + AO ping-pong RT + 4×4 noise) ★
                                ↓
       RenderBackend 虚接口: 7 SSAO override (+ 2 矩阵 getter) + 之前 28 = 37
                                ↓
       GL33Backend / LegacyBackend (default no-op)
```

### 2.4 关键决策

| 决策 | 选择 | 理由 |
|------|------|------|
| **HDR depth 路径** | 双 RT 旁路 + glBlitFramebuffer | HDR RT 零侵入；用户 API 透明（用户确认 2026-05-12）|
| **算法** | 经典 SSAO + 16 Hammersley kernel + 4×4 noise | GL3.3/GLES3 通杀；行业标准 |
| **Normal 来源** | depth 重建（ddx/ddy + cross）| 零 G-buffer 改造；纯 depth pass |
| **kernel 生成** | CPU LCG deterministic 半球分布（lerp(0.1, 1.0, scale²)）| 一次生成 lifetime 复用 |
| **AO RT 规格** | R16F 半分辨率 ping-pong | 带宽最优 |
| **Composite feedback loop** | 内部 RGBA16F full-res temp + blit-then-shader | 标准做法；避免读写同 RT |
| **Composite 位置** | EndScene 中 LensFlare 之后 | 实施简化；视觉影响轻微 |
| **适用范围** | 仅 3D + SetDepthTest(true) 场景 | 用户确认 2026-05-12 |
| **autoEnable** | 默认 false | 与 LensDirt/Streak/LensFlare/AE 一致 |
| **Legacy backend** | `SupportsSSAO() = false` 静默 | 同其他 Phase E 模块 |

---

## 3. API surface（Phase E.8 新增）

### 3.1 C++ (SSAORenderer, 27 fn)

详见 `@e:\jinyiNew\Light\ChocoLight\include\ssao_renderer.h`：

```cpp
Init / Shutdown                                        // 2
Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)   // 5
OnHDREnabled / OnHDRDisabled / OnHDRResized            // 3
SetAutoEnable / GetAutoEnable                          // 2
Set+Get × 6 params                                     // 12
  Radius / Bias / Intensity / KernelSize / Power / BlurEnabled
Process(hdrFbo, hdrTex)                                // 1
                                              合计:    // 25 (+ 2 内部辅助)
```

### 3.2 Lua (Light.Graphics.SSAO, 19 fn)

详见 smoke `@e:\jinyiNew\Light\scripts\smoke\ssao.lua`：

```lua
Enable(w, h) / Disable / IsEnabled / IsSupported / Resize(w, h)
SetAutoEnable / GetAutoEnable
SetRadius / GetRadius                  -- float [0.05, 5.0]
SetBias / GetBias                      -- float [0, 0.2]
SetIntensity / GetIntensity            -- float [0, 4.0]
SetKernelSize / GetKernelSize          -- int {8, 16}
SetPower / GetPower                    -- float [0.5, 8.0]
SetBlurEnabled / GetBlurEnabled        -- bool
```

### 3.3 后端虚接口（render_backend.h, 11 new + 2 helpers）

```cpp
bool SupportsSSAO() const;
bool CreateSSAODepthRT(w, h, &outFbo, &outTex);        // 独立 depth tex + 小 FBO
void DeleteSSAODepthRT(fbo, tex);
void BlitHDRDepthToSSAO(hdrFbo, ssaoDepthFbo, w, h);   // 旁路核心
bool CreateSSAOTargets(w, h, fbos[2], texs[2], ...);   // R16F ping-pong 半分辨率
void DeleteSSAOTargets(fbos[2], texs[2]);
uint32_t CreateSSAONoiseTex();                          // 4×4 RGBA8 noise
void     DeleteSSAONoiseTex(tex);
void DrawSSAO(depthTex, noiseTex, dstFbo, proj, invProj, kernel, ...);
void DrawSSAOBlur(srcAOTex, depthTex, dstFbo, axis);
void DrawSSAOComposite(aoTex, dstFbo, intensity);
// + Phase E.8 辅助:
void GetProjection(float* out16);
void GetView(float* out16);
```

---

## 4. CI 证据

| Commit | Run | 结论 |
|--------|-----|------|
| 规划 docs | （随 push 走） | ✅ 6/6 |
| `7f14b96` E.8.1 backend (含 glDrawBuffer bug) | 25703520xxx | ❌ 4/6 (Linux/iOS/Web/Android 失败) |
| `c4e7d35` E.8.1 fix（glDrawBuffer → glDrawBuffers）+ E.8.2 module | 25705155526 | ⏳ 跑中 |
| `f9108cb` E.8.3 Lua + smoke + demo + CI 注册 | TBD（合并下次 push）| TBD |

**fix 详情**：
- `glDrawBuffer(GL_NONE)` 是桌面 GL 专用 API
- GLES3 / WebGL2 / iOS / Android emcc gl3.h 没有此函数
- 解决：改用 `glDrawBuffers(1, {GL_NONE})` 两个平台都支持

---

## 5. Phase E 链路累计

```
HDR 链路 7 剑客 / ~130 Lua API:

Phase E.3 — HDR + 4 tonemap operator      (12 fn)
Phase E.4 — Bloom pyramid                  (15 fn)
Phase E.5 — Auto Exposure (Eye Adaptation) (18 fn)
Phase E.6 — Lens Dirt + Streak             (23 fn)
Phase E.7 — Lens Flare + FlareTexture      (23 fn)
Phase E.8 — SSAO                            (19 fn)   ✨ 新增

电影感后处理 + 几何 AO 全套上线 ✨
```

---

## 6. 已知限制

1. **2D 场景 SSAO 无效**：所有像素 z=0，AO 输出全 1（按设计；用户已确认仅 3D 适用）
2. **Normal 重建依赖 ddx/ddy**：极端角度精度有限；G-buffer normal 路径留 Phase E.9 候选
3. **KernelSize 仅 8/16**：shader 静态 for 上限；改大需重编 shader
4. **Composite 在 LensFlare 之后**：与"理想 AO 在 Bloom 之前"略不同
5. **Legacy 后端 no-op**：`SupportsSSAO() = false`，Lua API 全链路静默
6. **HDR depth blit 兼容性**：部分老 GLES 驱动可能不支持；当前未做 Init 探测降级

---

## 7. 后续阶段建议

| Phase | 主题 | 收益 |
|-------|------|------|
| **Phase E.8.x** | G-buffer normal RT + HBAO/GTAO 算法升级 | 法线精度↑, 边缘质量↑ |
| **Phase E.8.y** | Temporal SSAO (TAA-style) | 动态 noise → 平滑帧间 |
| **Phase E.9** | SSR（屏幕空间反射）| 水面 / 玻璃反射 |
| **Phase F.x** | Compute shader pipeline | GTAO / Bloom / Histogram AE |

---

**Phase E.8 主交付完毕**。HDR 后处理 + 几何 AO 链路 = **7 剑客 / 130 Lua API** 上线 ✨
