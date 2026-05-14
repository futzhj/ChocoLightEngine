# Phase E.12 Temporal SSR — CONSENSUS 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）— **方案锁定**
> **基线**：Phase E.11 Bilateral SSR Blur（commit `ebd069b`，CI 6/6 green）
> **拍板**：2026-05-12，用户选择「**全 A（业界标准 TAA-style，质量优先）**」
> **下一步**：进入阶段 2 Architect（DESIGN）

---

## 1. 锁定方案规格

### 1.1 核心算法

| 维度 | 锁定值 |
|---|---|
| Temporal 模式 | **Reverse Reprojection from Depth**（无 G-buffer velocity 改动） |
| Jitter 序列 | **Halton-2,3 8-sample，硬编码**（CPU 端表 + frameCounter 索引） |
| Jitter 范围 | **±0.5 pixel**（标准 TAA） |
| Jitter 应用 | SSR ray march 起点 UV 偏移（FS_SSR 内部） |
| Rejection 模式 | **mode=1 neighborhood clip**（9-tap min/max AABB clip） |
| 默认 TemporalEnabled | **true** |
| 默认 TemporalAlpha（history 权重） | **0.9** |
| TemporalAlpha clamp | **[0.5, 0.99]** |
| RejectionMode clamp | **{0, 1}**（int，0=current-depth threshold, 1=neighborhood） |
| History RT 分辨率 | **full-res RGBA16F**（与 SSR raw 一致） |
| History RT 数量 | **2 个 ping-pong** |
| 首帧处理 | `hasPrevViewProj=false` → shader 强制 outColor=current |
| Resize 处理 | 释放 history + reset hasPrevViewProj |
| 管线插入点 | **SSR raw → Temporal → Blur → Composite** |

### 1.2 用户拍板记录

> **选项 1：全 A（推荐）**<br>
> Q1=full-res RGBA16F history（~16MB@1080p）<br>
> Q2=默认 TemporalEnabled=true<br>
> Q3=neighborhood clip rejection<br>
> Q4=Halton-2,3 8-sample 硬编码<br>
> Q5=6 函数 API（jitter 不暴露）<br>
> Q6=默认 TemporalAlpha=0.9<br>
> 业界标准，质量优先

---

## 2. Lua API 锁定

### 2.1 新增 6 函数（Light.Graphics.SSR.* 子表）

| 函数 | 签名 | clamp | 默认 |
|---|---|---|---|
| `SetTemporalEnabled` | `(bool) -> nil` | — | `true` |
| `GetTemporalEnabled` | `() -> bool` | — | — |
| `SetTemporalAlpha` | `(number) -> nil` | `[0.5, 0.99]` | `0.9` |
| `GetTemporalAlpha` | `() -> number` | — | — |
| `SetRejectionMode` | `(int) -> nil` | `{0, 1}` | `1` |
| `GetRejectionMode` | `() -> int` | — | — |

**API 总数变化**：Light.Graphics.SSR 28 → **34**（+6）

### 2.2 ssr_funcs[] 注册位置

在现有 `Set/GetBlurDepthSigma` 之后追加，分组保持「ray march → composite → blur → bilateral → temporal」。

---

## 3. Shader 设计纲要

### 3.1 修改：FS_SSR（GLES3 + GL33 双 profile）

**新增 uniform**：
```glsl
uniform vec2 uJitterOffset;  // UV 空间偏移, backend 由 ±0.5 pixel / RT size 转换
```

**修改点**：
```glsl
// 原: vec2 uv = vUV;
vec2 uv = vUV + uJitterOffset;
// 后续 ray march 用 jittered uv
```

### 3.2 新增：FS_SSR_TEMPORAL（GLES3 + GL33 双 profile）

**uniform 表**：
| uniform | type | 含义 |
|---|---|---|
| `uCurReflectTex` | sampler2D (slot 0) | 当前帧 SSR raw |
| `uHistoryTex` | sampler2D (slot 1) | 上一帧 SSR temporal 输出 |
| `uDepthTex` | sampler2D (slot 2) | 当前帧 SSR depth |
| `uReprojectMat` | mat4 | `prevViewProj * curViewProjInv`（CPU 预计算） |
| `uInvProj` | mat4 | 当前帧 invProj（重建 view pos） |
| `uTexel` | vec2 | 1.0 / RT 尺寸 |
| `uBlendAlpha` | float | history 权重 [0.5, 0.99] |
| `uRejectionMode` | int | 0 / 1 |
| `uHasHistory` | int | 0 = 首帧, 1 = 有 history |

**主流程**：
```glsl
void main() {
    vec4 cur = texture(uCurReflectTex, vUV);

    if (uHasHistory == 0) {
        FragColor = cur;
        return;
    }

    // ① reproject
    float depth = texture(uDepthTex, vUV).r;
    vec4 ndc = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 prevClip = uReprojectMat * ndc;
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // ② out-of-screen rejection
    if (prevUV.x < 0.0 || prevUV.x > 1.0 ||
        prevUV.y < 0.0 || prevUV.y > 1.0) {
        FragColor = cur;
        return;
    }

    vec4 hist = texture(uHistoryTex, prevUV);

    // ③ neighborhood clip (mode=1)
    if (uRejectionMode == 1) {
        vec3 mn = cur.rgb, mx = cur.rgb;
        for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            vec3 s = texture(uCurReflectTex, vUV + uTexel * vec2(i, j)).rgb;
            mn = min(mn, s); mx = max(mx, s);
        }
        hist.rgb = clamp(hist.rgb, mn, mx);
    }

    // ④ blend
    FragColor = mix(cur, hist, uBlendAlpha);
}
```

---

## 4. Backend 接口变更

### 4.1 新增 3 个虚函数（render_backend.h）

```cpp
// History RT 管理（ping-pong）
virtual bool CreateSSRHistoryRT(int w, int h,
                                 uint32_t* outFbos2,
                                 uint32_t* outTexs2) { return false; }
virtual void DeleteSSRHistoryRT(uint32_t* fbos2, uint32_t* texs2) {}

// Temporal pass
virtual void DrawSSRTemporal(uint32_t curReflectTex,
                              uint32_t historyTex,
                              uint32_t depthTex,
                              uint32_t dstFbo,
                              int w, int h,
                              const float* reprojectMat4,
                              const float* invProjMat4,
                              float blendAlpha,
                              int   rejectionMode,
                              int   hasHistory) {}
```

### 4.2 修改 1 个虚函数（DrawSSR 增 jitter）

```cpp
virtual void DrawSSR(uint32_t depthTex, uint32_t normalTex, uint32_t hdrTex,
                     uint32_t dstFbo, int w, int h,
                     const float* projMat4, const float* invProjMat4,
                     int maxSteps, float stepSize, float thickness,
                     float maxDist, float edgeFade,
                     float jitterX, float jitterY) {}   // +2 参（默认 0.0 即旧行为）
```

**兼容性**：jitterX=jitterY=0.0 时画面与 Phase E.11 完全一致；外部调用方（SSRRenderer）按需传入。

---

## 5. SSRRenderer 状态扩展

### 5.1 State 字段（新增 9 个 + 1 数组）

```cpp
struct State {
    // ... Phase E.9-E.11 字段 ...

    // Phase E.12 — Temporal SSR
    bool      temporalEnabled    = true;        // 默认 ON
    float     temporalAlpha      = 0.9f;        // history 权重
    int       rejectionMode      = 1;           // 1 = neighborhood clip
    uint32_t  historyFbos[2]     = {0, 0};      // ping-pong
    uint32_t  historyTexs[2]     = {0, 0};
    int       historyIdx         = 0;           // 当前写入下标
    float     prevViewProj[16]   = {0};         // 上一帧矩阵缓存
    bool      hasPrevViewProj    = false;       // 首帧标志
    uint64_t  frameCounter       = 0;           // jitter 索引
};
```

### 5.2 Halton-2,3 8-sample 静态表

```cpp
// SSR 内部静态常量
static const float kHaltonJitter[8][2] = {
    { 0.0000f,  0.0000f},
    {-0.5000f,  0.3333f},
    { 0.2500f, -0.3333f},
    {-0.2500f,  0.1111f},
    { 0.3750f, -0.1111f},
    {-0.3750f,  0.4444f},
    { 0.1250f, -0.4444f},
    {-0.1250f,  0.2222f},
};
// 帧 N 取 kHaltonJitter[N % 8]
```

### 5.3 Process 流程（新增 reprojection 矩阵推导）

```cpp
void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // ... 现有 Phase E.9 自检 ...

    // 取当前 viewProj
    float curView[16], curProj[16];
    g.backend->GetView(curView);
    g.backend->GetProjection(curProj);
    float curViewProj[16];  Mat4Mul(curProj, curView, curViewProj);
    float invCurProj[16];   InvertMat4(curProj, invCurProj);

    // jitter
    float jitterX = 0, jitterY = 0;
    if (g.temporalEnabled) {
        int j = (int)(g.frameCounter % 8);
        jitterX = kHaltonJitter[j][0];
        jitterY = kHaltonJitter[j][1];
    }

    // 1. SSR raw with jitter
    g.backend->DrawSSR(g.depthTex, normalTex, hdrTex, g.reflectFbo,
                       g.srcW, g.srcH, curProj, invCurProj,
                       g.maxSteps, g.stepSize, g.thickness,
                       g.maxDistance, g.edgeFade,
                       jitterX, jitterY);

    // 2. Temporal pass (if enabled)
    uint32_t srcForBlur = g.reflectTex;
    if (g.temporalEnabled && g.historyFbos[0] && g.historyTexs[0]) {
        int writeIdx = g.historyIdx;
        int readIdx  = 1 - writeIdx;
        // reprojectMat = prevViewProj * invCurViewProj
        float invCurViewProj[16];  InvertMat4(curViewProj, invCurViewProj);
        float reprojMat[16];       Mat4Mul(g.prevViewProj, invCurViewProj, reprojMat);

        g.backend->DrawSSRTemporal(g.reflectTex, g.historyTexs[readIdx],
                                   g.depthTex, g.historyFbos[writeIdx],
                                   g.srcW, g.srcH,
                                   reprojMat, invCurProj,
                                   g.temporalAlpha, g.rejectionMode,
                                   g.hasPrevViewProj ? 1 : 0);

        srcForBlur = g.historyTexs[writeIdx];
        g.historyIdx = readIdx;
        memcpy(g.prevViewProj, curViewProj, sizeof(curViewProj));
        g.hasPrevViewProj = true;
    }

    // 3. Blur (Phase E.10/E.11) — 输入改用 srcForBlur
    // 4. Composite ...

    g.frameCounter++;
}
```

**注意**：实际实现时 `Mat4Mul` 和 `InvertMat4` 需在 ssr_renderer.cpp 中确保存在（InvertMat4 已有；Mat4Mul 需新加 helper）。

---

## 6. Demo 增强（samples/demo_ssr/main.lua）

### 6.1 新键位

| 键 | 操作 |
|---|---|
| `T` | 切换 TemporalEnabled |
| `U` | TemporalAlpha −0.02 |
| `I` | TemporalAlpha +0.02 |
| `R` | 切换 RejectionMode（0 ⇌ 1） |

### 6.2 HUD 行增加

```
Temporal: ON | alpha 0.90 | reject mode 1 (neighborhood)
```

### 6.3 Reset（H 键）扩展

把 Temporal=true / alpha=0.9 / rejection=1 一同 reset。

---

## 7. smoke 增量（scripts/smoke/ssr.lua）

### 7.1 检查点扩展（60 → 75-80）

新增 section M（Phase E.12 Temporal）：

- **默认值检查**（3 项）：TemporalEnabled=true, TemporalAlpha=0.9, RejectionMode=1
- **round-trip 检查**（3 项）：Set 后 Get 一致
- **clamp 边界**（6 项）：alpha [0.5, 0.99]，mode {0, 1}，越界回弹
- **API 函数存在性**（6 项）：6 个新函数 type 检查
- **联动检查**（3-5 项）：与 BlurEnabled / BilateralEnabled 任意组合下 SSR.Enable/Disable 无 crash

预计净增 **15-20 检查点**。

---

## 8. CI 集成

- 现有 `windows-runtime-smoke` 已涵盖 `scripts/smoke/ssr.lua`，无需新 workflow
- 期望 commit hash 触发 CI 后：6 平台 build success + Windows smoke `[Phase E.12] 通过 N / 失败 0`

---

## 9. 任务边界（明确不做）

| 不做项 | 原因 |
|---|---|
| G-buffer velocity attachment | HDR 全管线改动，留完整 TAA Phase（Phase E.13 候选） |
| 动态物体精准 reproject | 当前 reverse-reproj 已知限制，TODO 说明 |
| Variance-aware adaptive alpha | 复杂度高，留 Phase E.14 |
| Roughness-aware temporal weight | 需 G-buffer roughness 通道，留 Phase E.13 联动 |
| SVGF / A-trous 高级滤波 | 留 Phase E.15+ |
| Mat4Mul Lua API 暴露 | 内部 helper，不出栈 |

---

## 10. 验收标准（最终版）

### 10.1 功能验收

- [ ] T1 Backend：3 新接口 + 1 接口扩展（DrawSSR +2 参），0 编译 error
- [ ] T2 Shader：FS_SSR 增 jitter，FS_SSR_TEMPORAL 新增，双 profile（GL33 + GLES3）
- [ ] T3 SSRRenderer：State 字段扩展，setter/getter，Process 流水线插入
- [ ] T4 Lua API：6 函数实现 + ssr_funcs[] 注册
- [ ] T5.1 smoke：75-80 检查点全过
- [ ] T5.2 demo：T/U/I/R 键工作
- [ ] T5.3 docs：7 件套（ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO）
- [ ] T5.4 CI 6/6 green

### 10.2 质量验收

- [ ] 默认值符合规格（temporalEnabled=true, alpha=0.9, mode=1）
- [ ] clamp 边界回弹正确
- [ ] round-trip 通过
- [ ] 联动测试无 crash
- [ ] temporalEnabled=false 时画面 = Phase E.11 main HEAD 行为（向后兼容）

### 10.3 性能验收

- [ ] 1080p GPU 额外开销 < +0.5 ms（用户真机验收，留 TODO）
- [ ] 内存增量 ~16 MB（full-res history × 2 ping-pong）
- [ ] 无新 driver-specific 兼容性问题（CI 6 平台 build 验证）

---

## 11. 流程与签字

| 阶段 | 状态 | 签字 |
|---|---|---|
| ALIGNMENT 完成 | ✅ | 2026-05-12 |
| 用户拍板「全 A」 | ✅ | 2026-05-12 |
| CONSENSUS 锁定 | ✅ | 2026-05-12 |
| DESIGN 进行 | ⏳ | — |
| TASK 进行 | ⏳ | — |
| APPROVE 进行 | ⏳ | — |
| Automate（T1-T5） | ⏳ | — |
| ACCEPTANCE | ⏳ | — |

**下一步**：阶段 2 Architect → `DESIGN_PhaseE_12.md`（架构图、分层、接口契约、数据流、异常处理）。
