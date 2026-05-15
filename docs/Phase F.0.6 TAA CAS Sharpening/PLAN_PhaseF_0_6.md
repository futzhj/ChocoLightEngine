# Phase F.0.6 TAA CAS Sharpening — PLAN

> 6A 工作流 · 阶段 1-3 (Align + Architect + Atomize) 合并精简
> 关联：`ACCEPTANCE_PhaseF_0_6.md` / `FINAL_PhaseF_0_6.md` / `TODO_PhaseF_0_6.md`
> 基线：F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.7 (commit `a858a29`)
> 目标工作量：4h

---

## 1. 背景与目标

Phase F.0.1 已实现 4-tap unsharp mask sharpening（`sharpened = c + (c - avg4) × sharpness`），但有缺陷：

- **低对比区域被锁牰**：平滑天空 / 阴影里的轻微噪声会被等强度放大 → 视觉噪点
- **HDR firefly 加剧**：超亮像素 (c > 1.0) 被锐化后变得更亮，与 F.0.4 anti-flicker 矛盾
- **均匀强度**：不区分边缘强弱，导致细节区域过锐 / 平滑区域噪点

**Phase F.0.6 目标**：引入 AMD FidelityFX FSR1 的 **CAS (Contrast-Adaptive Sharpening)** 算法作为 F.0.1 的可选替代：

- **contrast-adaptive**：邻域 min/max 计算 dynamic range，低对比区域自动减弱锐化
- **HDR safe**：clamp 输出防 firefly 加剧
- **perceptual gamma**：sqrt 转换让人眼感受更线性

---

## 2. 任务范围与边界

### 包含

- 新增 `FS_CAS` shader (GLES3 + GL33 双版本)，与 `FS_SHARPEN` (F.0.1) 并存
- 新增 GL33Backend `programCAS` + 3 个 uniform locs (uInputTex / uTexelSize / uSharpness)
- 新增 backend virtual `DrawTAACASPass(srcTex, dstFbo, w, h, sharpness)`
- TAARenderer 加 `sharpenMode` state ("unsharp" / "cas")，Process 切分支
- Lua API + 2 fn (`SetSharpenMode` / `GetSharpenMode`)，TAA 子表 23 → 25 fn
- `taa.lua` smoke +9 个 PASS
- `demo_ssr` 加 Z 键切 sharpen mode + HUD 字段 `mode=unsharp/cas`
- `demo_taa_compare` preset 9 (CAS 模式) 可选扩展（暂不加，避免破坏 8 preset 设计）
- `Light_Graphics.md` 速查表 + Set/GetSharpenMode 完整文档段
- 6A 文档 4 件套

### 不包含

- ❌ 替换 F.0.1 unsharp mask（共存模式，零回归）
- ❌ FSR2 完整 algorithm (太复杂，留 Phase F.1)
- ❌ 5-tap CAS adaptive radius 调整（固定 5-tap, FSR1 简化版）
- ❌ 自动选 mode（用户 opt-in）

---

## 3. 决策矩阵（6/6 全自动决策）

| # | 决策点 | 候选 | 选择 | 理由 |
|---|--------|------|------|------|
| **D1** | CAS vs replace F.0.1 | (a) 替换 4-tap / (b) 共存模式选 | **(b) 共存** | 4-tap 在低对比场景已够用; CAS 多花 ALU 在低端硬件不友好；用户选 |
| **D2** | sharpness 语义统一 | (a) 各 mode 独立 sharpness / (b) 统一 sharpness 字段 | **(b) 统一** | 简化 API; mode 切换不需重设 sharpness; 内部按 mode 重映射 (unsharp [0,2] / CAS [0,1]) |
| **D3** | CAS sharpness 范围 | (a) [0, 1] FSR1 标准 / (b) [0, 2] 与 unsharp 对齐 | **(a) [0, 1]** | FSR1 spec 标准范围; ≥ 1 在 CAS 算法中无意义 (peak=-2 已是最强锐化) |
| **D4** | sharpness clamp | (a) 不 clamp / (b) per-mode clamp / (c) Lua 层统一 clamp | **(b) per-mode** | TAARenderer 内部按 mode clamp，调用方不感知边界差异 |
| **D5** | HDR clamp | (a) clamp(0, 1) FSR1 标准 / (b) clamp(0, ∞) HDR safe | **(b) clamp(0, ∞)** | ChocoLight 是 HDR pipeline，不能截亮上界 (会破坏 HDR 高光); 仅 clamp(0, ...) 防黑斑负值 |
| **D6** | 默认 mode | (a) "unsharp" 保 F.0.1 / (b) "cas" 升级体验 | **(a) "unsharp"** | 零回归 + 与 F.0.1/F.0.2/F.0.3/F.0.4/F.0.5 默认行为一致; 用户主动 opt-in CAS |

---

## 4. CAS 算法详解

### 4.1 算法步骤

```
Step 1 — 5-tap fetch:
    a = center, b = N, c = S, d = W, e = E

Step 2 — 邻域 min/max (per channel):
    mnRGB = min(a, b, c, d, e)
    mxRGB = max(a, b, c, d, e)

Step 3 — dynamic range 评估 (对暗部友好):
    mnRGB2 = min(mnRGB, 1.0 - mxRGB)            // 对称暗暗
    ampRGB = clamp(min(mnRGB, mnRGB2) / max(mxRGB, ε), 0, 1)
    ampRGB = sqrt(ampRGB)                        // perceptual gamma

Step 4 — peak 系数 (sharpness ∈ [0, 1]):
    peak = -1 / mix(8, 5, sharpness)             // FSR1: ∈ [-0.125, -0.2]
    wRGB = ampRGB × peak

Step 5 — 加权混合:
    rcpW = 1 / (4 × wRGB + 1)
    sum = a + (b + c + d + e) × wRGB
    sharpened = sum × rcpW

Step 6 — HDR safe:
    return max(sharpened, 0)                     // 防黑斑负值 (HDR 不截上限)
```

### 4.2 与 F.0.1 unsharp mask 对比

| 特性 | F.0.1 unsharp mask | F.0.6 CAS |
|------|-------------------|----------|
| sample 数 | 5 (center + N/S/E/W) | 5 (相同) |
| ALU 数 | ~6 | ~12 |
| 对比度自适应 | ❌ 均匀强度 | ✅ ampRGB 缩放 |
| 低对比抑制 | ❌ | ✅ ampRGB → 0 |
| HDR firefly | ⚠️ 加剧 | ✅ HDR safe |
| sharpness 范围 | [0, 2] | [0, 1] (FSR1 标准) |
| 性能 1080p | ~0.03 ms | ~0.05 ms (+0.02 ms) |
| 边缘锐度 | ⭐⭐⭐ | ⭐⭐⭐⭐ |
| 平滑区域 | ⭐⭐ | ⭐⭐⭐⭐⭐ (无噪点) |

### 4.3 shader 源码（GLES3 版）

```glsl
#version 300 es
precision highp float;
precision highp sampler2D;
in  vec2 vUV;
out vec4 FragColor;

uniform sampler2D uInputTex;       // slot 0: TAA blend 输出
uniform vec2  uTexelSize;          // 1.0 / (W, H)
uniform float uSharpness;          // CAS [0, 1]

void main() {
    vec3 c = texture(uInputTex, vUV).rgb;
    vec3 n = texture(uInputTex, vUV + vec2(0.0, uTexelSize.y)).rgb;
    vec3 s = texture(uInputTex, vUV - vec2(0.0, uTexelSize.y)).rgb;
    vec3 e = texture(uInputTex, vUV + vec2(uTexelSize.x, 0.0)).rgb;
    vec3 w = texture(uInputTex, vUV - vec2(uTexelSize.x, 0.0)).rgb;

    // 邻域 min/max (per channel)
    vec3 mnRGB = min(c, min(n, min(s, min(e, w))));
    vec3 mxRGB = max(c, max(n, max(s, max(e, w))));

    // 对暗部和亮部对称的 dynamic range (FSR1 trick)
    vec3 mnRGB2 = min(mnRGB, 1.0 - mxRGB);
    vec3 ampRGB = clamp(min(mnRGB, mnRGB2) / max(mxRGB, vec3(1e-4)), 0.0, 1.0);
    ampRGB = sqrt(ampRGB);                    // perceptual gamma

    // peak: sharpness=0 → peak=-1/8 (弱), sharpness=1 → peak=-1/5 (强)
    float peak = -1.0 / mix(8.0, 5.0, uSharpness);
    vec3 wRGB  = ampRGB * peak;
    vec3 rcpW  = 1.0 / (4.0 * wRGB + 1.0);

    vec3 sum = c + (n + s + e + w) * wRGB;
    vec3 sharpened = sum * rcpW;

    // HDR safe: 防黑斑负值 (HDR 不截上限保留高光)
    FragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
```

GL33 版仅切 `#version 330 core` + 去 `precision` qualifier。

---

## 5. 状态机与接口契约

### 5.1 TAARenderer state 扩展

```cpp
struct State {
    // ... 现有字段
    int sharpenMode = 0;    // Phase F.0.6: 0=unsharp (F.0.1 默认) / 1=cas
};
```

### 5.2 Process 流程

```
Process(sceneTex, depthTex, sceneFbo, hdrFbo):
    ... TAA blend pass (不变)

    if (sharpness > 0):
        if (sharpenMode == 1)  // CAS
            DrawTAACASPass(historyTex, sceneFbo, w, h, sharpness)
        else                    // unsharp (默认 F.0.1)
            DrawTAASharpenPass(historyTex, sceneFbo, w, h, sharpness)
    else:
        BlitTAAToHDR(historyTex, sceneFbo, ...)
```

### 5.3 SetSharpenMode 切换

```cpp
void SetSharpenMode(const char* mode) {
    int newMode = 0;  // unsharp default
    if (strcasecmp(mode, "cas") == 0)     newMode = 1;
    else if (strcasecmp(mode, "unsharp")) {/* unknown 保持 0 + log warn */}
    g.sharpenMode = newMode;
    // 不需重建 RT (shader 内部分支)
}

const char* GetSharpenMode() {
    return (g.sharpenMode == 1) ? "cas" : "unsharp";
}
```

### 5.4 Lua API 错误处理

```lua
-- 大小写不敏感 (与 SetClipMode 同模式)
TAA.SetSharpenMode("CAS")           -- ok → "cas"
TAA.SetSharpenMode("Unsharp")       -- ok → "unsharp"

-- 类型错误 raise
local ok, err = pcall(TAA.SetSharpenMode, 123)
print(ok, err)  -- false, "bad argument #1 to 'SetSharpenMode' (string expected)"

-- 无效字符串 raise
local ok, err = pcall(TAA.SetSharpenMode, "foo")
print(ok, err)  -- false, "invalid mode 'foo' (use 'unsharp' or 'cas')"
```

---

## 6. 实施顺序

### T0 (15 min) — PLAN 文档（本文件）

### T1 (60 min) — Backend (shader + impl)

- L2240+: GLES3 `FS_CAS_SOURCE` (~30 行 shader 源码)
- L2900+: GL33 `FS_CAS_SOURCE` (~28 行)
- backend struct: `programCAS` + 3 locs
- `Init`: 编译 programCAS + 绑 sampler slot 0
- `Shutdown`: glDeleteProgram(programCAS)
- `DrawTAACASPass` impl (复用 DrawTAASharpenPass 模式)
- `render_backend.h`: `virtual void DrawTAACASPass(...)` 默认 no-op fallback

### T2 (45 min) — TAARenderer

- header: `SetSharpenMode/GetSharpenMode` 声明
- impl: state +1 sharpenMode + 内部 mode 字符串到 int 转换 + Process 切分支

### T3 (60 min) — Lua + smoke + demo + docs

- `light_graphics.cpp`: `l_TAA_SetSharpenMode` (luaL_check string + invalid raise) + `l_TAA_GetSharpenMode` + taa_funcs +2
- `taa.lua`: surface 25 fn + 默认 "unsharp" + round-trip + 大小写不敏感 + invalid raise + type-error + 状态独立 + 六启共存（含 F.0.5 halfRes + CAS）
- `demo_ssr`: Z 键 toggle sharpenMode + HUD 字段
- `Light_Graphics.md`: 速查表 23 → 25 行 + Set/GetSharpenMode 完整文档段（算法对比表 / 推荐场景）

### T4 (30 min) — 6A 文档 ACCEPTANCE/FINAL/TODO

### T5 (15 min) — commit + push + CI 6/6 监控

---

## 7. 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| programCAS 编译失败（旧 GPU） | DrawTAACASPass 走 no-op | DrawTAACASPass 内 fallback 调 BlitTAAToHDR; HUD 显示 fallback warning |
| FSR1 peak 公式不直观 | 用户不理解 sharpness=0 仍有锐化 | 文档明示 sharpness=0 在 CAS 中是 peak=-1/8（最弱锐化）, ≠ 完全 disable |
| sharpness 在两 mode 间切换时语义错位 | unsharp 0.5 vs CAS 0.5 视觉强度不同 | 文档注明; demo HUD 用 mode-specific 文字描述 |
| smoke 现有 sharpness clamp [0, 2] 测试在 CAS 模式失败 | sharpness=2 在 CAS 应 clamp 到 1 | 修改 smoke: 切到 CAS 模式后再测 [0, 1] clamp; unsharp 模式仍测 [0, 2] |
| HUD 信息过载 | demo HUD 加 mode 字段后超 6 行 | 简化 mode 显示为 1 字符 (`U`/`C`) 或合并到 sharp 字段 |

---

## 8. 验收标准

### 功能
- [ ] CAS shader 编译通过 (GLES3 + GL33 双版本)
- [ ] DrawTAACASPass 调用成功 (programCAS != 0)
- [ ] sharpenMode 默认 "unsharp" (零回归)
- [ ] SetSharpenMode("cas") / SetSharpenMode("unsharp") round-trip
- [ ] 大小写不敏感 ("CAS" / "Cas" / "cas" / "UNSHARP" / "Unsharp" / "unsharp")
- [ ] invalid mode 抛错 (luaL_error)
- [ ] type-error (number/nil) 抛错
- [ ] sharpenMode 切换不影响其他参数 (状态独立)
- [ ] F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 六启共存验证

### 文档
- [ ] PLAN/ACCEPTANCE/FINAL/TODO 4 文档齐
- [ ] Light_Graphics.md 速查表 25 fn + Set/GetSharpenMode 完整段
- [ ] demo_ssr Z 键 + HUD 字段

### CI
- [ ] `lightc -p taa.lua` Exit 0
- [ ] GitHub Actions 6/6 平台 success
- [ ] runtime smoke 25/25 fn + 六启共存 PASS
