# Phase E.18.1 Velocity Dilation Half-Resolution — ALIGNMENT

> 6A 工作流 · 阶段 1 · Align
> 基线：Phase E.18 commit `8b7c25b`
> 上游 Phase：E.17 motion blur half-res（已落地）+ E.18 independent dilation pass（已落地）

---

## 1. 原始需求

将 Phase E.18 的 `dilatedVelocityTex` / `dilatedCameraVelocityTex` 从全分辨率改为半分辨率：

- VRAM 节省 **75%**（1080p：8MB → 2MB / RT；双 RT 总省 12MB）
- dilation pass 自身性能 **+4×**（fragment count -75%）
- 默认 **OFF**（与 Phase E.18 行为完全兼容）

---

## 2. 项目和任务特性规范

### 2.1 上游约束

- Phase E.17 motion blur half-res 已确立"半分辨率向上取整 `((W+1)/2, (H+1)/2)`、uTexel 保持全分辨率"模式
- Phase E.18 dilation pass 已确立"backend 层 RT 由 HDRRenderer 持有 + dilationPassActive 控制 consumer 路径"模式
- 现有 `Light.Graphics.HDR.SetVelocityDilation(bool)` 是 dilation 开关；本任务新增 `SetVelocityDilationHalfRes(bool)` 是 dilation **质量/性能** 调档

### 2.2 任务边界

✅ **包含**：
- `CreateVelocityDilateRT` / `DrawVelocityDilate` backend 接口扩 sw/sh 参数
- HDRRenderer 加 `dilationHalfRes` state + Lua API
- shader / consumer 零改动（透明）
- Lua API / smoke / demo / docs 更新

❌ **不包含**：
- 全新算法/质量提升（max-filter 算法本身不变）
- 单独控制 combined vs camera-only halfRes（双 RT 同步切换）
- 自适应分辨率（运行时根据分辨率自动选）

---

## 3. 现有项目的理解

### 3.1 现状（Phase E.18 完成态）

`render_backend.h` 关键接口签名：
```cpp
virtual uint32_t CreateVelocityDilateRT(int w, int h, uint32_t* outTex);
virtual void     DrawVelocityDilate(uint32_t srcVelocityTex, uint32_t dstFbo, int w, int h);
```

`render_gl33.cpp` 内 `DrawVelocityDilate` 关键行：
```cpp
glViewport(0, 0, w, h);                                     // full-res viewport
glUniform2f(locVDilate_Texel, 1.0f/w, 1.0f/h);              // uTexel = 1/(W, H)
```

`hdr_renderer.cpp::CreateRT` 创建调用：
```cpp
g.backend->CreateVelocityDilateRT(w, h, &dilatedTex);
```

### 3.2 Phase E.17 motion blur halfRes 参考

```cpp
// motion_blur_renderer.cpp
static inline void ComputeStorageSize(int w, int h, int& sw, int& sh) {
    if (g.halfRes) { sw = (w+1)/2; sh = (h+1)/2; }
    else           { sw = w; sh = h; }
}
// backend 接口: CreateMotionBlurRT(int w, int h, &tex, int sw, int sh)
//   w/h = logical (drives viewport for full-res pass2)
//   sw/sh = storage (actual RT size, half-res-friendly)
```

---

## 4. 疑问澄清（决策矩阵 · 10/10 已拍板）

按 6A 规则"基于现有项目内容和行业最佳实践决策"，10 个关键点全部自决：

| # | 决策点 | 拍板方案 | 依据 |
|---|--------|---------|------|
| 1 | **半分辨率公式** | `((W+1)/2, (H+1)/2)` 向上取整 | 与 Phase E.17 motion blur halfRes 一致，奇数分辨率不丢边 |
| 2 | **uTexel 策略** | `1.0/(sw, sh)` 半分辨率纹素间距 | 物理直觉：uTexel 跟随实际 viewport；max-filter 邻域更广(6 raw px)对快速运动更鲁棒 |
| 3 | **过滤模式** | `GL_LINEAR + GL_CLAMP_TO_EDGE` | 沿用 Phase E.18 默认；consumer 单点采 sub-pixel 自动 bilinear 上采 |
| 4 | **API 命名** | `HDR.SetVelocityDilationHalfRes(bool)` / `GetVelocityDilationHalfRes()` | 与 `SetVelocityDilation` 命名平行；明确语义"dilation pass 的半分辨率开关" |
| 5 | **默认值** | `false`（全分辨率） | 保持 Phase E.18 行为，零回归；用户主动启用 |
| 6 | **切换时机** | 立即重建 dilatedFbo/Tex (双 RT 同时) | 与 `MotionBlurRenderer::SetHalfRes` 立即 Resize 模式一致 |
| 7 | **backend 接口扩展** | `CreateVelocityDilateRT(w, h, sw, sh, &tex)` + `DrawVelocityDilate(srcTex, dstFbo, sw, sh)` | 显式分离 logical 与 storage 尺寸；DrawVelocityDilate 仅需 storage（viewport 决定） |
| 8 | **consumer 影响** | 零改动 | SSR/MotionBlur 单点采 dilatedTex（`texture(uVelocityTex, uv).rg`），UV 是 full-res 屏幕坐标，硬件自动 bilinear 上采 |
| 9 | **生效条件** | dilation pass 启用（SupportsVelocityDilation + dilationPassActive=true）时才有意义 | dilation OFF / fallback 时 halfRes 字段无效（dilated RT 不创建） |
| 10 | **零回归保障** | `dilationHalfRes=false` → sw=w, sh=h → 行为等价 Phase E.18 | CreateVelocityDilateRT 内 `glTexImage2D(GL_RG16F, sw, sh, ...)`，sw=w 即全分辨率 |

---

## 5. 决策详解（关键 4 项）

### 5.1 uTexel 策略：`1.0/(sw, sh)` 选择依据

两种候选方案：

| 方案 | uTexel 公式 | 9-tap 物理覆盖 (raw px) | 视觉效果 |
|------|-------------|-------------------------|---------|
| A | `1.0/(fullW, fullH)` | 3 raw 像素（与 full-res dilation 一致） | 完全等价 full-res 行为，但 half-res 输出空间有"采样跳跃" |
| B | `1.0/(sw, sh)` = `2.0/(fullW, fullH)` | 6 raw 像素（更广） | max-filter 邻域更鲁棒，快速运动捕获更完整 |

**选 B** 的理由：
1. **物理直觉一致**：uTexel 就是 dilated tex 的纹素间距，跟随 viewport
2. **max-filter 自动正确性**：max() 操作只会扩大覆盖范围，多采几个像素不会引入伪信号
3. **与 Phase E.17 等价**：motion blur halfRes 内 uTexel 也保持"与实际 viewport 一致"（虽然 motion blur 是反向 = 保持 full-res 因为 viewport 是 full-res）
4. **代码简洁**：DrawVelocityDilate 只需 sw/sh 一个尺寸，不用同时传 fullW/fullH

### 5.2 默认 false 的理由

- Phase E.18 默认行为是 full-res dilatedTex
- 启用 halfRes 是**性能优化档**，应明确开启
- 用户对视觉差异敏感时可选不启用
- 与 motion blur halfRes 默认 false 一致

### 5.3 仅 dilation 启用时生效

```
dilation OFF (HDR.SetVelocityDilation(false))
    → dilatedFbo 不创建
    → halfRes 字段未使用（无影响）

dilation ON + backend 不支持
    → dilatedFbo 不创建（silent fallback）
    → halfRes 字段未使用（无影响）

dilation ON + backend 支持
    → halfRes=false: dilatedTex 全分辨率（Phase E.18 行为）
    → halfRes=true:  dilatedTex 半分辨率（本任务行为）★
```

### 5.4 双 RT 同步切换

`dilatedVelocityFbo/Tex` (combined) 与 `dilatedCameraVelocityFbo/Tex` (camera-only) 同步切换：
- 不区分单独控制：增加复杂度无收益
- 共享一个 `dilationHalfRes` state 字段
- 两次 `CreateVelocityDilateRT(sw, sh, ...)` 调用，相同 sw/sh

---

## 6. 验收标准

- ✅ Lua API：`HDR.SetVelocityDilationHalfRes(bool)` / `GetVelocityDilationHalfRes() → bool`
- ✅ 切换时立即重建 dilated RT（无 Lua 端 Resize 操作要求）
- ✅ `dilationHalfRes=false` 默认；行为完全等价 Phase E.18
- ✅ `dilationHalfRes=true` 时 dilatedTex 为 `((W+1)/2, (H+1)/2)` 尺寸
- ✅ 9-tap 物理覆盖在 raw velocity space 为 6 raw 像素（max-filter 鲁棒）
- ✅ Consumer 路径（SSR/MotionBlur）零改动
- ✅ Smoke / demo / docs 三件套更新
- ✅ 6A 文档完备（ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO）
- ✅ CI 6/6 平台 success

---

## 7. 性能预算（理论）

### VRAM 节省（1080p）

| 资源 | full-res | half-res | 节省 |
|------|----------|----------|------|
| dilatedVelocityTex | 8 MB | 2 MB | 6 MB (-75%) |
| dilatedCameraVelocityTex | 8 MB | 2 MB | 6 MB (-75%) |
| **总计** | **16 MB** | **4 MB** | **12 MB (-75%)** |

### GPU 时间收益（dilation pass 自身）

| 配置 | 像素数 | fetch / pixel | 总 fetch | 估算 ms (RTX 3060 @1080p) |
|------|--------|--------------|----------|---------------------------|
| full-res dilation | 1920×1080 | 9 | 18.7 M | 0.10 ms |
| half-res dilation | 960×540 | 9 | 4.7 M | 0.025 ms |
| **节省** | -75% | 0 | **-14 M** | **-0.075 ms** |

### 总收益（SSR + MB 同开 N=8）

| 模式 | dilation pass | consumer fetch | 总 | 估算 ms |
|------|---------------|----------------|----|---------| 
| Phase E.18 (full-res) | 0.10 | 0.30 | 0.40 | 0.40 ms |
| Phase E.18.1 (half-res) | 0.025 | 0.30 | 0.325 | 0.33 ms |
| **节省** | -75% | 0% | -19% | **-0.075 ms** |

> 注：consumer fetch 不变（仍单点采 dilatedTex），主要收益在 dilation pass 自身 + VRAM。

---

## 8. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| half-res 邻域过广 (6 raw px) 导致 over-dilation | 中 | 边界模糊轻微扩张 | max-filter 不引入伪信号，且 SSR Temporal/Motion Blur 本身就是低频 |
| 半分辨率 1-pixel 物体丢失 | 低 | 极窄高速物体 motion blur 偏弱 | 与 motion blur halfRes 同样妥协，用户可自行关 halfRes |
| consumer 单点采上采插值误差 | 极低 | 硬件 bilinear 平滑 | dilatedTex `GL_LINEAR + CLAMP_TO_EDGE` 已配置 |
| dilation OFF 时 halfRes 字段无用 | 文档 | 用户误以为生效 | 文档明确"halfRes 仅在 dilation ON 时有意义" |

---

## 9. 共识达成

✅ 所有 10 个关键决策已基于现有代码/Phase E.17 模式自决
✅ 任务边界清晰（仅扩 backend 接口 + HDRRenderer + Lua 1 对 API）
✅ 验收标准具体可测试
✅ 零回归保障明确（默认 false）
✅ 行业最佳实践：与 motion blur halfRes 路径一致

**可进入 DESIGN 阶段。**
