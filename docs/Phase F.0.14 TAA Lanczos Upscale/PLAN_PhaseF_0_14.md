# Phase F.0.14 TAA Lanczos-2 25-tap Upscaler — PLAN

> 6A 工作流 · 阶段 1+2+3 合并
> 关联：`ACCEPTANCE_PhaseF_0_14.md` / `FINAL_PhaseF_0_14.md` / `TODO_PhaseF_0_14.md`
> 基线：F.0 + F.0.1~0.9 + F.0.12 + F.0.13 (commit `3923584`)

---

## 1. Align (对齐)

### 1.1 业务目标

引入 **Lanczos-2 25-tap 上采样** 作为 F.0.9 Catmull-Rom 9-tap 之外的高画质上采样选项。在 sharpness=0 + halfResHistory=true 路径下，提供 -10% blur vs Catmull-Rom（-55% vs bilinear）的画质提升，代价 +0.04 ms。

### 1.2 现状（基线）

- F.0.9: SetUpscaleMode("bilinear" / "bicubic")，bicubic = Catmull-Rom 9-tap (Sigggraph 2018 Filmic SMAA)
- 上采样仅在 halfResHistory=true && sharpness=0 时生效
- 老 Catmull-Rom 已较 bilinear -50% blur，但仍有微弱模糊；超高画质场景 (4K + 桌面 GPU) 期望进一步提升

### 1.3 用户价值

- **画质再 -10% blur** (Lanczos-2 vs Catmull-Rom)，4K 桌面下尤其明显
- **零 API 增量** (复用 SetUpscaleMode，仅多一个 "lanczos" 值)
- **零回归** (默认仍 "bilinear", 老调用 100% 兼容)

### 1.4 算法

```
Lanczos-2 kernel:
    sinc(x) = sin(πx) / (πx)
    L(x) = sinc(x) * sinc(x/2)   for |x| < 2 else 0

5x5 separable convolution (单 pass 25-tap):
    For each output pixel:
        srcCoord = vUV / uTexel
        srcInt   = floor(srcCoord - 0.5) + 0.5
        frac     = srcCoord - srcInt
        sum, wsum = 0
        For j in [-2, 2], i in [-2, 2]:
            sp = (srcInt + vec2(i, j)) * uTexel
            d  = vec2(i, j) - frac
            w  = lanczos(d.x) * lanczos(d.y)
            sum  += texture(uInputTex, sp).rgb * w
            wsum += w
        return sum / wsum
```

---

## 2. Architect (架构)

### 2.1 决策矩阵 (6/6 全自动决策)

| # | 决策 | 选择 | 依据 |
|---|------|------|------|
| D1 | API surface | 复用 SetUpscaleMode (string)，加 "lanczos" 值 | 与 F.0.9 一致, 零 Lua API 增量 |
| D2 | shader 实现 | 单 pass 25-tap (5x5 unrolled) | 与 F.0.9 单 pass 同结构, 不需 intermediate RT |
| D3 | sinc 实现 | 直接 sin/π 计算 (3 abs + 2 sin + 2 mul + 1 div) | mobile GLES highp 精度足够; LUT 反而增 bandwidth |
| D4 | 边界处理 | wsum 归一化 (除 wsum) | 处理 numerical drift, 与 Catmull-Rom 不同 (后者不需归一化) |
| D5 | 默认值 | 保持 "bilinear" | 零回归, 用户主动切换 "lanczos" 才生效 |
| D6 | mode 编码 | 0=bilinear / 1=bicubic / 2=lanczos | 升序扩展, 不破坏 F.0.9 现有 0/1 |

### 2.2 接口契约

```cpp
// render_backend.h (新增 1 virtual)
/// Phase F.0.14 — Lanczos-2 25-tap 5x5 单 pass 上采样
/// 默认 no-op (老 backend 不支持时静默回退到 BlitTAAToHDR)
virtual void DrawTAALanczosPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                int /*srcW*/, int /*srcH*/,
                                int /*dstW*/, int /*dstH*/) {}

// GL33Backend override 同 F.0.9 DrawTAAUpscalePass 模式 + 新 program
void DrawTAALanczosPass(...) override;
GLuint programLanczosUpscale = 0;
GLint  locLanczos_InputTex   = -1;
GLint  locLanczos_Texel      = -1;
```

```cpp
// taa_renderer.cpp parseUpscaleMode_ 加 "lanczos" 分支
static int parseUpscaleMode_(const char* mode) {
    // ... 现有 "bilinear" "bicubic" ...
    if (eq(mode, "lanczos")) return 2;
    return -1;
}

// Process 内 sharpness=0 路径
if (g.halfResHistory && g.upscaleMode == 1) {
    g.backend->DrawTAAUpscalePass(...);     // F.0.9 Catmull-Rom
} else if (g.halfResHistory && g.upscaleMode == 2) {
    g.backend->DrawTAALanczosPass(...);     // F.0.14 Lanczos-2
} else {
    g.backend->BlitTAAToHDR(...);           // F.0.5 bilinear
}
```

```lua
-- Lua API 不变 (零增量), 仅 SetUpscaleMode 接受新值
TAA.SetUpscaleMode("lanczos")    -- 新增值; "bilinear"/"bicubic" 保持
TAA.GetUpscaleMode() → "lanczos"
```

### 2.3 shader 关键片段 (GLES 3.0)

```glsl
#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;
uniform sampler2D uInputTex;
uniform vec2 uTexel;       // 1.0 / (srcW, srcH)

float lanczos(float x) {
    if (abs(x) < 1.0e-5) return 1.0;
    if (abs(x) >= 2.0) return 0.0;
    float pix = 3.141592653589793 * x;
    return (sin(pix) * sin(pix * 0.5)) / (pix * pix * 0.5);
}

void main() {
    vec2 srcCoord = vUV / uTexel;                // 在 src 像素空间的连续坐标
    vec2 srcInt   = floor(srcCoord - 0.5) + 0.5; // 中心 src 像素的整数中心
    vec2 frac     = srcCoord - srcInt;           // (0..1) sub-pixel offset
    vec3 sum  = vec3(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            vec2 sp = (srcInt + vec2(float(i), float(j))) * uTexel;
            vec2 d  = vec2(float(i), float(j)) - frac;
            float w = lanczos(d.x) * lanczos(d.y);
            sum  += texture(uInputTex, sp).rgb * w;
            wsum += w;
        }
    }
    FragColor = vec4(sum / wsum, 1.0);
}
```

---

## 3. Atomize (原子化)

| ID | 内容 | 输出 |
|----|------|------|
| T0 | PLAN | PLAN_PhaseF_0_14.md (本文档) |
| T1 | backend DrawTAALanczosPass virtual + FS_LANCZOS_UPSCALE GLES3+GL3.3 + GL33 override | render_backend.h +8 / render_gl33.cpp ~140 (shader×2 + program load + override) |
| T2 | TAARenderer parseUpscaleMode_ +"lanczos" + Process 分支 | taa_renderer.cpp ~12 |
| T3 | smoke + demo + docs | smoke +30 / demo 0 (P 键已轮转) / docs +30 |
| T4 | 6A ACCEPTANCE/FINAL/TODO | docs/Phase F.0.14 .../*.md |
| T5 | commit + push + CI 6/6 | GitHub `<sha>` |

---

## 4. 影响范围 / 兼容性

| 维度 | 影响 |
|------|------|
| 默认行为 | upscaleMode="bilinear" → 走 BlitTAAToHDR, 零回归 |
| 老 backend 兼容 | DrawTAALanczosPass 默认 no-op, 自动 fallback 到 Blit |
| 与 F.0.9 bicubic | 完全独立 (不同 mode value), 切换即生效 |
| 与 sharpness > 0 | 不生效 (sharpen pass 优先走 sharpenMode) |
| 与 halfResHistory=false | 不生效 (full-res 时 1:1 blit, 不需上采样) |
| Lua API 增量 | 0 (复用 SetUpscaleMode) |

---

## 5. 风险与对策

| 风险 | 等级 | 对策 |
|------|------|------|
| 25-tap 在 mobile GPU 性能不足 | 🟡 中 | doc 标注仅推荐桌面/4K 场景; mobile 用 "bicubic" |
| sinc 数值精度 (highp 在 some GLES 设备 fp32) | 🟢 低 | abs(x)<1e-5 提前返 1.0 (避免 0/0) |
| wsum 归一化 vs 直接乘 | 🟢 低 | Lanczos-2 sum_w 不严格=1 (理论 1, 数值 ~0.99-1.01); 归一化保稳 |
| GLES 3.0 25-iter for 展开 | 🟢 低 | const bound 必展开; 着色器 ~80 ALU + 25 fetch, 在 budget 内 |

---

## 6. 验收标准

### 功能
- [ ] `SetUpscaleMode("lanczos")` round-trip
- [ ] `SetUpscaleMode("Lanczos")` 大小写不敏感
- [ ] `SetUpscaleMode("LANCZOS")` 大小写不敏感
- [ ] 未识别字符串保持当前 state (与 F.0.9 一致)
- [ ] sharpness>0 时切 lanczos 不生效 (走 sharpen pass)
- [ ] halfRes=false 时切 lanczos 不生效 (走 BlitTAAToHDR 1:1)
- [ ] F.0.1+0.2+0.3+0.4+0.5+0.6+0.8+0.9+0.12+0.13+0.14 十一启共存

### CI
- [ ] runtime smoke 35/35 fn (Lua API 不变) + lanczos 测试段
- [ ] GitHub Actions 6/6 平台 success

---

## 7. 估算

| 项 | 估算 |
|----|------|
| 代码 (backend shader×2 + program + override + TAARenderer + parseMode) | ~160 行 |
| smoke + demo + docs | ~60 行 |
| 6A 文档 (4 份) | ~400 行 |
| 实施时间 | ~4 小时 |
| 总变更行 | ~620 行 |
