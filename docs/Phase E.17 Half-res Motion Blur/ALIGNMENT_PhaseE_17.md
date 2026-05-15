# Phase E.17 Half-res Motion Blur — ALIGNMENT（对齐文档）

> 6A 工作流 · 阶段 1
> 基线：Phase E.16 commit `8f3457d`（CI run 25896826324 6/6 success）

---

## 1. 项目上下文分析

### 1.1 现有架构

ChocoLight Engine HDR 后处理链路（Phase E 系列累计）：

```
HDRRenderer::EndScene
   ↓ Bloom / LensDirt / Streak / SSAO / SSR / LensFlare 累积到 hdrFbo
   ↓ MotionBlurRenderer::Process(hdrFbo, hdrTex)        ← Phase E.15 入口
       ├─ Pass1 (shader): sceneTex + velocityTex → motionBlurTex (w×h)
       └─ Pass2 (blit):    motionBlurTex → 覆盖 sceneTex (GL_NEAREST)
   ↓ DrawTonemapFullscreen → default fb
```

### 1.2 Phase E.15/E.16 现有 motion blur 资源

| 资源 | 当前规格 | VRAM @ 1080p |
|------|---------|--------------|
| `motionBlurTex` | RGBA16F, w×h（GL_LINEAR filter 已配） | **~8 MB** |
| `motionBlurFbo` | color-only, 无 depth | — |
| `velocityTex` | RG16F/RG8 (Phase E.14), w×h | 4/1 MB |
| `cameraVelocityTex` | RG16F/RG8, w×h（Phase E.16） | 4/1 MB |

### 1.3 关键代码定位

- **`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:6527-6563`** — `CreateMotionBlurRT(w, h, *outTex)`，已用 `GL_LINEAR` filter（Phase E.17 上采样可直接复用）
- **`@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp:6573-6643`** — `DrawMotionBlur(...)`：Pass1 `glViewport(0,0,w,h)` + Pass2 `glBlitFramebuffer(...,GL_NEAREST)`（→ Phase E.17 改 `GL_LINEAR` 即可上采样）
- **`@e:/jinyiNew/Light/ChocoLight/src/motion_blur_renderer.cpp`** — `g.width / g.height` 已缓存，`Process` 直接透传给 backend

### 1.4 技术栈

- Backend：GL3.3 Core / GLES3
- 后处理 framework：纯壳模式（MotionBlurRenderer 不直接 GL，全 backend 转发）
- shader：现有 `programMotionBlur` 完全无须改动（uTexel = 1/vec2(rtW, rtH)）

---

## 2. 原始需求

> "Phase E.17: Half-res Motion Blur 优化 — motionBlurTex 改 (w/2, h/2)，Pass2 用 bilinear 上采样回原分辨率。VRAM -50%、性能 ~4×、视觉质量损失 ≤ 5%。移动端 / 高分屏受益最大。新增 Lua API: SetHalfRes(bool) / GetHalfRes()。"

---

## 3. 边界确认

### 3.1 In Scope

1. ✅ MotionBlurRenderer 新增 `halfRes` 状态字段（默认 false）
2. ✅ Backend `CreateMotionBlurRT` 接受可选 storageW/storageH 参数（不传 = 全分辨率）
3. ✅ Backend `DrawMotionBlur` 接受 rtW/rtH 参数（Pass1 viewport + Pass2 blit src 矩形）
4. ✅ Pass2 改 `GL_LINEAR` filter 实现硬件 bilinear 上采样
5. ✅ Lua API: `SetHalfRes(bool)` / `GetHalfRes()`
6. ✅ smoke 增 3 PASS（默认 false / round-trip / Set→IsEnabled 不冲突）
7. ✅ demo_ssr 加 `[` 键切 halfRes + HUD 显示
8. ✅ docs/api/Light_Graphics.md 增 SetHalfRes / GetHalfRes 段

### 3.2 Out of Scope

1. ❌ 1/4 分辨率（quarter-res）— 留 Phase E.17.x 候选
2. ❌ 自动根据屏幕分辨率切换 halfRes — 用户显式控制即可
3. ❌ velocityTex / cameraVelocityTex 也降半分辨率 — 精度损失太大，不做
4. ❌ shader 改动（uTexel 已自动跟随 viewport）
5. ❌ 跨 backend（Legacy）支持 — Phase E.15 已限定 GL33+

---

## 4. 需求理解（对现有项目）

### 4.1 半分辨率原理

Pass1 把 `motionBlurTex` 改成 (w/2, h/2)：
- shader 像素运算量 **−75%**（4× 像素少）
- VRAM **−50%**（4 MB → 1 MB @ 1080p RGBA16F）
- 9-tap dilation × sampleCount 全部按半分辨率逐像素

Pass2 用 `GL_LINEAR` filter blit (w/2, h/2) → (w, h)：
- GPU 内置硬件 bilinear，**~free**
- 视觉损失：高频运动模糊细节略失，但运动模糊本来就是低频信号 → **肉眼几乎不可见**

### 4.2 行业基线

| 引擎 | half-res motion blur | 是否做 strength 补偿 |
|------|---------------------|---------------------|
| Unreal Engine 5 | 默认 ON | 否 |
| Unity URP/HDRP | 可选 | 否 |
| Frostbite | 默认 ON | 否 |

→ Phase E.17 跟随行业惯例：**不做 strength/sampleCount 自动补偿**，依靠 bilinear 上采样的低通效应自然平滑。

### 4.3 性能预期（1080p）

| 项 | Phase E.16 (full-res) | Phase E.17 (half-res) | 收益 |
|----|----------------------|----------------------|------|
| Pass1 sampleCount=8 dilation | ~0.5 ms | ~0.13 ms | **−75%** |
| Pass2 blit | ~0.05 ms (NEAREST) | ~0.07 ms (LINEAR) | +0.02 ms |
| **总计** | ~0.55 ms | ~0.20 ms | **−63%** |
| VRAM motionBlurTex | 8 MB | **2 MB** | **−75%** |

### 4.4 复用现有资产

- ✅ `programMotionBlur` shader：完全不改，uTexel = 1/textureSize 已自动跟随
- ✅ `GL_LINEAR` filter 已配在 Pass1 RT 上（`@render_gl33.cpp:6542-6543`），可直接复用做 Pass2 bilinear 上采样
- ✅ `motion_blur_renderer.cpp` 状态机框架（已有 SetMode/SetStrength/SetSampleCount 风格的 Get/Set 对）

---

## 5. 疑问澄清

### 5.1 自动决策项（基于现有项目惯例）

| # | 决策点 | 选择 | 依据 |
|---|--------|------|------|
| D1 | 半分辨率位置取整 | `(w+1)/2, (h+1)/2`（向上） | 防止奇数像素丢失最后一行/列；与 mip 链惯例一致 |
| D2 | Pass2 上采样方式 | `glBlitFramebuffer + GL_LINEAR` | 最简实施；硬件 bilinear ~free；不需新 program |
| D3 | SetHalfRes 切换时机 | 立即触发 RT 重建（Resize） | 用户体验流畅，与现有 SSR/Bloom 状态切换一致 |
| D4 | 是否补偿 strength/sampleCount | **否**（行业惯例） | UE/Unity/Frostbite 都不补偿；bilinear 自然低通 |
| D5 | 默认值 | `halfRes = false` | **零回归**：Phase E.16 用户完全无感 |
| D6 | RT filter 配置 | 沿用现有 `GL_LINEAR`（已配） | 无需改 CreateMotionBlurRT 中 filter 设置 |
| D7 | API 命名 | `SetHalfRes` / `GetHalfRes` | 与 `SetMode/SetStrength` 风格一致；简洁；HalfRes 是行业术语 |
| D8 | 接口扩展 | `CreateMotionBlurRT` + `DrawMotionBlur` 各 +2 参数（rtW/rtH）默认值 = w/h 兼容 | 与 Phase E.16 `outCameraVelocityTex=nullptr` 风格一致 |
| D9 | 状态机交互 | `SetHalfRes` 在 Enable 前调用 → 下次 Enable 用新尺寸；Enable 后调用 → 立即 Resize | 渐进式状态机 |
| D10 | shader uniform 改动 | **零** | uTexel 由 backend 端 `1.0 / vec2(rtW, rtH)` 给出，shader 完全不动 |

### 5.2 用户拍板项

无。本 phase 全部决策可基于行业惯例 + 现有项目模式自动定。如果用户对 D4（不补偿）有偏好，可在 §3.4 后续 phase 调整。

---

## 6. 风险评估

| 风险 | 等级 | 缓解 |
|------|------|------|
| 半分辨率 dilation 9-tap 邻域物理覆盖 2× → 可能 over-blur | 🟡 中 | bilinear 上采样自动低通；用户可手动调小 sampleCount |
| 极小尺寸（如 64×64）半分辨率 32×32 可能视觉劣化 | 🟢 低 | 由用户判断启停（典型分辨率 ≥ 720p） |
| Pass2 GL_LINEAR blit 在 RGBA16F 上的兼容性 | 🟢 低 | OpenGL 3.3 / GLES3 都支持 RGBA16F + LINEAR filter |
| HalfRes 切换时 RT 重建竞态 | 🟢 低 | 与 SetVelocityFormat 同模式：Disable → 新参 → Enable |
| 现有 21 个 motion_blur smoke PASS | 🟢 低 | 默认 false → 完全等价 Phase E.16 |

---

## 7. 验收标准

### 7.1 功能验收

1. `MotionBlur.SetHalfRes(true) → IsEnabled / Resize` 正确创建半分辨率 RT
2. `MotionBlur.SetHalfRes(false) → Resize` 还原全分辨率
3. 默认 `GetHalfRes() == false`
4. Disable 后再 Enable 保留 halfRes 状态（与 strength/sampleCount 风格一致）

### 7.2 视觉验收（CI 不可覆盖，TODO 推用户）

1. 全分辨率 vs 半分辨率视觉差异 ≤ 5%（肉眼）
2. 移动端高分屏帧率提升明显

### 7.3 测试验收

1. smoke 24 PASS（21 原 + 3 halfRes 新）
2. CI 6/6 平台全 success
3. 现有 16 phase smoke 零回归

---

## 8. 最终共识（先行）

1. ✅ 半分辨率位置 = `(w+1)/2, (h+1)/2`（D1）
2. ✅ Pass2 上采样 = `glBlitFramebuffer + GL_LINEAR`（D2）
3. ✅ SetHalfRes 立即重建 RT（D3）
4. ✅ 不做 strength/sampleCount 补偿（D4，行业惯例）
5. ✅ 默认 OFF（D5，零回归）
6. ✅ 接口扩展默认参数兼容（D8）
7. ✅ shader 零改动（D10）

**Lua API 增量**：13 → 15 fn（SetHalfRes / GetHalfRes）

进入 Architect 阶段。
