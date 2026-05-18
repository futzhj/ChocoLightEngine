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
  - ✅ [Phase G.1 已交付] 补充引擎级的显存 (VRAM) 追踪 — `Light.Graphics.GetMemoryStats()` / `ResetMemoryStats()` (commit `ef91120` + `a544fcf`)
  - ✅ [Phase G.1.1 已交付] Bloom / SSAO / AE / TAAU outputSceneTex 跟踪 (commit `fe916c3`)
  - **遗留 v1.2 P1**: 用户 Image / ImageData / Mesh / Font 跟踪 (见 `docs/Phase G.1 VRAM Tracking/TODO_PhaseG_1.md`)
  - **遗留**: Lua 垃圾回收 (GC) 与 C++ 侧资源的生命周期绑定强化。确保无主资源被及时 `glDeleteTextures`。

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

### ✅ [已交付 2026-05-18] Phase F.0.11.6.3.A9 — MP4 ROI 录屏
* **A9 ROI**: `Gfx.RecordMP4(path, {roi={x,y,w,h}})` — 仅录屏幕指定矩形区域, mp4 输出尺寸 = ROI 尺寸
* 坐标系: 屏幕左上原点 (Lua 友好), 内部转 GL 左下原点; libx264 偶数对齐自动 round down
* 越界保护: Open 时早失败 + 运行中窗口缩小 → CancelWriteSlot + warn (帧序不断)
* commit `3bb7d3b` (2 files +106/-5); smoke +4 用例 → screenshot 65 PASS
* 实现策略比预估简单 (~1.5h): 不改 backend, 仅在 light_graphics 层加 ROI 字段 + helper

### ✅ [已交付 2026-05-18] Phase F.0.11.6.4.A14 — MP4 GIF 录屏
* **A14 GIF**: `Gfx.RecordMP4("anim.gif", {fps=15})` 或 `Gfx.RecordGIF("demo.gif", ...)` — 后缀 .gif 自动切到 GIF encoder + BGR8 pix_fmt
* 复用 worker thread / ring buffer / Pause/Resume / GetStats 全部基础设施
* GIF muxer + GIF encoder + sws RGBA→BGR8 (256 色固定调色板, sws 内部 dither, 无需 palettegen)
* GIF 模式忽略 encoder pref / GOP / bitrate / BT.709 (gif 是无损 LZW + RGB direct)
* commit `4003414` (3 files +109/-35); smoke +6 用例 → screenshot 71 PASS

### ✅ [已交付 2026-05-18] Phase F.0.11.6.5.A13 — MP4 Audio Capture (Windows WASAPI loopback + AAC)
* **A13 Audio**: `Gfx.RecordMP4(path, {audio=true})` — 录系统扬声器输出 (loopback) 并 mux 进 mp4
* Windows WASAPI loopback (默认 render 设备共享模式) → ring buffer (16MB SPSC) → audio_thread → swr → AAC FLTP → mp4 audio stream
* 跨平台桩 (macOS/Linux 静默 disable audio, video 仍正常录)
* 任一 audio 步骤失败仅 disable audio, 不影响 video (健壮性)
* GetRecordStats 加 audio_enabled / audio_frames / audio_sample_rate / audio_channels / audio_dropped 5 字段
* commit `9000011` (5 files +686/-13); smoke +10 用例 → screenshot 81 PASS
* **未真机验收**: 用户需自行验证音质 / 同步 / AAC encoder 可用性 (作者无音频测试环境)
* 文档: `docs/Phase F.0.11.6.5 MP4 Audio Capture/FINAL_PhaseF_0_11_6_5.md`

### ✅ [已交付 2026-05-18] Phase G.0 — Lua 脚本热重载 (Light.Reload + lumen RestartScript)
* **新模块 Light.Reload (12 API)**: Module / File / Preserve / ResetState / WatchModule / UnwatchModule / SetErrorHandler / GetLastError / Stats / Clear / RestartScript / IsRestartPending
* **lumen-master 主入口热重启**: pMain 加 restart 循环, ChocoLight 通过 GetProcAddress 反查 light.exe 导出符号 (LUMEN_EXPORT)
* **状态保留**: `Preserve(key, factory)` 用 Lua registry 持 state, reload 后 angle/frame 等保留
* **错误恢复**: reload 失败保留老 package.loaded + SetErrorHandler hook + GetLastError 查询
* **demo_hot_reload 示例**: 旋转方块 + 在线改色 + 修 syntax error 不挂 demo
* smoke `reload.lua` **41 PASS / 0 FAIL** + `reload_restart_e2e.lua` 端到端 PASS, 全 8 套 smoke 零退化
* lumen 加载模块数 97 → 98
* 文档: `docs/Phase G.0 Lua Hot Reload/{ALIGNMENT,DESIGN,FINAL}_PhaseG_0.md`

### ✅ [已交付 2026-05-18] Phase G.1 — VRAM Tracking (Light.Graphics.GetMemoryStats + ResetMemoryStats)
* **新模块 LT::GpuMem** (`light_gpumem.cpp` ~260 LOC): 引擎自计 GPU 显存占用, 静态 64-slot 数组, 不依赖 OS API, 跨平台一致
* **跟踪范围 v1**: 高层 wrapper (HDR/TAA/SSR/Dilate/UBO Skin), 5 类高价值 RT
  - HDR FBO 5 组件: sceneTex (RGBA16F) + normalTex (RG16F) + velocityTex (RG16F/RG8) + cameraVelocityTex + depthRBO (DEPTH24)
  - TAA history × 2 (RGBA16F ping-pong)
  - SSR: depthTex (DEPTH32F) + reflectTex + blur×2 + history×2 (全 RGBA16F)
  - Velocity Dilate (combined + camera, RG16F/RG8)
  - UBO Skin: joints + prev (4096 B 各)
* **Lua API**: `Light.Graphics.GetMemoryStats()` → `{total_bytes, render_targets={count,bytes}, ubos={count,bytes}, items={{name,format,count,bytes,w,h}, ...}}` + `ResetMemoryStats()`
* **多 instance 友好**: `count` 自动累加 (split-screen HDR×4 显示 sceneTex ×4)
* commit `ef91120` (12 files +1084) + `a544fcf` (FINAL+TODO docs)
* smoke `gpumem.lua` **13/13 PASS** (headless graceful skip), CI 全 6 平台绿
* 文档: `docs/Phase G.1 VRAM Tracking/{ALIGNMENT,DESIGN,FINAL,TODO}_PhaseG_1.md`

### ✅ [已交付 2026-05-18] Phase G.1.1 — VRAM Tracking v1.1 (Bloom + SSAO + AE + TAAU)
* **闭合 G.1 P0 TODO 4 项**: 复用 `LT::GpuMem`, 零架构变更, 零新 API
* **新增跟踪 4 类 (9 unique items)**:
  - Bloom pyramid (RGBA16F per-level, 5 levels @ 1080p ≈ 22 MB)
  - SSAO depthTex (DEPTH24 full-res) + AO×2 (R16F half-res)
  - AE luminance (R16F base 级, mipmap 简化不计)
  - HDR outputSceneTex (TAAU, RGBA16F at outputW×outputH) — G.1 漏 Track 修复
* **hook 算法严格对齐 backend**: Bloom mip 递推与 `render_gl33.cpp:5688-5689` 完全一致; SSAO depthTex 用 DEPTH24 (与 SSR DEPTH32F 区分)
* **多 instance 友好**: Bloom 4 + AE 4 instance, count 自然累加
* commit `fe916c3` (6 files +308 LOC, 含 ALIGNMENT + FINAL)
* smoke `gpumem.lua` **14/14 PASS** (G.1 13 + G.1.1 +1), CI 全 6 平台绿
* 实绩: 估时 1.8h, 实际 1.6h (-11%)
* 文档: `docs/Phase G.1.1 VRAM Tracking v1.1/{ALIGNMENT,FINAL}_PhaseG_1_1.md`

---

## 3. 下一步候选方向 (Phase G.1.1 收尾后)

### 选项 A — 非渲染基础架构 (推荐, 长期价值高)
1. ~~**VRAM tracking + `Light.Graphics.GetMemoryStats()`**~~: ✅ 已交付 Phase G.1
2. **Tick-Render 解耦**: 60Hz 逻辑 + VSync 渲染, 为未来插帧 / 网络同步铺路 (估时 8-15h, 架构级)
3. ~~**Lua 热重载**: 不仅 LUT, 包括脚本逻辑热重载~~: ✅ 已交付 Phase G.0
4. **Lua API 容错 audit**: 错误传参不应 crash, 全 API audit + nil+err 返回 (估时 4-6h, **推荐顺序处理优先**)
5. ~~**Bloom mipmap / SSAO / AE / TAAU**~~: ✅ 已交付 Phase G.1.1
6. **VRAM v1.2**: 用户 Image / Mesh / Font 跟踪 (估时 5h, 见 G.1 TODO P1)
7. **Async Asset Management**: 异步资源加载 (Phase 2 §1, 大场景加载不卡顿, 估时 6-10h)

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
