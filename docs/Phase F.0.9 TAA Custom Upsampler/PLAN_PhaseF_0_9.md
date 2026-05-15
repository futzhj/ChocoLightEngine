# Phase F.0.9 TAA Custom Upsampler — PLAN

> 6A 工作流 · 阶段 1-3 (Align + Architect + Atomize) 合并精简
> 关联：`ACCEPTANCE_PhaseF_0_9.md` / `FINAL_PhaseF_0_9.md` / `TODO_PhaseF_0_9.md`
> 基线：F.0 + F.0.1/0.2/0.3/0.4/0.5/0.6/0.7/0.8 (commit `f415890`)
> 目标工作量：4h

---

## 1. 背景与目标

Phase F.0.5 启用 halfRes history 时，history bilinear 上采样到 full-res sceneTex 引入 ~1px 模糊。F.0.1 unsharp / F.0.6 CAS sharpening 可弥补，但：

- `sharpness=0` 配置（最低开销）下走 `BlitTAAToHDR` GL_LINEAR stretch — 没有任何弥补，~1px 模糊明显
- 高画质 4K 移动场景希望保留 sharpness=0 的低开销，但提升 history → sceneTex 上采样质量

**Phase F.0.9 目标**：引入 Catmull-Rom bicubic 5-tap 优化版自定义上采样，仅在 `sharpness=0 && halfRes=true` 路径启用，让用户在性能与画质间多一个选择：

| 配置 | 行为 |
|------|------|
| `sharpness=0` + `halfRes=false` + `upscale=bilinear` (F.0.5 默认) | 走 BlitTAAToHDR 1:1 GL_NEAREST 老行为 |
| `sharpness=0` + `halfRes=true`  + `upscale=bilinear` (默认) | 走 BlitTAAToHDR GL_LINEAR stretch (老行为) |
| `sharpness=0` + `halfRes=true`  + `upscale=bicubic` (**新**) | 走 DrawTAAUpscalePass Catmull-Rom 5-tap (-50% blur vs bilinear) |
| `sharpness>0` (含 unsharp / CAS) | 不变（shader 内已自带 sample，无需切上采样） |

---

## 2. 任务范围与边界

### 包含

- 新 shader `FS_BICUBIC_UPSCALE_SOURCE` (GLES3 + GL33 双版本) — Catmull-Rom 5-tap 优化版
- GL33Backend: `programBicubicUpscale` + 2 uniform locs (uInputTex + uTexel)
- 新 backend virtual `DrawTAAUpscalePass(srcTex, dstFbo, srcW, srcH, dstW, dstH)` 默认 no-op
- TAARenderer state: `upscaleMode` (int, 0=bilinear / 1=bicubic), 默认 0
- Process 切分支: sharpness=0 + halfRes + bicubic mode → DrawTAAUpscalePass
- Lua API +2 (`Set/GetUpscaleMode`), TAA 子表 29 → 31 fn
- smoke / demo / docs / 6A 文档

### 不包含

- ❌ Lanczos-2 (25-tap, 工作量大且 1080p 边际收益小)
- ❌ FSR2 EASU/RCAS (留 Phase F.0.12)
- ❌ 影响 sharpness>0 路径 (sharpen/CAS shader 内部 sample 不动)
- ❌ 影响 halfRes=false 路径 (1:1 blit 无需上采样)

---

## 3. 决策矩阵（7/7 全自动决策）

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| **D1** | 上采样算法 | **Catmull-Rom bicubic 5-tap 优化** | bilinear 4-tap → bicubic 5-tap (~+25% ALU, -50% blur)；Lanczos-2 需 25 sample 太重 |
| **D2** | 影响范围 | **仅 BlitTAAToHDR 路径 (sharpness=0)** | sharpen/CAS pass 已自带 shader sample，强行替换会复杂化 sharpen 算法 |
| **D3** | sharpness=0 + halfRes=false 时 | **保持 GL_NEAREST 1:1 blit** | 不需要上采样，零 ALU 开销 |
| **D4** | 默认 mode | **`"bilinear"`** | 零回归 (F.0.5 行为完全等价) |
| **D5** | API 命名 | `SetUpscaleMode(mode)` | mode 字符串 (与 SetClipMode/SetSharpenMode 模式一致), 未来扩展 lanczos/fsr 灵活 |
| **D6** | 错误处理 | **大小写不敏感 + 未识别返 nil+err** | 与 SetClipMode/SetSharpenMode 同模式 |
| **D7** | 5-tap Catmull-Rom 优化版 | Sigggraph 2018 "Filmic SMAA Slidedeck" 算法 | 5 hardware bilinear 等效 9-tap bicubic, 工业标准 |

---

## 4. 算法详解 — Catmull-Rom 5-tap

### 4.1 数学原理

理论 Catmull-Rom bicubic 是 4×4 = 16 sample (Catmull-Rom 卷积核)。Sigggraph 2018 "Filmic SMAA" 提出 5-tap 优化版：

```
通过 hardware bilinear 一次 sample = 4 个像素加权平均
合理选择 5 个 sample 位置和权重 → 等效 9-tap bicubic
```

### 4.2 shader 实现 (5-tap Catmull-Rom)

```glsl
vec4 SampleTextureCatmullRom(sampler2D tex, vec2 uv, vec2 texSize) {
    vec2 samplePos = uv * texSize;
    vec2 texPos1 = floor(samplePos - 0.5) + 0.5;

    vec2 f = samplePos - texPos1;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);

    vec2 w12 = w1 + w2;
    vec2 offset12 = w2 / (w1 + w2);

    vec2 texPos0 = texPos1 - 1.0;
    vec2 texPos3 = texPos1 + 2.0;
    vec2 texPos12 = texPos1 + offset12;

    texPos0  /= texSize;
    texPos3  /= texSize;
    texPos12 /= texSize;

    vec4 result = vec4(0.0);
    result += texture(tex, vec2(texPos0.x, texPos0.y))   * (w0.x  * w0.y);
    result += texture(tex, vec2(texPos12.x, texPos0.y))  * (w12.x * w0.y);
    result += texture(tex, vec2(texPos3.x, texPos0.y))   * (w3.x  * w0.y);

    result += texture(tex, vec2(texPos0.x, texPos12.y))  * (w0.x  * w12.y);
    result += texture(tex, vec2(texPos12.x, texPos12.y)) * (w12.x * w12.y);
    result += texture(tex, vec2(texPos3.x, texPos12.y))  * (w3.x  * w12.y);

    result += texture(tex, vec2(texPos0.x, texPos3.y))   * (w0.x  * w3.y);
    result += texture(tex, vec2(texPos12.x, texPos3.y))  * (w12.x * w3.y);
    result += texture(tex, vec2(texPos3.x, texPos3.y))   * (w3.x  * w3.y);

    return result;
}
```

实际是 **9 sample**（3×3 优化布局），等效 16-tap Catmull-Rom。比标准 bicubic 节省 ~44%。

### 4.3 性能 (1080p)

| 算法 | sample 数 | ALU | 时间 (1080p) | 视觉模糊 |
|------|----------|-----|-------------|---------|
| GL_LINEAR (F.0.5 老路径) | 1 hw bilinear (4 pixel weighted) | ~2 | <0.01 ms | 1.0× (基准) |
| Catmull-Rom 5-tap (F.0.9) | 9 sample | ~30 | ~0.03 ms | 0.5× |
| Lanczos-2 (未实现) | 25 sample | ~80 | ~0.10 ms | 0.4× |

---

## 5. 状态机与接口契约

### 5.1 TAARenderer state 扩展

```cpp
struct State {
    // ... 现有字段
    int upscaleMode = 0;    // Phase F.0.9: 0=bilinear (F.0.5 默认) / 1=bicubic Catmull-Rom
};
```

### 5.2 Process 切分支

```cpp
if (sharpness > 0) {
    // F.0.1/F.0.6: sharpen/CAS pass (不变)
} else {
    // sharpness=0 路径
    if (halfResHistory && upscaleMode == 1) {
        // F.0.9: Catmull-Rom bicubic 上采样
        DrawTAAUpscalePass(historyTex, hdrFbo, historyW, historyH, width, height);
    } else {
        // F.0.5 老路径: bilinear stretch (halfRes) 或 1:1 nearest (full-res)
        BlitTAAToHDR(historyTex, hdrFbo, historyW, historyH, width, height);
    }
}
```

### 5.3 Lua API

```lua
-- "bilinear" / "bicubic" 大小写不敏感, 默认 "bilinear" (零回归)
TAA.SetUpscaleMode("bicubic")
TAA.GetUpscaleMode() → "bilinear" / "bicubic"

-- 错误处理
local ok, err = TAA.SetUpscaleMode("foo")  -- nil, "TAA.SetUpscaleMode: 未识别的 mode 'foo'..."
```

---

## 6. 实施顺序

### T0 (15 min) — PLAN（本文件）

### T1 (90 min) — shader + backend

- L2270+ / L3000+: GLES3 + GL33 FS_BICUBIC_UPSCALE_SOURCE (~50 行 GLSL)
- backend: programBicubicUpscale + 2 locs (uInputTex + uTexel)
- Init: glGetUniformLocation + 一次性 sampler 绑 slot 0
- Shutdown: glDeleteProgram + locs reset
- DrawTAAUpscalePass override (复用 vaoTonemap 全屏 quad)
- render_backend.h: virtual DrawTAAUpscalePass 默认 no-op

### T2 (45 min) — TAARenderer

- header: SetUpscaleMode / GetUpscaleMode 声明
- impl: state +1 + parseUpscaleMode_ 手写 case-insensitive (复用 parseClipMode_/parseSharpenMode_ 模式) + Process 切分支

### T3 (60 min) — Lua + smoke + demo + docs

- light_graphics.cpp: l_TAA_SetUpscaleMode / Get (与 SetClipMode 同模式) + taa_funcs +2 (29 → 31)
- taa.lua: 默认 "bilinear" / round-trip / 大小写不敏感 / invalid raise / type-error / 状态独立 / 八启共存
- demo_ssr: P 键 toggle upscaleMode + HUD `up=bil/bic` 字段
- Light_Graphics.md: 速查表 +1 + Set/GetUpscaleMode 完整文档段

### T4 (30 min) — 6A 文档

### T5 (15 min) — commit + push + CI 监控

---

## 7. 风险与缓解

| 风险 | 缓解 |
|------|------|
| Catmull-Rom shader 编译失败（旧 GPU）| DrawTAAUpscalePass 内部 fallback 到 BlitTAAToHDR；HUD warning |
| 边界 (UV [0,1]) wrapping 问题 | shader 内 clamp UV 到 [texelHalf, 1-texelHalf] |
| 与 sharpness>0 路径行为差异 | 文档明示：F.0.9 仅影响 sharpness=0 路径 |
| ALU 增加在低端 GPU 卡顿 | 用户可切回 "bilinear"; 默认 OFF |

---

## 8. 验收标准

### 功能
- [ ] upscaleMode 默认 "bilinear" (零回归)
- [ ] SetUpscaleMode("bicubic"/"bilinear") round-trip + 大小写不敏感
- [ ] invalid mode 返 nil+err, state 不变
- [ ] sharpness=0 + halfRes=true + upscale=bicubic → 走 DrawTAAUpscalePass
- [ ] sharpness>0 不受 upscaleMode 影响
- [ ] halfRes=false 不受 upscaleMode 影响
- [ ] F.0.x 八启共存 (含 motion-adaptive γ + sharpenMode + upscaleMode)

### CI
- [ ] GitHub Actions 6/6 平台 success
- [ ] runtime smoke 31/31 fn + 八启共存
