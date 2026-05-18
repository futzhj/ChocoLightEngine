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

### ✅ [已交付 2026-05-18] Phase F.0.11.6.1 — MP4 录屏 Worker + A1~A7 全套优化
* F.0.11.6.1: worker thread + queue + back-pressure (commit `d506ad7`)
* A1: ring buffer + zero-copy `AcquireWriteSlot/CommitWriteSlot` (commit `8bdd888`)
* **A4**: `CancelWriteSlot` API — Readback 失败精确取消, 不写坏帧
* **A3**: NVENC 硬件编码优先 (libx264/AMF fallback), `preset='p4' rc='cbr' tune='hq'`
* **A2**: PBO async readback 接入 mp4 (use_async=true), 主线程不 stall GPU (commit `81789dd`)
* **A5**: Lua opts 显式 encoder pref (`encoder=...` / `prefer_hwenc=...`) — 用户可强制软编/硬编
* **A6**: BT.709 color metadata (`colorspace/primaries/trc/range`) — 主流播放器颜色一致
* **A7**: REC OSD 录屏指示器 (`DrawRecordOSD` 在 readback 之后绘制, 不进 mp4) + `Set/GetRecordOSD` API (commit `0e5487b` + `9b0507f`)
* **累计性能**: 主线程帧时 25-40ms → ~0.5-2ms (~95% 降幅, 软编/硬编都达到)
* API 完全兼容 (Lua 不感知); encoder state 严格 worker thread 独占
* 文档: `docs/Phase F.0.11.6.1 MP4 Worker/FINAL_PhaseF_0_11_6_1.md` (含十一章全部交付细节)

### ✅ [已交付 2026-05-18] Phase F.0.11.6.2 — MP4 录屏功能扩展 (A8 + A10 + A11 + A12)
* **A8 GOP / 关键帧间隔**: `RecordMP4(path, {gop_size=N})` — N=1 全 I 帧 / N=15 高频关键帧 / 0=默认 fps×2
* **A10 Pause/Resume**: `Gfx.PauseRecord() / ResumeRecord() / IsRecordPaused()` — pts 不前进, mp4 时间线无缝衔接
* **A11 max_size_bytes**: `Gfx.SetRecordMaxSize(bytes)` — 0=无限, 不自动停 (由脚本查 stats 决定切分)
* **A12 GetRecordStats**: `Gfx.GetRecordStats()` → `{mode, active, tick_frame_count, frames, bytes, max_bytes, encoder, paused}`
* commit `2ef81ea` (4 files +341/-10)
* smoke `screenshot` 增 19 用例 → 61 PASS / 0 FAIL; 全 7 套 smoke 回归 PASS
* 文档: `docs/Phase F.0.11.6.2 MP4 Recording Ext/FINAL_PhaseF_0_11_6_2.md`

---

## 3. 下一步候选方向 (Phase F.0.11.6.1 收尾后)

### 选项 A — 非渲染基础架构 (推荐, 长期价值高)
1. **VRAM tracking + `Light.Graphics.GetMemoryStats()`**: 多 instance HDR / TAA history / Dilation RT 显存占用追踪
2. **Tick-Render 解耦**: 60Hz 逻辑 + VSync 渲染, 为未来插帧 / 网络同步铺路
3. **Lua 热重载**: 不仅 LUT, 包括脚本逻辑热重载
4. **Lua API 容错**: 错误传参不应 crash, 全 API audit + nil+err 返回

### 选项 B — 渲染管线收尾
1. **F.1.2 Velocity Nearest-Filter**: 仅当 F.1.1 真机测试 ghost 严重时启用
2. **A8+**: MP4 多轨道音频 / 字幕 / 关键帧间隔 / 录屏 ROI

### 选项 C — 工程化 / 工具链
1. CI smoke 矩阵补全 (mp4 录屏 / async loading 端到端)
2. 性能 profiling 工具集成 (Tracy / RenderDoc 接入)
3. demo 美化 (demo_taau / demo_ssr OSD 整合)

### 选项 D — 用户特定需求
按用户拍板.

---

## 4. 交接给新 AI 的启动指令建议

> "请阅读 `docs/HANDOFF_REMAINING_TASKS.md` 了解目前引擎的进度。当前基线是 **Phase F.0.11.6.1 (MP4 Worker A1~A7 全套优化交付完成)**. 渲染管线方向已闭环 (10 后处理多实例 + TAAU 全联动 + 截图录屏 + MP4 异步编码 95% 主线程降幅). 下一步参考 §3 候选方向, 请先确认用户优先级."
