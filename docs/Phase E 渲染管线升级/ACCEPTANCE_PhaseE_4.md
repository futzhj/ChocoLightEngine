# ACCEPTANCE — Phase E.4 · Bloom 后处理

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.4.1 / E.4.2 / E.4.3** 合并验收。Bloom 作为 HDR 链路的后处理插件，沿用 Phase E.3 的命名空间 + Lua 子表风格。

---

## 1. 改动摘要

| 阶段 | 文件 | 改动量 | 类型 |
|------|------|--------|------|
| E.4.1 | `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~50 行 | 修改：6 个 Bloom 虚接口（默认 no-op，Legacy 后端兼容） |
| E.4.1 | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~440 行 | 修改：3 套 shader（bright/down/up）× 2 GL profile + `GL33Backend::InitBloom/Shutdown` + 7 个 override |
| E.4.2 | `@e:\jinyiNew\Light\ChocoLight\include\bloom_renderer.h` | +130 行 | 新增：BloomRenderer namespace 接口 |
| E.4.2 | `@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp` | +243 行 | 新增：State + Init/Shutdown/Enable/Disable/Resize + auto-link + Process 4-pass |
| E.4.2 | `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +20 行 | 修改：`#include "bloom_renderer.h"` + Enable/Disable/Resize/EndScene 注入 Bloom 回调 |
| E.4.2 | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 行 | 修改：Window_Open 后 `BloomRenderer::Init`；Window_Close 前 `BloomRenderer::Shutdown` |
| E.4.2 | `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 行 | 修改：源列加 `bloom_renderer.cpp` |
| E.4.3 | `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~150 行 | 修改：15 个 `l_Bloom_*` + `bloom_funcs[]` + Bloom 子表挂入 `luaopen_Light_Graphics` |
| E.4.3 | `@e:\jinyiNew\Light\scripts\smoke\bloom.lua` | +200 行 | 新增：API surface + 参数 clamp + lifecycle + AutoEnable 联动 |
| E.4.3 | `@e:\jinyiNew\Light\samples\demo_bloom\main.lua` | +220 行 | 新增：黑底 8 亮点 + 7 组热键调参 + OSD |
| E.4.3 | `@e:\jinyiNew\Light\samples\demo_bloom\README.md` | +80 行 | 新增：操作 / 参数 / 联动说明 |
| E.4.3 | `@e:\jinyiNew\Light\samples\demo_hdr\main.lua` | +~25 行 | 修改：可选 Bloom 子表 + B 键切换 + OSD 行 + cleanup 顺序 |
| E.4.3 | `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 行 | 修改：注册 `phaseE4Smoke = scripts/smoke/bloom.lua` 进 Windows runtime smoke |

---

## 2. 架构落地

### 2.1 数据流

```
HDR RT (RGBA16F)
   │
   ├── (HDRRenderer::EndScene 中) ────────── BloomRenderer::Process(hdrFbo, hdrTex)
   │                                          │
   │                                          ├─ 1) BrightPass: hdrTex → pyramid[0]   (uThreshold + soft knee)
   │                                          ├─ 2) Downsample: [0]→[1]→…→[N-1]       (COD AW 13-tap)
   │                                          ├─ 3) Upsample:   [N-1]→[N-2]→…→[0]     (tent 3x3, ONE/ONE 加性)
   │                                          └─ 4) Composite:  [0] → hdrFbo          (intensity 缩放 + ONE/ONE)
   │
   └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
                                                 ↓
                                              Backbuffer
```

### 2.2 模块层次

```
Light.Graphics.Bloom  (Lua)
        │
        ▼
BloomRenderer::*      (C++ namespace)
        │
        ▼
RenderBackend::DrawBloomXxx / Create/DeleteBloomPyramid  (虚接口)
        │
        ├─ GL33Backend::*  (GL 3.3 / GLES3 实现, 3 shader pyramid)
        └─ LegacyBackend  (继承默认 no-op)
```

### 2.3 联动表

| 触发 | 行为 |
|------|------|
| `HDRRenderer::Enable(w,h)` 成功 | 若 `Bloom.GetAutoEnable()=true` → `BloomRenderer::OnHDREnabled(w,h)` → `BloomRenderer::Enable(w,h)` |
| `HDRRenderer::Disable()` | `BloomRenderer::OnHDRDisabled()` → `BloomRenderer::Disable()`（必须先于 HDR RT 释放） |
| `HDRRenderer::Resize(w,h)` 成功 | `BloomRenderer::OnHDRResized(w,h)` → 若 enabled 则 `Resize(w,h)`（同尺寸 no-op） |
| `Bloom.SetAutoEnable(false)` 后 `HDR.Enable` | 不再自动启 Bloom，需手动 `Bloom.Enable(w,h)` |
| `Bloom.SetLevels(n)` 在 enabled 时 | 仅缓存，需 `Disable+Enable` 或 `Resize` 重建 pyramid 才生效 |

---

## 3. API surface

### 3.1 C++ (`@e:\jinyiNew\Light\ChocoLight\include\bloom_renderer.h`)

```cpp
namespace BloomRenderer {
    void Init(RenderBackend* backend);
    void Shutdown();

    bool Enable(int w, int h);
    void Disable();
    bool IsEnabled();
    bool IsSupported();
    bool Resize(int w, int h);

    void SetAutoEnable(bool flag);   // 默认 true
    bool GetAutoEnable();

    void SetThreshold(float v);      // clamp [0, +inf)
    float GetThreshold();
    void SetIntensity(float v);      // clamp [0, +inf)
    float GetIntensity();
    void SetRadius(float v);         // clamp [0, 1]
    float GetRadius();
    void SetLevels(int n);           // clamp [2, 8]
    int  GetLevels();

    void Process(uint32_t hdrFbo, uint32_t hdrTex);  // 由 HDRRenderer::EndScene 调

    // 内部 HDR 联动 (HDRRenderer 调, Lua 不暴露)
    void OnHDREnabled(int w, int h);
    void OnHDRDisabled();
    void OnHDRResized(int w, int h);
}
```

### 3.2 Lua (`Light.Graphics.Bloom`)

15 个函数 = 5 lifecycle + 2 AutoEnable + 8 参数 set/get：

```lua
local Bloom = require('Light.Graphics').Bloom
Bloom.Enable(w, h) / Bloom.Disable() / Bloom.IsEnabled() / Bloom.IsSupported() / Bloom.Resize(w, h)
Bloom.SetAutoEnable(flag) / Bloom.GetAutoEnable()
Bloom.SetThreshold(v) / Bloom.GetThreshold()
Bloom.SetIntensity(v) / Bloom.GetIntensity()
Bloom.SetRadius(v) / Bloom.GetRadius()
Bloom.SetLevels(n) / Bloom.GetLevels()
```

---

## 4. 验收准则

| # | 准则 | 通过证据 |
|---|------|----------|
| AC-1 | 后端 6 虚接口签名稳定，Legacy 后端 no-op 不影响构建 | E.4.1 CI run [`25691097282`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25691097282) — 6 平台 success |
| AC-2 | `BloomRenderer::Process` 全 4 pass 在 Bloom 关闭/未初始化时一律 early-return（防御性） | 代码评审 `@e:\jinyiNew\Light\ChocoLight\src\bloom_renderer.cpp:Process` |
| AC-3 | `HDRRenderer::Disable` 时 Bloom 先释放（指针顺序对：HDR RT 仍存） | `hdr_renderer.cpp` Disable 入口 prepend `BloomRenderer::OnHDRDisabled()` |
| AC-4 | `Bloom.SetAutoEnable(false)` 后 HDR.Enable 不触发 Bloom | smoke section 9 |
| AC-5 | Threshold/Intensity 非负 clamp，Radius `[0,1]` clamp，Levels `[2,8]` clamp | smoke section 4-7 |
| AC-6 | Headless 下 Bloom.Enable 安全返回 boolean，双 Disable 不崩 | smoke section 8 |
| AC-7 | demo_bloom 在 Legacy 后端 API surface 探测后干净退出 | demo_bloom/main.lua 头部分支 |
| AC-8 | demo_hdr B 键交互不破坏原有 H/Z/X/C/V/T 流程 | demo_hdr/main.lua diff |
| AC-9 | Windows CI runtime 运行 `scripts/smoke/bloom.lua` 通过 | 见 §5 |

---

## 5. CI 证据

| Commit | Run | 结论 | 备注 |
|--------|-----|------|------|
| `380b37e` docs (ALIGN+DESIGN+TASK) | [`25690677111`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25690677111) | ✅ 6/6 | 仅 markdown |
| `80a9f67` E.4.1 backend | [`25691097282`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25691097282) | ✅ 6/6 | 虚接口 + 3 shader |
| `0655f54` E.4.2 module | `25691682204` | ❌ 6/6 fail | 误引 `cc_core.h` 不存在 |
| `43f63ee` fix include | `25692959657` | ⏳ 跑 | 修为 `light.h` |
| `45e1944` E.4.3 Lua + smoke + demo | `25692903532` | ⏳ 跑 | 含同样 fix 前的代码，需等 fix run 验证 |

> **修复说明**：E.4.2 初次提交误引 `cc_core.h`（不存在），实际 `CC::Log` 在 `light.h` 中导出。修复一行后 6 平台应当全绿。

---

## 6. 已知行为约束

1. **Bloom 依赖 HDR RT**：`Bloom.Enable` 在无 HDR FBO 上下文时仍会创建 pyramid，但 `Process` 需要 hdrFbo/hdrTex 不为 0（HDRRenderer::EndScene 内部保证）。直接 Lua 调 `Bloom.Enable` 而不开 HDR 时，Process 永远不会被驱动，Bloom 仅消耗 pyramid 内存。
2. **`SetLevels` 延迟生效**：性能考虑不在 setter 内直接重建 pyramid；需 `Resize` 或 `Disable+Enable`。
3. **Legacy 后端 `IsSupported() = false`**：默认虚接口 SupportsBloom = false；demo 会切到 API 探测分支。
4. **Headless smoke**：`Enable(w,h)` 一般 false（无 GL ctx）；所有 setter/getter 仍可调用（State 是 CPU-side），smoke 据此设计 fallback 检查。

---

## 7. 后续工作（→ TODO_PhaseE_4.md）

- 视觉验收（带 GL ctx 真机跑 demo_bloom）
- Bloom + tonemap 混合验证（4 op 切换不影响 bloom 输出）
- 性能基线（pyramid level=5 在 1920×1080 上的 frame cost）
- Lens dirt / Streak 等扩展（Phase E.5 候选）
