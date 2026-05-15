# Phase E.17 Half-res Motion Blur — DESIGN（架构详设）

> 6A 工作流 · 阶段 2
> 基线：ALIGNMENT_PhaseE_17.md 的 10 个决策已就位

---

## 1. 整体架构

### 1.1 数据流图

```
Phase E.16 (full-res, 现状):
  hdrTex (w×h) ─┬─→ MotionBlur Pass1 [shader, viewport=(0,0,w,h)] ──→ motionBlurTex (w×h)
                │                                                              │
                │   velocityTex (w×h), cameraVelocityTex (w×h)                  │
                │                                                              ▼
                └────────────────── Pass2: blit (w×h)→(w×h) GL_NEAREST ──→ hdrFbo

────────────────────────────────────────────────────────────────────────────────────

Phase E.17 (half-res, 新):
  halfRes=true:
    rtW = (w+1)/2, rtH = (h+1)/2

  hdrTex (w×h) ─┬─→ MotionBlur Pass1 [shader, viewport=(0,0,rtW,rtH)] ──→ motionBlurTex (rtW×rtH)
                │                                                                   │
                │   velocityTex (w×h), cameraVelocityTex (w×h)  ★ 全分辨率不变       │
                │                                                                   ▼
                └────────────────── Pass2: blit (rtW×rtH)→(w×h) GL_LINEAR ──→ hdrFbo
                                              ↑ 硬件 bilinear 上采样
```

### 1.2 关键洞察

- **velocity 输入保持全分辨率** → Pass1 在半分辨率读全分辨率 velocityTex 时，bilinear filter（已配 GL_LINEAR）自动平滑采样，**精度无损**
- **shader 完全不动** → Pass1 viewport 缩半，gl_FragCoord 自动变 (0..rtW, 0..rtH)；vUV 仍是 (0..1)；uTexel = 1.0/vec2(rtW, rtH) 由 backend 自动给出
- **Pass2 GL_LINEAR blit** → src=(0,0,rtW,rtH)→dst=(0,0,w,h)，硬件 bilinear 上采样，零开销

### 1.3 模块依赖图

```
┌────────────────────────────────────┐
│  Light.Graphics.MotionBlur.SetHalfRes (Lua)
│  Light.Graphics.MotionBlur.GetHalfRes
└─────────────┬──────────────────────┘
              │
              ▼
┌────────────────────────────────────┐
│  MotionBlurRenderer (壳层)
│  + g.halfRes : bool                ★ 新字段
│  + SetHalfRes(bool)                ★ 新 fn (触发 Resize)
│  + GetHalfRes()                    ★ 新 fn
│  + Process: 算 rtW/rtH 透传 backend
└─────────────┬──────────────────────┘
              │
              ▼
┌────────────────────────────────────┐
│  RenderBackend (虚接口)
│  + CreateMotionBlurRT(w, h, *tex,  
│                       storageW=0,  ★ 新参 (=0 → 沿用 w)
│                       storageH=0)  ★ 新参
│  + DrawMotionBlur(..., w, h,       ← dstFbo 尺寸
│                   strength, n, mode,
│                   rtW=0, rtH=0)    ★ 新参 (=0 → 沿用 w/h)
└─────────────┬──────────────────────┘
              │
              ▼
┌────────────────────────────────────┐
│  GL33Backend
│  - CreateMotionBlurRT 用 storageW/H 实际分配
│  - DrawMotionBlur Pass1 viewport (rtW, rtH) + Pass2 blit src=(rtW, rtH) GL_LINEAR
└────────────────────────────────────┘
```

---

## 2. 接口契约定义

### 2.1 RenderBackend 虚接口扩展

#### 2.1.1 `CreateMotionBlurRT` 签名扩展

```cpp
// 原 (Phase E.15/E.16):
virtual uint32_t CreateMotionBlurRT(int w, int h, uint32_t* outTex);

// 新 (Phase E.17):
virtual uint32_t CreateMotionBlurRT(int w, int h, uint32_t* outTex,
                                     int storageW = 0,    // ★ 新参 0 → 沿用 w (向后兼容)
                                     int storageH = 0);   // ★ 新参 0 → 沿用 h
```

**语义**：`w/h` 为逻辑尺寸（决定 dstFbo blit dst 矩形）；`storageW/H` 为 RT 实际分配尺寸（决定 Pass1 viewport）。`storageW==0 || storageH==0` → fallback 到 `w/h`。

#### 2.1.2 `DrawMotionBlur` 签名扩展

```cpp
// 原 (Phase E.16):
virtual void DrawMotionBlur(uint32_t sceneTex, uint32_t velocityTex,
                            uint32_t cameraVelocityTex,
                            uint32_t motionBlurFbo, uint32_t motionBlurTex,
                            uint32_t dstFbo,
                            int w, int h,
                            float strength, int sampleCount,
                            int mode);

// 新 (Phase E.17):
virtual void DrawMotionBlur(uint32_t sceneTex, uint32_t velocityTex,
                            uint32_t cameraVelocityTex,
                            uint32_t motionBlurFbo, uint32_t motionBlurTex,
                            uint32_t dstFbo,
                            int w, int h,                     // dstFbo 尺寸
                            float strength, int sampleCount,
                            int mode,
                            int rtW = 0, int rtH = 0);        // ★ 新参 0 → w/h
```

**语义**：`w/h` = dstFbo 尺寸（Pass2 blit dst）；`rtW/rtH` = motionBlurTex 实际尺寸（Pass1 viewport + Pass2 blit src）。`rtW==0 || rtH==0` → fallback 到 `w/h`。

### 2.2 MotionBlurRenderer 命名空间扩展

```cpp
// motion_blur_renderer.h 新增 (在 SetMode/GetMode 之后):

/// Phase E.17 — half-res motion blur (默认 false)
/// true  = motionBlurTex 改为 ((w+1)/2, (h+1)/2)，Pass2 用 bilinear 上采样
/// false = full-res（与 Phase E.15/E.16 一致）
/// 切换时已 Enable 则立即 Resize 重建 RT；未 Enable 则下次 Enable 生效
void SetHalfRes(bool flag);
bool GetHalfRes();
```

### 2.3 Lua API 增量

```lua
Light.Graphics.MotionBlur.SetHalfRes(true)    -- 启用半分辨率
local on = Light.Graphics.MotionBlur.GetHalfRes()
```

---

## 3. 核心组件详设

### 3.1 GL33Backend::CreateMotionBlurRT 改动（~10 行）

```cpp
uint32_t CreateMotionBlurRT(int w, int h, uint32_t* outTex,
                             int storageW, int storageH) override {
    if (outTex) *outTex = 0;
    if (!motionBlurSupported || w <= 0 || h <= 0 || !outTex) return 0;

    // ★ Phase E.17: storageW/H == 0 → fallback w/h (向后兼容)
    int sw = (storageW > 0) ? storageW : w;
    int sh = (storageH > 0) ? storageH : h;

    // ... GenFramebuffers + GenTextures（无变化）...

    // 用 sw, sh 而非 w, h 分配 tex
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, sw, sh, 0, GL_RGBA, GL_FLOAT, nullptr);
    // GL_LINEAR filter 已配（Phase E.15）→ 直接复用做 Pass2 bilinear 上采样

    // ... attach + status check（无变化）...
}
```

### 3.2 GL33Backend::DrawMotionBlur 改动（~8 行）

```cpp
void DrawMotionBlur(..., int w, int h, ..., int rtW, int rtH) override {
    // ★ Phase E.17: rtW/H == 0 → fallback w/h
    int passW = (rtW > 0) ? rtW : w;
    int passH = (rtH > 0) ? rtH : h;

    // ... safeMode fallback（无变化）...

    // Pass1: viewport 用 passW/passH（半分辨率渲染）
    glBindFramebuffer(GL_FRAMEBUFFER, motionBlurFbo);
    glViewport(0, 0, passW, passH);                 // ★ 改：原是 w, h
    // ... shader uniforms + draw（无变化，uTexel 自动计算）...

    // Pass2: blit (passW, passH) → (w, h) 用 GL_LINEAR
    glBindFramebuffer(GL_READ_FRAMEBUFFER, motionBlurFbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
    glBlitFramebuffer(0, 0, passW, passH,           // ★ src 改为 passW/passH
                      0, 0, w, h,
                      GL_COLOR_BUFFER_BIT,
                      (passW == w && passH == h) ? GL_NEAREST : GL_LINEAR);  // ★ 自动选 filter
}
```

**关键点**：
- `rtW = w && rtH = h` 时 fallback `GL_NEAREST`（性能最优）
- `rtW < w` 时用 `GL_LINEAR`（硬件 bilinear 上采样）

### 3.3 MotionBlurRenderer 改动（~15 行）

```cpp
// State 字段
struct State {
    // ... 原字段 ...
    bool halfRes = false;   // ★ Phase E.17
};

// 新增 fn
void SetHalfRes(bool flag) {
    if (g.halfRes == flag) return;          // no-op
    g.halfRes = flag;
    if (g.enabled) {
        // 立即 Resize 重建 RT（决策 D3）
        Resize(g.width, g.height);
    }
}
bool GetHalfRes() { return g.halfRes; }

// 内部辅助：算实际 RT 尺寸
inline void ComputeStorageSize(int w, int h, int& sw, int& sh) {
    if (g.halfRes) {
        sw = (w + 1) / 2;   // 决策 D1: 向上取整
        sh = (h + 1) / 2;
    } else {
        sw = w;
        sh = h;
    }
}

// CreateRT 改：传 storageW/storageH
bool CreateRT(int w, int h) {
    int sw, sh;
    ComputeStorageSize(w, h, sw, sh);
    uint32_t fbo = g.backend->CreateMotionBlurRT(w, h, &tex, sw, sh);
    // ...
}

// Process 改：透传 rtW/rtH
void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // ... 原防御 ...
    int rtW, rtH;
    ComputeStorageSize(g.width, g.height, rtW, rtH);
    g.backend->DrawMotionBlur(..., g.width, g.height, ..., g.mode, rtW, rtH);
}
```

### 3.4 Lua API 改动（light_graphics.cpp，~12 行）

```cpp
static int l_MB_SetHalfRes(lua_State* L) {
    MotionBlurRenderer::SetHalfRes(lua_toboolean(L, 1) != 0);
    return 0;
}
static int l_MB_GetHalfRes(lua_State* L) {
    lua_pushboolean(L, MotionBlurRenderer::GetHalfRes() ? 1 : 0);
    return 1;
}

// mb_funcs[] 加 2 条:
{"SetHalfRes",     l_MB_SetHalfRes},
{"GetHalfRes",     l_MB_GetHalfRes},
```

---

## 4. 接口契约调用时序

### 4.1 用户启用 halfRes 时序

```
时刻 t0: HDR.Enable(1280, 720) → MotionBlur.Enable(1280, 720)
   - g.width = 1280, g.height = 720, g.halfRes = false (默认)
   - CreateMotionBlurRT(1280, 720, *tex, 1280, 720)  // 全分辨率

时刻 t1: MotionBlur.SetHalfRes(true)
   - g.halfRes = true
   - 因 g.enabled=true → Resize(1280, 720)
   - Resize → ReleaseRT + CreateRT(1280, 720)
   - CreateRT → ComputeStorageSize(1280, 720, sw, sh) → sw=640, sh=360
   - CreateMotionBlurRT(1280, 720, *tex, 640, 360)   // 半分辨率

时刻 t2: 主循环 EndScene
   - MotionBlur.Process(hdrFbo, hdrTex)
   - ComputeStorageSize(1280, 720, rtW, rtH) → rtW=640, rtH=360
   - DrawMotionBlur(..., 1280, 720, ..., mode, 640, 360)
   - GL33: Pass1 viewport(0,0,640,360); Pass2 blit (640,360)→(1280,720) GL_LINEAR

时刻 t3: MotionBlur.SetHalfRes(false)
   - g.halfRes = false
   - Resize → CreateMotionBlurRT(1280, 720, *tex, 1280, 720)   // 还原
   - 后续 Pass2 自动 GL_NEAREST
```

### 4.2 SetHalfRes 在 Enable 前后的差异

| 调用顺序 | 行为 |
|---------|------|
| `Enable → SetHalfRes(true)` | 立即 Resize 重建 RT 为半分辨率 |
| `SetHalfRes(true) → Enable` | Enable 用 g.halfRes=true 创建半分辨率 RT |
| `SetHalfRes(true) → Disable → Enable` | Enable 用保留的 g.halfRes=true 创建半分辨率 RT |

→ 状态机一致：`halfRes` 是参数性质，与 strength/sampleCount/mode 同地位。

---

## 5. 异常处理策略

| 异常 | 处理 | 日志 |
|------|------|------|
| `storageW <= 0 || storageH <= 0`（未来扩展） | fallback 到 w, h | 不日志（默认行为） |
| 半分辨率 RT 创建失败（OOM 极少） | DeleteMotionBlurRT 兜底 + Log ERROR | 同 Phase E.15 |
| `rtW > w || rtH > h`（不应发生但防御） | glBlitFramebuffer 接受任意 src/dst 尺寸（GL 自动 clamp） | 不日志 |
| SetHalfRes 在 Init 前调用 | g.halfRes 字段更新；Enable 时生效 | 不日志（与 SetStrength 风格一致） |

---

## 6. 数据流向图（详细）

```
[Lua]
  MB.SetHalfRes(true)
    ↓
[motion_blur_renderer.cpp::SetHalfRes]
  g.halfRes = true; if (enabled) Resize()
    ↓
[Resize → ReleaseRT → CreateRT]
  ComputeStorageSize(g.width, g.height, sw, sh) → sw=640, sh=360
    ↓
[backend->CreateMotionBlurRT(1280, 720, &tex, 640, 360)]
  glTexImage2D(GL_RGBA16F, 640, 360, ...)  ← RT 实际尺寸
  glFramebufferTexture2D(...)
  return fbo
    ↓
[motion_blur_renderer::Process(hdrFbo, hdrTex) per frame]
  ComputeStorageSize(g.width, g.height, rtW, rtH) → 640, 360
  backend->DrawMotionBlur(hdrTex, vTex, cvTex, fbo, tex, hdrFbo,
                          1280, 720,        ← w, h (dstFbo 尺寸)
                          strength, 8, mode,
                          640, 360)         ← rtW, rtH
    ↓
[GL33::DrawMotionBlur]
  Pass1: glBindFramebuffer(motionBlurFbo)
         glViewport(0, 0, 640, 360)
         shader 跑 640×360 = 230,400 fragments  (vs 921,600 全分辨率, 25%)
         uTexel = 1/vec2(640, 360) 自动正确
         output → motionBlurTex (640×360 RGBA16F)
  Pass2: glBlitFramebuffer(0,0,640,360 → 0,0,1280,720, GL_LINEAR)
         硬件 bilinear 上采样 → hdrFbo (1280×720)
```

---

## 7. 性能预算（设计估算）

### 7.1 Pass1 fragment 数量

| 分辨率 | full-res fragments | half-res fragments | 减少比例 |
|--------|-------------------|-------------------|---------|
| 720p   | 921,600 | 230,400 | **−75%** |
| 1080p  | 2,073,600 | 518,400 | **−75%** |
| 1440p  | 3,686,400 | 921,600 | **−75%** |
| 4K     | 8,294,400 | 2,073,600 | **−75%** |

### 7.2 时间预算（@ 1080p, sampleCount=8, dilation ON）

| Phase | Pass1 | Pass2 | 合计 |
|-------|-------|-------|------|
| E.15/E.16 (full-res) | ~0.50 ms | ~0.05 ms (NEAREST) | **~0.55 ms** |
| E.17 (half-res) | **~0.13 ms** | ~0.07 ms (LINEAR) | **~0.20 ms** |
| 节省 | −74% | +0.02 ms | **−64%** |

### 7.3 VRAM @ 1080p RGBA16F

| Phase | motionBlurTex | 节省 |
|-------|--------------|------|
| E.15/E.16 (full-res) | 8 MB | — |
| E.17 (half-res) | **2 MB** | **−75%** |

---

## 8. 与现有系统集成

### 8.1 SSR Temporal 不动
SSR 用 velocityTex（全分辨率）做 reproject，与 motionBlurTex 尺寸完全无关。Phase E.17 仅改 motionBlurTex，SSR 路径零影响。

### 8.2 Phase E.16 mode 切换兼容
mode 切换是 shader 内逻辑（基于 sampler），与 RT 尺寸正交。halfRes ON/OFF × mode 0/1/2 = 6 种组合全部兼容。

### 8.3 OnHDRResized 联动
HDRRenderer.Resize 触发 MotionBlurRenderer.OnHDRResized → Resize(w, h)。Resize 内部用 g.halfRes 算新 RT 尺寸，halfRes 状态自动保留。

---

## 9. 测试设计

### 9.1 smoke 增量（3 PASS）

```lua
-- §8) Phase E.17 — Half-res default / round-trip
-- 默认 false
assert(MB.GetHalfRes() == false, "Default GetHalfRes() must be false")
pass("GetHalfRes() default = false")

-- round-trip
MB.SetHalfRes(true)
assert(MB.GetHalfRes() == true, "SetHalfRes(true) round-trip failed")
MB.SetHalfRes(false)
assert(MB.GetHalfRes() == false, "SetHalfRes(false) round-trip failed")
pass("SetHalfRes / GetHalfRes round-trip ok")

-- Set 不影响 Enable 状态（headless 友好）
MB.SetHalfRes(true)
assert(type(MB.IsEnabled()) == "boolean", "SetHalfRes 不应影响 IsEnabled 类型")
pass("SetHalfRes does not corrupt IsEnabled state")

MB.SetHalfRes(false)   -- 复位
```

### 9.2 demo_ssr 验证
按 `[` 键切 halfRes，HUD 显示状态 + Pass1 viewport 尺寸；用户视觉对比帧率与质量。

---

## 10. 推进确认

DESIGN 文档完成。下一步：Atomize（任务拆分）。
