# Phase G.1.1 Shared GL Context — 验收记录

> **阶段**：6A Workflow — 阶段 5 Automate / 阶段 6 Assess
> **完成日期**：2026-05-17
> **关联文档**：[ALIGNMENT](ALIGNMENT_PhaseG_1_1.md) · [DESIGN](DESIGN_PhaseG_1_1.md) · [TASK](TASK_PhaseG_1_1.md)

---

## 一、本次覆盖范围

### 代码改动（两个模块 + 一个调用点）

- **PlatformWindow 抽象层**：新增 `CreateSharedGLContext(win)` API，桌面用 `SDL_GL_SHARE_WITH_CURRENT_CONTEXT=1` 派生共享 ctx；移动 / Web 桩函数直接返 nullptr
- **AssetLoader 异步资源加载**：
  - `Init` 签名改 `Init(void* mainWin, void* mainCtx)`，启动时 probe Shared GL Context
  - probe 成功 ⇒ worker 持有共享 ctx，`Image / LUT` 两类直接 worker `glTexImage2D + glFenceSync`
  - probe 失败 / 移动平台 ⇒ 透明回落 G.1.0 主线程上传路径
  - `FutureState` 加 `glFence` + `fenceWaitFrames` 字段
  - `Tick` 加 fence 翻转路径：`glClientWaitSync(timeout=0)` → Ready / Retry / Error 三态
  - `Shutdown` 加 worker ctx 销毁 + 残留 fence 清理 + dtor 兜底
- **light_ui.cpp**：`AssetLoader::Init()` ⇒ `AssetLoader::Init(g_mainWindow, g_glContext)`

### Lua 表面零变化

- 5 类异步 API 签名 / 行为 / 错误语义全部不变
- `Future:Get()` 三态语义 (`Pending/Ready/Error`) 不变

### 范围裁剪（本期不做，留 G.1.2）

- **GLTF Mesh worker 上传**：依赖 backend Mesh 抽象（VAO+VBO+IBO 多对象绑定），契约更复杂
- **Font** 主线程路径无 GL 操作（atlas lazy bake），worker 加速无意义，维持 G.1.0
- **Sound** 不涉及 GL，与本期无关

---

## 二、原子任务完成情况

| 任务 | 范围 | 状态 | 关键文件 |
|----|----|----|----|
| **T1** | `PlatformWindow::CreateSharedGLContext` 声明 + 实现 | ✅ | `@e:/jinyiNew/Light/ChocoLight/include/platform_window.h:140-146` · `@e:/jinyiNew/Light/ChocoLight/src/platform_window_sdl3.cpp:271-295` |
| **T2** | `AssetLoader::Init` 签名变更 + probe 主流程 | ✅ | `@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h:140-153` · `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:597-640` |
| **T3** | WorkerMain GL 上传 (`Image/LUT`) + WorkerUpload helper | ✅ | `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:429-533` (helpers) · `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:570-581` (WorkerMain 调用点) |
| **T4** | `Tick` fence 翻转路径 + 重试队列 | ✅ | `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:812-844` (`CheckFenceState_`) · `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:846-921` (`Tick`) |
| **T5** | `light_ui.cpp` 调用点签名更新 | ✅ | `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp:530-535` |
| **T6** | Shutdown 销毁 worker ctx + 残留 fence 清理 | ✅ | `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:642-690` · dtor 兜底 `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:58-65` |
| **T7** | smoke 验证 (构建 + 4 个 smoke + probe) | ✅ | 见下表 |
| **T8** | 6A 收尾文档 (ACCEPTANCE / FINAL / TODO) | ✅ | 当前文档 + `FINAL_PhaseG_1_1.md` + `TODO_PhaseG_1_1.md` |

---

## 三、验证记录

### 构建

| 命令 | 结果 |
|------|------|
| `cmake --build build --config Release --target Light` | ✅ 通过 (exit 0)，无 warning 增量 |

### Headless smoke（已纳入既有测试套件）

| 脚本 | 结果 | 关键断言 |
|------|------|---------|
| `light.exe scripts/smoke/asset_loader_async.lua` | ✅ PASS | "async asset API surface ok" + "async future userdata error behavior ok" |
| `light.exe scripts/smoke/mesh_3d.lua` | ✅ PASS | "mesh_3d smoke ok"（GLTF 异步路径无回归） |
| `light.exe scripts/smoke/audio_3d_mixer_effect.lua` | ✅ PASS | "All 40 assertions PASSED"（Sound 异步路径无回归） |
| `light.exe scripts/smoke/graphics.lua` | ✅ PASS | "通过 40 / 失败 0"（图形栈整体无回归） |

### Probe 日志验证（带 GL 真窗口）

| 脚本 | 关键日志 | 结果 |
|------|---------|------|
| `light.exe scripts/smoke/asset_loader_async_probe.lua` | `[I] AssetLoader: Shared GL Context enabled (worker direct upload + fence)` | ✅ probe 路径在 Windows + NVIDIA 560.94 + GL 3.3 Core 上稳定生效 |

### 不变量验证

- ✅ Lua 5 类 LoadXxxAsync API 签名 / 错误语义零变化（已纳入 `asset_loader_async.lua` smoke）
- ✅ `Future:Get()` 三态语义不变
- ✅ 无 worker 直接 GL 上传时（probe 失败分支）行为完全等价 G.1.0
- ✅ `FutureState::~FutureState` 加 fence 兜底清理，防止泄漏

---

## 四、已知边界

### 平台覆盖

- ✅ 桌面（Windows / Linux / Mac）：probe + worker GL 上传路径
- ⏸ 移动（Android / iOS）/ Web（Emscripten）：`CreateSharedGLContext` 永远返 nullptr，自动回落主线程上传（无功能损失，仅无加速收益）

### 资源覆盖

- ✅ Image：worker 直接 `glGenTextures + glTexImage2D(RGBA8) + glFenceSync`
- ✅ LUT (Cube / HALD)：worker 直接 `glGenTextures + glTexImage3D(RGB8/RGB16F) + glFenceSync`
- ⏸ Mesh：留 G.1.2（VAO + VBO + IBO 多对象绑定）
- ⏸ Font：主线程上传无 GL，worker 加速无意义
- ⏸ Sound：与 GL 无关

### Probe 脚本退出语义

- `asset_loader_async_probe.lua` 是带 GL 真窗口的人工验证脚本（仅本地开发用，不接入 CI）
- CI 仍用 `asset_loader_async.lua`（headless）覆盖 API 不变量

---

## 五、设计偏离记录

| 偏离项 | 原计划 | 实际做法 | 原因 |
|----|----|----|----|
| T3 范围 | 覆盖 Image / LUT / Font 三类 | **只覆盖 Image / LUT 两类** | Font 主线程 `UploadFont_` 无 GL 操作（atlas lazy bake 在 GetGlyph 时做），worker 上传无价值。纯执行收益 0，故撤出范围 |
| `CreateSharedGLContext` 内 unbind | 原版 `SDL_GL_MakeCurrent(nullptr, nullptr)` | 改为 **不 unbind**，由调用方拉回主 ctx | 原版会同时 unbind 主 ctx，破坏后续主线程 GL 调用。改由 `AssetLoader::Init` 立即 `MakeCurrent(mainWin, mainCtx)` 拉回 |
| fence 重试机制 | 直接 push 回队列 | 用本地 `retry` 容器收集，循环末 `push_front` 批量回填 | 避免在迭代 `local` 时同时改写 `g_resultQueue`；并保证下帧 fence-pending task 被优先处理 |
