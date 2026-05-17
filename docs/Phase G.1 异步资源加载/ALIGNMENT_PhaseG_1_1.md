# Phase G.1.1 Shared GL Context — ALIGNMENT (对齐) 文档

> **阶段**：6A Workflow — 阶段 1 Align
> **创建日期**：2026-05-17
> **依赖**：[ALIGNMENT_PhaseG_1.md](ALIGNMENT_PhaseG_1.md)、[CONSENSUS_PhaseG_1.md](CONSENSUS_PhaseG_1.md)、[DESIGN_PhaseG_1.md](DESIGN_PhaseG_1.md)、[TODO_PhaseG_1.md](TODO_PhaseG_1.md)

---

## 1. 任务背景与基线

G.1.0 已交付：worker 线程做 CPU 解码 + 主线程 `Tick` 内 GL 上传 + Lua callback dispatch。性能瓶颈在大批量纹理到达时仍可能压主线程。本子任务实现 G.1 设计原意中的 **Shared GL Context** 路径，让 worker 直接 `glTexImage2D` + `glFenceSync`，主线程仅做 `glClientWaitSync` 翻状态。

## 2. 范围确认

### 2.1 包含

- `PlatformWindow` 增加共享 GL context 创建入口（与现有 `CreateGLContext` 解耦）。
- `AssetLoader::Init` 接受主窗口/主 ctx 句柄，启动时 probe 共享 context 是否可用。
- worker 线程入口 `MakeCurrent(workerCtx)`；解码后**有条件**直接做 GL upload + `glFenceSync`。
- `Tick` 改造：拿到 fence 的 result 走 `glClientWaitSync(timeout=0)` 路径；fence 未完成则放回队列下帧再试。
- probe 失败 / fence 不可用时**自动回落到 G.1.0 主线程上传路径**（不破坏现有行为）。
- 启动日志清晰区分两种模式：`Shared GL Context enabled` / `fallback to main-thread upload`。

### 2.2 不包含

- 多 worker 线程池（仍为单 worker）。
- worker 直接调 `backend->CreateTexture` 这种"经过引擎抽象层"的路径——抽象层（`RenderBackend`）仍是单线程契约，worker 直接走原生 GL（仅 `glGenTextures` / `glTexImage2D` / `glFenceSync` 这几个无状态调用）。
- 资源缓存 / 引用计数（仍由 G.2 处理）。
- glTF material + 内嵌纹理异步（`TODO_PhaseG_1.md` T2 单独追踪）。
- Web/Android：移动 GLES 上 share context 行为不可靠，本子任务**只对桌面 GL 3.3 Core 启用**，移动平台沿用 G.1.0 主线程上传。

## 3. 现状摘要

| 当前位置 | 现状 | G.1.1 改动点 |
|---------|------|-------------|
| `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:13-14` | 自标 G.1.1 TODO，明示路径 | 删 TODO 注释，落地实现 |
| `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:454-468` `Init()` | 无参版本 | 新签名带主窗口/主 ctx |
| `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:415-420` `WorkerMain` | 无 GL 调用 | 入口 MakeCurrent + 出口翻 fence |
| `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp:531-533` | `AssetLoader::Init()` 调用 | 改 `Init(g_mainWindow, g_glContext)` |
| `@e:/jinyiNew/Light/ChocoLight/include/platform_window.h:134-151` | `CreateGLContext` 单形态 | 加共享 ctx 创建/MakeCurrentNone helper |
| `@e:/jinyiNew/Light/ChocoLight/src/platform_window_sdl3.cpp:262-277` | 直接 `SDL_GL_CreateContext` | 加 `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` 路径 |

## 4. 需要用户决断的关键问题

### Q1: `PlatformWindow` 共享 ctx 入口形态

| 方案 | 说明 | 影响面 |
|------|------|-------|
| **A. 扩展现有** `CreateGLContext(win, bool share=false)` | 同函数加默认参数 | platform_window.h 改 1 行；调用方默认行为不变 |
| **B. 新增** `CreateSharedGLContext(win)` | 独立函数，语义直白 | 新增 1 个 API，但 `CreateGLContext` 完全不动 |
| C. AssetLoader 直接调 SDL3 | `#include <SDL3/SDL.h>`，绕过 PlatformWindow | 破坏 `PlatformWindow` 平台抽象，**不推荐** |

**推荐**：方案 B。语义最清晰，与 `MakeCurrent` 的调用模式一致，向后兼容性最高。

### Q2: probe 失败时的回落策略

| 方案 | 说明 |
|------|------|
| **A. 自动回落到 G.1.0 主线程上传**（透明） | worker 完成解码后把 raw buffer 入 result_queue，`Tick` 走原 `UploadXxx_` 路径 |
| B. probe 失败直接关闭异步加载 | `Init` 返 false，所有 `LoadXxxAsync` 走同步 fallback |

**推荐**：方案 A。CONSENSUS Q3 已锁死 auto-fallback；用户视角无感知，仅日志标注。

### Q3: fence 等待预算

| 方案 | 说明 |
|------|------|
| **A. `glClientWaitSync(timeout=0)`** + 队列回放 | 不阻塞主线程；未完成的下帧再试 |
| B. `glClientWaitSync(timeout=N ns)` 短超时 | 主线程偶尔短等，简化代码 |
| C. `glFinish()` 强同步 | 无队列回放但 stall 主线程，违反异步初衷 |

**推荐**：方案 A，与 DESIGN §3.1 一致。

## 5. 验收口径

1. ✅ Build clean（桌面 / Web / Android 三个 target，受 `#if defined(__EMSCRIPTEN__)` 等保护）。
2. ✅ 现有 smoke 全过（`asset_loader_async.lua` + `mesh_3d.lua` + `audio_3d_mixer_effect.lua` + `graphics.lua`）。
3. ✅ 启动日志能看到 `AssetLoader: Shared GL Context enabled` 或 `AssetLoader: fallback to main-thread upload`。
4. ✅ probe 失败注入测试可触达 fallback 路径（通过临时改 `CreateSharedGLContext` 返 nullptr 验证）。
5. ✅ 6A 文档：ALIGNMENT_G1_1 / DESIGN_G1_1 / TASK_G1_1 / ACCEPTANCE_G1_1 / FINAL_G1_1。

## 6. 风险与备用

| 风险 | 备用方案 |
|------|---------|
| Intel iGPU / 某些 OEM 驱动 share context 不稳定 | probe 失败 → 走 G.1.0 主线程上传路径 |
| `SDL_GL_SHARE_WITH_CURRENT_CONTEXT` 在某 SDL3 版本未实现 | platform_window 内 fallback：当前 ctx unbind + create 第二个 ctx + 期待驱动默认共享；仍失败则视为 probe 失败 |
| worker 线程 `glTexImage2D` 与主线程并发 GL 状态污染 | 仅在 worker ctx 上调，且不调 backend 抽象层（无状态机改动） |
| `glClientWaitSync(timeout=0)` 长期 PENDING 导致 result_queue 堆积 | 加最大重试次数（如 60 帧 = 1s @60fps），超过转 Error |
