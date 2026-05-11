# ALIGNMENT — Phase E.3 · HDR + Tonemapping

> 6A 工作流 · 阶段 1 · Align
> 模糊需求 → 精确规范

---

## 1. 原始需求

> **Phase E.3 HDR + Tonemapping**（FINAL_PhaseE_2 推荐 4-5 天）
> 视觉天花板大幅提升：offscreen RT (float16) + ACES tonemapping + exposure 控制；为 Phase E.4 Bloom 铺路。

---

## 2. 项目上下文分析（既有架构）

### 2.1 渲染管线现状

| 组件 | 文件 | 状态 |
|------|------|------|
| `RenderBackend` 抽象接口 | `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | 已有 |
| GL33Core 实现 | `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | 已有，含 `CreateFBO/BindFBO/UnbindFBO` |
| Legacy GL 实现 | `@e:\jinyiNew\Light\ChocoLight\src\render_legacy.cpp` | 已有，不支持 shader/FBO |
| `Light.Graphics.Canvas` | `@e:\jinyiNew\Light\ChocoLight\src\light_graphics_canvas.cpp` | 已有，Lua 暴露 FBO API |
| `BatchRenderer` / `LitBatchRenderer` | Phase A7 / E.2.3 | 已有 |
| Window:__call 主循环 | `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp:620-676` | 已有 |

### 2.2 现有 FBO 能力

```cpp
// @e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:1877-1905
uint32_t CreateFBO(int w, int h, uint32_t* outTex, uint32_t* outDepthRB) override {
    GLuint tex = CreateTexture(w, h, 4, nullptr);          // RGBA8 only!
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
    // glFramebufferTexture2D(... GL_COLOR_ATTACHMENT0 ...);
}
```

**关键观察**：
- 现有 `CreateFBO` 只支持 **RGBA8 + Depth16**
- 没有 float / half-float / sRGB 支持
- Canvas 类用的就是 RGBA8 FBO — 用户角度足够

### 2.3 主循环 hook 点

```cpp
// @e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp:634-672
g_render->BeginFrame(0, 0, 0, 1);               // 清屏 default fb
BatchRenderer::BeginFrame();
LitBatchRenderer::BeginFrame();
// ── Lua Draw 回调 ──
LitBatchRenderer::EndFrame();                    // Flush Lit
BatchRenderer::EndFrame();                       // Flush 普通
g_render->EndFrame();                            // 没做事 (default fb 已就绪)
PlatformWindow::SwapBuffers(g_mainWindow);
```

**Phase E.3 插入点**：HDR 开启时
```cpp
g_render->BeginFrame(0, 0, 0, 1);
+ if (HDR::IsEnabled()) HDR::BeginScene();       // 切到 HDR RT (float16)
BatchRenderer::BeginFrame();
LitBatchRenderer::BeginFrame();
// ── Lua Draw ── 所有 sprite 画到 HDR RT
LitBatchRenderer::EndFrame();
BatchRenderer::EndFrame();
+ if (HDR::IsEnabled()) HDR::EndScene();         // tonemap → default fb
g_render->EndFrame();
PlatformWindow::SwapBuffers();
```

### 2.4 shader 体系

| Shader | 用途 | 输出格式 |
|--------|------|----------|
| `program` (2D unlit) | 普通 sprite | linear RGBA (HDR 模式下) |
| `programLit2D` | Lit sprite | linear RGBA + lighting accumulation (HDR 模式下) |

**关键变化**：HDR 开启时，shader 输出**线性 HDR**（可能 > 1.0），由 tonemap shader 在末尾做 ACES + sRGB encode。

LDR 模式（HDR 关闭）保持现状：shader 输出 sRGB-clamped (0..1)。

---

## 3. 任务范围确认

### 3.1 In Scope（Phase E.3）

| 项 | 描述 |
|----|------|
| **HDR RT 创建** | RGBA16F + Depth24（替代 RGBA8 + Depth16） |
| **`RenderBackend::CreateFBOHDR` 虚接口** | GL33 实现；Legacy 默认 no-op |
| **`RenderBackend::SupportsHDR()` 能力检测** | GL33 返回 true（需 `GL_EXT_color_buffer_float` 或 core 3.3 自带 RGBA16F），Legacy 返回 false |
| **`HDR` 内部管理模块** | `include/hdr_renderer.h` + `src/hdr_renderer.cpp`；命名空间式 API（与 `BatchRenderer` 同风格）：`Init / Shutdown / Enable / Disable / IsEnabled / BeginScene / EndScene / SetExposure / GetExposure / Resize` |
| **ACES tonemap shader** | Fitted（Krzysztof Narkowicz 2-pass 简化版）+ sRGB encode in-shader |
| **Fullscreen quad blit** | tonemap pass: HDR RT (sample texture) → default fb |
| **Lua API** | `Light.Graphics.HDR.Enable(w, h)` / `Disable()` / `IsEnabled()` / `SetExposure(v)` / `GetExposure()` / `Resize(w, h)` |
| **主循环集成** | `light_ui.cpp:Window_Call` 加 `BeginScene/EndScene` hook |
| **`SetCanvas` 透明兼容** | 用户调 `SetCanvas(canvas)` 切到自己 RT 时 HDR 暂停；`SetCanvas(nil)` 恢复 HDR RT |
| **smoke 测试** | `scripts/smoke/hdr.lua`：API surface + 无窗口 guard + 能力检测 |
| **demo** | `samples/demo_hdr/main.lua`：展示曝光调整 + HDR 颜色（亮度 > 1.0 的 lit sprite） |

### 3.2 Out of Scope

| 项 | 原因 |
|----|------|
| Bloom / 后处理链 | Phase E.4 范围 |
| 自适应曝光 (auto-exposure) | 复杂度高，需 luminance histogram；留给 Phase E.5 |
| HDR 显示器 (HDR10 / scRGB 输出) | 平台依赖；本 phase 仅做 SDR 显示器上的 HDR 内部管线 |
| 多 MRT / G-Buffer | 3D 延迟着色范畴，非 2D 必需 |
| `.hdr` 图像加载（已被 stb_image 内置但未启用） | 单独任务，与渲染管线无关 |

---

## 4. 智能决策（基于现有项目自动选定）

| # | 决策点 | 决策 | 理由 |
|---|--------|------|------|
| **D1** | HDR 默认开关 | **默认关闭**，需 Lua 显式 `HDR.Enable(w, h)` | 向后兼容：老 demo 零修改继续跑 LDR；新 demo 主动开启 |
| **D2** | HDR RT 像素格式 | **RGBA16F**（half-float） | 既能存 HDR 值，又比 RGBA32F 显存省一半；GL 3.3 core 标配 |
| **D3** | HDR RT 深度 | **Depth24**（与现有 Depth16 不同） | 现有 RGBA8 路径 Depth16 够用；HDR 路径升级 Depth24 给 3D 留空间 |
| **D4** | tonemap 算法 | **ACES fitted (Narkowicz)** | 学界标杆 + 实现简单（const 4x4 矩阵 + 2 行 fragment 代码） |
| **D5** | sRGB encode | **tonemap shader 内做** (`pow(c, 1/2.2)`) | 不依赖 `GL_SRGB8` framebuffer（跨后端跨平台兼容） |
| **D6** | 默认 exposure | **1.0** | linear 通过 → ACES 直出；调用方可 0.5 暗 / 2.0 亮 |
| **D7** | `Light.Graphics.HDR` 命名空间位置 | Lua 模块 `Light.Graphics.HDR`（与 `Light.Lighting2D` 平级） | 与 `Light.Graphics.Canvas` / `Light.Lighting2D` 一致的"图形子系统"风格 |
| **D8** | 后端能力检测 | 新虚接口 `RenderBackend::SupportsHDR()`；Legacy 返回 false | 与 `SupportsLit2D()` 同模式 |
| **D9** | Legacy GL 行为 | 调 `HDR.Enable` 时返回 false + warn log，所有 HDR API 静默 no-op | 一致的 graceful degradation |
| **D10** | SetCanvas 期间 HDR 行为 | 用户调 `SetCanvas(canvas)` 切到自己 RT 时**暂停** HDR 累积；`SetCanvas(nil)` 时切回 HDR RT；`BeginScene/EndScene` 仍正常 | 用户主动写 RT 一般是 mini-map / 自定义画板，不应被 HDR 干扰 |
| **D11** | window resize 行为 | 用户负责调 `HDR.Resize(w, h)`；内部检测尺寸不匹配时仅 warn 不自动重建 | 简单可靠；与 SDL3 main thread 解耦 |
| **D12** | fullscreen quad 顶点数据 | 内置静态 VBO（`hdrFullscreenVAO/VBO`），4 个顶点 + 静态 EBO | 避免每帧上传；与 `eboLit2D` 同模式 |
| **D13** | tonemap shader uniform | `uHDRTex` (sampler2D), `uExposure` (float), `uGamma` (float, 默认 2.2) | 简单可读；gamma 可关闭通过 1.0 |
| **D14** | smoke 验证范围 | API surface + 能力检测 + 无窗口 guard；视觉验收留 demo | 与 E.2.3 同模式，CI 跑得过 |

---

## 5. 待澄清问题（请用户确认）

| # | 问题 | 默认推荐 | 备选 |
|---|------|----------|------|
| **Q1** | HDR.Enable 失败 (后端不支持) 时如何回调通知 Lua？ | `Enable(w, h)` 返回 `bool` (true=成功)；Lua 自己判断 | 抛 Lua 错误（不推荐，破坏向后兼容） |
| **Q2** | Lua API 命名空间路径？ | `Light.Graphics.HDR` (子表，与 Canvas 平级) | `Light.HDR` (顶级模块) |
| **Q3** | exposure 是否影响 LDR 模式? | **不影响**，LDR 模式 SetExposure 静默忽略 | LDR 模式也用 exposure (但需要每个 sprite shader 处理，复杂) |
| **Q4** | demo 内容方向？ | 自定义的 lit sprite 场景：4 张 baseColor，每张配 normalMap；slider 调 exposure；多 PointLight 动画 | 单 sprite 高对比测试 / 多场景对比 |
| **Q5** | Phase E.3 任务粒度？ | **3 个原子任务**（E.3.1 HDR RT + backend / E.3.2 tonemap shader + 主循环 hook / E.3.3 Lua API + smoke + demo） | 5 个细粒度任务（每个 API 独立任务） |
| **Q6** | window resize 同步策略？ | 用户在 Lua 端 `Light.UI.OnResize` 内手动调 `HDR.Resize` | 引擎自动监听 resize 事件并 resize HDR RT |

---

## 6. 关键技术约束

| 约束 | 影响 |
|------|------|
| **GL 3.3 core 要求** | RGBA16F 是 core 标配；不需要 extension | 
| **half-float texture sample 性能** | 在桌面 GPU 上无开销；移动端可能稍慢但可接受 |
| **PowerVR / Mali Android** | 部分老移动 GPU 不支持 RGBA16F render target；需 `SupportsHDR` 返回 false |
| **macOS Metal/MoltenVK** | RGBA16F 支持完整 |
| **不引入 Phase E.4 数据结构** | mipmap chain / multi-pass blur 留给 Bloom 任务 |
| **smoke headless 限制** | 无 GL context 时 `HDR.Enable` 必返回 false 不崩；smoke 仅验证 API surface |

---

## 7. 验收标准（草案）

1. ✅ `Light.dll` 编译通过（GL33 + Legacy 双后端）
2. ✅ `Light.Graphics.HDR.Enable / Disable / IsEnabled / SetExposure / GetExposure / Resize` 完整 Lua 暴露
3. ✅ `RenderBackend::SupportsHDR` 虚接口；GL33 true，Legacy false
4. ✅ `scripts/smoke/hdr.lua` 全 PASS（≥ 6 个断言）
5. ✅ `samples/demo_hdr/main.lua` 可运行，曝光调整视觉响应
6. ✅ 既有 smoke (`lighting2d.lua` 40 PASS / `ecs_render.lua` / `graphics.lua`) 零回归
7. ✅ CI Build Templates 通过
8. ✅ 文档：`ALIGNMENT_PhaseE_3.md`（本文）+ `CONSENSUS` + `DESIGN` + `TASK` + `ACCEPTANCE` × 3 + `FINAL_PhaseE_3.md`

---

## 8. 下一步（待用户确认 Q1-Q6）

确认问题后进入：
- **阶段 2: Architect** → `DESIGN_PhaseE_3.md`（架构图 + 模块依赖 + 接口契约）
- **阶段 3: Atomize** → `TASK_PhaseE_3.md`（3 原子任务）
- **阶段 4: Approve** → 用户最终确认开始实施
