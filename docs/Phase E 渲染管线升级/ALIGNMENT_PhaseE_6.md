# ALIGNMENT — Phase E.6 · Lens Dirt + Streak (镜头后处理)

> 6A 工作流 · 阶段 1 · Align
> 目标：模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 前置基础（已完成）

- **Phase E.3 (HDR + Tonemap)**：HDR RT (RGBA16F) + 4 tonemap operators
- **Phase E.4 (Bloom)**：多尺度金字塔 + auto-link
- **Phase E.5 (Auto Exposure)**：log luma → mipmap reduce → CPU readback → 时间平滑
- **`HDRRenderer::EndScene` 当前流程**：
  ```
  UnbindFBO
  BloomRenderer::Process(fbo, sceneTex)         ← Phase E.4 (写回 HDR RT)
  UnbindFBO
  AutoExposureRenderer::Process(sceneTex, dt)   ← Phase E.5 (更新 currentExposure)
  exposure = AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
  DrawTonemapFullscreen(sceneTex, exposure, gamma, mode)
  ```
- **Bloom pyramid[0] 输出**：HDR RT 内已含 bloom 加性结果；DESIGN 决定 Lens Dirt 在 Bloom 之后、tonemap 之前介入
- **HDR/Bloom/AE 累计 45 个 Lua API**

### 1.2 现有类似子系统作参考

- `@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp` — namespace + auto-link 模板
- `@e:\jinyiNew\Light\ChocoLight\src\auto_exposure_renderer.cpp` — namespace + Process 流程模板
- `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:InitBloom` — 多 shader 编译模板
- `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:DrawBloomComposite` — 加性合成 (`GL_ONE/GL_ONE`) 模板

### 1.3 技术栈

- C++ → GL33 + GLES3 shader (双 profile)
- Lua 5.1 → `light_graphics.cpp` 新增 `LensDirt` + `Streak` 两个子表
- Lens Dirt 需要可加载图像作为脏污纹理（用现有 `Image` 资源系统）
- CI: Windows runtime smoke via `scripts/smoke/lens_fx.lua`（合并文件）

---

## 2. 需求理解

### 2.1 Lens Dirt（镜头脏污）

**概念**：模拟相机镜头表面的污渍/灰尘/水渍/划痕，让强光通过 bloom 时显示出"脏镜头"的视觉特征。

**算法**：
```
final_hdr = hdr_rt + bloomTex × dirtTex × dirtIntensity
```
其中 `dirtTex` 是用户提供的灰度（或彩色）噪点图，`dirtIntensity` 是合成强度。

**典型素材**：512×512 灰度图，含 spots / scratches / smudges（GitHub 上有大量公开 lens dirt textures）。

### 2.2 Streak（横向条纹光晕 / Anamorphic Flare）

**概念**：电影 anamorphic 镜头的特征 —— 高亮处会有水平/垂直方向的长条纹光晕（"星芒"或"光带"），强化电影感。

**算法**：
```
streak_pyramid[0] = brightPass(hdrTex, threshold)          // 提取亮度
for i in 1..N:                                              // N 次方向倍距迭代
    streak_pyramid[i] = blur1D(streak_pyramid[i-1],
                                length × 2^i,
                                direction)                  // direction = (1,0) 横向
final_hdr = hdr_rt + streak_pyramid[N-1] × intensity
```

**关键参数**：
- `length`：单步采样距离（决定每次迭代扩展长度）
- `direction`：方向向量 (常用：水平 / 垂直 / 斜 45°)
- `intensity`：合成强度
- `iterations`：迭代次数（4-6 通常足够）

### 2.3 用户期望的体验（典型 Lua 用法）

```lua
local Gfx = require("Light.Graphics")
Gfx.HDR.Enable(1920, 1080)
Gfx.Bloom.Enable(1920, 1080)         -- bloom 是前提

-- Lens Dirt
local dirtImg = Gfx.Image.Load("assets/lens_dirt.png")
Gfx.LensDirt.SetDirtTexture(dirtImg)
Gfx.LensDirt.SetIntensity(0.4)
Gfx.LensDirt.Enable()                 -- 一键启用

-- Streak
Gfx.Streak.Enable(1920, 1080)
Gfx.Streak.SetThreshold(1.5)         -- 亮度阈值
Gfx.Streak.SetIntensity(0.3)
Gfx.Streak.SetLength(0.02)           -- 单步 UV 距离
Gfx.Streak.SetDirection(1.0, 0.0)    -- 水平
Gfx.Streak.SetIterations(5)

-- 主循环正常绘制；带高亮的像素自动有 dirt + streak 效果
```

### 2.4 边界 / 必做 vs 不做

#### 必做（In Scope）

| 能力 | 说明 |
|------|------|
| **Lens Dirt** | 用户提供 dirt 纹理，bloom × dirt × intensity 加到 HDR RT |
| **Streak** | bright pass + N 次方向倍距 1D 模糊 + 加性合成 |
| **HDR auto-link** | `HDR.Enable/Disable/Resize` 联动 Streak RT 生命周期（Lens Dirt 不需 RT，仅启用 flag） |
| **AutoEnable flag** | 两者默认 false（与 AE 同；Bloom 默认 true 是特例） |
| **参数 clamp** | length [0, 0.1]、intensity [0, +∞)、threshold [0, +∞)、iterations [1, 8] |
| **Direction normalize** | shader 内部 normalize；Lua 不强制单位向量 |
| **GL33 / GLES3 双 shader** | streak bright + streak blur1D + lens dirt composite |
| **Legacy fallback** | `IsSupported()=false`，所有 API no-op |
| **Lens Dirt 默认纹理** | 引擎内置一个简易程序化 dirt 纹理（如果用户不指定）|
| **Lua API** | `Light.Graphics.LensDirt`（10 函数）+ `Light.Graphics.Streak`（13 函数） |
| **smoke 覆盖** | `scripts/smoke/lens_fx.lua`（≥ 25 PASS） |
| **demo_lens_fx** | 交互式：dirt on/off + streak on/off + 参数调节 |
| **6 平台 CI 绿** | Windows / macOS / Linux / Android / iOS / Web |

#### 不做（Out of Scope）

| 排除 | 原因 |
|------|------|
| 程序化 dirt 纹理生成器 | v1 用 1×1 白色 fallback 或要求用户提供；procedural 留作扩展 |
| Lens flare（鬼影 ghost） | 需要采样光源主方向；与 streak 不同，独立 phase |
| Anamorphic horizontal 模糊（专用扁椭圆 kernel） | v1 用通用方向 1D 模糊，参数化 direction 实现 |
| Animated dirt（湿气 / 雨滴动画） | 对 dirt texture 做 UV scroll 留 v2 |
| HDR-aware dirt blending（dirt 用 HDR 强度乘） | v1 用 LDR 灰度乘；HDR dirt 需特殊 texture，留 v2 |
| Streak 多方向 (3-way star)| v1 只一个 direction；用户多次 Enable 多 instance 留 v2 |

---

## 3. 技术决策（智能决策策略输出）

### 3.1 Lens Dirt 数据流

```
HDRRenderer::EndScene 内:
    ... Bloom Process 完成 (HDR RT 已含 bloom 加性结果) ...
    UnbindFBO
    AE Process
    
    [新插点] LensDirtRenderer::Process(hdrFbo, bloomTex, hdrRtW, hdrRtH)
        - DrawLensDirtComposite(bloomTex, hdrFbo, dirtTex, intensity, w, h)
        - shader: hdr_dst += bloomTex × dirtTex × intensity
    
    [新插点] StreakRenderer::Process(hdrFbo, hdrTex)
        - DrawStreakBright(hdrTex → streakRT[0], threshold)
        - for i in 1..N: DrawStreakBlur(streakRT[i-1] → streakRT[i], len × 2^i, dir)
        - DrawStreakComposite(streakRT[N-1], hdrFbo, intensity)
    
    DrawTonemapFullscreen(...)
```

**插点选择**：Lens Dirt 在 AE 之后但 tonemap 之前，让 dirt 效果包含在 AE 测量后的画面里（实际 AE 测的是含 bloom 的画面，dirt 来自 bloom 加成）。Streak 在 Lens Dirt 之后，最后合成到 tonemap 输入。

### 3.2 Streak Bright Pass

复用 Bloom bright shader 即可（`luminance threshold + soft knee`），后端可直接复用 `programBloomBright` 跑到 streak RT。**v1 决策：复用 Bloom 的 BrightPass shader，避免冗余 shader**。

### 3.3 Streak Blur1D Shader

```glsl
uniform sampler2D uSrc;
uniform vec2  uTexel;       // 1.0 / dst size
uniform float uLength;      // 单步 UV 距离
uniform vec2  uDirection;   // 方向向量 (normalize 在 shader 内)

void main() {
    vec2 dir = normalize(uDirection) * uLength;
    vec3 c = vec3(0.0);
    // 6-tap 单方向高斯权重 [0.05, 0.1, 0.2, 0.3, 0.2, 0.1, 0.05]
    c += texture(uSrc, vUV - 3.0 * dir).rgb * 0.05;
    c += texture(uSrc, vUV - 2.0 * dir).rgb * 0.1;
    c += texture(uSrc, vUV - 1.0 * dir).rgb * 0.2;
    c += texture(uSrc, vUV).rgb * 0.3;
    c += texture(uSrc, vUV + 1.0 * dir).rgb * 0.2;
    c += texture(uSrc, vUV + 2.0 * dir).rgb * 0.1;
    c += texture(uSrc, vUV + 3.0 * dir).rgb * 0.05;
    FragColor = vec4(c, 1.0);
}
```

### 3.4 Streak RT 数量

需要 ping-pong 两个 RT 即可（每次 blur 写入对方）。固定 2 个 RT，分辨率 = HDR RT / 2（节省 fragment）。N 次迭代靠循环 ping-pong。

### 3.5 Lens Dirt Composite Shader

```glsl
uniform sampler2D uBloomTex;
uniform sampler2D uDirtTex;
uniform float uIntensity;

void main() {
    vec3 bloom = texture(uBloomTex, vUV).rgb;
    vec3 dirt  = texture(uDirtTex, vUV).rgb;       // 灰度图也可 (rgb=vec3(g, g, g))
    FragColor = vec4(bloom * dirt * uIntensity, 1.0);   // additive blend in framebuffer
}
```
合成靠 `GL_ONE/GL_ONE` blend 加到 HDR RT。

### 3.6 后端虚接口设计

#### Lens Dirt（3 个）
```cpp
virtual bool SupportsLensDirt() const { return false; }

/// shader: hdr_dst += bloomTex × dirtTex × intensity (additive blend ONE/ONE)
virtual void DrawLensDirtComposite(uint32_t bloomTex, uint32_t dirtTex,
                                    uint32_t hdrFbo,
                                    int w, int h, float intensity) {}
// 不需要 Create/Delete RT (不持有自己 RT, 直接写 hdrFbo)
```

#### Streak（5 个）
```cpp
virtual bool SupportsStreak() const { return false; }

/// 创建 ping-pong streak RT 对 (2 个 FBO + 2 个 RGBA16F tex, 同尺寸)
virtual bool CreateStreakTargets(int srcW, int srcH,
                                  uint32_t outFbos[2], uint32_t outTexs[2],
                                  int* outW, int* outH) { return false; }

virtual void DeleteStreakTargets(uint32_t fbos[2], uint32_t texs[2]) {}

/// 复用 Bloom bright pass shader, 输出到 streak RT[0]
virtual void DrawStreakBright(uint32_t hdrTex, uint32_t outFbo,
                               int w, int h, float threshold) {}
// 注: v1 决策为后端复用 programBloomBright; 如果后续 bright pass 有 streak 专属调整, 加专属 shader

/// 1D 方向模糊: srcTex → dstFbo
virtual void DrawStreakBlur(uint32_t srcTex, uint32_t dstFbo,
                             int w, int h,
                             float length, float dirX, float dirY) {}

/// 加性合成: streakTex additive → hdrFbo (intensity scaled)
virtual void DrawStreakComposite(uint32_t streakTex, uint32_t hdrFbo,
                                  int w, int h, float intensity) {}
```

#### 累计：8 个新虚接口（vs Phase E.5 的 6 个）

### 3.7 Lua API 设计

#### `Light.Graphics.LensDirt`（10 函数）
```
Enable() -> bool          (无需尺寸; 直接启用)
Disable()
IsEnabled() -> bool
IsSupported() -> bool
SetAutoEnable(flag)        默认 false
GetAutoEnable() -> bool
SetDirtTexture(image)      Image userdata; nil 重置为内置默认
GetDirtTexture() -> image  返回当前 image (可能 nil)
SetIntensity(v)            clamp [0, +∞)
GetIntensity() -> number
```

#### `Light.Graphics.Streak`（13 函数）
```
Enable(w, h) -> bool       (需要 RT 尺寸)
Disable()
IsEnabled() -> bool
IsSupported() -> bool
Resize(w, h) -> bool
SetAutoEnable(flag)        默认 false
GetAutoEnable() -> bool
SetThreshold(v)            亮度阈值 clamp [0, +∞), 默认 1.0
GetThreshold() -> number
SetIntensity(v)            clamp [0, +∞), 默认 0.3
GetIntensity() -> number
SetLength(v)               单步 UV 距离 clamp [0, 0.1], 默认 0.02
GetLength() -> number
SetDirection(x, y)         shader 内 normalize
GetDirection() -> x, y
SetIterations(n)           clamp [1, 8], 默认 5
GetIterations() -> int
```

→ 共 23 函数（Lens Dirt 10 + Streak 13）。

---

## 4. 疑问澄清（已自决）

| Q | 决策 | 依据 |
|---|------|------|
| 1. 一个合并子表 LensFx vs 两个独立子表？ | **两个独立**（LensDirt + Streak） | 各自独立 Enable / Disable / 参数；与 Bloom + AE 风格一致 |
| 2. AutoEnable 默认 true / false？ | **false**（与 AE 同） | Lens dirt + streak 是审美强烈的特效，不应默认接管 |
| 3. Lens Dirt 默认纹理？ | **1×1 白色 fallback**（启用但未 SetDirtTexture 时用） | 让 dirt 启用即生效（× 白色 = bloom × intensity 效果，可见 dirt 启用了） |
| 4. dirt texture 资源类型？ | **Image userdata**（现有引擎类型） | 复用 `Image.Load`，无需新建 texture loader |
| 5. Streak ping-pong RT 大小？ | **HDR / 2**（节省 fragment） | 行业实践 |
| 6. Bright pass 复用 Bloom 还是独立？ | **复用 Bloom programBloomBright** | v1 简化；后续如需 streak 专属 threshold 公式再分 |
| 7. Streak direction 默认？ | **(1.0, 0.0)** 水平 | 最经典电影感方向 |
| 8. 顺序：Lens Dirt vs Streak 先后？ | **Lens Dirt 先，Streak 后** | dirt 是 bloom 的"加料"；streak 是独立加性。两者 commutative，但选定顺序便于 demo |
| 9. Iterations 上限 8 合理？ | **是** | length × 2^7 = 128 步 UV 已极长，过此无视觉收益 |

---

## 5. 项目特性规范对齐

| 维度 | 规范 |
|------|------|
| 命名空间 | `LensDirtRenderer` + `StreakRenderer`（C++），`Light.Graphics.LensDirt` + `Light.Graphics.Streak`（Lua） |
| 模块文件 | `lens_dirt_renderer.{h,cpp}` + `streak_renderer.{h,cpp}` |
| Lua 子表 | 两个，挂在 `luaopen_Light_Graphics`，`Bloom`/`AutoExposure` 之后 |
| 后端虚接口 | 8 个（Lens Dirt 3 + Streak 5），默认 no-op |
| 测试 | `scripts/smoke/lens_fx.lua`（合并文件，ASCII-only, ≥ 25 PASS） |
| CI | `phaseE6Smoke` 注册 |
| demo | `samples/demo_lens_fx/{main.lua, README.md}`（合并 demo） |
| 文档 | ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO 6 件套 |

---

## 6. 最终共识

所有歧义已自决，可直接进入 **DESIGN** 阶段。

**关键不变量**：
- 两者均默认 `autoEnable = false`
- Lens Dirt 不持有 RT；启用即可（用 1×1 白纹理 fallback）
- Streak 持有 ping-pong RT 对（HDR / 2 大小）
- Bloom 是 Streak 的可选前提（streak bright pass 来自 hdrTex，已含 bloom 但不强求 bloom 启用）
- 两者均在 AE.Process 之后插入 HDRRenderer::EndScene

——

进入 **DESIGN_PhaseE_6.md** ✅
