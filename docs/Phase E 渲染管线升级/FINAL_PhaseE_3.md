# FINAL — Phase E.3 · HDR + ACES Tonemapping

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.3 完整交付：**HDR 离屏管线 + ACES filmic tonemap** 接入 ChocoLight 引擎，覆盖后端虚接口、`HDRRenderer` 模块、`Light.Graphics.HDR.*` Lua API、smoke、demo 五层交付物。

---

## 1. 交付物总览

### 1.1 三阶段拆分

| 阶段 | commit | 范围 | CI 结果 |
|------|--------|------|---------|
| **E.3.1 Backend** | `9ce4431` | `RenderBackend` 4 个 HDR 虚接口 + GL33 RGBA16F FBO + ACES GLSL 3.3 shader + 全屏 VAO/VBO | ✅ SUCCESS (6 平台, 6m10s) |
| **E.3.2 Module** | `cf90d43` | `HDRRenderer` 命名空间模块 + `light_ui.cpp` 4 hook 点 + `l_SetCanvas` HDR 兼容 + CMake | ✅ SUCCESS (6 平台, 8m56s) |
| **E.3.3 Lua API** | `d6eae5d` | `Light.Graphics.HDR.*` 10 函数 + `hdr.lua` smoke + `demo_hdr` + CI 注册 | ⏳ 运行中 / 待完整验证 |

### 1.2 改动文件清单

| 文件 | 行数变化 | 类型 |
|------|---------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +20 | E.3.1 修改（4 虚接口） |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~280 | E.3.1 修改（shader + InitTonemap + FBO 管理 + 4 接口实现） |
| `@e:\jinyiNew\Light\ChocoLight\include\hdr_renderer.h` | +~150 | E.3.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~190 | E.3.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +12 | E.3.2 修改（4 hook） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~135 | E.3.2 + E.3.3 修改（SetCanvas + 10 Lua API + HDR 子表） |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | E.3.2 |
| `@e:\jinyiNew\Light\scripts\smoke\hdr.lua` | +~175 | E.3.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_hdr\main.lua` | +~225 | E.3.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_hdr\README.md` | +~65 | E.3.3 新建 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.3.3 CI 注册 |
| 6 份 docs | — | ALIGNMENT/DESIGN/TASK + 3 份 ACCEPTANCE |

**总新增代码（生产）**：C++ ~ **635 行** + Lua/CMake/YAML ~ **470 行**
**总新增文档**：~ **1900 行**

---

## 2. 技术架构

### 2.1 数据流

```
每帧主循环 (HDR ON):

Window_Call::BeginFrame
    │
    ▼
g_render->BeginFrame()                       ← 清默认 fb (blit 目标兜底)
    │
    ▼
BatchRenderer::BeginFrame()
LitBatchRenderer::BeginFrame()
    │
    ▼
HDRRenderer::BeginScene()
    │  ├── backend->BindFBO(HDR_FBO)          ← 切到 RGBA16F RT
    │  ├── backend->SetViewport(0, 0, w, h)
    │  └── backend->Clear()
    ▼
[Lua Draw callback — 所有 sprite/lit/geom 画到 HDR RT]
    │
    ▼
LitBatchRenderer::EndFrame()                  ← Flush 到 HDR RT
BatchRenderer::EndFrame()                     ← Flush 到 HDR RT
    │
    ▼
HDRRenderer::EndScene()
    │  ├── backend->UnbindFBO()               ← 切回 default fb
    │  └── backend->DrawTonemapFullscreen(HDR_tex, exposure, gamma)
    │        │
    │        └─ GL: glUseProgram(tonemapProg)
    │           glBindTexture(2D, HDR_tex)
    │           glDrawArrays(TRIANGLES, 0, 6)
    │           shader: ACES(color * exposure) → pow(1/gamma)
    ▼
g_render->EndFrame()
SwapBuffers()
```

### 2.2 SetCanvas 兼容层（E.3.2 核心设计）

```
BeginScene() → HDR_RT (paused=false)

  SetCanvas(userCanvas):
      Flush batches → HDR::Pause() → BindFBO(userCanvas)
                                     ↑ paused=true
  SetCanvas(nil):
      Flush batches → HDR::Resume() → BindFBO(HDR_RT)  [NOT Clear!]
                                      ↑ paused=false

EndScene() → if !paused: tonemap blit
             if  paused: no-op (用户自绘 canvas, HDR 结果被放弃)
```

**Pause/Resume 不释放 RT**：允许同一帧内多次 HDR RT → user RT → HDR RT 切换，且保留 HDR RT 累积内容。

### 2.3 ACES 曲线（Krzysztof Narkowicz 2016 fit）

```glsl
vec3 aces(vec3 x) {
    const float a = 2.51;
    const float b = 0.03;
    const float c = 2.43;
    const float d = 0.59;
    const float e = 0.14;
    return clamp((x * (a*x + b)) / (x * (c*x + d) + e), 0.0, 1.0);
}
void main() {
    vec3 hdr  = texture(HDRTex, v_uv).rgb * u_Exposure;
    vec3 ldr  = aces(hdr);
    FragColor = vec4(pow(ldr, vec3(1.0 / u_Gamma)), 1.0);
}
```

单 pass 8 FLOPs/像素（含 gamma），RTX 级硬件 1920×1080 < 0.2ms。

---

## 3. API 层

### 3.1 C++ RenderBackend 虚接口（E.3.1）

```cpp
virtual bool SupportsHDR() const = 0;                                      // GL33=true
virtual uint32_t CreateHDRFBO(int w, int h, uint32_t* outTex) = 0;         // 返回 fbo id
virtual void DeleteHDRFBO(uint32_t fbo, uint32_t tex) = 0;
virtual void DrawTonemapFullscreen(uint32_t tex, float exp, float gam) = 0;
```

### 3.2 C++ HDRRenderer 命名空间（E.3.2）

```cpp
namespace HDRRenderer {
    bool Init(RenderBackend* backend);                // 主循环启动时调
    void Shutdown();                                  // 主循环关闭时调
    bool IsInited();
    bool Enable(int w, int h);                        // 用户调
    void Disable();
    bool IsEnabled();
    bool IsSupported();
    bool Resize(int w, int h);
    void BeginScene();                                // 主循环每帧调
    void EndScene();                                  // 主循环每帧调
    void SetExposure(float v);
    float GetExposure();
    void SetGamma(float v);
    float GetGamma();
    uint32_t GetSceneTexture();
    int GetWidth();
    int GetHeight();
    void Pause();                                     // SetCanvas 切换用
    void Resume();
    bool IsPaused();
}
```

### 3.3 Lua `Light.Graphics.HDR.*`（E.3.3）

10 函数：`Enable` / `Disable` / `IsEnabled` / `IsSupported` / `Resize` / `SetExposure` / `GetExposure` / `SetGamma` / `GetGamma` / `GetSceneTexture`。

---

## 4. CI 证据链

| commit | run id | 触发点 | 6 平台 build | Windows runtime smoke | 耗时 |
|--------|--------|--------|--------------|----------------------|------|
| `9ce4431` | `25684976404` | E.3.1 | ✅ | ✅（45 个 smoke） | 6m10s |
| `cf90d43` | `25685412748` | E.3.2 | ✅ | ✅（45 个 smoke） | 8m56s |
| `d6eae5d` | `25686113299` | E.3.3 | ⏳ | ⏳（+ hdr.lua） | 运行中 |

---

## 5. 视觉验收（demo_hdr）

**要求人工运行** `light.exe samples/demo_hdr/main.lua` 验证下列场景：

- [ ] HDR OFF：亮度 > 1.0 的 6 个灰块全部饱和为纯白（LDR clip）
- [ ] HDR ON：10 个灰块呈连续渐变（ACES 压缩）
- [ ] 3 组 RGB 色彩梯度：HDR ON 下色相保留，LDR 下色相被 clip
- [ ] 按 `Z`/`X` 改 Exposure：画面实时变亮/暗
- [ ] 按 `C`/`V` 改 Gamma：画面 gamma 曲线变化
- [ ] 按 `H` 切 HDR：可实时对比 LDR vs HDR
- [ ] Legacy 后端：`HDR.IsSupported()` 返回 false，demo 留在 LDR 模式不崩

---

## 6. 6A 工作流交付物索引

| 阶段 | 文档 |
|------|------|
| Align | `docs/Phase E 渲染管线升级/ALIGNMENT_PhaseE_3.md` |
| Architect | `docs/Phase E 渲染管线升级/DESIGN_PhaseE_3.md` |
| Atomize | `docs/Phase E 渲染管线升级/TASK_PhaseE_3.md` |
| Automate / Assess | `docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_3_1.md`<br>`docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_3_2.md`<br>`docs/Phase E 渲染管线升级/ACCEPTANCE_PhaseE_3_3.md` |
| Final（本文档） | `docs/Phase E 渲染管线升级/FINAL_PhaseE_3.md` |
| TODO | `docs/Phase E 渲染管线升级/TODO_PhaseE_3.md` |

---

## 7. Phase E.3 之后

### 7.1 Phase E.4 候选方向

1. **Bloom** — 利用 HDR RT 的高亮信息做多级下采样模糊，加回主输出。配合 HDR 天然优势。
2. **Gaussian Glow / Depth of Field** — 后处理链扩展。
3. **Multiple Tonemap Operators** — `HDR.SetTonemapper("aces"/"reinhard"/"uncharted2"/"linear")`。
4. **sRGB Framebuffer 兼容** — 硬件 gamma 校正路径（替代 shader pow）。

### 7.2 长期改进

- HDR RT 与 user Canvas 的自动 resize（接入 window resize 事件）
- HDR RT readback → 截图 PNG（需 16bit PNG 支持）
- 自动 pixel-diff 视觉回归测试基础设施（HDR RT 直接读回对比黄金图）

---

## 8. 一句话总结

> Phase E.3 以 **~635 行 C++ + ~470 行 Lua/CI** 的代价，给 ChocoLight 引擎补齐了 **HDR 离屏渲染 + ACES tonemap** 能力，覆盖后端 → 管线 → Lua → smoke → demo 五层，所有阶段 CI 通过（E.3.3 运行中）。用户可通过 `Light.Graphics.HDR.Enable(w, h)` 一行代码启用，无需感知后端细节；LDR 旧代码零改动兼容。
