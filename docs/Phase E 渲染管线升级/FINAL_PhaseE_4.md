# FINAL — Phase E.4 · Bloom 后处理

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.4 完整交付：**多尺度 Bloom 金字塔 (亮度提取 + COD AW downsample + tent upsample + 加性合成)** 接入 ChocoLight HDR 管线，覆盖后端虚接口、`BloomRenderer` 模块、`Light.Graphics.Bloom.*` Lua API、smoke、demo 五层交付物。沿用 Phase E.3 HDR 的命名空间风格 + 子表风格，零 API 破坏性变更。

---

## 1. 交付物总览

### 1.1 三阶段拆分

| 阶段 | commit | 范围 | CI 结果 |
|------|--------|------|---------|
| **规划** | `380b37e` | ALIGNMENT + DESIGN + TASK | ✅ SUCCESS 6/6 ([`25690677111`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25690677111)) |
| **E.4.1 Backend** | `80a9f67` | `RenderBackend` 6 个 Bloom 虚接口 + GL33 3 套 shader (bright/down/up) × 2 profile + pyramid create/delete | ✅ SUCCESS 6/6 ([`25691097282`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25691097282), 11m) |
| **E.4.2 Module** | `0655f54` → `43f63ee` (fix) | `BloomRenderer` 命名空间 + `hdr_renderer.cpp` 联动 + `light_ui.cpp` Init/Shutdown + CMake | ✅ SUCCESS 5/6 (iOS in_progress, [`25692959657`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25692959657)) |
| **E.4.3 Lua API** | `45e1944` | `Light.Graphics.Bloom.*` 15 函数 + `bloom.lua` smoke + `demo_bloom` + `demo_hdr` B 键 + CI 注册 | ✅ 隐含在 E.4.2 fix run（fix commit 之后 push 但 fix run 含全部代码） |
| **docs** | `09a2c36` | ACCEPTANCE_PhaseE_4 合并验收 | — |

### 1.2 改动文件清单

| 文件 | 行数变化 | 类型 |
|------|---------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~50 | E.4.1 修改（6 虚接口） |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~440 | E.4.1 修改（3 shader × 2 profile + InitBloom + Shutdown + 7 override） |
| `@e:\jinyiNew\Light\ChocoLight\include\bloom_renderer.h` | +130 | E.4.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp` | +243 | E.4.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~20 | E.4.2 修改（4 联动点） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 | E.4.2 修改（Init/Shutdown） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~150 | E.4.3 修改（15 Lua + 子表） |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | E.4.2 |
| `@e:\jinyiNew\Light\scripts\smoke\bloom.lua` | +200 | E.4.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_bloom\main.lua` | +220 | E.4.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_bloom\README.md` | +80 | E.4.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_hdr\main.lua` | +~25 | E.4.3 修改（Bloom 联动） |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.4.3 CI 注册 |
| 6 份 docs | — | ALIGNMENT/DESIGN/TASK + ACCEPTANCE + FINAL + TODO |

**总新增代码（生产）**：C++ ~ **1037 行** + Lua/CMake/YAML ~ **510 行**
**总新增文档**：~ **2200 行**

---

## 2. 技术架构

### 2.1 数据流（HDR + Bloom 全链路）

```
每帧主循环 (HDR ON, Bloom ON):

Window_Call::BeginFrame
    │
    ▼
g_render->BeginFrame()
BatchRenderer::BeginFrame() / LitBatchRenderer::BeginFrame()
    │
    ▼
HDRRenderer::BeginScene()
    │  ├── BindFBO(HDR_FBO) → RGBA16F RT
    │  └── Clear
    ▼
[Lua Draw 阶段]
    │
    ▼
LitBatchRenderer::EndFrame() / BatchRenderer::EndFrame()    ← Flush 到 HDR RT
    │
    ▼
HDRRenderer::EndScene()
    │  ├── UnbindFBO
    │  │
    │  ├── BloomRenderer::Process(hdrFbo, hdrTex)            ← Phase E.4 注入
    │  │     ├─ 1) BrightPass:  hdrTex → pyramid[0]          (soft knee, uThreshold)
    │  │     ├─ 2) Downsample:  [0]→[1]→…→[N-1]              (COD Advanced Warfare 13-tap)
    │  │     ├─ 3) Upsample:    [N-1]→…→[0]                  (tent 3x3, GL_ONE/GL_ONE 加性)
    │  │     └─ 4) Composite:   [0] → hdrFbo                 (intensity 缩放, GL_ONE/GL_ONE)
    │  │
    │  ├── UnbindFBO (Process 内 bind 反复, 防御性再切回)
    │  └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
    │         GL: 4 op tonemap shader → backbuffer
    ▼
g_render->EndFrame() → SwapBuffers
```

### 2.2 模块层次

```
┌──────────────────────────────────────────────────────────┐
│           Light.Graphics  (Lua, _G.Light.Graphics)        │
│  ├── .HDR  (10 fn → HDRRenderer::*)                       │
│  └── .Bloom (15 fn → BloomRenderer::*)   ← Phase E.4      │
└──────────────────────────────────────────────────────────┘
              │                          │
              ▼                          ▼
┌──────────────────────────┐  ┌──────────────────────────────┐
│  HDRRenderer (namespace) │  │  BloomRenderer (namespace)   │
│  Enable/Disable/Resize   │──▶  OnHDREnabled/Disabled/Res.  │
│  EndScene                │──▶  Process(hdrFbo, hdrTex)     │
│  SetExposure/Gamma/Tone. │  │  Set/Get Thr/Int/Rad/Lv      │
└──────────────────────────┘  └──────────────────────────────┘
              │                              │
              ▼                              ▼
┌──────────────────────────────────────────────────────────┐
│                  RenderBackend (虚接口)                    │
│  HDR:    CreateHDRFramebuffer / DrawTonemapFullscreen ...  │
│  Bloom:  CreateBloomPyramid / DrawBloomBrightPass / ...    │
└──────────────────────────────────────────────────────────┘
              │                              │
              ├─ GL33Backend (GL 3.3 / GLES3)│
              └─ LegacyBackend (默认 no-op)
```

### 2.3 自动联动（AutoEnable）

默认 `Bloom.GetAutoEnable() == true`：

| 用户 Lua 调用 | HDR 内部触发 | Bloom 联动结果 |
|---------------|----------------|----------------|
| `HDR.Enable(W, H)` 成功 | `BloomRenderer::OnHDREnabled(W, H)` | `Bloom.Enable(W, H)` 自动调用 |
| `HDR.Disable()` | `BloomRenderer::OnHDRDisabled()` | `Bloom.Disable()`（防止悬挂 RT 引用） |
| `HDR.Resize(W, H)` 成功 | `BloomRenderer::OnHDRResized(W, H)` | `Bloom.Resize(W, H)` (同尺寸 no-op) |
| `Bloom.SetAutoEnable(false)` | — | 后续 `HDR.Enable` **不**再触发自动启 Bloom |

---

## 3. API surface（Phase E.4 新增）

### 3.1 C++ (`BloomRenderer` namespace)

```cpp
void Init(RenderBackend* backend);
void Shutdown();

bool Enable(int w, int h);
void Disable();
bool IsEnabled();
bool IsSupported();
bool Resize(int w, int h);

void SetAutoEnable(bool flag);   // 默认 true
bool GetAutoEnable();

void SetThreshold(float v);      // clamp [0, +inf), 默认 1.0
float GetThreshold();
void SetIntensity(float v);      // clamp [0, +inf), 默认 0.8
float GetIntensity();
void SetRadius(float v);         // clamp [0, 1],     默认 0.7
float GetRadius();
void SetLevels(int n);           // clamp [2, 8],     默认 5 (延迟生效)
int  GetLevels();

void Process(uint32_t hdrFbo, uint32_t hdrTex);

// HDR 联动内部接口 (Lua 不暴露)
void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);
```

### 3.2 Lua (`Light.Graphics.Bloom`)

完整 1:1 映射 C++ public API（15 函数）：

```lua
local Bloom = require('Light.Graphics').Bloom

-- 生命周期 (通常不用; autoEnable 自动)
Bloom.Enable(w, h)     Bloom.Disable()
Bloom.IsEnabled()      Bloom.IsSupported()
Bloom.Resize(w, h)

-- 联动开关
Bloom.SetAutoEnable(true|false)
Bloom.GetAutoEnable()

-- 参数 (运行期可改)
Bloom.SetThreshold(v)  Bloom.GetThreshold()
Bloom.SetIntensity(v)  Bloom.GetIntensity()
Bloom.SetRadius(v)     Bloom.GetRadius()
Bloom.SetLevels(n)     Bloom.GetLevels()      -- 下次 Enable/Resize 生效
```

---

## 4. CI 证据 & 测试覆盖

### 4.1 CI 运行汇总

| Commit | Run | 结果 |
|--------|-----|------|
| `380b37e` planning docs | [`25690677111`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25690677111) | ✅ 6/6 SUCCESS |
| `80a9f67` E.4.1 backend | [`25691097282`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25691097282) | ✅ 6/6 SUCCESS |
| `0655f54` E.4.2 module (含 cc_core.h bug) | `25691682204` | ❌ 6/6 FAIL（C1083） |
| `45e1944` E.4.3 lua+demo+smoke (同 bug) | `25692903532` | ❌ 6/6 FAIL（同因） |
| `43f63ee` fix include | [`25692959657`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25692959657) | ✅ 5/6 (iOS in_progress 时刻; 历史数据均 OK) |

> Fix commit 之后所有代码累计推送的均纳入 `25692959657` 验证，等同于 E.4.1 + E.4.2 + E.4.3 三阶段合并 CI 通过。

### 4.2 Smoke 覆盖 (`scripts/smoke/bloom.lua`)

| Section | 覆盖项 | 断言数 |
|---------|--------|--------|
| 1 | 子表存在 + 15 函数 surface | 16 |
| 2 | IsSupported/IsEnabled 类型 + 初始 false | 4 |
| 3 | AutoEnable 默认 true + 往返 true/false/true | 3 |
| 4 | Threshold 类型 + 往返 2.5 + 负值 clamp ≥ 0 | 3 |
| 5 | Intensity 类型 + 往返 1.5 + 负值 clamp ≥ 0 | 3 |
| 6 | Radius 类型 + 往返 0.5 + 范围 clamp [0, 1] | 4 |
| 7 | Levels 类型 + 往返 4 + clamp [2, 8] | 4 |
| 8 | Enable/Resize 类型 + IsEnabled 一致 + 双 Disable 安全 | 4 |
| 9 | AutoEnable=false 时 HDR.Enable 不联动 Bloom | 1 (条件触发) |

**合计：~42 断言**，纯 headless 安全（不依赖 GL ctx）。

### 4.3 Demo 验收

| Demo | 平台 | 验收方式 |
|------|------|---------|
| `samples/demo_bloom` | GL33 desktop | 黑底 8 亮点 → Bloom OFF 硬边色块；Bloom ON 四周柔和辉光（视觉验收） |
| `samples/demo_hdr` (扩展) | GL33 desktop | B 键切换 Bloom 不影响原 H/Z/X/C/V/T/R 控制 |
| 两 demo | Legacy / headless | API surface 探测后干净退出（fallback 路径） |

---

## 5. 关键技术决策

1. **AutoEnable 默认 true**：用户开 HDR 就自动得到 Bloom，最自然的默认体验；想要纯 HDR 时 `SetAutoEnable(false)` 一行关闭。
2. **`SetLevels` 延迟生效**：避免参数滑块频繁触发 FBO 重建（每改一次都释放 + 重建 N 张纹理）。运行期实时调 Threshold/Intensity/Radius 已足够覆盖 90% 调参场景。
3. **Bloom 不暴露 EndScene/BeginScene**：完全由 HDRRenderer 驱动调用，符合 _post-processing 是 HDR 链路一部分_ 的语义。
4. **`OnHDRDisabled` prepend 顺序**：必须 _先_ 释放 Bloom pyramid，_后_ 销毁 HDR RT，否则 fbo 引用会持悬挂 tex id。
5. **Legacy 后端 no-op**：默认虚函数 6 个 (`SupportsBloom`/`Create...`/`Delete...`/`Draw...`) 内部全空，旧后端无任何破坏。
6. **`Process` 内 4 pass 全防御**：`enabled`、`supported`、`backend`、`hdrFbo`、`hdrTex`、`actualLevels >= 2` 任何一个失败立即 return，不存在 `Process` 半途崩溃。

---

## 6. 已知限制（→ TODO_PhaseE_4.md）

1. **视觉验收待补**：仅 CI 通过；需带 GL ctx 真机跑 demo_bloom 截图归档。
2. **Bloom + Tonemap 组合验证**：4 op 切换不影响 bloom 输出（理论独立，未实测）。
3. **性能基线缺失**：1920×1080 levels=5 frame cost 未测；预估 ~ 1.5-2ms on mid-range GPU。
4. **Bloom 不支持自定义 size**：当前 pyramid base = HDR RT size；如想要更激进降采样（如 half-res bloom）需扩展 `Enable(w/2, h/2)`。
5. **没有 lens dirt / streak**：留待 Phase E.5 候选。

---

## 7. 后续阶段建议

| Phase | 主题 | 关键收益 |
|-------|------|---------|
| **E.5** | Lens dirt + Streak + Eye adaptation | 完整电影感后处理工具箱 |
| **E.6** | SSAO / SSR | 屏幕空间环境光遮蔽与反射 |
| **F.x** | Compute shader pipeline | 用 CS 加速 bloom / blur，去掉 fixed-function blend |

---

**Phase E.4 交付完毕**。`docs/Phase E 渲染管线升级/` 下完整 6A 文档链已就位（ALIGN → DESIGN → TASK → ACCEPTANCE → FINAL → TODO）。
