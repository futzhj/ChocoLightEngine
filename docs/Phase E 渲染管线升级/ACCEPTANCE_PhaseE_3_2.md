# ACCEPTANCE — Phase E.3.2 · HDRRenderer 模块 + 主循环 hook

> 6A 工作流 · 阶段 6 · Assess
> 原子任务 **E.3.2**：`HDRRenderer` 命名空间模块 + `light_ui.cpp` 主循环 hook + `l_SetCanvas` HDR 兼容。

---

## 1. 改动摘要

| 文件 | 改动量 | 类型 | 关键点 |
|------|--------|------|--------|
| `@e:\jinyiNew\Light\ChocoLight\include\hdr_renderer.h` | ~150 行 | 新建 | 命名空间式 API（与 `BatchRenderer` / `LitBatchRenderer` 同风格）；API 分组：生命周期 / HDR 开关 / 主循环 hook / 曝光 gamma / 高级查询 / SetCanvas 兼容 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | ~190 行 | 新建 | 无 GL 依赖，所有操作经 RenderBackend 虚接口；匿名 namespace `State` 全局状态；`ReleaseRT` / `CreateRT` 内部辅助；16 个公开函数实现 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +12 行 | 修改 | 4 个 hook 点：include → `Window_Open::Init` → `Window_Call::BeginScene/EndScene` → `Window_Close::Shutdown` |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +20 行 | 修改 | include → `l_SetCanvas` 切换 RT 前 `BatchRenderer::Flush` + `LitBatchRenderer::Flush` → 分支 (HDR 已启用 && Paused → Resume) / (HDR 未启用 → 原逻辑) / (切到 user canvas → Pause) |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 行 | 修改 | `hdr_renderer.cpp` 加入源文件列表 |

---

## 2. 关键设计

### 2.1 HDRRenderer 状态机

```
       Init(backend)            Enable(w, h)            Disable()
        ─────────→              ─────────────→           ────────→
[未初始化]     [已初始化]         [已启用]              [未启用]
                 ↑                  │  │
                 └──────────────────┘  │
                   Shutdown()          │
                                       │  BindFBO + SetViewport + Clear
                                       │  ↓
                                    [BeginScene]
                                       │
                                       │  UnbindFBO + DrawTonemapFullscreen
                                       │  ↓
                                    [EndScene]

Pause/Resume 是正交状态: enabled ∧ ¬paused = 实际渲染 HDR
                        enabled ∧  paused = 暂停, SetCanvas 期间用户自己画 user RT
```

### 2.2 主循环 hook 顺序

```cpp
// light_ui.cpp::Window_Call 每帧:
g_render->BeginFrame(0, 0, 0, 1);           // 清默认 fb (即使 HDR 启用也清, 作为 blit 目标兜底)
BatchRenderer::BeginFrame();
LitBatchRenderer::BeginFrame();
HDRRenderer::BeginScene();                  // ★ 切到 HDR RT (若启用)

/* Lua Draw callback — 所有 sprite/lit/geom 画到 HDR RT */

LitBatchRenderer::EndFrame();               // Flush 所有 lit batch 到 HDR RT
BatchRenderer::EndFrame();                  // Flush 所有普通 batch 到 HDR RT
HDRRenderer::EndScene();                    // ★ 解绑 HDR RT + ACES tonemap blit 到 default fb
g_render->EndFrame();
SwapBuffers();
```

**关键点**：HDR::EndScene 必须在 BatchRenderer::EndFrame **之后**，才能保证 HDR RT 已累积完所有帧内容。

### 2.3 SetCanvas 的三种流程

**场景 A: HDR 未启用（向后兼容）**
```
SetCanvas(canvas)  → BindFBO(user fbo)                          (与老版本完全一样)
SetCanvas(nil)     → UnbindFBO() + restore default viewport
```

**场景 B: HDR 启用 + 用户在 Draw 中调 SetCanvas(userCanvas)**
```
BeginScene()       → BindFBO(HDR_RT)                            (帧开始)
[画部分 sprite 到 HDR RT]
SetCanvas(user)    → Flush batches → HDR::Pause() → BindFBO(user)  (切 user RT)
[画部分 sprite 到 user RT]
SetCanvas(nil)     → Flush batches → HDR::Resume() → BindFBO(HDR_RT) (切回 HDR, 继续累积)
[继续画 sprite 到 HDR RT]
EndScene()         → UnbindFBO() + tonemap blit (HDR RT 包含所有累积内容)
```

**HDR::Pause/Resume 不释放 RT，仅切换 `paused` 标志**：
- `Pause()` 下，`BeginScene/EndScene` 静默 no-op（Begin 已经绑定过，End 不做 tonemap）
- `Resume()` 调用 `backend->BindFBO(HDR_RT)` 并**不 Clear**，保留之前累积的内容

但注意：场景 B 下的 `EndScene` 时机。如果用户 SetCanvas(userCanvas) 但**没**调 SetCanvas(nil) 就结束帧，HDR 的 paused 标志还在，`EndScene` 静默 no-op。此时屏幕上看不到 tonemap 结果，**但 user canvas 上的内容已经渲染**。这是合理的 fallback 行为（用户明确把帧绘到别处了）。

实施上 `EndScene` 条件 `!g.paused` 守护；下一帧 `BeginScene` 也会继续 no-op（because paused 仍为 true）。这是**隐式行为**：建议在文档里提示用户 SetCanvas 后必须 SetCanvas(nil) 恢复 HDR 管线。

### 2.4 为什么 Disable 要释放 RT

对比 LitBatchRenderer 的 Init/Shutdown（GL 对象生命周期与模块生命周期一致），HDR 的 Enable/Disable 更像"运行时切换"。游戏可能：
- 开场 menu LDR → 主场景切 HDR → 返回 menu 切 LDR
- 设置界面切低画质 → 关 HDR

保留 HDR RT 占 `W * H * 8B (RGBA16F) + W * H * 3B (Depth24)` 显存，1920×1080 约 24MB。Disable 时释放避免显存长期占用。

再次 Enable 重新分配（毫秒级，可接受）。

---

## 3. 验收清单（本地无编译，等 CI）

| 标准 | 状态 | 证据 |
|------|------|------|
| `include/hdr_renderer.h` 头文件存在 + API 完整 | ✅ | 代码 review |
| `src/hdr_renderer.cpp` 实现完整 | ✅ | 代码 review，16 个公开函数全部实现 |
| `light_ui.cpp` 4 个 hook 点全部加入 | ✅ | include / Init / BeginScene/EndScene / Shutdown |
| `light_graphics.cpp` l_SetCanvas 兼容三流程 | ✅ | Flush batches + HDR Pause/Resume 分支 |
| `CMakeLists.txt` hdr_renderer.cpp 加入源列表 | ✅ | line 291 |
| Light.dll 编译通过（所有平台） | ⏳ | 等 CI |
| 既有 smoke 零回归 | ⏳ | 等 CI |

---

## 4. 单元层验证策略

因本 phase 改动集中在生命周期管理 + 主循环 hook，单元测试主要为：

1. **编译不崩**：Light.dll 能 build（GL33 + Legacy + 所有 template）
2. **不启用时静默**：Lua 不调 `HDR.Enable` 时，主循环 hook 检查 `IsEnabled() = false` → no-op，管线走 LDR 路径；现有 demo/smoke 零行为变化。
3. **既有 smoke 不破**：`lighting2d.lua` / `ecs_render.lua` / `graphics.lua` 跑完全 PASS（不走 HDR 路径，无意外副作用）
4. **API surface 完整**：`HDRRenderer::Init/Enable/Disable/IsEnabled/IsSupported/BeginScene/EndScene/Resize/Set/GetExposure/Set/GetGamma/GetSceneTexture/GetWidth/GetHeight/Pause/Resume/IsPaused/Shutdown` 全部可调

真正 HDR 管线视觉验收留 E.3.3 demo（此 phase 无 Lua API 暴露，无法真测）。

---

## 5. 已知限制 / 留给 E.3.3

| 限制 | 留给 |
|------|------|
| 无 Lua API `Light.Graphics.HDR.*`（C++ 模块但用户无法调用） | E.3.3 |
| 无 smoke `hdr.lua` API surface 验证 | E.3.3 |
| 无 demo_hdr | E.3.3 |
| `SetCanvas(userCanvas)` 后不调 `SetCanvas(nil)` 就结束帧时 HDR::EndScene 静默 no-op | 文档提示（E.3.3 Lua API 文档） |
| 不支持多 HDR RT（只能单一场景 RT） | 未来 phase（Bloom 可能需要副 RT） |
| window resize 需用户调 `HDR.Resize` | 设计决策 D11，文档提醒 |

---

## 6. 下一步

**E.3.3**：`Light.Graphics.HDR.*` 9 个 Lua API + `scripts/smoke/hdr.lua` + `samples/demo_hdr/main.lua`。
