# Phase F.0.6 TAA CAS Sharpening — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告
> 关联：`PLAN_PhaseF_0_6.md` / `ACCEPTANCE_PhaseF_0_6.md` / `TODO_PhaseF_0_6.md`

---

## 1. 交付物总览

| 交付物 | 文件 | 行数变化 |
|--------|------|---------|
| GLES3 shader | `ChocoLight/src/render_gl33.cpp` (`FS_CAS_SOURCE`) | +43 行 (含注释) |
| GL33 shader | `ChocoLight/src/render_gl33.cpp` (`FS_CAS_SOURCE`) | +40 行 |
| Backend struct | `ChocoLight/src/render_gl33.cpp` (programCAS + 3 locs) | +6 行 |
| Backend Init | `ChocoLight/src/render_gl33.cpp` (compile + bind) | +14 行 |
| Backend Shutdown | `ChocoLight/src/render_gl33.cpp` | +3 行 |
| Backend Pass | `ChocoLight/src/render_gl33.cpp` (DrawTAACASPass override) | +29 行 |
| Backend interface | `ChocoLight/include/render_backend.h` (DrawTAACASPass virtual) | +9 行 |
| TAARenderer | `ChocoLight/include/taa_renderer.h` + `src/taa_renderer.cpp` | +43 行 (state +1 + parseSharpenMode_ + Set/Get + Process 切分支) |
| Lua API | `ChocoLight/src/light_graphics.cpp` | +44 行 (Set/Get + taa_funcs +2) |
| smoke | `scripts/smoke/taa.lua` | +110 行 (默认 / round-trip / 大小写 3 / invalid / type-error 2 / 状态独立 / 六启共存) |
| demo | `samples/demo_ssr/main.lua` | +14 行 (Z 键切换 + HUD sharpStr + Keys help) |
| API doc | `docs/api/Light_Graphics.md` | +130 行 (速查表 +1 + Set/GetSharpenMode 完整段) |
| 6A docs | `docs/Phase F.0.6 TAA CAS Sharpening/` 4 文档 | ~700 行 |

**累计**：代码 ~210 行 + 文档 ~830 行（含 6A）

---

## 2. 核心方案 — 5-tap CAS 与 4-tap unsharp 共存

参考 AMD FidelityFX FSR1 (2021) 的标准 CAS 算法，5-tap contrast-adaptive sharpening。

### 数据流图

```
┌────────────────────────────────────────────────────────┐
│ TAARenderer Process (sharpness > 0):                   │
│                                                        │
│   if (sharpenMode == 1)  // CAS (Phase F.0.6)         │
│     casSharpness = clamp(sharpness, 0, 1)              │
│     DrawTAACASPass(historyTex, hdrFbo, w, h, casS)     │
│                                                        │
│   else                    // unsharp (F.0.1 默认)       │
│     DrawTAASharpenPass(historyTex, hdrFbo, w, h, s)    │
└────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────┐
│ FS_CAS shader (5-tap):                                 │
│                                                        │
│   c, n, s, e, w = 5-tap fetch                         │
│   mnRGB = min(c, n, s, e, w)  per channel             │
│   mxRGB = max(c, n, s, e, w)  per channel             │
│                                                        │
│   ampRGB = sqrt(clamp(...))   // dynamic range gamma  │
│   peak   = -1 / mix(8, 5, sharpness)                   │
│   wRGB   = ampRGB * peak                               │
│                                                        │
│   sum = c + (n+s+e+w) * wRGB                          │
│   sharpened = sum / (4 * wRGB + 1)                     │
│                                                        │
│   FragColor = vec4(max(sharpened, 0), 1)              │
└────────────────────────────────────────────────────────┘
```

### 关键性质

- **零新 RT**：复用 F.0.1 的 in-place 写回 sceneTex 模式
- **零 Lua API 破坏**：用户切到 CAS 后 F.0.1 行为完全不变（默认 mode = "unsharp"）
- **零 backend 接口破坏**：DrawTAACASPass 是新 virtual 默认 no-op，老 backend 零改动
- **HDR safe**：clamp(0, ∞) 防黑斑，保留高光（与 FSR1 LDR clamp(0,1) 不同）
- **跨平台 case-insensitive**：parseSharpenMode_ 手写 lambda 避免 strcasecmp Windows 问题

---

## 3. API surface

### Lua API（新增 2 个，TAA 子表 23 → 25 fn）

```lua
-- 切换到 CAS (HDR 强高光场景推荐)
Light.Graphics.TAA.SetSharpenMode("cas")    -- ON: AMD FSR1 5-tap CAS
Light.Graphics.TAA.SetSharpenMode("unsharp") -- OFF: F.0.1 4-tap unsharp (默认)
Light.Graphics.TAA.GetSharpenMode()         -- → "unsharp" / "cas"

-- 大小写不敏感
TAA.SetSharpenMode("CAS")     -- ok
TAA.SetSharpenMode("Cas")     -- ok
TAA.SetSharpenMode("UNSHARP") -- ok

-- 错误处理 (与 SetClipMode 同模式)
local ok, err = TAA.SetSharpenMode("foo")
-- ok=nil, err="TAA.SetSharpenMode: 未识别的 mode 'foo' (...)"
```

### Backend 接口扩展（向后兼容默认 no-op）

```cpp
// render_backend.h base virtual
virtual void DrawTAACASPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                            int /*w*/, int /*h*/, float /*sharpness*/) {}
```

老 backend (Legacy / D3D11 / Vulkan placeholders) 自动零行为；GL33Backend override 实现实际 CAS pass。

---

## 4. 与 F.0 系列的协同

| Phase | 作用 | F.0.6 (CAS) 影响 |
|-------|------|---------------------|
| F.0 (TAA 主管线) | jitter + reproject + clip + blend | 不变 |
| F.0.1 (unsharp sharpening) | 4-tap unsharp mask | **共存**（默认 mode）|
| F.0.2 (YCoCg AABB) | YCoCg 空间 9-tap clip | 不变 |
| F.0.3 (Variance) | mean ± γ·σ clip | 不变 |
| F.0.4 (Anti-flicker) | Karis luma weighting | 不变 |
| F.0.5 (Half-res) | history RT 半分辨率 | 不变 |
| F.0.7 (Compare demo) | 8 preset 切换 | 未含 CAS preset (留 F.0.7.1 扩展) |

**完全正交**：F.0.6 仅切换 sharpen pass shader，不影响 TAA blend/clip 逻辑。

| 配置 | 视觉效果 |
|------|---------|
| F.0 baseline (sharpness=0) | F.0 完全等价 |
| F.0.6 unsharp (默认) | F.0.1 完全等价 |
| F.0.6 cas (HDR 强光场景推荐) | 平滑区域不锐化 + 边缘强化 + HDR safe |

---

## 5. 性能基线（理论, 1080p）

| 阶段 | F.0.1 unsharp | F.0.6 CAS |
|------|--------------|----------|
| Sharpen pass 时间 | ~0.03 ms | ~0.05 ms (+0.02 ms) |
| ALU 数 | ~6 | ~12 (+6) |
| sample 数 | 5 | 5 |
| VRAM | 0 (in-place) | 0 (in-place) |

### CI runtime smoke 期望日志

```
PASS: Default SharpenMode = 'unsharp' (Phase F.0.6 零回归)
PASS: SharpenMode round-trip 'cas' / 'unsharp' ok
PASS: SharpenMode case-insensitive 'CAS' / 'Cas' / 'UNSHARP' → 'cas' / 'cas' / 'unsharp' ok
PASS: SharpenMode invalid 'foo' → nil+err, state 不变 ok
PASS: SharpenMode type-error rejected (number / boolean)
PASS: SharpenMode 切换不影响其他参数 (状态独立)
PASS: F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 六启共存 ok
=== Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 TAA smoke: ALL TESTS PASSED ===
Functions covered: 25 / 25
```

---

## 6. CI 状态（待回填）

| 平台 | 状态 | 备注 |
|------|------|------|
| build-windows | ⏳ | runtime smoke 含 25 fn + 10 个 F.0.6 PASS |
| build-linux | ⏳ | 纯构建 |
| build-macos | ⏳ | 纯构建 |
| build-android | ⏳ | 纯构建 |
| build-ios | ⏳ | 纯构建 |
| build-web | ⏳ | Emscripten WASM |

GitHub Run ID: `<pending>`
Commit hash: `<pending>`
Date: `<pending>`
Total duration: `<pending>`

---

## 7. 工程反思

### 做得好的地方

1. **共存模式胜过替换**：F.0.1 + F.0.6 双 sharpen 算法保留，用户可按硬件/场景选择，零回归保障
2. **接口默认 no-op 避免 break-change**：DrawTAACASPass virtual 默认空实现，老 backend 零改动
3. **跨平台 case-insensitive 复用**：parseSharpenMode_ 与 parseClipMode_ 同模式，避免 strcasecmp Windows 兼容性问题
4. **HDR safe clamp(0, ∞)**：与 FSR1 LDR clamp(0, 1) 不同，适配 ChocoLight HDR pipeline；保留高光，无 firefly 加剧
5. **sharpness 字段共享**：mode 切换不需重设 sharpness，API 简洁
6. **决策矩阵全自动**：6/6 决策点基于业界标准 (FSR1 spec) + Phase F.0.x 一致性，零用户拍板需求

### 可改进点

1. **不是真 RCAS (Robust CAS)**：FSR2 高级形式，处理 deringing；留 Phase F.1
2. **未做真机 A/B 测试**：FLIP / SSIM 量化 unsharp vs CAS 视觉差异
3. **sharpness 语义在两 mode 间不同**：用户切 mode 后视觉强度变化；只能文档明示
4. **未实现 sharpness preset hint**：HUD 不能告诉用户"CAS 推荐 0.6"；用户需自行调

### 工程经验

1. **shader 共存比替换更安全**：成熟功能（F.0.1 已发布）替换会破坏 user expectation; 新算法通过 mode 切换 opt-in
2. **跨平台 string 比较手写更简洁**：8 行 lambda vs 跨平台 `#ifdef strcasecmp/_stricmp`
3. **HDR pipeline 算法移植要警惕 LDR 假设**：FSR1 spec clamp(0,1) 是 LDR 默认；移到 HDR 必须改为 clamp(0,∞)
4. **mode 字段是 API 演进的好工具**：单一 sharpness + mode 比双字段 sharpnessUnsharp + sharpnessCAS 简洁
5. **decisional binary "默认" 决策**：D6 默认 mode 是零回归保证的关键；用户主动 opt-in CAS 的好处不是 backward compat 的代价

---

## 8. Phase F.0 系列累计 (F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.6 + F.0.7)

| Phase | 功能 | Lua API | shader / demo |
|-------|------|---------|--------------|
| F.0 | TAA 主管线 | 13 fn | FS_TAA shader |
| F.0.1 | 4-tap unsharp sharpening | +2 (15) | +FS_SHARPEN shader |
| F.0.4 | Karis anti-flicker | +2 (17) | 改 FS_TAA blend |
| F.0.2 | YCoCg AABB clip | +2 (19) | 改 FS_TAA clip |
| F.0.3 | YCoCg variance clip | +2 (21) | 改 FS_TAA clip |
| F.0.5 | Half-res history | +2 (23) | shader 不变 |
| F.0.7 | Compare demo | +0 (23) | demo only |
| **F.0.6** | **5-tap CAS sharpening (AMD FSR1)** | **+2 (25)** | **+FS_CAS shader** |

**Phase F.0 系列**：25 fn Lua API / 3 shader (FS_TAA + FS_SHARPEN + FS_CAS) / 2 backend pass (DrawTAASharpenPass + DrawTAACASPass) / 1 backend 接口扩展 (BlitTAAToHDR dst* defaults) / 3 demos

---

## 9. Phase F.0.x 后续路线

### 短期

1. **Phase F.0.8** — Motion-adaptive variance γ（基于 velocity 长度动态调整, 3h）
2. **Phase F.0.9** — Custom upsampler (bicubic / Lanczos)：替代 F.0.5 bilinear

### 中期

3. **Phase F.0.10** — TAARenderer 多实例化 + 真 split-screen demo（6h+）
4. **Phase F.0.11** — Demo 截图 / 录屏功能（3h）
5. **Phase F.0.12** — RCAS (Robust CAS) FSR2 升级

### 长期

6. **Phase F.1** — DLSS-like TAAU (upscale 1/2 → 2× 输出)
7. **Phase F.2** — Bloom + TAA sharp HDR 联动
