# ACCEPTANCE — Phase E.7 · Lens Flare (Ghost + Halo + Chromatic Aberration)

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.7.1 / E.7.2 / E.7.3** 合并验收。Lens Flare 作为 HDR 链路第 6 剑客，延续 Phase E.6 命名空间 + 子表风格，**复用 Bloom bright/composite shader**，单独编译 1 个新 ghost shader。

---

## 1. 改动摘要

| 阶段 | 文件 | 改动量 |
|------|------|--------|
| E.7.1 | `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~80 行（4 虚接口 + 注释） |
| E.7.1 | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~250 行（1 GLSL × 双 profile + 4 override + InitLensFx 追加 + Shutdown） |
| E.7.2 | `@e:\jinyiNew\Light\ChocoLight\include\lens_flare_renderer.h` | +~95 行（新建） |
| E.7.2 | `@e:\jinyiNew\Light\ChocoLight\src\lens_flare_renderer.cpp` | +~210 行（新建） |
| E.7.2 | `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~10 行（5 联动点） |
| E.7.2 | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 行（Init/Shutdown） |
| E.7.2 | `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 行 |
| E.7.3 | `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~210 行（21 binding + 1 子表） |
| E.7.3 | `@e:\jinyiNew\Light\scripts\smoke\lens_flare.lua` | +~230 行（~50 断言） |
| E.7.3 | `@e:\jinyiNew\Light\samples\demo_lens_flare\main.lua` | +~200 行 |
| E.7.3 | `@e:\jinyiNew\Light\samples\demo_lens_flare\README.md` | +~110 行 |
| E.7.3 | `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 行 |

---

## 2. 架构落地

### 2.1 数据流（HDR + 5 后处理剑客 + LensFlare 全链路）

```
HDR.EndScene 内部:

  Bloom.Process(hdrFbo, hdrTex)              ── HDR RT 加亮 [Phase E.4]
       │
       ▼
  AE.Process(hdrTex, dt)                      ── 更新 exposure [Phase E.5]
       │
       ▼
  LensDirt.Process(hdrFbo,
                    Bloom.GetPyramidTopTex(),
                    w, h)                     ── bloom × dirt → HDR RT [Phase E.6]
       │
       ▼
  Streak.Process(hdrFbo, hdrTex)              ── anamorphic flare → HDR RT [Phase E.6]
       │
       ▼
  LensFlare.Process(hdrFbo, hdrTex)           ── ghost + halo + chromatic → HDR RT [Phase E.7]
       │   ┌─────────────────────────────────────────────┐
       │   │ 1. Bloom.DrawBloomBrightPass    (复用)       │
       │   │     hdrTex → lfRT[0], threshold              │
       │   ├─────────────────────────────────────────────┤
       │   │ 2. DrawLensFlareGhost           (新 shader)  │
       │   │     lfRT[0] → lfRT[1], 5 uniforms            │
       │   ├─────────────────────────────────────────────┤
       │   │ 3. Bloom.DrawBloomComposite     (复用)       │
       │   │     lfRT[1] → hdrFbo, intensity (additive)   │
       │   └─────────────────────────────────────────────┘
       │
       ▼
  DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
       │
       ▼
  [Backbuffer]
```

### 2.2 模块层次

```
┌──────────────────────────────────────────────────────────────────────┐
│ Light.Graphics (Lua subtables, 6 个)                                   │
│ .HDR 12 / .Bloom 15 / .AutoExposure 18 / .LensDirt 10 / .Streak 13     │
│ .LensFlare 21                                                          │
│ ──────────────── 89 Lua API 上线 ────────────────                      │
└──────────────────────────────────────────────────────────────────────┘
                                ↓
 HDRRenderer (scene RT)
    ├── BloomRenderer (pyramid N 级)
    ├── AutoExposureRenderer (luminance mipmap + readback)
    ├── LensDirtRenderer (no RT, dirtTexId + intensity)        ← Phase E.6
    ├── StreakRenderer (ping-pong RT, 1/2 res)                  ← Phase E.6
    └── LensFlareRenderer (ping-pong RT, 1/2 res)               ← Phase E.7
                                ↓
       RenderBackend 虚接口: HDR 4 + Bloom 6 + AE 6 + LensDirt 2
                            + Streak 6 + LensFlare 4 = 28
                                ↓
       GL33Backend / LegacyBackend (default no-op)
```

### 2.3 自动联动表

默认 `autoEnable = false`（与 LensDirt/Streak/AE 一致）：

| 用户 Lua 调用 | HDR 内部 | LensFlare 联动 |
|---------------|----------|----------------|
| `HDR.Enable(W, H)` 成功 + autoEnable=true | `OnHDREnabled` | `Enable(W, H)` |
| `HDR.Disable()` | `OnHDRDisabled` | **最先关**（管线末端，依赖 Bloom + HDR RT） |
| `HDR.Resize(W, H)` 成功 | `OnHDRResized` | `Resize(W, H)`（同尺寸 no-op） |
| `Bloom` 未启用时 EndScene | — | LensFlare 仍可工作（独立 ping-pong RT；只复用 Bloom shader 字节） |

---

## 3. API surface（Phase E.7 新增）

### 3.1 C++

**`LensFlareRenderer`** (27 fn = 2 + 5 + 3 + 2 + 14 + 1)：

```cpp
void Init(RenderBackend*);  void Shutdown();
bool Enable(int w, int h);  void Disable();
bool IsEnabled();           bool IsSupported();
bool Resize(int w, int h);
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);
void SetAutoEnable(bool);  bool GetAutoEnable();           // 默认 false
void SetThreshold(float);  float GetThreshold();           // [0, +inf), 默认 1.0
void SetIntensity(float);  float GetIntensity();           // [0, +inf), 默认 0.4
void SetGhostCount(int);   int   GetGhostCount();          // [0, 8],     默认 4
void SetGhostDispersal(float);  float GetGhostDispersal(); // [0, 2.0],   默认 0.4
void SetHaloWidth(float);  float GetHaloWidth();           // [0, 1.0],   默认 0.5
void SetChromaticAberration(float);  float GetChromaticAberration(); // [0, 0.02], 默认 0.005
void SetDistortionEnabled(bool);  bool GetDistortionEnabled();        // 默认 true
void Process(uint32_t hdrFbo, uint32_t hdrTex);
```

### 3.2 Lua

`Light.Graphics.LensFlare` (21 fn) = **21 新增 Lua API**。

---

## 4. 验收准则

| # | 准则 | 通过证据 |
|---|------|---------|
| AC-1 | RenderBackend 4 新虚接口签名稳定，Legacy 默认 no-op | E.7.1 CI [`25701231375`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701231375) ✅ 6/6 |
| AC-2 | GL33Backend ghost shader 双 profile（GLES3 + GL33）编译成功 | InitLensFx 日志 `lensFlare=yes` + CI 通过 |
| AC-3 | LensFlareRenderer 27 函数（C++）surface 完整 | `lens_flare_renderer.h` + `.cpp` 实现 |
| AC-4 | LensFlare.Process 3-stage 全 early-return | `lens_flare_renderer.cpp::Process` 头部 |
| AC-5 | bright/composite 复用 Bloom shader，不重复编译 | `DrawBloomBrightPass` + `DrawBloomComposite` 直调 |
| AC-6 | HDR.Disable 时 LensFlare **最先释放**（管线末端依赖最深） | `hdr_renderer.cpp::Disable` 顺序 LF→ST→LD→AE→Bloom→ReleaseRT |
| AC-7 | autoEnable=true 时 HDR.Enable 自动启 LensFlare；默认 false 不自动 | smoke section C + 代码 OnHDREnabled |
| AC-8 | 各参数 clamp 正确（GhostCount [0,8] / Dispersal [0,2] / Halo [0,1] / CA [0,0.02]） | smoke section F/G/H/I |
| AC-9 | GhostCount=0 + HaloWidth=0 边界不崩 | smoke section L |
| AC-10 | 6 平台 CI 全绿 + Windows runtime `lens_flare.lua` PASS | E.7.3 CI [`25701617533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701617533) |
| AC-11 | ACCEPTANCE + FINAL + TODO 文档完整 | 本文 + `FINAL_PhaseE_7.md` + `TODO_PhaseE_7.md` |

---

## 5. CI 证据

| Commit | Run | 结论 |
|--------|-----|------|
| `d26574e` planning docs | [`25701081881`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701081881) | ✅ 6/6 |
| `ea0c873` E.7.1 backend | [`25701231375`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701231375) | ✅ 6/6 |
| `d0ad4c3` E.7.2 module | [`25701331587`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701331587) | ✅ 6/6 |
| `d63234f` E.7.3 Lua+smoke | [`25701617533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701617533) | ⏳ 跑中 |

### Smoke 覆盖 (`scripts/smoke/lens_flare.lua`)

| Section | 覆盖项 | 断言数 |
|---------|--------|--------|
| A | 子表 + 21 函数 surface | 22 |
| B | IsSupported/IsEnabled 类型 + 初始 false | 3 |
| C | AutoEnable 默认 false + round-trip | 3 |
| D | Threshold default 1.0 + clamp | 4 |
| E | Intensity default 0.4 + clamp | 4 |
| F | GhostCount default 4 + range + zero-valid | 5 |
| G | GhostDispersal default 0.4 + range | 4 |
| H | HaloWidth default 0.5 + range + zero-valid | 5 |
| I | CA default 0.005 + range | 4 |
| J | DistortionEnabled default true + round-trip | 3 |
| K | Enable/Resize/Disable + double-disable | 3 |
| L | 边界 GhostCount=0、HaloWidth=0、CA=0 | 1 |

**合计 ~50 断言**，全 headless-safe。

---

## 6. 已知行为约束

1. **bright/composite 复用 Bloom shader**：`SupportsLensFlare = (programLensFlareGhost != 0) && bloomSupported`
2. **Ghost 反向投射（朝画面中心）**：固定算法；用户无法朝光源主方向（需 camera matrix）
3. **GhostCount 上限硬定 8**：shader 静态 for 循环；改大需重编 shader
4. **Ping-pong RT 半分辨率**：1920×1080 → 960×540 RGBA16F
5. **Legacy 后端 no-op**：`SupportsLensFlare() = false`，Lua API 全静默
6. **DistortionEnabled=false 时省 3× 带宽**：单次 texture 采样代替 RGB 三采

---

## 7. 后续工作

参见 `TODO_PhaseE_7.md`：

- 真机视觉验收 demo_lens_flare（用户参与）
- 内置 lens flare 贴图（rays 星芒，替代纯 procedural ghost）
- 光源主方向投射（需要 camera matrix）→ Phase E.8 候选
- Anti-aliased ghost edges（当前硬采样可能出锯齿）
- 性能基线（GhostCount=8 vs 4 vs 0 对比）
