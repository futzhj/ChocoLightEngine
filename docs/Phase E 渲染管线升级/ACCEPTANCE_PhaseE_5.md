# ACCEPTANCE — Phase E.5 · Auto Exposure (Eye Adaptation)

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.5.1 / E.5.2 / E.5.3** 合并验收。AE 作为 HDR 链路上的"自动相机"，沿用 Phase E.4 Bloom 的命名空间 + 子表风格。

---

## 1. 改动摘要

| 阶段 | 文件 | 改动量 | 类型 |
|------|------|--------|------|
| E.5.1 | `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~70 行 | 修改：6 个 AE 虚接口（默认 no-op，Legacy 后端兼容） |
| E.5.1 | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~230 行 | 修改：1 套 luma extract shader × 2 GL profile + `InitAutoExposure` + 6 override + Shutdown 释放 |
| E.5.2 | `@e:\jinyiNew\Light\ChocoLight\include\auto_exposure_renderer.h` | +160 行 | 新增：18 函数 namespace 接口 |
| E.5.2 | `@e:\jinyiNew\Light\ChocoLight\src\auto_exposure_renderer.cpp` | +~250 行 | 新增：State + Init/Shutdown/Enable/Disable/Resize + auto-link + Process 4-stage |
| E.5.2 | `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~30 行 | 修改：`#include "auto_exposure_renderer.h"` + Enable/Disable/Resize/EndScene 4 联动点 + `<chrono>` dt 计算 + exposure 覆盖逻辑 |
| E.5.2 | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 行 | 修改：Window_Open 后 `AE::Init`；Window_Close 前 `AE::Shutdown` |
| E.5.2 | `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 行 | 修改：源列加 `auto_exposure_renderer.cpp` |
| E.5.3 | `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~170 行 | 修改：18 个 `l_AE_*` + `ae_funcs[]` + AutoExposure 子表挂入 `luaopen_Light_Graphics` |
| E.5.3 | `@e:\jinyiNew\Light\scripts\smoke\auto_exposure.lua` | +250 行 | 新增：API surface + 参数 clamp + lifecycle + AutoEnable=false 联动验证 |
| E.5.3 | `@e:\jinyiNew\Light\samples\demo_auto_exposure\main.lua` | +250 行 | 新增：3 场景模式 (split/dark/bright) + 7 组热键调参 + OSD |
| E.5.3 | `@e:\jinyiNew\Light\samples\demo_auto_exposure\README.md` | +100 行 | 新增：操作 / 参数 / 联动说明 |
| E.5.3 | `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 行 | 修改：注册 `phaseE5Smoke = scripts/smoke/auto_exposure.lua` |

---

## 2. 架构落地

### 2.1 数据流（HDR + Bloom + AE 全链路）

```
HDR RT (RGBA16F)
   │
   ├── HDRRenderer::EndScene 中 ──── BloomRenderer::Process(hdrFbo, hdrTex)
   │                                 │
   │                                 └─ 4 pass → 写回 HDR RT (additive)
   │
   ├── HDRRenderer::EndScene 中 ──── AutoExposureRenderer::Process(hdrTex, dt)
   │                                 │
   │                                 ├─ 1) DrawLuminanceExtract → R16F lumRT (480×270)
   │                                 ├─ 2) GenerateLuminanceMipmap → 1×1 平均
   │                                 ├─ 3) ReadbackLuminance1x1 → CPU (sync, 4 bytes)
   │                                 └─ 4) lerp(currentEV, targetEV, dt × speed)
   │
   └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
            ↑
            └── exposure = AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
                                                                 │
                                                                 └─ Backbuffer
```

### 2.2 模块层次

```
Light.Graphics.AutoExposure  (Lua, 18 fn)
        │
        ▼
AutoExposureRenderer::*      (C++ namespace, 18 public + 3 internal hooks)
        │
        ▼
RenderBackend::6 个 AE 虚接口 (Supports / Create+Delete LuminanceTarget /
                                Draw LuminanceExtract / GenerateMipmap /
                                Readback1x1)
        │
        ├─ GL33Backend::*  (R16F mipmap-able + glReadPixels GL_FLOAT 兼容路径)
        └─ LegacyBackend  (默认 no-op)
```

### 2.3 联动表

| 触发 | 行为 |
|------|------|
| `HDRRenderer::Enable(w,h)` 成功 | 若 `AE.GetAutoEnable()=true` → `AE::OnHDREnabled` → `AE::Enable(w,h)` |
| `HDRRenderer::Disable()` | `AE::OnHDRDisabled` → `AE::Disable()`（先于 Bloom/HDR RT 释放） |
| `HDRRenderer::Resize(w,h)` 成功 | `AE::OnHDRResized` → `Resize(w,h)`（同尺寸 no-op） |
| `AE.SetAutoEnable(false)` 后 `HDR.Enable` | 不再自动启 AE，需手动 `AE.Enable(w,h)` |
| `AE.IsEnabled()=true` 时 EndScene | tonemap exposure = `AE.GetCurrentExposure()` 覆盖 manual |
| `AE.Disable()` 后 EndScene | tonemap exposure = `g.exposure` (`HDR.SetExposure`) 立即回归 |

---

## 3. API surface

### 3.1 C++ (`@e:\jinyiNew\Light\ChocoLight\include\auto_exposure_renderer.h`)

```cpp
namespace AutoExposureRenderer {
    // 生命周期 (5)
    void Init(RenderBackend* backend);
    void Shutdown();
    bool Enable(int w, int h);
    void Disable();
    bool IsEnabled();
    bool IsSupported();
    bool Resize(int w, int h);

    // HDR 联动 (内部, 不暴露 Lua) (3 + 2)
    void OnHDREnabled(int w, int h);
    void OnHDRDisabled();
    void OnHDRResized(int w, int h);
    void SetAutoEnable(bool flag);   // 默认 false (与 Bloom 默认 true 区别)
    bool GetAutoEnable();

    // EV-based 参数 (10)
    void  SetTargetEV(float v);   float GetTargetEV();   // 默认 0.0
    void  SetSpeedUp(float v);    float GetSpeedUp();    // 默认 3.0, clamp [0.1, 20]
    void  SetSpeedDown(float v);  float GetSpeedDown();  // 默认 1.0, clamp [0.1, 20]
    void  SetMinEV(float v);      float GetMinEV();      // 默认 -8.0, 强制 min<=max
    void  SetMaxEV(float v);      float GetMaxEV();      // 默认 +8.0

    // 调试 / OSD getter (3)
    float GetCurrentEV();
    float GetCurrentExposure();
    float GetMeasuredLuminance();

    // 主循环 hook (HDRRenderer::EndScene 内部调) (1)
    void Process(uint32_t hdrTex, float dt);
}
```

### 3.2 Lua (`Light.Graphics.AutoExposure`)

18 个函数 = 5 lifecycle + 2 AutoEnable + 8 参数 set/get + 3 debug：

```lua
local AE = require('Light.Graphics').AutoExposure
AE.Enable(w, h) / AE.Disable() / AE.IsEnabled() / AE.IsSupported() / AE.Resize(w, h)
AE.SetAutoEnable(flag) / AE.GetAutoEnable()
AE.SetTargetEV(v) / AE.GetTargetEV()
AE.SetSpeedUp(v) / AE.GetSpeedUp()
AE.SetSpeedDown(v) / AE.GetSpeedDown()
AE.SetMinEV(v) / AE.GetMinEV()
AE.SetMaxEV(v) / AE.GetMaxEV()
AE.GetCurrentEV() / AE.GetCurrentExposure() / AE.GetMeasuredLuminance()
```

---

## 4. 验收准则

| # | 准则 | 通过证据 |
|---|------|---------|
| AC-1 | 后端 6 虚接口签名稳定，Legacy 默认 no-op 不破坏构建 | E.5.1 CI run [`25697376364`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697376364) — 6 平台 success |
| AC-2 | `AutoExposureRenderer::Process` 4 阶段在 disabled / 未初始化时一律 early-return | 代码评审 `auto_exposure_renderer.cpp:Process` 头部多重 guard |
| AC-3 | `HDR.Disable` 时 AE 先释放（防 RT 悬挂） | `hdr_renderer.cpp:Disable` 入口先调 `AE::OnHDRDisabled` 再 `Bloom::OnHDRDisabled` 再 `ReleaseRT` |
| AC-4 | `AE.SetAutoEnable(true)` 后 HDR.Enable 自动启 AE；默认 false 不自动 | smoke section 10 |
| AC-5 | EV 参数 clamp 正确：speed [0.1, 20]，minEV ≤ maxEV 强制 | smoke section 5/6/7 |
| AC-6 | Tonemap shader 接收 `2^currentEV` 作 exposure；AE 关时回归 manual SetExposure | `hdr_renderer.cpp:EndScene` 三元运算符 |
| AC-7 | Headless 下 Enable 安全返回 boolean；getter 全部返回合法默认值 | smoke section 9（含 `currentExposure default 1.0` 判定） |
| AC-8 | demo_auto_exposure 在 Legacy 后端 API surface 探测后干净退出 | demo_auto_exposure/main.lua 头部分支 |
| AC-9 | 6 平台 CI 全绿 + Windows runtime 运行 `auto_exposure.lua` 通过 | 见 §5 |

---

## 5. CI 证据

| Commit | Run | 结论 | 备注 |
|--------|-----|------|------|
| `168e683` 规划文档 | [`25696282774`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25696282774) | ✅ 6/6 | ALIGN+DESIGN+TASK |
| `4b88569` E.5.1 backend | [`25697376364`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697376364) | ✅ 6/6 | 虚接口 + luma shader + R16F mipmap RT |
| `1a9d013` E.5.2 module | [`25697700176`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25697700176) | ⏳ 4/6 + iOS/Win pending | namespace + HDR 联动 |
| `00e3463` E.5.3 Lua+smoke+demo | TBD | ⏳ 跑 | runtime smoke 验证 |

---

## 6. 已知行为约束

1. **CPU 同步 readback**：v1 实现用 `glReadPixels` 同步读，约 1 frame stall 但仅 4 字节，可接受。PBO 异步标 TODO_PhaseE_5。
2. **AE 测量含 Bloom**：DESIGN 决定 AE 在 Bloom 之后插入，这样测量值与玩家实际看到的画面一致。代价是关 Bloom 时 AE 测量值会跳变（一帧）。
3. **R16F 半精度**：log luma clamp [-12, 12]，覆盖亮度 6e-6 ~ 162754，远超任何真实场景。
4. **AutoEnable 默认 false**：与 Bloom 默认 true 不同。AE 改 exposure 行为强烈，不应默认接管；用户须显式 `SetAutoEnable(true)` 或手动 `Enable()`。
5. **第一帧无 history**：`hasFirstSample` flag 让首次测量直接设 currentEV=targetEV，避免长 fade-in。
6. **dt clamp [0, 0.1]**：防长时间挂起后 dt 巨大导致 EV 跳变。
7. **Legacy 后端 no-op**：`SupportsAutoExposure()=false`，所有 setter/getter 仍可调（State CPU-side）。

---

## 7. 后续工作（→ TODO_PhaseE_5.md）

- 视觉验收（带 GL ctx 真机跑 demo_auto_exposure）
- PBO 异步 readback v2 优化
- Center-weighted / spot metering 模式
- AutoExposure histogram-based mode (GLES 3.1+ compute)
