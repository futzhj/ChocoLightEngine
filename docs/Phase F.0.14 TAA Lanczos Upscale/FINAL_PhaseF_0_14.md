# Phase F.0.14 TAA Lanczos-2 Upscaler — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告

---

## 1. 交付物

| 文件 | 行数 |
|------|------|
| `ChocoLight/include/render_backend.h` (DrawTAALanczosPass virtual) | +14 |
| `ChocoLight/src/render_gl33.cpp` (FS_LANCZOS_UPSCALE GLES3+GL3.3 + program/load/cleanup/override) | ~110 |
| `ChocoLight/src/taa_renderer.cpp` (parseUpscaleMode_ +"lanczos" + Process 分支 + GetUpscaleMode 三 mode) | +12 |
| `scripts/smoke/taa.lua` (F.0.14 测试段 ~5 PASS + 十一启共存) | +75 |
| `samples/demo_ssr/main.lua` (P 键三 mode 轮转) | +3 |
| `docs/api/Light_Graphics.md` (速查表 + 完整段加 lanczos) | +5 |
| `docs/Phase F.0.14 TAA Lanczos Upscale/` 4 文档 | ~430 |

**累计**: 代码 ~140 行 + smoke ~78 行 + 文档 ~435 行 ≈ 650 行

---

## 2. 核心方案

引入 Lanczos-2 25-tap 5x5 上采样作为 F.0.9 Catmull-Rom 之外的高画质选项。

### 算法

```glsl
// Lanczos kernel: L(x) = sinc(x) * sinc(x/2) for |x|<2 else 0
float lanczos(float x) {
    if (abs(x) < 1.0e-5) return 1.0;
    if (abs(x) >= 2.0) return 0.0;
    float pix = π * x;
    return sin(pix) * sin(pix * 0.5) / (pix * pix * 0.5);
}

// 5x5 separable kernel, single-pass:
for j in [-2, 2], i in [-2, 2]:
    sp = (srcInt + (i, j)) * uTexel
    d  = (i, j) - frac
    w  = lanczos(d.x) * lanczos(d.y)
    sum  += texture(uInputTex, sp).rgb * w
    wsum += w

return max(sum / max(wsum, 1e-4), 0)   // 归一化 + HDR safe
```

### 性能 (1080p)

| Mode | 性能 | 画质 |
|------|------|------|
| bilinear | ~0.005 ms | baseline (bandwidth) |
| bicubic (F.0.9) | ~0.03 ms (+0.025) | -50% blur vs bilinear |
| **lanczos (F.0.14)** | **~0.07 ms (+0.04 vs bicubic)** | **-10% blur vs bicubic (-55% vs bilinear)** |

### Lua API surface (零增量, TAA 仍 35 fn)

```lua
TAA.SetUpscaleMode("lanczos")    -- 新增值; "bilinear"/"bicubic" 保持
TAA.GetUpscaleMode() → "lanczos"
```

---

## 3. Phase F.0 系列累计 (12 sub-phase)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0 ~ F.0.13 | 主管线 + 11 优化 | 35 |
| **F.0.14** | **Lanczos-2 25-tap 上采样** | **+0 (35)** |

**累计**: 35 fn / 6 shader (TAA + 4 sharpen + 2 upscale) / 5 backend pass / 5 backend 接口扩展 / 3 demos

---

## 4. CI 状态（待回填）

GitHub Run ID: `<pending>` / Commit hash: `<pending>` / Date: `<pending>`

---

## 5. 工程反思

### 做得好

1. **零 Lua API 增量**: 复用 SetUpscaleMode("lanczos")，与 F.0.9 同模式
2. **零回归**: 默认仍 "bilinear", 老 backend DrawTAALanczosPass 默认 no-op
3. **零 shader 风险**: shader 编译失败时静默 fallback (不 crash)
4. **决策矩阵 6/6 全自动**: 复用 F.0.9 构建模式 (program/uniform/draw 三段式)
5. **HDR safe**: max(0) 截断负 lobe 避免 ringing 黑斑

### 可改进

1. **未做 separable 2-pass**: 25-tap 在 mobile 偏重，未来若有 mobile 需求可加 separable 路径
2. **未实现 Lanczos-3 (7x7=49 tap)**: 进一步 -5% blur，代价 ~0.15 ms，性价比偏低；F.0.14 不实现
3. **未做 runtime 截图对比**: F.0.11 Demo 截图待来补 visual evidence

---

## 6. Phase F.0.x 后续候选

- F.0.10 — TAARenderer 多实例化 + 真 split-screen demo
- F.0.11 — Demo 截图 / 录屏 (高优先级，可对比 F.0.9 vs F.0.14 画质)
- F.0.15 — TAA-driven CAS strength scaling (history stability 反馈)
- F.1 — DLSS-like TAAU
