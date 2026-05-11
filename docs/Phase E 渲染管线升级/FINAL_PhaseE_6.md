# FINAL — Phase E.6 · Lens Dirt + Streak

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.6 完整交付：**电影感镜头后处理二剑客** — **Lens Dirt**（镜头脏污）+ **Streak**（anamorphic flare 横向条纹光晕）接入 HDR 链路。沿用 Phase E.4/E.5 的命名空间 + 子表风格，零 API 破坏性变更。

---

## 1. 交付物总览

### 1.1 四阶段拆分

| 阶段 | commit | 范围 | CI |
|------|--------|------|-----|
| **规划** | `4e090f2` | ALIGNMENT + DESIGN + TASK | ✅ 6/6 |
| **E.6.1 Backend** | `ee006d5` | RenderBackend 8 虚接口 + GL33 3 shader (2 profile) + ping-pong RT + 1×1 白 fallback | ✅ 6/6 ([`25698921003`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25698921003)) |
| **E.6.2 Modules** | `a259cf4` | LensDirtRenderer + StreakRenderer namespace + Bloom GetPyramidTopTex + HDR 5 联动点 + light_ui Init/Shutdown + CMake | ⏳ 4/6 + (Win/iOS) |
| **E.6.3 Lua API** | `25da24e` | 2 子表 23 函数 + `lens_fx.lua` smoke (~46 断言) + demo_lens_fx + CI 注册 | ⏳ 跑中 |
| **docs** | `pending` | ACCEPTANCE + FINAL + TODO | — |

### 1.2 改动文件清单

| 文件 | 行数变化 | 类型 |
|------|---------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~85 | E.6.1 修改（8 虚接口） |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~430 | E.6.1 修改（3 shader + InitLensFx + 8 override） |
| `@e:\jinyiNew\Light\ChocoLight\include\bloom_renderer.h` / `.cpp` | +20 | E.6.2 修改（GetPyramidTopTex 新增） |
| `@e:\jinyiNew\Light\ChocoLight\include\lens_dirt_renderer.h` | +100 | E.6.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\lens_dirt_renderer.cpp` | +110 | E.6.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\include\streak_renderer.h` | +110 | E.6.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\streak_renderer.cpp` | +200 | E.6.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~20 | E.6.2 修改（5 联动点） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +8 | E.6.2 修改 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~230 | E.6.3 修改（23 binding + 2 子表） |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +2 | E.6.2 |
| `@e:\jinyiNew\Light\scripts\smoke\lens_fx.lua` | +260 | E.6.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_lens_fx\main.lua` | +180 | E.6.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_lens_fx\README.md` | +100 | E.6.3 新建 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.6.3 CI 注册 |
| 6 份 docs | — | — |

**总新增代码（生产）**：C++ ~ **1285 行** + Lua/CMake/YAML ~ **700 行**
**总新增文档**：~ **2400 行**

---

## 2. 技术架构

### 2.1 数据流（HDR 链路 5 剑客全链路）

```
每帧主循环 (HDR + Bloom + AE + LensDirt + Streak 全开):

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
[Lua Draw — 写入 HDR RT]
    │
    ▼
LitBatchRenderer / BatchRenderer ::EndFrame (写回 HDR RT)
    │
    ▼
HDRRenderer::EndScene()
    │  ├── UnbindFBO
    │  │
    │  ├── BloomRenderer::Process(hdrFbo, hdrTex)              [E.4]
    │  │     └─ 4 pass (bright/down/up/composite) → HDR RT additive
    │  │
    │  ├── UnbindFBO
    │  │
    │  ├── AutoExposureRenderer::Process(hdrTex, dt)            [E.5]
    │  │     └─ 测量 log-luma → currentExposure (time-smoothed)
    │  │
    │  ├── LensDirtRenderer::Process(hdrFbo, Bloom.PyramidTop, w, h)  [E.6]
    │  │     └─ DrawLensDirtComposite additive
    │  │        hdr += bloom × dirt × intensity
    │  │
    │  ├── StreakRenderer::Process(hdrFbo, hdrTex)              [E.6]
    │  │     ├─ DrawStreakBright(hdrTex → streakRT[0]) (复用 Bloom bright)
    │  │     ├─ N iter ping-pong blur: length × 2^i, direction
    │  │     └─ DrawStreakComposite(streakRT[final] → hdrFbo) additive
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
│         Light.Graphics  (Lua subtables, 5 个)                      │
│  .HDR 12 / .Bloom 15 / .AutoExposure 18 / .LensDirt 10 / .Streak 13│
│  ──────────────────────── 68 Lua API ─────────────────────────────│
└──────────────────────────────────────────────────────────────────┘
                              ↓
 HDRRenderer (scene RT)
    ├── BloomRenderer (pyramid N 级)
    ├── AutoExposureRenderer (luminance mipmap + readback)
    ├── LensDirtRenderer (no RT, dirtTexId + intensity)   ← Phase E.6
    └── StreakRenderer (ping-pong RT 对, 1/2 res)          ← Phase E.6
                              ↓
       RenderBackend 虚接口: HDR 4 + Bloom 6 + AE 6 + LensDirt 2 + Streak 6 = 24
                              ↓
       GL33Backend / LegacyBackend (default no-op)
```

### 2.3 自动联动表

默认 `autoEnable = false`（LensDirt + Streak 同 AE；Bloom 例外为 true）：

| 用户 Lua 调用 | HDR 内部触发 | LD / ST 联动 |
|---------------|--------------|-------------|
| `HDR.Enable(W, H)` 成功 + autoEnable=true | `OnHDREnabled` | `LD.Enable()` / `ST.Enable(W, H)` |
| `HDR.Disable()` | `OnHDRDisabled` | 先关 `ST` → `LD` → `AE` → `Bloom` 再释放 HDR RT |
| `HDR.Resize(W, H)` 成功 | `OnHDRResized` | `ST.Resize`（同尺寸 no-op）；`LD` 无 RT 则纯 no-op |
| `LD.SetDirtTexture(Image)` | — | Lua 端 `:GetTextureId()` → C++ 存 uint32_t |
| `LD.SetDirtTexture(nil / 0)` | — | 后端 Draw 时 fallback 到 1×1 白纹理 |
| `ST.SetDirection(0, 0)` | — | 拒绝（防 shader NaN），保留旧值 |
| `Bloom` 未启用时 EndScene | — | `LD.Process` 的 `bloomTex=0` → silent no-op |

---

## 3. API surface（Phase E.6 新增）

### 3.1 C++

**`LensDirtRenderer`** (12 fn)：

```cpp
void Init(RenderBackend*);  void Shutdown();
bool Enable();              void Disable();
bool IsEnabled();           bool IsSupported();
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);
void SetAutoEnable(bool flag);  bool GetAutoEnable();    // 默认 false
void SetDirtTextureId(uint32_t);  uint32_t GetDirtTextureId();   // 0=fallback
void SetIntensity(float);  float GetIntensity();         // 默认 0.4
void Process(uint32_t hdrFbo, uint32_t bloomTex, int w, int h);
```

**`StreakRenderer`** (17 fn)：

```cpp
void Init(RenderBackend*);  void Shutdown();
bool Enable(int w, int h);  void Disable();
bool IsEnabled();           bool IsSupported();
bool Resize(int w, int h);
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);
void SetAutoEnable(bool flag);  bool GetAutoEnable();    // 默认 false
void SetThreshold(float);    float GetThreshold();       // [0, +inf), 默认 1.0
void SetIntensity(float);    float GetIntensity();       // [0, +inf), 默认 0.3
void SetLength(float);       float GetLength();          // [0, 0.1],  默认 0.02
void SetDirection(float x, float y);
void GetDirection(float& outX, float& outY);             // 默认 (1, 0) 水平
void SetIterations(int);     int GetIterations();        // [1, 8], 默认 5
void Process(uint32_t hdrFbo, uint32_t hdrTex);
```

### 3.2 Lua

`Light.Graphics.LensDirt` (10 函数) + `Light.Graphics.Streak` (13 函数) = **23 新增 Lua API**。

LensDirt.SetDirtTexture 三态接受：
```lua
LensDirt.SetDirtTexture(img)        -- Image table (会调 :GetTextureId())
LensDirt.SetDirtTexture(texId)      -- number (原生 GL tex id)
LensDirt.SetDirtTexture(nil)        -- 重置到内置 1×1 白 fallback
```

---

## 4. CI 证据

### 4.1 CI 运行汇总

| Commit | Run | 结果 |
|--------|-----|------|
| `4e090f2` planning | — | ✅ 6/6 |
| `ee006d5` E.6.1 backend | [`25698921003`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25698921003) | ✅ 6/6 |
| `a259cf4` E.6.2 modules | [`25699153435`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25699153435) | ⏳ 4/6 + Win/iOS |
| `25da24e` E.6.3 Lua+demo | [`25699325751`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25699325751) | ⏳ 跑中 |

### 4.2 Smoke 覆盖 (`scripts/smoke/lens_fx.lua`)

合并两个模块到单文件；共 ~46 断言，headless 安全。

---

## 5. 关键技术决策

1. **两个独立子表**（`LensDirt` + `Streak`）而非合并 `LensFx`：各自独立 Enable/Disable，符合 Bloom+AE 已建立的风格。
2. **AutoEnable 默认 false**：电影感特效审美强烈，不应默认接管；用户显式启用。
3. **Bright Pass 复用 Bloom shader**：`programBloomBright` 同算法（soft knee），省一个 shader。
4. **Ping-pong RT 半分辨率**：HDR × 1/2（节省 fragment）；每次 Draw 设全 uniform（无状态污染）。
5. **倍距迭代**（length × 2^i）：5 iter + length=0.02 → 最大步 0.32 UV；视觉长条纹 effect 显著。
6. **1×1 白 fallback**：LensDirt 启用但未设 dirtTex 时，乘白色 = bloom × intensity 原样；不崩。
7. **(0,0) 方向拒绝**：shader normalize 会 NaN，Lua 端提前拒绝。
8. **LensDirt 无独立 RT**：仅持 uint32_t dirtTexId；dirt 纹理生命周期由用户（Lua Image userdata）管。

---

## 6. 已知限制

1. **视觉验收待补**（真机跑 demo_lens_fx）
2. **内置 dirt 纹理缺失**：仅 1×1 白 fallback，真实使用需用户提供 PNG
3. **Lens flare（鬼影）未做**：需要光源主方向采样，留作 Phase E.x 候选
4. **Animated dirt 未做**：UV scroll / 时间动画留 v2
5. **Streak 单方向**：多方向（星芒）需用户多次调 SetDirection 并分帧混合

---

## 7. 后续阶段建议

| Phase | 主题 | 收益 |
|-------|------|------|
| **Phase E.6.x** | 默认 dirt 纹理包 + Animated dirt / Streak 多方向 | 开箱即用的电影感套件 |
| **Phase E.7** | Lens Flare（鬼影 ghost） | 加一层光源方向飞影 |
| **Phase E.8** | SSAO / SSR | 3D 场景屏幕空间效果 |
| **Phase F.x** | Compute shader pipeline | CS 加速全链路 |

---

**Phase E.6 主交付完毕**。HDR 链路累计 **5 剑客** (HDR + Bloom + AE + LensDirt + Streak)，共 **68 个 Lua API** 上线。电影感后处理工具箱雏形已成 ✨
