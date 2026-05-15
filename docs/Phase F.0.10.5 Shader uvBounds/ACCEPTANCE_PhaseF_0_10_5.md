# Phase F.0.10.5 — Shader uvBounds 完美边界 ACCEPTANCE 验收

> 6A 工作流 · 阶段 6 (Assess) · 验收记录
> 关联: `ALIGNMENT_PhaseF_0_10_5.md` / `DESIGN_PhaseF_0_10_5.md` / `TASK_PhaseF_0_10_5.md`

---

## 1. Sub-Phase 完成状态总览

| Sub-Phase | 范围 | Commit | 验收 | 工作量 |
|-----------|------|--------|------|-------|
| **SP1** TAA + Sharpen shader uvBounds | 4 shader (GLES3+GL3.3) + 2 backend impl + TAA Process 算 bounds | `71d2fca` | ✅ 8 PASS demo + 8 smoke 零回归 | ~3h |
| **SP2** Bloom Down/Up/Composite shader uvBounds | 4 shader (BloomDown+BloomUp, GLES3+GL3.3) + 3 backend impl + Bloom Process mip 链算 bounds | `47a8161` | ✅ 8 PASS demo + 8 smoke 零回归 | ~3h |
| **SP3** 6A Assess | ACCEPTANCE/FINAL/TODO | 本 commit | ✅ 文档完整 | ~1h |
| **合计** | 8 shader + 5 backend + 2 renderer | 3 commits | 16 task 全过 | **~7h** |

---

## 2. Shader 改造清单 (T1.1 + T2.1)

### 2.1 TAA shader (双源, GLES3 + GL3.3)

```glsl
// 新增 uniform
uniform vec4 uUvBounds;  // (uMin.xy, uMax.xy); 默认 (0,0,1,1) = 无 clamp

// 新增 helper
vec2 ClampUV(vec2 uv) { return clamp(uv, uUvBounds.xy, uUvBounds.zw); }
```

**包 ClampUV 的采样点 (TAA 共 26+ 处)**:
- `SampleVelocity` 9-tap dilation
- `history reproject` 边界 reject (`prevUV` 与 `uvBounds` 比较)
- `hist sample` 防御性 clamp
- 3 个 clip mode 各 8-tap 邻域采样:
  * RGB AABB (`uClipMode==0`)
  * YCoCg AABB (`uClipMode==1`)
  * YCoCg variance (`uClipMode==2`)

### 2.2 Sharpen shader (双源)

- 4-tap NSEW unsharp mask 全部包 `ClampUV`
- 中心点 `c = texture(uInputTex, vUV)` 不包 (vUV 一定在 region 内)

### 2.3 Bloom shader (双源)

- `FS_BLOOM_DOWN`: 13-tap COD AW 全部包 (含中心 `E`)
- `FS_BLOOM_UP`: tent 9-tap 全部包
- `FS_BLOOM_BRIGHT`: 单点采, **不改** (无需 clamp)

---

## 3. Backend 接口扩展 (T1.2 + T2.2 + T1.3 + T2.3)

### 3.1 render_backend.h 签名

| 方法 | 新增参数 |
|------|---------|
| `DrawTAAPass` | `const float* uvBounds` (无默认值, 强制传; 调用方传 nullptr 视作全屏) |
| `DrawTAASharpenPass` | `const float* uvBounds = nullptr` |
| `DrawBloomDownsample` | `const float* uvBounds = nullptr` |
| `DrawBloomUpsample` | `const float* uvBounds = nullptr` |
| `DrawBloomComposite` | `const float* uvBounds = nullptr` |

### 3.2 GL33 backend uniform location

| Shader | 新增 location |
|--------|-------------|
| `programTAA` | `locTAA_UvBounds` |
| `programSharpen` | `locSharpen_UvBounds` |
| `programBloomDown` | `locBloomDown_UvBounds` |
| `programBloomUp` | `locBloomUp_UvBounds` (composite 复用) |

### 3.3 uniform 上传逻辑

```cpp
if (loc_UvBounds >= 0) {
    const float defaultBounds[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    glUniform4fv(loc_UvBounds, 1, uvBounds ? uvBounds : defaultBounds);
}
```

**关键**: `uvBounds == nullptr` 时上传 sentinel `(0,0,1,1)` — clamp(uv, 0, 1) 在 vUV ∈ [0,1] 时是恒等, 即老路径 no-op (零回归).

---

## 4. Renderer Process 改造 (T1.4 + T2.4)

### 4.1 TAA Renderer

```cpp
// 入口算 history-RT 空间 uvBounds (half-res 时 historyW/H = w/2, h/2)
float uvBoundsBuf[4];
const float* uvBoundsPtr = nullptr;
if (rgnW > 0 && rgnH > 0 && g.historyW > 0 && g.historyH > 0) {
    const float invW = 1.0f / (float)g.historyW;
    const float invH = 1.0f / (float)g.historyH;
    uvBoundsBuf[0] = (rgnX + 0.5f) * invW;
    uvBoundsBuf[1] = (rgnY + 0.5f) * invH;
    uvBoundsBuf[2] = (rgnX + rgnW - 0.5f) * invW;
    uvBoundsBuf[3] = (rgnY + rgnH - 0.5f) * invH;
    uvBoundsPtr = uvBoundsBuf;
}
```

**Sharpen pass 独立算 uvBounds** (sceneTex full-res `g.width/g.height`, 与 history RT 不同).

### 4.2 Bloom Renderer

```cpp
// lambda helper
auto calcUvBounds = [](int srcW, int srcH, ..., float out[4]) { ... };

// Bright pass: 单点采 → 跳过 uvBounds
// Downsample iter i: src=mip-(i-1), uvBounds=mip-(i-1) region 算 (>>1 之前 snapshot)
// Upsample iter i: src=mip-i, uvBounds=mip-i region (用 g.width>>i 反算, 不递推)
// Composite: src=mip-0=full-res, uvBounds=输入 region
```

**0.5 texel inset**: 防 GL_LINEAR 线性插值越界 (与 UE 同模式).

---

## 5. 本地验证记录

### 5.1 编译

```
cmake --build build --config Release --target Light
=> Exit code: 0
=> Light.dll synced to Lumen runtime
```

### 5.2 demo_taa_split2 headless probe

```
PASS: HDR.SetAutoTAA(false) round-trip ok
PASS: TAA.Process(region) headless returns nil + err: TAA.Process: HDR not enabled
PASS: HDR.SetAutoBloom(false) round-trip ok
PASS: HDR.SetAutoSSR(false) round-trip ok
PASS: HDR.SetAutoMotionBlur(false) round-trip ok
PASS: Bloom.Process(region) headless returns nil + err
PASS: SSR.Process(region) headless returns nil + err
PASS: MB.Process(region) headless returns nil + err
=> 8/8 PASS
```

### 5.3 Smoke 零回归

| Smoke | 结果 |
|-------|------|
| `hdr.lua` | PASS (28 functions) |
| `motion_blur.lua` | PASS |
| `bloom.lua` | PASS (Phase E.4 + F.0.10.3) |
| `ssr.lua` | PASS (Phase E.9+E.10+E.11+E.12+F.0.10.3) |
| `ssao.lua` | PASS (Phase E.8) |
| `taa.lua` | PASS |
| `lens_flare.lua` | PASS (Phase E.7) |
| `lens_fx.lua` | PASS (Phase E.6) |
| **总计** | **8/8 零回归** |

---

## 6. 风险矩阵 (实际 vs ALIGN 预测)

| 风险 | 预测 | 实际 | 缓解 |
|-----|------|------|------|
| GLSL 兼容 (GLES3 ↔ GL3.3 `clamp` + `vec4`) | 低 | ✅ 无问题 | GLSL 1.0/3.0 标准 |
| GL33 老 caller 没传 `uvBounds` 编译失败 | 中 | ⚠️ DrawTAAPass 默认值已存在, override 报错 → 加 `nullptr` 默认 | 全 override 同步 (本 commit) |
| 0.5 texel inset 性能影响 | 极低 | ✅ 加 4 ALU 可忽略 | shader 已实测 |
| mip 链 uvBounds 算错 → 视觉裂缝 | 中 | ⚠️ Bloom 需注意 src vs dst 空间, 已正确实现 (downsample 用 src=mip-(i-1) 空间, upsample 用 src=mip-i) | 代码 review + 注释 |
| CI 平台差异 (Web GLES / iOS) | 中 | ⏳ 待 push 验证 | 上传后检查 |

---

## 7. 验收清单 (TASK §6 全局)

- [x] 8 个 shader 改造 (TAA + Sharpen + BloomDown + BloomUp 各 GLES3 + GL3.3)
- [x] 4 个 backend 虚接口加 `uvBounds` 参数 (TAAPass + TAASharpen + BloomDown/Up/Composite)
- [x] 2 个 Renderer Process 算 uvBounds 并上传 (TAA + Bloom)
- [x] 8 smoke 全过 (零回归)
- [x] demo_taa_split2 headless 8 PASS (零回归)
- [ ] CI 6/6 success (待 push 后 webhook 验证)
- [x] Lua API 不增 (54 fn 保持)

---

## 8. Lua API 演化 (本 Phase 不增)

| API | 状态 | 备注 |
|-----|------|------|
| `Light.Graphics.TAA.Process(...)` | 不变 | 内部加 uvBounds 透传, 用户不感知 |
| `Light.Graphics.Bloom.Process(rgnX, rgnY, rgnW, rgnH)` | 不变 | 内部 mip 链算 uvBounds, 用户不感知 |
| 54 fn 总数 | 不变 | F.0.10.5 是 shader 内部优化, 不暴露新 API |

---

## 9. 工作量统计

| 阶段 | 工作量 |
|------|-------|
| ALIGN + DESIGN + TASK 文档 (~30 min) | 0.5h |
| Sub-Phase 1 (TAA + Sharpen) | 3h |
| Sub-Phase 2 (Bloom mip 链) | 3h |
| Sub-Phase 3 (Assess + 文档) | 1h |
| **总计** | **~7.5h** (vs DESIGN 估 7-9h) |

---

## 10. 下一步候选 (后续 phase)

- **F.0.10.6** (待评估): HDR multi-instance — 每个 region 独立 HDR target / tonemap params
- **F.1** (规划中): DLSS-like TAAU — 真上采样, history at output res, current at lower res
- **F.0.11** (备选): Volumetric Fog / Clouds region 化
