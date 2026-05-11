# ALIGNMENT — Phase E.5 · Auto Exposure (Eye Adaptation)

> 6A 工作流 · 阶段 1 · Align
> 目标：模糊需求 → 精确规范

---

## 1. 项目上下文分析

### 1.1 前置基础（已完成）

- **Phase E.3 (HDR + Tonemap, 4 op)**: HDR RT (RGBA16F) + ACES/Reinhard/Uncharted2/Linear 切换
- **Phase E.4 (Bloom)**: 多尺度金字塔 + auto-link 接入 `HDRRenderer::EndScene`
- **`HDRRenderer::EndScene` 当前流程**：
  ```
  UnbindFBO()
  BloomRenderer::Process(g.fbo, g.sceneTex)        ← Phase E.4 插点
  UnbindFBO()
  DrawTonemapFullscreen(sceneTex, exposure, gamma, mode)
  ```
- **`HDRRenderer::SetExposure(v)` 当前是 manual 设值**，每帧 EndScene 传给 tonemap shader

### 1.2 现有类似子系统作参考

- `@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp` — 模块状态 + auto-link 联动模板（OnHDREnabled/Disabled/Resized）
- `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` — namespace module 生命周期 + RT 管理
- `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:InitBloom` — 多 shader 全屏 quad 模式
- `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` — 后端虚接口规范（HDR 4 + Bloom 6）

### 1.3 技术栈

- C++ → GL33 + GLES3 shader（双 profile，Phase E.3/E.4 已对齐 `#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)`）
- Lua 5.1 → `light_graphics.cpp` 新增 `AutoExposure` 子表
- CI: Windows runtime smoke via `scripts/smoke/auto_exposure.lua`（新建）

---

## 2. 需求理解

### 2.1 什么是 Auto Exposure / Eye Adaptation

**Auto Exposure（自动曝光）** 是 HDR 管线的"自动相机"：
- 在场景平均亮度变化时（例如玩家从黑暗洞穴走到强光户外），程序自动调整 `HDR.SetExposure` 的值
- 模拟人眼**视觉适应**（暗适应 ~30s，亮适应 ~0.5s）：从亮到暗较慢，从暗到亮较快
- 没有 AE 时玩家会经历"瞬间过爆 → 一直白屏"或"瞬间欠曝 → 完全看不见"的糟糕体验

### 2.2 用户期望的体验（典型 Lua 用法）

```lua
local Gfx = require("Light.Graphics")
Gfx.HDR.Enable(1920, 1080)
Gfx.HDR.SetExposure(1.0)         -- manual 默认值

Gfx.AutoExposure.Enable()                  -- 一键启用（接管 exposure）
Gfx.AutoExposure.SetTargetEV(0.0)          -- 目标中灰 = 0 EV (gray18%)
Gfx.AutoExposure.SetSpeedUp(3.0)           -- 暗→亮 适应速度 (EV/sec)，眼睛快
Gfx.AutoExposure.SetSpeedDown(1.0)         -- 亮→暗 适应速度 (EV/sec)，眼睛慢
Gfx.AutoExposure.SetMinEV(-8.0)            -- 曝光值下限（夜场）
Gfx.AutoExposure.SetMaxEV( 8.0)            -- 曝光值上限（强光）

-- 主循环正常绘制，AE 每帧自动测量场景 + 平滑调 HDR exposure
-- 用户可调试用 Gfx.AutoExposure.GetMeasuredLuminance() / GetCurrentEV()
```

启用 AE 后 `HDR.SetExposure` 的 manual 值被 AE 覆盖；`AutoExposure.Disable()` 后 manual 值立即回归。

### 2.3 边界 / 必做 vs 不做

#### 必做（In Scope）

| 能力 | 说明 |
|------|------|
| **Log-luminance extraction** | hdrTex → R16F luminance RT（log 域，鲁棒于极端值） |
| **GPU mipmap reduction** | `glGenerateMipmap` 让 GPU 自动算到 1×1 平均亮度 |
| **CPU readback** | 同步 `glReadPixels` 读 1×1 R16F（v1 接受 1 frame stall, ~2 bytes） |
| **Time smoothing** | `lerp(currentEV, targetEV, dt × speed)` 带 up/down 双速度 |
| **EV-based API** | 用 EV (stops) 而非线性 exposure 倍率，更符合摄影直觉 |
| **HDR auto-link** | `HDR.Enable/Disable/Resize` 自动驱动 AE 资源（同 Bloom 模式） |
| **AutoEnable flag** | 默认 false（manual exposure 是大多数 demo 的预期） |
| **EV clamp** | min/max EV 防 over/under-exposure |
| **GL33 / GLES3 双 shader** | luminance extract shader |
| **Legacy fallback** | `IsSupported()=false`，所有 API no-op |
| **Lua API** | `Light.Graphics.AutoExposure` 子表（≥ 13 函数）|
| **smoke 覆盖** | `scripts/smoke/auto_exposure.lua`（≥ 20 PASS） |
| **demo_auto_exposure** | 交互式两场景切换（暗 / 亮） |
| **6 平台 CI 绿** | Windows / macOS / Linux / Android / iOS / Web |

#### 不做（Out of Scope）

| 排除 | 原因 |
|------|------|
| Histogram-based AE | 需 compute shader (GLES 3.1+)；非全平台 |
| Async PBO double-buffer readback | v1 简化用同步 readback，2 bytes/frame 可接受；标 TODO |
| Center-weighted / spot metering | v1 全画面 average；后续可加 metering region 参数 |
| Auto white balance / chromatic AE | 仅亮度通道（color 自适应是另一个话题） |
| AE 输出影响 Bloom threshold | bloom threshold 不联动 AE；如有需要用户手动调 |
| AE 历史曲线绘制 / OSD 内置 | demo 层可做，模块不内建 |
| GPU-side ping-pong exposure（无 CPU readback 路径） | v2 优化方向，v1 先 CPU 简单 |

---

## 3. 技术决策（智能决策策略输出）

### 3.1 luminance 计算公式

```glsl
float luminance(vec3 rgb) {
    return dot(rgb, vec3(0.2126, 0.7152, 0.0722));   // Rec.709
}

// log domain 防止极端高光支配 & 0 防溢出
float logLuma = log(max(luminance(rgb), 0.0001));
```

### 3.2 EV ↔ exposure 换算（摄影标准）

```
exposure = 2^EV       (EV=0 ↔ exposure=1.0 ↔ 中灰反射率 18%)
EV = log2(exposure)
```

`Set/GetTargetEV` 用 EV 域更符合摄影直觉；内部最后转 exposure 给 tonemap shader。

### 3.3 时间平滑（双速度）

```cpp
float deltaEV = targetEV - currentEV;
float speed   = (deltaEV > 0) ? speedUp : speedDown;  // 暗→亮快/亮→暗慢
currentEV += clamp(deltaEV, -speed*dt, speed*dt);     // 限速 lerp
currentEV  = clamp(currentEV, minEV, maxEV);
```

### 3.4 后端虚接口（5 个，类似 Bloom）

```cpp
virtual bool SupportsAutoExposure() const { return false; }

virtual bool CreateLuminanceTarget(int srcW, int srcH,
                                    uint32_t* outFbo, uint32_t* outTex,
                                    int* outW, int* outH) { return false; }

virtual void DeleteLuminanceTarget(uint32_t fbo, uint32_t tex) {}

virtual void DrawLuminanceExtract(uint32_t hdrTex,
                                   uint32_t lumFbo,
                                   int w, int h) {}

virtual void GenerateLuminanceMipmap(uint32_t lumTex) {}

virtual float ReadbackLuminance1x1(uint32_t lumFbo, int lastMipLevel) { return 0.0f; }
```

### 3.5 模块插点（HDRRenderer::EndScene 中）

```
UnbindFBO()
BloomRenderer::Process(g.fbo, g.sceneTex)
UnbindFBO()
AutoExposureRenderer::Process(g.sceneTex, dt)        ← NEW
    ├ 1) DrawLuminanceExtract → lumFbo  (log Y)
    ├ 2) GenerateLuminanceMipmap        (R16F 1×1)
    ├ 3) ReadbackLuminance1x1           (CPU 同步)
    └ 4) 时间平滑 lerp(currentEV, targetEV, dt × speed)
float exposure = AutoExposureRenderer::IsEnabled()
                    ? AutoExposureRenderer::GetCurrentExposure()
                    : g.exposure;
DrawTonemapFullscreen(sceneTex, exposure, gamma, mode)
```

dt 由 HDRRenderer::EndScene 用 `static auto last = clock::now()` 算（无需额外 Time 依赖）。

### 3.6 Lua API surface（≥ 13 函数）

```
Light.Graphics.AutoExposure
  Enable(w, h) -> bool          (复用 HDR RT 大小; 通常 autoEnable)
  Disable()
  IsEnabled() -> bool
  IsSupported() -> bool
  Resize(w, h) -> bool
  SetAutoEnable(flag)            HDR.Enable 是否自动启 AE (默认 false)
  GetAutoEnable() -> bool
  SetTargetEV(v)                 中灰 EV (默认 0.0)
  GetTargetEV() -> number
  SetSpeedUp(v)                  暗→亮 EV/sec (默认 3.0)
  GetSpeedUp() -> number
  SetSpeedDown(v)                亮→暗 EV/sec (默认 1.0)
  GetSpeedDown() -> number
  SetMinEV(v)                    曝光下限 (默认 -8.0)
  GetMinEV() -> number
  SetMaxEV(v)                    曝光上限 (默认 +8.0)
  GetMaxEV() -> number
  GetCurrentEV() -> number       平滑后当前 EV (debug/OSD)
  GetCurrentExposure() -> number 当前 EV 转 exposure 倍率 (= 2^EV)
  GetMeasuredLuminance() -> number  上一帧测得 log luma (debug)
```

→ 共 18 函数（5 lifecycle + 2 autoEnable + 8 参数 set/get + 3 debug getter）。

---

## 4. 疑问澄清（已自决）

| Q | 决策 | 依据 |
|---|------|------|
| 1. AE 应覆盖 manual exposure 还是叠加？ | **覆盖**（Enable 时尊重 AE，Disable 立即回归 manual） | Phase E.4 Bloom autoEnable 模式同款，最少认知负担 |
| 2. AE 默认 autoEnable=true 还是 false？ | **false**（与 Bloom 默认 true 相反） | AE 改变 exposure 行为强烈，demo_hdr 测 tonemap 等应保持 manual 可控 |
| 3. CPU readback 同步 vs PBO 异步？ | **v1 同步**，标 TODO 后续异步 | 1×1 R16F 仅 2 字节，stall 极小（~10us），首版简单优先 |
| 4. 测量 RT 应用 hdrTex 完整尺寸还是 downscaled？ | **downscaled 1/4 起手**（如 1920×1080 → 480×270 → mipmap chain → 1×1） | 减少 fragment shader 工作量 |
| 5. EV 还是 exposure 倍率作主 API？ | **EV 主**（摄影直觉），exposure 作衍生 getter | 行业惯例（UE / Unity / Cry / Frostbite 都用 EV） |
| 6. lerp 还是 exponential decay？ | **限速 lerp**（速度单位 EV/sec） | 可预测，UI 上"速度=3 表示每秒最多 3 EV 跳"易理解 |
| 7. SetExposureBias (EV offset) 单独 API？ | **暂不引入**，TargetEV 已覆盖 | 简化；后续如有需要再加 |
| 8. Bloom + AE 顺序？ | **Bloom 先，AE 后**（AE 测量含 bloom 的 HDR RT） | 用户看到的画面亮度 = bloom 后的；AE 应基于此自适应 |

---

## 5. 项目特性规范对齐

| 维度 | 规范 |
|------|------|
| 命名空间 | `AutoExposureRenderer`（C++），`Light.Graphics.AutoExposure`（Lua） |
| 模块文件 | `auto_exposure_renderer.h` / `.cpp`（与 `hdr_renderer` / `bloom_renderer` 同目录） |
| Lua 子表 | 挂在 `luaopen_Light_Graphics` 中，在 `Bloom` 之后 |
| 后端虚接口 | 5 个，默认 no-op；GL33 实现；Legacy 不实现 |
| 测试 | `scripts/smoke/auto_exposure.lua`（ASCII-only, ≥ 20 PASS） |
| CI | `phaseE5Smoke` 注册到 `.github/workflows/build-templates.yml` |
| demo | `samples/demo_auto_exposure/{main.lua, README.md}` |
| 文档 | ALIGNMENT/DESIGN/TASK/ACCEPTANCE/FINAL/TODO 6 件套 |

---

## 6. 最终共识（→ CONSENSUS）

所有歧义已自决，可直接进入 **DESIGN** 阶段。无需用户中断澄清。

**关键不变量**：
- AE 默认 `autoEnable = false`（不自动启）；用户调 `Enable()` 才接管 exposure
- `Enable()` 接收 w/h，建议从 HDR RT 取
- AE Process 必须在 Bloom Process 之后、tonemap 之前
- CPU readback v1 同步可接受
- 全 EV 域 API，exposure 作衍生

——

进入 **DESIGN_PhaseE_5.md** ✅
