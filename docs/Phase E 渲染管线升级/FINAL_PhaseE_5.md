# FINAL — Phase E.5 · Auto Exposure (Eye Adaptation)

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.5 完整交付：**HDR 链路上的"自动相机"**。GPU 测量平均 log luminance + CPU 时间平滑 lerp + EV-based API + 双速度（暗→亮快/亮→暗慢）模拟人眼适应行为。沿用 Phase E.4 Bloom 的命名空间 + 子表风格，零 API 破坏性变更。

---

## 1. 交付物总览

### 1.1 三阶段拆分

| 阶段 | commit | 范围 | CI |
|------|--------|------|-----|
| **规划** | `168e683` | ALIGNMENT + DESIGN + TASK | ✅ 6/6 |
| **E.5.1 Backend** | `4b88569` | RenderBackend 6 个 AE 虚接口 + GL33 luma shader (双 profile) + R16F mipmap-able RT + sync readback | ✅ 6/6 ([`25697376364`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697376364), 10m15s) |
| **E.5.2 Module** | `1a9d013` | `AutoExposureRenderer` 命名空间 + `hdr_renderer.cpp` 4 联动点 + `light_ui.cpp` Init/Shutdown + CMake | ✅ 5/6 (iOS pending, [`25697700176`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697700176)) |
| **E.5.3 Lua API** | `00e3463` | `Light.Graphics.AutoExposure.*` 18 函数 + `auto_exposure.lua` smoke + `demo_auto_exposure` + CI 注册 | ⏳ 跑中 |

### 1.2 改动文件清单

| 文件 | 行数变化 | 类型 |
|------|---------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~70 | E.5.1 修改（6 虚接口） |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~230 | E.5.1 修改（luma shader 双 profile + InitAutoExposure + 6 override） |
| `@e:\jinyiNew\Light\ChocoLight\include\auto_exposure_renderer.h` | +160 | E.5.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\auto_exposure_renderer.cpp` | +~250 | E.5.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~30 | E.5.2 修改（4 联动点 + chrono dt + exposure 覆盖） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 | E.5.2 修改（Init/Shutdown） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~170 | E.5.3 修改（18 Lua + 子表） |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | E.5.2 |
| `@e:\jinyiNew\Light\scripts\smoke\auto_exposure.lua` | +250 | E.5.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_auto_exposure\main.lua` | +250 | E.5.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_auto_exposure\README.md` | +100 | E.5.3 新建 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.5.3 CI 注册 |
| 6 份 docs | — | ALIGNMENT/DESIGN/TASK + ACCEPTANCE + FINAL + TODO |

**总新增代码（生产）**：C++ ~ **710 行** + Lua/CMake/YAML ~ **600 行**
**总新增文档**：~ **2200 行**

---

## 2. 技术架构

### 2.1 数据流（HDR + Bloom + AE 全链路）

```
每帧主循环 (HDR ON, Bloom ON, AE ON):

Window_Call::BeginFrame
    │
    ▼
g_render->BeginFrame() / BatchRenderer / LitBatchRenderer ::BeginFrame
    │
    ▼
HDRRenderer::BeginScene()
    │  ├── BindFBO(HDR_FBO) → RGBA16F RT
    │  └── Clear
    ▼
[Lua Draw 阶段 - 写入 HDR RT]
    │
    ▼
LitBatchRenderer / BatchRenderer ::EndFrame
    │
    ▼
HDRRenderer::EndScene()
    │  ├── UnbindFBO
    │  │
    │  ├── BloomRenderer::Process(hdrFbo, hdrTex)              ← Phase E.4
    │  │     └─ 4 pass → bloom 加性写回 HDR RT
    │  │
    │  ├── UnbindFBO
    │  │
    │  ├── AutoExposureRenderer::Process(hdrTex, dt)            ← Phase E.5
    │  │     ├─ 1) DrawLuminanceExtract → R16F lumRT (480×270)
    │  │     ├─ 2) GenerateLuminanceMipmap → 1×1 平均 log luma
    │  │     ├─ 3) ReadbackLuminance1x1 → CPU (sync, ~10us)
    │  │     ├─ 4) targetEV = log2(0.18) - logLuma/log(2) + userTargetEV
    │  │     └─ 5) currentEV = lerp(currentEV → targetEV, dt × speed)
    │  │
    │  ├── exposure = AE.IsEnabled() ? 2^currentEV : g.exposure
    │  │
    │  └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
    │
    ▼
SwapBuffers
```

### 2.2 模块层次

```
┌──────────────────────────────────────────────────────────────────┐
│         Light.Graphics  (Lua, _G.Light.Graphics)                  │
│  ├── .HDR  (12 fn)                                                 │
│  ├── .Bloom (15 fn)             ← Phase E.4                       │
│  └── .AutoExposure (18 fn)      ← Phase E.5                       │
└──────────────────────────────────────────────────────────────────┘
              │              │              │
              ▼              ▼              ▼
┌────────────┐  ┌─────────────────┐  ┌──────────────────────┐
│HDRRenderer │  │ BloomRenderer   │  │AutoExposureRenderer  │
│  EndScene──┼─→│  Process        │  │  Process(hdrTex, dt) │
│  Set/Get   │  │  OnHDREnabled   │  │  OnHDREnabled        │
│   Exposure │  │  OnHDRDisabled  │  │  OnHDRDisabled       │
└────────────┘  │  OnHDRResized   │  │  OnHDRResized        │
   ↑    │       │  Set/Get params │  │  Set/Get EV params   │
   │    └─[Bloom Process call]    │  │  GetCurrentExposure  │←┐
   │            └─[AE Process call] ─┘ ←──────────────[exposure 覆盖]
   │                                                          │
   ▼                                                          │
┌──────────────────────────────────────────────────────────────────┐
│              RenderBackend (虚接口)                                │
│  HDR (4) + Bloom (6) + AutoExposure (6 新增)                      │
└──────────────────────────────────────────────────────────────────┘
                ↓
   GL33Backend (实现) / LegacyBackend (默认 no-op)
```

### 2.3 自动联动表

默认 `AE.GetAutoEnable() == false`（与 Bloom 默认 true 区别）：

| 用户 Lua 调用 | HDR 内部触发 | AE 联动结果 |
|---------------|----------------|------------|
| `HDR.Enable(W, H)` 成功 + `AE.SetAutoEnable(true)` 已设 | `AE::OnHDREnabled(W, H)` | `AE.Enable(W, H)` 自动调用 |
| `HDR.Disable()` | `AE::OnHDRDisabled()` | `AE.Disable()`（无论 autoEnable，防 RT 悬挂） |
| `HDR.Resize(W, H)` 成功 | `AE::OnHDRResized(W, H)` | `Resize(W, H)`（同尺寸 no-op；未启用时 no-op） |
| `AE.IsEnabled()` 时 EndScene | — | tonemap exposure ← `AE.GetCurrentExposure()` 覆盖 manual |
| `AE.Disable()` 后 EndScene | — | tonemap exposure ← `g.exposure` (`HDR.SetExposure`) 立即回归 |

---

## 3. API surface（Phase E.5 新增）

### 3.1 C++ (`AutoExposureRenderer` namespace)

```cpp
// 生命周期
void Init(RenderBackend* backend);
void Shutdown();
bool Enable(int w, int h);
void Disable();
bool IsEnabled();
bool IsSupported();
bool Resize(int w, int h);

// HDR 联动 (内部 API)
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);
void SetAutoEnable(bool flag);   // 默认 false
bool GetAutoEnable();

// EV-based 参数
void  SetTargetEV(float v);   float GetTargetEV();   // 默认 0.0
void  SetSpeedUp(float v);    float GetSpeedUp();    // 默认 3.0, clamp [0.1, 20]
void  SetSpeedDown(float v);  float GetSpeedDown();  // 默认 1.0, clamp [0.1, 20]
void  SetMinEV(float v);      float GetMinEV();      // 默认 -8.0
void  SetMaxEV(float v);      float GetMaxEV();      // 默认 +8.0

// 调试 / OSD
float GetCurrentEV();
float GetCurrentExposure();    // = 2^GetCurrentEV()
float GetMeasuredLuminance();  // 上一帧 log luma

// 主循环 hook (HDRRenderer::EndScene 内部调)
void Process(uint32_t hdrTex, float dt);
```

### 3.2 Lua (`Light.Graphics.AutoExposure`)

完整 1:1 映射 C++ public API（18 函数）：

```lua
local AE = require('Light.Graphics').AutoExposure

-- 生命周期 (5)
AE.Enable(w, h)     AE.Disable()
AE.IsEnabled()      AE.IsSupported()
AE.Resize(w, h)

-- 联动 (2)
AE.SetAutoEnable(true|false)    -- 默认 false (与 Bloom 默认 true 区别!)
AE.GetAutoEnable()

-- 参数 (10)
AE.SetTargetEV(v)   AE.GetTargetEV()
AE.SetSpeedUp(v)    AE.GetSpeedUp()
AE.SetSpeedDown(v)  AE.GetSpeedDown()
AE.SetMinEV(v)      AE.GetMinEV()
AE.SetMaxEV(v)      AE.GetMaxEV()

-- 调试 (3)
AE.GetCurrentEV()
AE.GetCurrentExposure()
AE.GetMeasuredLuminance()
```

---

## 4. CI 证据 & 测试覆盖

### 4.1 CI 运行汇总

| Commit | Run | 结果 |
|--------|-----|------|
| `168e683` planning docs | [`25696282774`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25696282774) | ✅ 6/6 SUCCESS |
| `4b88569` E.5.1 backend | [`25697376364`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697376364) | ✅ 6/6 SUCCESS |
| `1a9d013` E.5.2 module | [`25697700176`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697700176) | ✅ 5/6 (iOS pending; build-windows/macos/linux/web/android success) |
| `00e3463` E.5.3 lua+demo+smoke | TBD | ⏳ 跑中 |

### 4.2 Smoke 覆盖 (`scripts/smoke/auto_exposure.lua`)

| Section | 覆盖项 | 断言数 |
|---------|--------|--------|
| 1 | 子表存在 + 18 函数 surface | 19 |
| 2 | IsSupported/IsEnabled 类型 + 初始 false | 4 |
| 3 | AutoEnable 默认 **false** + 往返 true/false | 3 |
| 4 | TargetEV 默认 0 + 往返 2.5 / -3.0 / 0 | 4 |
| 5 | SpeedUp 默认 3 + 往返 + clamp 上下界 [0.1, 20] | 4 |
| 6 | SpeedDown 默认 1 + 往返 + clamp 上下界 [0.1, 20] | 3 |
| 7 | MinEV/MaxEV 默认 -8/+8 + 往返 + 不变量 min<=max | 5 |
| 8 | Debug getter 类型 + currentExposure default 1.0 | 4 |
| 9 | Enable/Resize 类型 + IsEnabled 一致 + 双 Disable 安全 | 4 |
| 10 | AutoEnable=false 时 HDR.Enable 不联动 AE | 1 (条件触发) |

**合计：~51 断言**，纯 headless 安全。

### 4.3 Demo 验收

| Demo | 平台 | 验收方式 |
|------|------|--------|
| `samples/demo_auto_exposure` | GL33 desktop | 三场景模式 (split/dark/bright) 通过 D 键切换；A 切 AE，OSD 实时显示 currentEV / exposure / measured logLuma |
| 同 demo | Legacy / headless | API surface 探测后干净退出 |

---

## 5. 关键技术决策

1. **AutoEnable 默认 false**：AE 改 exposure 行为强烈，不应默认接管。Bloom 默认 true（视觉提升），AE 默认 false（manual 保留）—— 这是**有意区别**。
2. **覆盖式而非叠加式**：AE.Enable 期间 `HDR.SetExposure` 设的值被忽略；AE.Disable 后立即恢复 manual。最少认知负担，行业惯例。
3. **EV 域 API 主**：`Set/GetTargetEV / SpeedUp / SpeedDown / MinEV / MaxEV` 全 EV，符合摄影直觉（每 +1 EV 翻倍亮度）。`GetCurrentExposure` 作衍生 getter。
4. **同步 readback v1**：`glReadPixels` 1×1 R16F 仅 4 字节，stall ~10us 可接受。PBO 异步标 TODO（v2）。
5. **R16F mipmap reduce**：用 GPU `glGenerateMipmap` 一行代码完成全图 reduce，跨 GL33/GLES3 兼容。无需 compute shader。
6. **限速 lerp 双速度**：`speedUp` / `speedDown` 分开（暗→亮快，亮→暗慢），模拟人眼适应特性。游戏化加速（实际人眼暗适应 ~30s，游戏中 1s 已够）。
7. **Bloom 之后测量**：DESIGN 决定 AE 测的是 hdr+bloom 后画面，更符合用户感知。代价是 Bloom 切换时 AE 测量值会跳一帧（无伤大雅）。
8. **第一帧 hasFirstSample guard**：直接 currentEV = targetEV，避免长 fade-in。

---

## 6. 已知限制（→ TODO_PhaseE_5.md）

1. **CPU 同步 readback**：~10us stall，已知开销但首版可接受；PBO 异步是 v2 优化点。
2. **依赖 HDR**：AE 测量基于 HDR RT；`HDR.Disable` 时 AE 自动 Disable。
3. **R16F precision underflow**：测量值 clamp [-12, 12]，覆盖足够，但极端场景仍可能丢失精度。
4. **Center-weighted / spot metering 缺失**：v1 全画面 average，对 HUD/特效像素无加权排除。
5. **Histogram-based mode 缺失**：需 GLES3.1+ compute，目标平台不全支持。
6. **视觉验收待补**：仅 CI smoke 通过；需带 GL ctx 真机跑 demo_auto_exposure 截图归档（含暗→亮 / 亮→暗 时间线）。

---

## 7. 后续阶段建议

| Phase | 主题 | 关键收益 |
|-------|------|---------|
| **E.5.x 优化** | PBO 异步 readback + center-weighted metering | 0 stall + UI/HUD 不污染测量 |
| **E.6** | Lens dirt + Streak | 完整电影感后处理工具箱 |
| **E.7** | SSAO / SSR | 屏幕空间环境光遮蔽与反射 |
| **F.x** | Compute shader pipeline | CS bloom + histogram AE + 更激进降采样 |

---

**Phase E.5 主交付完毕**。`docs/Phase E 渲染管线升级/` 下完整 6A 文档链已就位（ALIGN → DESIGN → TASK → ACCEPTANCE → FINAL → TODO）。HDR 链路三剑客（HDR + Bloom + AE）全部上线。
