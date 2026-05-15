# Phase F.0.10.5 — Shader uvBounds 完美边界 DESIGN 架构设计

> 6A 工作流 · 阶段 2 (Architect) · 设计
> 关联: `ALIGNMENT_PhaseF_0_10_5.md`

---

## 1. 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│            Renderer::Process(rgnX, rgnY, rgnW, rgnH)          │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ ComputeUvBounds(rgnX, rgnY, rgnW, rgnH, fullW, fullH)  │  │
│  │   → uvMinX, uvMinY, uvMaxX, uvMaxY (with 0.5 texel inset)│ │
│  └────────────────────────────────────────────────────────┘  │
│                          │                                    │
│                          ▼                                    │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ backend->DrawXxx(..., uvBounds[4])                     │  │
│  │   GL3.3 / GLES3 backend                                │  │
│  └────────────────────────────────────────────────────────┘  │
│                          │                                    │
│                          ▼                                    │
│  ┌────────────────────────────────────────────────────────┐  │
│  │ shader: uniform vec4 uUvBounds; (default = 0,0,1,1)    │  │
│  │   texture(..., clamp(uv, uUvBounds.xy, uUvBounds.zw))  │  │
│  └────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────┘
```

---

## 2. 数据流 (mip 缩放)

### 2.1 TAA / Sharpen (full-res, 直接传 uvBounds)

```
TAARenderer::Process(rgnX, rgnY, rgnW, rgnH)
   │
   ▼ ComputeUvBounds(rgnX, rgnY, rgnW, rgnH, w, h)
   │
   ▼ uvBounds = (uMinX + 0.5/w, uMinY + 0.5/h, uMaxX - 0.5/w, uMaxY - 0.5/h)
   │
   ▼ backend->DrawTAAPass(..., uvBounds)  // 传 vec4
   │
   ▼ backend->DrawTAASharpenPass(..., uvBounds)
```

### 2.2 Bloom mip 链 (每级 bounds 缩放)

```
BloomRenderer::Process(rgnX, rgnY, rgnW, rgnH)
   │
   ▼ Level 0 (full-res) — bright pass: 中心采样, 无需 uvBounds
   │
   ▼ Level i (downsample, i=1..N):
   │   rgnX_i = rgnX >> i;   rgnY_i = rgnY >> i
   │   rgnW_i = max(1, rgnW >> i);   rgnH_i = max(1, rgnH >> i)
   │   srcW_i, srcH_i = mip(i-1) size
   │   uvBounds_i = ComputeUvBounds(rgnX_{i-1}, rgnY_{i-1}, rgnW_{i-1}, rgnH_{i-1}, srcW_i, srcH_i)
   │   backend->DrawBloomDownsample(..., uvBounds_i)
   │
   ▼ Level i (upsample, i=N-1..0):
   │   src mip (i+1), dst mip i
   │   uvBounds_i = ComputeUvBounds(rgnX_{i+1}, ..., srcW_{i+1}, srcH_{i+1})
   │   backend->DrawBloomUpsample(..., uvBounds_i)
   │
   ▼ Composite (i=0): 与 upsample 同 shader, uvBounds 取 mip 1
```

**注**: 老 F.0.10.3 已经实现 mip region 缩放 (`@bloom_renderer.cpp:203-269`), 仅需在每级算完 region 后追算 uvBounds 即可.

---

## 3. Shader 修改方案

### 3.1 FS_TAA (4 处采样, GL3.3 + GLES3 双版)

**修改前** (line 3082-3134):
```glsl
uniform sampler2D uCurHdrTex;
// ...
s = texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb;  // ⚠️ 跨边界
// ... 8 个 ±1 邻域采样 ...
```

**修改后**:
```glsl
uniform sampler2D uCurHdrTex;
uniform vec4 uUvBounds;   // Phase F.0.10.5 — (uMin.xy, uMax.xy); default (0,0,1,1)

vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

// neighborhood clip (RGB AABB):
s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb;
// ... 全部 8 个 ±1 邻域采样包 ClampUV ...

// SampleVelocity 9-tap dilation:
vec2 v = DecodeVelocity(texture(uVelocityTex, ClampUV(uv + vec2(float(dx), float(dy)) * uTexel)).rg);

// history reproject: prevUV 出 [0,1] reject 改为出 uvBounds reject
if (prevUV.x < uUvBounds.x || prevUV.x > uUvBounds.z ||
    prevUV.y < uUvBounds.y || prevUV.y > uUvBounds.w) {
    FragColor = cur; return;
}
vec4 hist = texture(uHistoryTex, ClampUV(prevUV));   // 防御性 clamp
```

### 3.2 FS_SHARPEN (4-tap, GL3.3 + GLES3 双版)

**修改前** (line 3168-3171):
```glsl
vec3 n = texture(uInputTex, vUV + vec2(0.0, uTexelSize.y)).rgb;
// ... NSEW 4 tap ...
```

**修改后**:
```glsl
uniform vec4 uUvBounds;    // default (0,0,1,1)
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

vec3 n = texture(uInputTex, ClampUV(vUV + vec2(0.0, uTexelSize.y))).rgb;
// ... 4 tap 全部 ClampUV ...
```

### 3.3 FS_BLOOM_DOWN (13-tap, GL3.3 + GLES3 双版)

**修改前** (line 1454-1467):
```glsl
vec3 A = texture(uSrc, vUV + uTexel * vec2(-2.0, 2.0)).rgb;
// ... 13 个 ±2 / ±1 邻域采样 ...
```

**修改后**:
```glsl
uniform vec4 uUvBounds;    // default (0,0,1,1)
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

vec3 A = texture(uSrc, ClampUV(vUV + uTexel * vec2(-2.0, 2.0))).rgb;
// ... 13 个采样全部 ClampUV ...
```

### 3.4 FS_BLOOM_UP (tent 3x3, GL3.3 + GLES3 双版)

**修改前** (line 1486-1495):
```glsl
vec3 c = texture(uSrc, vUV).rgb * 4.0;
c += texture(uSrc, vUV + vec2(-d.x, 0.0)).rgb * 2.0;
// ... 9-tap tent ...
```

**修改后**:
```glsl
uniform vec4 uUvBounds;    // default (0,0,1,1)
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }

vec3 c = texture(uSrc, ClampUV(vUV)).rgb * 4.0;
c += texture(uSrc, ClampUV(vUV + vec2(-d.x, 0.0))).rgb * 2.0;
// ... 9-tap tent 全部 ClampUV ...
```

---

## 4. Backend 接口扩展

### 4.1 共同模式 (vs F.0.10.3 region 参数)

| F.0.10.3 (region) | F.0.10.5 (uvBounds) |
|--------------------|---------------------|
| 加默认 0 region 参数 (int×4) | 加默认 nullptr uvBounds 参数 (const float*) |
| 零参数 = 全屏老路径 | nullptr = shader 内 (0,0,1,1) 默认值 = 全屏老路径 |
| backend 内 GL_SCISSOR_TEST | backend 内 glUniform4fv(locUvBounds, ...) |

### 4.2 改动接口清单

| 接口 | F.0.10.3 签名 | F.0.10.5 追加 |
|------|--------------|--------------|
| `DrawTAAPass` | (..., int rgnX, rgnY, rgnW, rgnH) | `+ const float* uvBounds /* nullptr OK */` |
| `DrawTAASharpenPass` | (...) | `+ const float* uvBounds` |
| `DrawBloomDownsample` | (..., int rgnX, ...) | `+ const float* uvBounds` |
| `DrawBloomUpsample` | (..., int rgnX, ...) | `+ const float* uvBounds` |

**默认 nullptr 零回归**: backend 内 `if (uvBounds && locUvBounds >= 0) glUniform4fv(...); else use default (0,0,1,1) (shader 端 uniform 初始化已是 0)`.

> **关键**: shader 端 uniform 不显式初始化时, OpenGL 标准保证全 0. 而 `clamp(uv, vec2(0), vec2(0))` 会把所有 uv 强制成 0 — **错误**! 必须 caller 总是上传 uvBounds, 或 shader 端检测 `if (uUvBounds == vec4(0))` 退化.
>
> **方案**: 用 sentinel 值 `(0, 0, 1, 1)` 作为 "no clamping" — caller 传 nullptr 时, backend 内自动上传 `{0.0f, 0.0f, 1.0f, 1.0f}`. shader 不变, `clamp(uv, vec2(0), vec2(1))` 即恒等 (因为 uv 本来就在 [0,1]).

### 4.3 Caller (Renderer::Process)

```cpp
// TAARenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH)
float uvBoundsBuf[4];
const float* uvBounds = nullptr;
if (rgnW > 0 && rgnH > 0) {
    // compute uvBounds with 0.5 texel inset
    uvBoundsBuf[0] = ((float)rgnX + 0.5f) / (float)w;
    uvBoundsBuf[1] = ((float)rgnY + 0.5f) / (float)h;
    uvBoundsBuf[2] = ((float)(rgnX + rgnW) - 0.5f) / (float)w;
    uvBoundsBuf[3] = ((float)(rgnY + rgnH) - 0.5f) / (float)h;
    uvBounds = uvBoundsBuf;
}
backend->DrawTAAPass(..., rgnX, rgnY, rgnW, rgnH, uvBounds);
```

---

## 5. 风险矩阵

| Risk ID | 描述 | 概率 | 影响 | 缓解 |
|---------|------|------|------|------|
| R-1 | shader 加 uniform 后老代码 default 0 触发 uv clamp 错误 | 高 | 高 (全屏渲染崩) | caller 传 nullptr 时, backend 自动上传 (0,0,1,1) 作为 no-op 默认值 |
| R-2 | GLES 3.0 与 GL 3.3 shader 双源不一致 | 中 | 中 (web/移动平台 build fail) | 每改 1 处 GL3.3, 同步改 GLES3, 字面对照 |
| R-3 | Bloom mip 链 uvBounds 算错 (off-by-one) | 中 | 中 (边界 1px 锯齿) | 用 `std::max(1, ...) clamp` + 与 F.0.10.3 mip region 算法一致 |
| R-4 | 0.5 texel inset 过严, 边界 1px 黑边 | 低 | 低 | 业界标准, UE 同模式; 若问题改为 0 inset 退回 |
| R-5 | uvBounds uniform location 取不到 (老 shader fallback) | 低 | 低 | `glGetUniformLocation` 失败时 backend 跳过上传, 不致 crash |
| R-6 | Lua API 不变 (uvBounds 是 backend 内部) | 0 | 0 | F.0.10.5 不动 Lua API surface |

---

## 6. 测试策略

### 6.1 单元 (smoke)
- 现有 taa.lua / bloom.lua region defense 测试不变 (零回归验证)
- 不新增 smoke fn surface (无 Lua API 变更)

### 6.2 视觉 (demo_taa_split2)
- F.0.10.4 demo 直接受益: 边界 ~1px ghost 消失
- 用户跑 demo 肉眼对比 F.0.10.4 vs F.0.10.5

### 6.3 跨平台
- CI 6 平台 (Win/Linux/macOS/Web/Android/iOS) build 验证 shader 双源
- GLES 3.0 路径专项 (Web/iOS) 验证 GLSL 字面 portable

### 6.4 性能
- ALU 增量: 每 tap + 1 clamp (2 min + 2 max) = 4 ALU
- TAA 8-tap clip: +32 ALU (相比 8 个 texture sample ~16 cycle, 几乎免费)
- Bloom DOWN 13-tap: +52 ALU (相比 13 个 sample, < 1% overhead)
- 预期: < 0.05ms/frame 性能影响, 1080p Mid 显卡 acceptable

---

## 7. 决策追溯

详见 `ALIGNMENT_PhaseF_0_10_5.md` §5 "关键决策清单" (D-1 ~ D-8).
