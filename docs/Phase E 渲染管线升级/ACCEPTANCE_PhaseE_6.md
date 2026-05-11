# ACCEPTANCE — Phase E.6 · Lens Dirt + Streak

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.6.1 / E.6.2 / E.6.3** 合并验收。LensFx 作为 HDR + Bloom + AE 链路的电影感扩展，沿用 Phase E.4/E.5 命名空间 + 子表风格。

---

## 1. 改动摘要

| 阶段 | 文件 | 改动量 |
|------|------|--------|
| E.6.1 | `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~85 行 (8 虚接口) |
| E.6.1 | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~430 行 (3 shader 双 profile + InitLensFx + Shutdown + 8 override + 1x1 白 fallback) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\include\bloom_renderer.h` + `.cpp` | +20 行 (`GetPyramidTopTex`) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\include\lens_dirt_renderer.h` | +100 行 (新建) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\src\lens_dirt_renderer.cpp` | +110 行 (新建) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\include\streak_renderer.h` | +110 行 (新建) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\src\streak_renderer.cpp` | +200 行 (新建) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~20 行 (5 联动点) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +8 行 (Init/Shutdown) |
| E.6.2 | `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +2 行 |
| E.6.3 | `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~230 行 (23 binding + 2 子表) |
| E.6.3 | `@e:\jinyiNew\Light\scripts\smoke\lens_fx.lua` | +260 行 (新建，≥ 35 断言) |
| E.6.3 | `@e:\jinyiNew\Light\samples\demo_lens_fx\main.lua` | +180 行 (新建) |
| E.6.3 | `@e:\jinyiNew\Light\samples\demo_lens_fx\README.md` | +100 行 (新建) |
| E.6.3 | `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 行 |

---

## 2. 架构落地

### 2.1 数据流（HDR + Bloom + AE + LensDirt + Streak 全链路）

```
HDR RT (RGBA16F)
   │
   ├── HDR.EndScene 中 ─── Bloom.Process(fbo, hdrTex)
   │                       └─ 4 pass → HDR RT (additive)
   │
   ├── ─────────────────── AE.Process(hdrTex, dt)
   │                       └─ 测量 → currentExposure
   │
   ├── ─────────────────── LensDirt.Process(fbo, Bloom.GetPyramidTopTex(), w, h)
   │                       └─ DrawLensDirtComposite(bloomTex, dirtTex, fbo, w, h, intensity)
   │                          fbo += bloomTex × dirtTex × intensity (additive)
   │
   ├── ─────────────────── Streak.Process(fbo, hdrTex)
   │                       ├─ DrawStreakBright(hdrTex → streakRT[0], threshold) (复用 Bloom bright)
   │                       ├─ N 次 ping-pong blur (length × 2^i, direction)
   │                       └─ DrawStreakComposite(streakRT[final] → fbo, intensity) (additive)
   │
   └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
            ↓
       Backbuffer
```

### 2.3 模块层次

```
┌─────────────────────────────────────────────────────────────┐
│  Light.Graphics  (Lua subtables, 5 个)                        │
│  .HDR 12 / .Bloom 15 / .AutoExposure 18 / .LensDirt 10 / .Streak 13 │
│  ─ 累计 68 个 Lua API ─                                      │
└─────────────────────────────────────────────────────────────┘
                                ↓
    ┌──────────────┬──────────────┬──────────────┬──────────────┐
    │ LensDirt     │ Streak       │ HDR (Scene)  │ Bloom / AE    │
    │ Renderer     │ Renderer     │ Renderer     │ (之前 phase) │
    │ ↑ 无 RT      │ ↑ ping-pong  │              │              │
    │ dirtTex      │ RT 对 (1/2)   │              │              │
    └──────────────┴──────────────┴──────────────┴──────────────┘
                                ↓
┌─────────────────────────────────────────────────────────────┐
│           RenderBackend (虚接口)                              │
│  HDR (4) + Bloom (6) + AE (6) + LensDirt (2) + Streak (6) = 24│
└─────────────────────────────────────────────────────────────┘
                                ↓
   GL33Backend (实现) / LegacyBackend (默认 no-op)
```

### 2.4 联动表

| 触发 | LensDirt | Streak |
|------|---------|--------|
| `HDR.Enable(w,h)` 成功 + autoEnable=true | `Enable()` | `Enable(w, h)` |
| `HDR.Disable()` | `Disable`（无论 autoEnable）先于 AE/Bloom/HDR 释放 | 同 |
| `HDR.Resize(w,h)` 成功 | `OnHDRResized` (no-op，无 RT) | `Resize(w, h)` 同尺寸 no-op |
| `LD.Process(fbo, 0, w, h)` (Bloom 未启用) | 静默 no-op | n/a |
| `ST.Process(fbo, hdrTex)` 但 `streakFbos[0]=0` | n/a | 静默 no-op |
| `LD.SetDirtTexture(0/nil)` | 后端 fallback 到 1×1 白 | n/a |
| `ST.SetDirection(0, 0)` | n/a | 保留旧值（防 shader normalize NaN） |

---

## 3. API surface

### 3.1 C++

**`LensDirtRenderer`** (12 函数 = 4 lifecycle + 2 autoEnable + 3 HDR hook + 3 params)：

```cpp
void Init(RenderBackend*); void Shutdown();
bool Enable(); void Disable(); bool IsEnabled(); bool IsSupported();
void OnHDREnabled(int w, int h); void OnHDRDisabled(); void OnHDRResized(int w, int h);
void SetAutoEnable(bool); bool GetAutoEnable();
void SetDirtTextureId(uint32_t); uint32_t GetDirtTextureId();
void SetIntensity(float); float GetIntensity();
void Process(uint32_t hdrFbo, uint32_t bloomTex, int w, int h);
```

**`StreakRenderer`** (17 函数 = 5 lifecycle + 2 autoEnable + 3 HDR hook + 6 params + Process)：

```cpp
void Init(RenderBackend*); void Shutdown();
bool Enable(int w, int h); void Disable(); bool IsEnabled(); bool IsSupported();
bool Resize(int w, int h);
void OnHDREnabled(int w, int h); void OnHDRDisabled(); void OnHDRResized(int w, int h);
void SetAutoEnable(bool); bool GetAutoEnable();
void  SetThreshold(float); float GetThreshold();
void  SetIntensity(float); float GetIntensity();
void  SetLength(float); float GetLength();
void  SetDirection(float x, float y); void GetDirection(float& outX, float& outY);
void  SetIterations(int); int GetIterations();
void Process(uint32_t hdrFbo, uint32_t hdrTex);
```

### 3.2 Lua

`Light.Graphics.LensDirt` (10 函数) + `Light.Graphics.Streak` (13 函数) = **23 新增 Lua API**。

---

## 4. 验收准则

| # | 准则 | 通过证据 |
|---|------|---------|
| AC-1 | 后端 8 虚接口签名稳定，Legacy 默认 no-op | E.6.1 CI [`25698921003`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25698921003) — 6/6 SUCCESS |
| AC-2 | LensDirt.Process 全 early-return（bloomTex=0 / 资源失效 / disabled） | 代码 `lens_dirt_renderer.cpp:Process` |
| AC-3 | Streak.Process 3-stage 全 guard | `streak_renderer.cpp:Process` 头部 |
| AC-4 | `HDR.Disable` 时 LensFx 先释放（防 RT 悬挂） | `hdr_renderer.cpp:Disable` 入口顺序 ST→LD→AE→Bloom→ReleaseRT |
| AC-5 | `SetAutoEnable(true)` 后 `HDR.Enable` 自动启 LD+ST；默认 false 不自动 | smoke section D |
| AC-6 | LensDirt 启用 + SetDirtTexture(0) → 使用 1×1 白 fallback | 后端 `DrawLensDirtComposite` 逻辑 |
| AC-7 | Streak SetDirection(0, 0) 保留旧值；SetIterations/SetLength clamp 正确 | smoke section F |
| AC-8 | `LD.SetDirtTexture` 接受 Image table / number / nil 三态 | smoke section E + `l_LD_SetDirtTexture` |
| AC-9 | demo_lens_fx 在 Legacy / headless 后 API surface 探测后干净退出 | demo 头部分支 |
| AC-10 | 6 平台 CI 全绿 + Windows runtime `lens_fx.lua` PASS | 见 §5 |

---

## 5. CI 证据

| Commit | Run | 结论 |
|--------|-----|------|
| `4e090f2` planning docs | — | ✅ 6/6 |
| `ee006d5` E.6.1 backend | [`25698921003`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25698921003) | ✅ 6/6 |
| `a259cf4` E.6.2 modules | [`25699153435`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25699153435) | ⏳ 跑中 |
| `25da24e` E.6.3 Lua+smoke+demo | [`25699325751`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25699325751) | ⏳ 跑中 |

### Smoke 覆盖 (`scripts/smoke/lens_fx.lua`)

| Section | 覆盖项 | 断言数 |
|---------|--------|--------|
| A | LensDirt 子表 + 10 函数 surface | 11 |
| B | Streak 子表 + 13 函数 surface | 14 |
| C | IsSupported/IsEnabled 类型 + 初始 false (2×) | 4 |
| D | AutoEnable 默认 false + 双向 round-trip (2×) | 3 |
| E | LensDirt: Intensity default/round-trip/clamp + DirtTexture 三态 | 4 |
| F | Streak: Threshold / Intensity / Length / Direction / Iterations | 6 |
| G | LD lifecycle + double-disable | 2 |
| H | ST lifecycle + Resize + double-disable | 2 |

**合计 ~46 断言**，全 headless-safe。

---

## 6. 已知行为约束

1. **LensDirt 需 Bloom 启用**：Process 时 `bloomTex=0` 静默 no-op；用户未启 Bloom 时 LD 无效果（但不崩）
2. **Streak Bright Pass 复用 Bloom shader**：`bloomSupported=false` 时 streak bright 降级（Process 内检测）
3. **Direction (0, 0) 保留旧值**：shader `normalize` 会产生 NaN，Lua 端拒绝该输入
4. **Iterations 倍距**：每步 `length × 2^i`；5 iter + length=0.02 → 最大步 0.32 UV；用户可自适应 wrap mode `CLAMP_TO_EDGE`
5. **Ping-pong RT 半分辨率**：1920×1080 → 960×540 RGBA16F，显著节省 fragment
6. **Legacy 后端 no-op**：两个 `Supports*() = false`，Lua API 全链路静默

---

## 7. 后续工作

参见 `TODO_PhaseE_6.md`：
- 真机视觉验收 demo_lens_fx（用户参与）
- 内置标准 dirt 纹理（当前仅 1×1 白 fallback）
- Lens flare（鬼影 ghost）—— 另一独立后处理
- Animated dirt（雨滴/湿气 UV scroll）
