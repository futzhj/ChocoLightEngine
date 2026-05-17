# Phase G.1 异步资源加载 — TODO (待办清单)

> **阶段**：6A Workflow — 阶段 6 Assess 收尾
> **创建日期**：2026-05-17

---

## 1. 工程类待办（建议进入 Phase G.1.1）

### T1. Shared GL Context probe + worker 直接上传

- **现状**：worker 仅 CPU 解码，所有 GL 上传仍在主线程 `Tick`，4K 纹理大批量到达时仍可能出现帧抖动。
- **位置**：
  - `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:13`（已自标 TODO）
  - `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:415-420` `WorkerMain` 入口
  - `@e:/jinyiNew/Light/ChocoLight/docs/Phase G.1 异步资源加载/DESIGN_PhaseG_1.md` §3.1
- **建议路径**：
  1. `AssetLoader::Init` 增加 `(SDL_Window* mainWin, SDL_GLContext mainCtx)` 参数。
  2. 启动 worker 前先在主线程 `SDL_GL_CreateContext` 创建第二个共享上下文。
  3. `WorkerMain` 首条指令 `SDL_GL_MakeCurrent(workerCtx)`，主线程立刻 `MakeCurrent(mainCtx)` 复位。
  4. 解码完成后 worker 直接 `glGenTextures` + `glTexImage2D` + `glFenceSync`，把 fence 入 `result_queue`。
  5. `Tick` 拿到 fence 后 `glClientWaitSync(timeout=0)`，未完成则放回队列下一帧再试。
  6. probe 失败（创建 context 返 null 或 worker 第一条 GL 调用报错）时设 `g_sharedCtxOk=false`，自动走当前主线程上传路径。
- **依赖**：`PlatformWindow`（SDL3）暴露 main `SDL_Window*` / `SDL_GLContext`。

### T2. glTF 异步加载：material + 内嵌纹理

- **现状**：`Mesh.LoadGLTFAsync` 仅返回基础 `MeshUserdata`，丢弃 material 与 embedded image。
- **位置**：`@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp::Decode_GLTF`（worker）+ `@e:/jinyiNew/Light/ChocoLight/src/light_graphics_mesh.cpp::MeshPushResult_`。
- **建议路径**：
  1. `FutureState` 扩 `MaterialDesc` + 内嵌纹理像素 buffer 列表。
  2. worker 在解析阶段同步用 `stbi_load_from_memory` 解码所有 embedded image（已在 worker 线程，安全）。
  3. `Tick` 在 `UploadGLTF_` 阶段为每张 embedded image 调 `backend->CreateTexture`，写入 material slot。
  4. `MeshPushResult_` 加可选 `withMaterial` 第二返回值（与同步 `Mesh.LoadGLTF` 对齐）。

### T3. 端到端真实资源 smoke

- **现状**：`scripts/smoke/asset_loader_async.lua` 只覆盖 API 表面 + 缺失文件错误路径，未跑成功 ready 链路。
- **建议路径**：
  1. 利用现有 `scripts/smoke/asset_loader_async.lua` 的 headless 上下文，新增带真实小尺寸资源（PNG / WAV / TTF / .cube / .glb）的子用例。
  2. 资源放 `scripts/smoke/assets_g1/`，CI 时跟 smoke 一起检出。
  3. 用 Future 风格 poll 直到 `IsReady()`，再断言 `Get()` 返非 nil。
  4. 受限于 headless 无 GL context，可仅断言 callback 风格 + 错误路径，或仅在带 GL 的 smoke 子集中跑。

### T4. Auto-fallback 路径自动化验证

- **现状**：CONSENSUS Q3 提的 worker 启动失败 fallback，目前依赖手动篡改 `Init` 失败注入。
- **建议路径**：在 `AssetLoader` 暴露 `EnableForceFallbackForTest()` 测试 hook，smoke 内调用并断言 `LoadImageAsync` 走同步路径。

## 2. 文档类待办

### D1. 缺失的 6A `TASK_PhaseG_1.md`

- **现状**：本期跳过原子化阶段，直接从 DESIGN 进入实现，未生成 `TASK_PhaseG_1.md`。
- **影响**：后续接手 G.1.1 时缺少粒度化任务清单，需要从 FINAL + DESIGN 反推。
- **建议**：G.1.1 启动时补出，按 T1–T4 生成原子任务卡。

### D2. Lua 端使用样例

- **现状**：CONSENSUS §2.2 给出 Future / Callback 写法，但 `samples/` 目录无端到端示例。
- **建议**：在 `samples/` 增加 `demo_async_assets/`，串联 Image + Mesh + Sound 三类异步加载，作为接入参考。

## 3. 配置 / 环境提示

| 项 | 是否需要操作 | 说明 |
|----|-------------|------|
| `.env` / API Key | 否 | 本期不涉外部 API |
| CI workflow | 已自动接入 | `@e:/jinyiNew/Light/.github/workflows/build-templates.yml:106,224-225` |
| 资源 fixture | T3 时需要补 | 当前 smoke 不依赖任何 binary 资源 |

## 4. 推荐解决顺序

1. **T1 Shared GL Context probe**（性能价值最高，是 G.1 设计原意）
2. **T3 端到端真实资源 smoke**（验证 T1 改造正确性）
3. **T2 glTF material 异步**（功能闭环，使 `LoadGLTFAsync` 与 `LoadGLTF` 对齐）
4. **D1 / D2 文档与样例**（接手前补齐）
5. **T4 fallback 自动化验证**（最低优先级，受益小）
