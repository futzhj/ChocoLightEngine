# Phase F.0.10.5 — Shader uvBounds 完美边界 ALIGNMENT 对齐

> 6A 工作流 · 阶段 1 (Align) · 需求对齐
> 关联前置: F.0.10.2 (TAA region) / F.0.10.3 (Bloom/SSR/MB region) / F.0.10.4 (demo 升级)
> 总估时: 7-9h, 分 3 个 sub-phase

---

## 1. 原始需求

F.0.10.2 + F.0.10.3 用 GL_SCISSOR_TEST 限定后处理 raster 写区域, 但 shader 的 **邻域采样** 会跨 region 边界采到另一半屏内容, 造成 ~1px 边界泄漏 / ghost 残影.

本 phase 给关键 shader 加 `uvMin/uvMax` (即 region UV bounds) uniform, 在邻域采样前 `clamp(uv, uvMin, uvMax)`, 彻底消除边界泄漏.

---

## 2. 边界确认

### 2.1 范围内 (in-scope)

- ✅ **TAA shader** (`FS_TAA`): 4 处跨边界采样
  1. neighborhood clip (8-tap, 3 modes: RGB/YCoCg/Variance) — 视觉影响最大
  2. velocity dilation (9-tap inline) — 影响 reproject 准确性
  3. history reproject (`prevUV`) — 已有 [0,1] reject, 改成 region bounds reject
  4. velocity sample 本身 — 中心采样不跨边界, 但 dilation 会

- ✅ **FS_SHARPEN** (4-tap NSEW): TAA 后锐化 pass, 顺便加

- ✅ **FS_BLOOM_DOWN** (13-tap COD AW ±2 texel): 13-tap 邻域跨边界更明显
- ✅ **FS_BLOOM_UP** (tent 3x3 ±radius texel): upsample 阶段

### 2.2 范围外 (out-of-scope)

- ❌ **SSR ray march** (跨 region 反射): 物理正确, 不算 bug, 不动
- ❌ **MotionBlur velocity 轨迹采样**: 沿运动矢量轨迹跨边界, 但视觉无明显 artifact
- ❌ **FS_BICUBIC_UPSCALE / FS_LANCZOS_UPSCALE**: split-screen 极少同时启用 (上采样常配合 halfRes history), 边际收益低
- ❌ **FS_TAA_CAS / FS_TAA_RCAS**: 4-tap / 9-tap 锐化, 边际影响小, 留 F.0.10.5+
- ❌ **shader 编译 fallback**: 老 backend (Legacy) 无 shader, 不动

---

## 3. 现状对照

### 3.1 当前邻域采样代码 (TAA 示例)

```glsl
// FS_TAA neighborhood clip (RGB AABB mode, line 3122-3134):
vec3 mn = cur.rgb;
vec3 mx = cur.rgb;
vec3 s;
s = texture(uCurHdrTex, vUV + uTexel * vec2(-1.0, -1.0)).rgb; mn = min(mn, s); mx = max(mx, s);
// ... 7 次类似采样, 全部不带 uv clamp ...
hist.rgb = clamp(hist.rgb, mn, mx);
```

**问题**: `vUV + uTexel * (-1, -1)` 在 region 左下角时会越界采到另一半屏.

### 3.2 改造后 (目标)

```glsl
// FS_TAA + uvBounds (Phase F.0.10.5):
uniform vec4 uUvBounds;  // (uMin.x, uMin.y, uMax.x, uMax.y); 默认 (0, 0, 1, 1) = 全屏老行为

// 辅助函数: 邻域采样自动 clamp UV 到 region
vec2 ClampUV(vec2 uv) {
    return clamp(uv, uUvBounds.xy, uUvBounds.zw);
}

// 应用:
s = texture(uCurHdrTex, ClampUV(vUV + uTexel * vec2(-1.0, -1.0))).rgb;
// ... 全部 8 个邻域采样都包 ClampUV ...
```

**默认 0/0/1/1 = 全屏老行为, 零回归**.

---

## 4. 疑问澄清 (5 问)

### Q1: uvBounds 是用 vec4 (uMin.xy, uMax.xy) 还是 vec2+vec2 两个 uniform?

**自答 (基于性能 + GLES 兼容)**:
- 用 **vec4 (单 uniform)** — GLES 上传 4 个 float 1 次开销, 比 2 个 vec2 少 1 次 driver call
- 命名 `uUvBounds` (xy=min, zw=max), 与业界 UE / godot 命名一致

### Q2: ClampUV 函数放 shader 内还是预计算 region UV 再传?

**自答**: shader 内 `clamp()` 内联. 原因:
- shader compiler 会自动内联, 无函数调用开销
- 调用方只需传 region UV bounds, 不需 per-tap 预计算

### Q3: caller 怎么算 uvBounds?

**自答**: `Renderer::Process(rgnX, rgnY, rgnW, rgnH)` 入口算:
```cpp
// 防御性: 全屏 (rgnW=0) 时退化为 (0, 0, 1, 1)
float uvMinX = (rgnW > 0) ? (float)rgnX / (float)w : 0.0f;
float uvMinY = (rgnH > 0) ? (float)rgnY / (float)h : 0.0f;
float uvMaxX = (rgnW > 0) ? (float)(rgnX + rgnW) / (float)w : 1.0f;
float uvMaxY = (rgnH > 0) ? (float)(rgnY + rgnH) / (float)h : 1.0f;
// 收 0.5 texel inset 防止线性插值越界 (sub-texel clamp 业界标准):
const float texelInsetX = 0.5f / (float)w;
const float texelInsetY = 0.5f / (float)h;
uvMinX += texelInsetX; uvMaxX -= texelInsetX;
uvMinY += texelInsetY; uvMaxY -= texelInsetY;
```

### Q4: Bloom mip 链每级 uvBounds 怎么算?

**自答**: 与 F.0.10.3 mip region 缩半同理:
- downsample mip i: 输入 mip (i-1) 的 region (downsample 是 ÷2, region 也 ÷2)
- upsample mip i: 输入 mip (i+1), 也是缩放后 region
- 注意 `std::max(1, region >> level)` 兜底 1×1 防越界
- bounds 转 UV 同样 0.5 texel inset

### Q5: GLES 3.0 路径是否同步改?

**自答**: **必须同步** — F.0 系列严格保持 GLES 3.0 / GL 3.3 双源等价, F.0.10.5 同样.
- 每改一处 GL 3.3 shader, 同步改 GLES 3.0 版本
- shader 添加 uniform 后, backend 必须双版同上传

---

## 5. 关键决策清单

| ID | 决策 | 选项 | 选定 |
|----|------|------|------|
| D-1 | uvBounds 类型 | vec4 / 2×vec2 / 4×float | **vec4** (1 次 uniform, GLES 友好) |
| D-2 | ClampUV 实现 | shader 内 clamp / caller 预计算 | **shader 内 clamp()** |
| D-3 | 默认值 | (0,0,1,1) / 必传 | **(0,0,1,1) 全屏老行为, 零回归** |
| D-4 | inset | 0 / 0.5 texel | **0.5 texel** (业界标准防线性插值越界) |
| D-5 | Bloom mip 链 | 递推 / 按级反算 | **按级反算** (与 F.0.10.3 一致) |
| D-6 | GLES 3.0 同步 | 必须 / 仅 GL 3.3 | **必须双版同步** |
| D-7 | Sub-Phase 拆分 | 1 个 / 多个 | **3 sub-phase** (TAA / Bloom / Assess) |
| D-8 | 跳过的 shader | 包括 / 跳过 SSR-ray/MB/Upscale | **跳过, 留 F.0.10.5+** |

---

## 6. 任务边界确认

- **覆盖范围**: 5 个 shader (TAA, Sharpen, BloomDown, BloomUp, GLES + GL 3.3 双版 = 10 处) × 1 个 uvBounds uniform
- **不动**: SSR / MB / Upscale shader (SSR 物理正确, MB 视觉无 artifact, Upscale 在 split-screen 罕用)
- **零回归**: uvBounds 默认 (0,0,1,1), 老 caller 不传仍走全屏老行为

---

## 7. 验收标准

- ✅ TAA neighborhood + velocity dilation + history reproject 在 region 内零跨边界采样
- ✅ FS_SHARPEN 4-tap 在 region 内零跨边界
- ✅ BLOOM_DOWN / UP 在每 mip 级正确 clamp 到 mip 缩放后 region
- ✅ split-screen demo 双 player 边界处无可见锯齿/ghost (肉眼对比 F.0.10.4 vs F.0.10.5)
- ✅ 默认全屏 (uvBounds=0,0,1,1) 老路径零回归: 8 smoke 全过
- ✅ GLES 3.0 / GL 3.3 shader 双源一致 (字面对照)
- ✅ headless probe + 4 CI 平台 build 通过 (Web/iOS GLES 验证)
