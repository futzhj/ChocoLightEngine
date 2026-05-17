# Phase G.1 异步资源加载 — FINAL (项目总结) 报告

> **阶段**：6A Workflow — 阶段 6 Assess
> **创建日期**：2026-05-17
> **基线对照**：
> - [ALIGNMENT_PhaseG_1.md](ALIGNMENT_PhaseG_1.md)
> - [CONSENSUS_PhaseG_1.md](CONSENSUS_PhaseG_1.md)
> - [DESIGN_PhaseG_1.md](DESIGN_PhaseG_1.md)
> - [ACCEPTANCE_PhaseG_1.md](ACCEPTANCE_PhaseG_1.md)

---

## 1. 交付概览

| 子模块 | 范围 | 状态 |
|--------|------|------|
| AssetLoader 基础设施 | 单 worker thread + mutex/cv 双队列 + Future 框架 + 同步 fallback | 已交付 |
| Image 异步 | `Light.Graphics.Image.LoadAsync` (Future + Callback) | 已交付 |
| LUT 异步 | `Light.Graphics.HDR.LoadCubeLUTAsync` / `LoadHaldLUTAsync` | 已交付 |
| Mesh 异步 | `Light.Graphics.Mesh.LoadGLTFAsync` (基础 mesh) | 已交付 |
| Font 异步 | `Light.Graphics.Font.LoadAsync(path, size)` | 已交付 |
| Sound 异步 | `Light.Audio.Sound.LoadAsync` | 已交付 |
| Future userdata | 共享 metatable `Light.Graphics._AsyncFuture`, 5 类资源复用 | 已交付 |
| Smoke + CI | `scripts/smoke/asset_loader_async.lua` + Windows runtime smoke | 已交付 |
| Shared GL Context probe | DESIGN §3.1 描述 + ALIGNMENT §1 风险项 | **未交付**（移交 G.1.1） |

## 2. 与 CONSENSUS 范围的对齐

| CONSENSUS 范围条目 | 实际状态 | 备注 |
|--------------------|----------|------|
| 单 worker thread + 双 lock-free 队列 | 用 `std::mutex` + `std::deque` 实现（CONSENSUS Q2 决议） | 性能瓶颈在解码不在排队，确认无需 lock-free |
| Shared GL Context probe | **未实装** | worker 仅 CPU 解码，主线程 `Tick` 上传 |
| Image 内存解码 (`LoadFromMemoryAsync`) | 不在范围 | 移交 G.x |
| glTF material / 内嵌纹理异步 | 不在范围 | 仅基础 mesh，已在 ACCEPTANCE 标注 |
| LUT hot reload 异步化 | 不在范围 | 移交 G.x |
| Font 动态字形烘焙异步 | 不在范围 | 仅 ASCII 预烘焙路径 |
| Sound streaming 大音频流式异步 | 不在范围 | miniaudio 内部线程安全的一次性加载 |

## 3. 关键架构决策落地情况

| 决策点 | 落地实现 | 文件 |
|--------|----------|------|
| Future 用 `shared_ptr<FutureState>` 双方持有 | `AssetLoader::FutureState` + `FutureUserdata` 各持一份 | `asset_loader.h:124-128`, `asset_loader.cpp:687-783` |
| 错误传播：`errorMsg` + `status==Error` | Worker 设错；`Tick` 仅做翻转；`Future:Get` 返 `nil, err` | `asset_loader.cpp::Tick` |
| Lua callback 仅主线程 dispatch | 所有 dispatcher 通过 `Tick` 触发，worker 永不触 lua_State | `asset_loader.cpp::Tick` 末段 |
| 同步 fallback (worker 未启动) | 5 类 `LoadXxxAsync` 各自走对应同步加载路径 | 例：`asset_loader.cpp::LoadCubeLUTAsync` |
| Future userdata 跨资源类型共用 | `kFutureMetaName` + `ResultPusher` 函数指针（每类资源各注册自己的 push） | `asset_loader.h:124-128` |
| 主线程 GL upload | 5 个 `UploadXxx_` helper 通过 `switch (task.type)` 分派 | `asset_loader.cpp::Tick` |

## 4. 集成生命周期

```
[启动] light_ui.cpp:531 → AssetLoader::Init()  ─ 启动 worker thread
[每帧] light_ui.cpp:690 → AssetLoader::Tick()  ─ drain result_queue + GL upload + dispatch cb
[退出] light_ui.cpp:788 → AssetLoader::Shutdown() ─ join worker, 标 pending = Error
```

## 5. 验证证据

| 验证项 | 结果 |
|--------|------|
| `cmake --build build --config Release --target Light` | 退出码 0，`Light.dll` 生成并同步到 lumen runtime |
| `lightc.exe -p scripts/smoke/asset_loader_async.lua` | 退出码 0（Lua 语法 OK） |
| `light.exe scripts/smoke/asset_loader_async.lua` | 退出码 0，`PASS: async asset API surface ok`、`PASS: async future userdata error behavior ok` |
| `light.exe scripts/smoke/mesh_3d.lua` | 退出码 0，原有 Mesh smoke 全 PASS（无回归） |
| `light.exe scripts/smoke/audio_3d_mixer_effect.lua` | 退出码 0，All 40 assertions PASSED |
| `light.exe scripts/smoke/graphics.lua` | 退出码 0，40/0 PASS |

## 6. 已知边界 / 未交付项

详见 `TODO_PhaseG_1.md`，主要包括：

- Shared GL Context probe 未实装，worker 不直接上传 GL（性能受限于主线程 `Tick`）。
- `LoadGLTFAsync` 未带 material / 内嵌纹理。
- 端到端 smoke 仅覆盖 API 表面与缺失文件错误路径，未带真实资源跑 ready→Get 链路。
- Auto-fallback 路径未单独自动化测试（worker 启动失败的人工验证流程）。

## 7. 接管 G.1.1 / G.x 的入口

- `asset_loader.cpp:13` 已自标注 G.1.1 待办。
- DESIGN §3.1 描述了 Shared Context + glFenceSync 的目标实现路径。
- 5 类资源 `LoadXxxAsync` 已统一签名，未来接 Shared Context 时仅扩展 `WorkerMain` 的上传分支即可，Lua 表面不需变动。
