# ChocoLight Engine - 交接与未完成任务清单 (Handoff Summary)

> **创建时间**: 2026-05-17
> **最后更新**: 2026-05-18 (G.1.5 异步 GLTF Material 收尾 + F.0.11.6.1 MP4 worker thread 已交付)
> **当前基线**: G.1.5 完结 (异步 GLTF + worker pool + sampler + benchmark + 失败注入) + F.0.11.6.1 (MP4 编码 worker thread)
> **目的**: 供下一个 AI 助手接手开发时，快速建立上下文并明确下一步任务优先级。

---

## 1. 核心渲染与优化任务 (Rendering & Optimization)

### ✅ [已交付 2026-05-17] Phase F.1.0 — TAAU 单 Instance 主路径
### ✅ [已交付 2026-05-17] Phase F.1.0.1 — Multi-HDR × TAAU
### ✅ [已交付 2026-05-17] Phase F.1.1 — Mipmap LOD Bias
### ✅ [已交付 2026-05-17] Phase F.0.11.5 — EXR 截图 (tinyexr)
### ✅ [已交付 2026-05-17] Phase F.0.11.7 — 多实例指定截图 (instance_id 参数)

### ✅ [已交付 2026-05-17] Phase F.0.11.6 — MP4 录屏 (FFmpeg libavcodec)
* FFmpegLib 扩展 14 个 encoder/muxer 符号 (av_opt_set / avformat_alloc_output_context2 / etc)
* `record_mp4.cpp` H.264 编码状态机 (sws RGBA→YUV420 + libx264 medium preset crf=23, B-frame 禁用)
* Lua API: `Light.Graphics.RecordMP4(path, {fps, bitrate, max_frames, frame_skip})` + `GetRecordMode()`
* demo_taau M 键演示 + smoke 6 检查点 + 3 件套文档 `docs/Phase F.0.11.6 MP4 Recording/`
* **真机要求**: lib/ 目录需要 avcodec-59.dll / avformat-59.dll / avutil-57.dll / swscale-9.dll

### ✅ [已交付 2026-05-17] Phase F.2 — 渲染架构补齐
* G1 P0: HDR TAAU 切换主动通知 8 个下游后处理 OnHDRResized(renderW, renderH)
* G2 P1: SSAO/LensFlare/LensDirt/Streak/AutoExposure 5 模块全部接入 multi-instance (4 instance + Clone), Lua 新增 30 个 binding
* G3 P1: LitBatchRenderer 加 OnHDREnabled/Disabled/Resized stub 三件套 (接口一致性)
* 文档: `docs/Phase F.2 渲染架构补齐/` 7 件套 (ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO)
* smoke: `scripts/smoke/phase_f2_multi_instance.lua` (30 binding + 6 行为通过)

### 🥈 [可选] Phase F.1.2 — Velocity Nearest-Filter
仅当 F.1.1 真机测试显示 ghost 严重时启用.

---

## 2. 非渲染基础架构与引擎优化 (Non-Rendering & Infrastructure)

### 1. 异步资源加载 (Async Asset Management)
* **现状**: 模型的加载 (Assimp) 和纹理的加载 (stb_image) 通常是同步阻塞的, 在加载大场景时会导致主线程卡顿。
* **需求**:
  - 实现基于线程池的后台文件 I/O 与解码。
  - OpenGL 纹理/VBO 的上传需要同步回主线程 (或利用 OpenGL Shared Context 在后台线程上传)。

### 2. 内存与显存管理优化 (Memory & VRAM Profiling)
* **需求**:
  - 补充引擎级的显存 (VRAM) 追踪。目前动态创建的 FBO、RT (特别是多实例 HDR、TAA 历史帧、Dilation pass) 占用了大量显存。需要一套 API `Light.Graphics.GetMemoryStats()` 返回 VRAM 使用量。
  - Lua 垃圾回收 (GC) 与 C++ 侧资源的生命周期绑定强化。确保无主资源被及时 `glDeleteTextures`。

### 3. 多线程与物理逻辑分离 (Tick vs Render)
* **需求**:
  - 游戏逻辑帧 (Tick, 比如 60Hz) 与渲染帧 (Render, 不限帧或 VSync) 解耦。
  - 这对于未来的插帧 (Frame Generation) 或者网络同步是必须的架构基础。

### 4. Lua API 健壮性与类型检查加强
* **现状**: 已经实现了大量的 Lua 绑定。
* **需求**:
  - 进一步完善对错误传参的 C++ 侧容错, 避免导致 Engine Crash。
  - 完善 Lua 端的热重载 (Hot Reload) 机制, 不仅仅是 LUT 的热重载, 还包括 Lua 脚本本身逻辑的热重载。

### ✅ [已交付 2026-05-18] Phase F.0.11.6.1 — MP4 录屏 Worker Thread 编码 + A1 Ring Buffer
* `record_mp4.cpp` 加 worker thread + queue + back-pressure (commit `d506ad7`)
* A1: ring buffer + zero-copy `AcquireWriteSlot/CommitWriteSlot` API (commit `8bdd888`)
* 主线程帧时 25-40ms → ~3-5ms (降低 85-90%, A1 在 worker 基础上再降 30%)
* API 完全兼容 (Lua 不感知); encoder state 严格 worker thread 独占
* 文档: `docs/Phase F.0.11.6.1 MP4 Worker/FINAL_PhaseF_0_11_6_1.md`
* 后续候选: PBO async readback 接入 mp4 (A2) / NVENC 硬编 (A3) / CancelWriteSlot (A4)

---

## 3. 交接给新 AI 的启动指令建议

> "请阅读 `docs/HANDOFF_REMAINING_TASKS.md` 了解目前引擎的进度。当前基线是 Phase F.2 (渲染架构补齐完结). 渲染管线方向已闭环 (10 后处理多实例 + TAAU 全联动 + 截图录屏). 下一步根据用户需求选择: (a) 非渲染基础架构 (async asset / VRAM tracking / Tick-Render 解耦 / Lua 健壮性 + 热重载), (b) F.1.2 Velocity nearest-filter, (c) F.0.11.6 worker thread 编码, (d) 其他用户特定需求. 请先确认用户优先级."
