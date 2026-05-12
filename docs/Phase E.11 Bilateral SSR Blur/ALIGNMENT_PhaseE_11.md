# Phase E.11 Bilateral SSR Blur — ALIGNMENT 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）
> **目标**：模糊需求 → 精确规范
> **基线**：Phase E.10 SSR Blur（commit `d64e6b4`，CI 6/6 green，main HEAD）
> **主题**：在 Phase E.10 half-res Gaussian 基础上叠加 **depth-aware bilateral 权重门控**

---

## 1. 项目上下文分析

### 1.1 现有管线（Phase E.10 已交付）

```
HDR color (full-res)                     HDR depth (full-res)
      │                                           │
      ├─(MRT normal slot1)                        │
      │          │                                ▼
      ▼          ▼                          BlitHDRDepthToSSAO
  SSR raw  ◄── SSR depthTex (full-res, NEAREST) ◄┘
      │ RGBA16F full-res (reflectTex)
      │
      ▼
  [Phase E.10] half-res Gaussian 5-tap blur (ping-pong)
      │ blurTexs[0] → blurTexs[1]
      │ 统一权重 [6/16, 4/16, 1/16] — 不考虑 depth
      ▼
  SSR Composite → HDR color
  （bilinear upscale 到 full-res）
```

**已有资源可复用**：
- `g.depthTex`（**full-res**, `BlitHDRDepthToSSAO` 产物，SSR raw shader 已在用）
- `g.blurFbos[2]` / `g.blurTexs[2]`（half-res ping-pong, RGBA16F）
- `SSAOBlur` 已有 bilateral 实现（参考 @e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:1507-1536）

### 1.2 Phase E.10 的已知问题

1. **边缘反射 leak**：half-res + 统一高斯 → 前景深度像素在 blur 时会混入背景深度像素的反射，或反之，上采样时在物体轮廓附近造成模糊扩散
2. **自由表面对比度降低**：高粗糙度场景下大 blur radius 时问题显著
3. **无法分辨"应该模糊"和"应该锐利"**：Gaussian 无法感知几何边界

### 1.3 参考解决方案：SSAOBlur bilateral（Phase E.8）

**shader 核心**（`@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:1519-1535`）：

```glsl
void main() {
    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    float cDepth = texture(uDepthTex, vUV).r;
    float sum = 0.0, wsum = 0.0;
    for (int i = -2; i <= 2; i++) {
        vec2 uv = vUV + dir * float(i);
        float ao = texture(uSSAOTex, uv).r;
        float d  = texture(uDepthTex, uv).r;
        // depth-aware 权重: 跨 depth 边界时权重骤降, 保留物体边缘
        float w  = exp(-abs(cDepth - d) * 200.0);
        sum  += ao * w;
        wsum += w;
    }
    float o = sum / max(wsum, 1e-4);
    FragColor = vec4(o, o, o, 1.0);
}
```

**关键公式**：`w = exp(-abs(cDepth - d) * sigma)`
- σ = 200（SSAO 硬编码）
- depth 差越大，weight 越小，权重归一化前自动 softly drop to 0

### 1.4 当前 backend 接口差距

```cpp
// Phase E.10 现状（@ChocoLight/include/render_backend.h:1031-1033）
virtual void DrawSSRBlur(uint32_t srcTex, uint32_t dstFbo,
                          int dstW, int dstH,
                          int axis, float radius) {}
// 缺: depthTex 参数

// 参考 Phase E.8（@ChocoLight/include/render_backend.h:857-861 附近）
virtual void DrawSSAOBlur(uint32_t srcAOTex, uint32_t depthTex, uint32_t dstFbo,
                           int w, int h, int axis) {}
// 有 depthTex
```

---

## 2. 原始需求（用户选择）

用户在 Phase E.10 闭环后选 **"A — Phase E.11 Bilateral Blur"**：

> "在 Phase E.10 基础上加 depth-aware 门控，消除 half-res 边缘 leak。工作量 ~1 日，依赖已就绪，风险低。"

**直接映射的需求要点**：
- 复用 Phase E.10 的 half-res RT 基础设施
- 在 shader 中引入 depth-aware 权重
- 消除反射 leak 到不相关物体的问题
- 保持 Phase E.10 的 Lua API 兼容（24 函数不变 / 或 + 新开关）

---

## 3. 边界确认（明确任务范围）

### 3.1 纳入范围（IN SCOPE）

- ✅ Phase E.10 `FS_SSR_BLUR` shader → 升级为 bilateral（GLES3 + GL33 双 profile）
- ✅ `DrawSSRBlur` backend 接口签名扩展（追加 `depthTex` 参数）
- ✅ `SSRRenderer::Process` 传递 `g.depthTex`
- ✅ 对应 smoke 测试更新（不回归，新检查点视决策而定）
- ✅ demo HUD 展示 "SSR Blur: Bilateral" 而非 "Gaussian"
- ✅ 文档完整：ALIGNMENT / CONSENSUS / DESIGN / TASK / ACCEPTANCE / FINAL / TODO（7 份）

### 3.2 排除范围（OUT OF SCOPE）

- ❌ Roughness-aware 半径（需要 G-buffer roughness channel，Phase E.12+）
- ❌ Temporal accumulation（Phase E.13+）
- ❌ Blur quality preset（3/5/7-tap 切换，Phase E.10.x 候选）
- ❌ Stochastic SSR（hizz / cone-trace，Phase E.14+）
- ❌ 改动 Phase E.9 SSR raw shader（只改 blur pass）

---

## 4. 需求理解（对现有项目的理解）

### 4.1 SSR depth RT 架构共识

SSR 已经创建了独立的 `depthTex`（不与 SSAO 复用），通过 `BlitHDRDepthToSSAO` 从 HDR FBO 复制。
**Phase E.11 不需要新增 depth RT**，直接复用 `SSRRenderer::g.depthTex`。

### 4.2 Half-res blur + full-res depth 采样的权衡

- blur RT 是 half-res（960×540 @ 1080p），采样的 UV 是 [0,1]
- depth tex 是 full-res（1920×1080），通过 UV 自动 2×2 下采样访问
- GL 默认 NEAREST filter（SSR depth 创建时也是 NEAREST），采样 full-res 时取最近一个像素
- **无需创建 half-res depth**（SSAO 也是这样做的，参考 @e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:4184 同时绑 full-res depthTex）

### 4.3 Backend 接口签名变更的兼容策略

Phase E.10 `DrawSSRBlur(srcTex, dstFbo, dstW, dstH, axis, radius)` — 6 参数
Phase E.11 `DrawSSRBlur(srcTex, depthTex, dstFbo, dstW, dstH, axis, radius)` — 7 参数

**做法**：直接修改 virtual 签名（只有 GL33Backend + Legacy Backend 实现，无外部实现）。
Legacy 继续 no-op（`SupportsSSR=false`），不受影响。

### 4.4 Lua API 兼容

- 默认 Phase E.11 bilateral 权重 **始终开启**（替换原纯 Gaussian）
- 不引入新 Lua 开关 → 保持 24 函数不变
- 行为变化：`BlurEnabled=true` 时质量提升，radius 的感知可能略变紧（bilateral 权重使跨边模糊收窄）

---

## 5. 用户视角期望

### 5.1 视觉期望

```
Phase E.10 Gaussian blur:
    ┌──── 前景立方体 ────┐
    │  反射纹理 A       │ ←── 在边缘被 blur 稀释为 B
    └───────────────────┘
       ↑ 反射 leak 到背景
       ↓
    ┌──── 背景远景 ─────┐
    │  反射纹理 B       │ ←── 被 blur 污染了一圈 A
    └───────────────────┘

Phase E.11 Bilateral blur:
    ┌──── 前景立方体 ────┐
    │  反射纹理 A       │ ←── 深度边界 weight→0, A 不扩散
    └───────────────────┘
    ┌──── 背景远景 ─────┐
    │  反射纹理 B       │ ←── B 不受 A 污染, 保持锐利边界
    └───────────────────┘
```

### 5.2 性能预期

- Phase E.10: ~0.3 ms @ 1080p（GPU，含 H+V+composite 读取）
- Phase E.11: ~0.35 ms @ 1080p（+2 tex fetches per sample × 5 samples × 2 pass = +20 fetch/pixel, half-res）
- **额外开销估算：~0.05 ms（约 +17%）**
- 仍在"可忽略不计"级别，不需新增 quality preset

### 5.3 API 调用期望

**用户代码完全不变**：

```lua
-- Phase E.10 代码（仍然有效）
SSR.SetBlurEnabled(true)
SSR.SetBlurRadius(2.0)
-- Phase E.11 自动升级为 bilateral, 无需用户代码变更
```

---

## 6. 算法设计预览

### 6.1 目标 shader 伪代码

```glsl
// FS_SSR_BLUR (Phase E.11 bilateral, dual profile GLES3 + GL33)
uniform sampler2D uSrcTex;      // 反射 tex (full-res on H pass, half-res on V pass)
uniform sampler2D uDepthTex;    // SSR depth tex (full-res, NEAREST)
uniform vec2  uTexel;           // 1 / half-res
uniform int   uAxis;            // 0=H, 1=V
uniform float uRadius;          // [0.5, 4.0] texel 乘子
uniform float uDepthSigma;      // bilateral 深度 sigma (Phase E.11 新增)

void main() {
    vec2 dir = (uAxis == 0) ? vec2(uTexel.x, 0.0) : vec2(0.0, uTexel.y);
    float cDepth = texture(uDepthTex, vUV).r;

    // 5-tap 高斯权重（Phase E.10 保留）
    const float W0 = 0.227027;
    const float W1 = 0.194594;
    const float W2 = 0.121622;

    vec2 off1 = dir * uRadius;
    vec2 off2 = dir * uRadius * 2.0;

    vec4 sum = vec4(0.0); float wsum = 0.0;

    // center
    float w = W0;  // 中心 weight 无需 depth gating
    sum  += texture(uSrcTex, vUV) * w;
    wsum += w;

    // ±1
    { vec2 uv = vUV + off1; float d = texture(uDepthTex, uv).r;
      w = W1 * exp(-abs(cDepth - d) * uDepthSigma);
      sum += texture(uSrcTex, uv) * w; wsum += w; }
    { vec2 uv = vUV - off1; float d = texture(uDepthTex, uv).r;
      w = W1 * exp(-abs(cDepth - d) * uDepthSigma);
      sum += texture(uSrcTex, uv) * w; wsum += w; }

    // ±2
    { vec2 uv = vUV + off2; float d = texture(uDepthTex, uv).r;
      w = W2 * exp(-abs(cDepth - d) * uDepthSigma);
      sum += texture(uSrcTex, uv) * w; wsum += w; }
    { vec2 uv = vUV - off2; float d = texture(uDepthTex, uv).r;
      w = W2 * exp(-abs(cDepth - d) * uDepthSigma);
      sum += texture(uSrcTex, uv) * w; wsum += w; }

    FragColor = sum / max(wsum, 1e-4);
}
```

### 6.2 关键不同于 SSAOBlur

| 维度 | SSAOBlur（Phase E.8） | SSRBlur（Phase E.11） |
|------|----------------------|----------------------|
| 颜色空间 | scalar R16F (AO only) | vec4 RGBA16F（反射颜色） |
| 分辨率 | half-res src & depth（同半） | half-res src, full-res depth |
| 权重分布 | box 5-tap 均匀（`w = exp()` only） | **Gaussian × bilateral 组合** |
| sigma | 硬编码 200 | 可参数化（本 phase 决策点） |

---

## 7. 集成期望

### 7.1 Process 改动最小化

```cpp
// ssr_renderer.cpp Phase E.10 (已有)
g.backend->DrawSSRBlur(g.reflectTex, g.blurFbos[0], g.blurW, g.blurH, 0, g.blurRadius);
g.backend->DrawSSRBlur(g.blurTexs[0], g.blurFbos[1], g.blurW, g.blurH, 1, g.blurRadius);

// Phase E.11 只需追加 depthTex 参数
g.backend->DrawSSRBlur(g.reflectTex, g.depthTex, g.blurFbos[0], g.blurW, g.blurH, 0, g.blurRadius);
g.backend->DrawSSRBlur(g.blurTexs[0], g.depthTex, g.blurFbos[1], g.blurW, g.blurH, 1, g.blurRadius);
```

### 7.2 backend GL33 改动最小化

```cpp
// render_gl33.cpp — FS_SSR_BLUR shader 修改：+1 sampler + bilateral loop
// GL33Backend state — +1 uniform loc (locSSRBlur_DepthTex, locSSRBlur_Sigma)
// DrawSSRBlur 实现 — +1 depth tex bind (slot 1)
```

---

## 8. 疑问澄清（存在歧义的地方）

### 8.1 关键决策点（需要用户拍板）

#### Q1: depthSigma 参数化 vs 硬编码？

**选项 A**（推荐）：**硬编码 200**
- 优点：与 SSAO 一致，代码最简，无新增 API
- 缺点：不同场景可能需要微调（远距反射场景 σ=200 可能太严）

**选项 B**：暴露为 Lua API `SetBlurDepthSigma(float [50, 500])`
- 优点：可调节，场景特化
- 缺点：API 扩展，用户需理解物理意义

**我的建议**：**A**（保持简单，后续 Phase E.11.x 视反馈决定）

#### Q2: 用户 Lua API 需要额外开关吗？

**选项 A**（推荐）：**无开关，默认替换为 bilateral**
- 优点：用户无感知，质量自动提升
- 缺点：无法回退到纯 Gaussian（但 Phase E.10 已废弃，无回退需求）

**选项 B**：`SetBilateralEnabled(bool)` + 默认 true
- 优点：A/B 对比方便
- 缺点：API 扩展，语义混淆（blur 分两档：off / on Gaussian / on Bilateral）

**我的建议**：**A**（bilateral 永远好，无 A/B 需求）

#### Q3: smoke 测试是否新增检查点？

**选项 A**：不新增（bilateral 是 shader 内部升级，Lua API 不变）
**选项 B**：新增 1 个"质量级别"元测试，确认 shader 已升级（通过 log 或元数据）

**我的建议**：**A**（shader 升级通过 CI build 验证，smoke 无需改动，但文档同步必要）

---

## 9. 中断询问（等待用户决策）

请用户就以下 3 点拍板：

1. **depthSigma** 硬编码 200 还是参数化？
2. **Lua API** 是否新增 `SetBilateralEnabled` 开关？
3. **smoke** 是否需要额外新检查点？

> 默认推荐：**硬编码 + 无开关 + smoke 不改** → Phase E.11 是"纯升级"最简路线。
> 用户拍板后进入 CONSENSUS → DESIGN → TASK → 代码实现。

---

## 10. 行业参考

### 10.1 同类实现

- **Unreal Engine SSR** (LumenSSR): bilateral + roughness-aware radius, σ 按 scene depth range scale
- **Unity HDRP**: SSR with depth-aware filter, Ref: HDRP Graphics Shaders/Runtime/Lighting/ScreenSpaceLighting/ScreenSpaceReflection.shader
- **Crytek CryEngine 3**: Stochastic SSR + bilateral upsample, Ref: GDC 2016 "SSR at Crytek"

### 10.2 公式溯源

- **Bilateral Filter**：Tomasi & Manduchi 1998 IEEE ICCV. 原始公式 `w = exp(-||Δp||²/σ²_s - ||Δc||²/σ²_r)` 空间+范围双高斯
- **本 Phase 简化**：空间用 Gaussian 权重（W0/W1/W2），范围仅用 depth-aware 单维 exp decay
- **合理性**：SSR blur 要的不是完美 bilateral，而是"不跨深度边"，简化公式即可

---

## 11. 约束（技术栈 / 架构对齐）

- ✅ 必须保持 Phase E.9 SSR 向后兼容（`BlurEnabled=false` 行为与 Phase E.10 完全一致）
- ✅ 必须保持 dual-profile shader（GLES3 + GL33），覆盖移动端
- ✅ 不新增第三方依赖 / CMake / 资源文件
- ✅ 不破坏 ABI（render_backend.h 只能追加 virtual 或修改已存在的 virtual 签名 — 后者仅允许外部无实现时）
- ✅ CI workflow 不改（已有 ssr.lua smoke 链）

---

## 12. 风险评估（预视）

| 风险项 | 概率 | 影响 | 缓解 |
|-------|-----|------|-----|
| bilateral 权重过强 → blur 不够 | 低 | 低 | σ=200 已在 SSAO 验证过；如 OK 对 SSR 也 OK |
| GLES3 precision 导致 exp() 不稳定 | 低 | 中 | 用 `precision highp float` + mediump fallback（和 SSAO 做法一致） |
| full-res depth 采样 half-res RT 时 UV 偏差 | 低 | 低 | GL 内置 UV normalization 自动解决；NEAREST 采样最近像素 |
| 性能回退（+20 tex fetch） | 极低 | 低 | 预算 0.05 ms 额外，在 1080p 可忽略 |

---

## 13. 验收标准（Align 阶段暂拟，CONSENSUS 阶段锁定）

**硬指标**（需通过）：
1. `FS_SSR_BLUR` shader 编译通过（GLES3 + GL33 两 profile）
2. SSRRenderer smoke 不回归：49/49 PASS
3. CI 6/6 green
4. demo_ssr 开 Blur（B 键）时反射 leak 视觉改善（需实机或截图）

**软指标**（期望）：
1. bilateral 默认 σ=200 视觉合理
2. 额外 GPU 耗时 < 0.1 ms @ 1080p（如有 profile 数据）
3. API_REFERENCE 文档同步更新（Phase E.11 描述）

---

> **下一步**：用户决策三个关键问题后，生成 CONSENSUS_PhaseE_11.md 锁定最终方案，然后进入 DESIGN 阶段。
