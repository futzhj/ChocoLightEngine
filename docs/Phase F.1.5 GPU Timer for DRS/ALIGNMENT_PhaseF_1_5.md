# Phase F.1.5 GPU Timer for DRS — ALIGNMENT 文档

> **阶段**: 6A Workflow — 阶段 1 Align (项目上下文 + 边界确认)
> **基线**: TODO_PhaseF_1_4.md §3.1 / 用户已选 "最务实: 全平台 + 整帧粒度"
> **日期**: 2026-05-19

---

## 1. 原始需求

> 整合真实 GPU profiler, 解决 F.1.4 TODO §1.2 主循环耦合问题: avgFrameTimeMs 包含整个主循环 (Lua tick / draw / TAA / 后处理 / SwapBuffer), 不能精确归因到 GPU. 用户场景下 Lua 逻辑很重时, DRS 可能误判为 GPU 瓶颈而无效降画质.

**目标**: 让 DRS 决策基于真实 GPU 时间 (而非 CPU 主循环时间), 提升判定精度.

---

## 2. 项目上下文分析

### 2.1 现有技术栈

- **图形 API**: OpenGL 3.3 Core (桌面 Windows/Linux/macOS) + GLES 3.0 (Android/iOS) + WebGL 2.0 (Web)
- **Backend 抽象**: `RenderBackend` 基类 (`@e:\jinyiNew\Light\ChocoLight\include\render_backend.h`) + `GL33Backend` (`@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:3525`) + `LegacyGLBackend` (`@e:\jinyiNew\Light\ChocoLight\src\render_legacy.cpp:53`)
- **TAA 模块**: `TAARenderer` 命名空间 (`@e:\jinyiNew\Light\ChocoLight\src\taa_renderer.cpp`) — 严格通过 backend 接口操作, 零 GL 直接依赖
- **DRS 现状 (F.1.4)**: per-instance 滑动窗口 + cooldown + hysteresis, 用 CPU dt 决策 (`drsFrameTimes[120]` ringbuffer)

### 2.2 现有 capability 模式

项目使用 "capability bit + virtual no-op" 模式:
- **Phase AW GPUSkinning**: `gpuSkinningSupported` bit + `SupportsGPUSkinning()` virtual; 失败时 fallback CPU
- **Phase F.0 TAA**: `taaSupported` bit + `SupportsTAA()` virtual; legacy backend 永 false
- **Phase G.1.5 PBO async**: 双 PBO ping-pong (`m_pbo_pending[2]`) — 与 GPU timer query async readback 模式相同

新模块 F.1.5 应模仿这套模式.

### 2.3 已有 BeginFrame/EndFrame hook

```cpp
@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp:7483-7492
void BeginFrame(float cr, float cg, float cb, float ca) override { ... }
void EndFrame() override { ... }
```

这是天然的整帧 timer 包裹点, 无需新增 hook.

### 2.4 平台 GL 头文件路径 (已在 render_gl33.cpp 顶部处理)

```cpp
#if defined(__EMSCRIPTEN__)
  #include <GLES3/gl3.h>
#elif defined(__ANDROID__)
  #include <GLES3/gl3.h>
#elif defined(CHOCO_PLATFORM_IOS)
  #include <OpenGLES/ES3/gl.h>
#else  // 桌面
  #include <glad/gl.h>
#endif
```

---

## 3. 任务范围与边界

### 3.1 IN-SCOPE (本阶段必做)

1. **Backend 虚接口扩展** (`RenderBackend` 基类):
   - `SupportsGpuTimer() const` — capability bit (默认 false)
   - `BeginGpuTimer()` — 整帧 begin (BeginFrame 自动调或外部显式调)
   - `EndGpuTimer()` — 整帧 end
   - `PollGpuTimer(double* outMs) -> bool` — 异步 poll, 数据未到 return false
2. **GL33Backend 实现**:
   - `GL_TIMESTAMP` query (GL3.3 core / GLES3 + EXT_disjoint_timer_query / WebGL2 + EXT_disjoint_timer_query_webgl2)
   - 双 query ping-pong (避免 stall, 当帧 issue + 上帧 poll)
   - 运行时 ext 探测 + capability bit
3. **TAARenderer 集成**:
   - `State` 加 GPU 时间相关字段 (gpuFrameTimeMs, lastSource enum)
   - `UpdateDRS` 决策路径升级: 优先 GPU (有效) → CPU dt (现有) fallback
   - 新 stats 字段 in `GetDynamicStats`
4. **smoke + demo + 6A 文档**

### 3.2 OUT-OF-SCOPE (本阶段不做)

- ❌ NSight Aftermath / RenderDoc API 整合 (会引入外部 SDK 依赖, 跨平台困难)
- ❌ Per-pass 拆分 (TAA pass / sharpen / blit 各自 timer) — 用户选择 "整帧粒度"
- ❌ Vulkan / Metal backend 支持 (项目当前仅用 GL/GLES)
- ❌ 实时 GPU profiler UI (留 Phase F.1.6+, 与 ImGui 整合)

### 3.3 不变量 / 约束

- **零回归**: 默认 `gpuTimerEnabled=false` (即 DRS 仍走 CPU 路径), 不调 `SetDynamicEnabled(true)` 时与 F.1.4 完全一致
- **TAARenderer 零 GL 依赖**: 必须经 `g.backend->BeginGpuTimer()` 等接口, 不直接调 `glQueryCounter`
- **跨 6 平台编译通过**: Windows/Linux/macOS/Android/iOS/Web 全部 PASS (CI 强制)
- **Async readback 不 stall**: 必须用双 query ping-pong, 当帧 issue, N+1 帧 poll
- **Backend 不支持时静默 fallback**: `SupportsGpuTimer()=false` 时 DRS 自动走 CPU 路径, 用户无感

---

## 4. 需求理解

### 4.1 决策路径升级 (UpdateDRS)

```
当前 F.1.4:
   ratio = avgFrameTimeMs (CPU dt avg) / targetMs

升级后 F.1.5:
   if (gpuTimerSupported && gpuFrameTimeMs > 0):
       ratio = gpuFrameTimeMs / targetMs    // 优先用 GPU
       lastSource = "gpu"
   else:
       ratio = avgCpuFrameTimeMs / targetMs  // F.1.4 路径 fallback
       lastSource = "cpu"
```

### 4.2 GPU timer query async 模式

```
Frame N:
  BeginFrame()
    glQueryCounter(g_query[N%2 START], GL_TIMESTAMP)  // start of frame N
  ... draw ...
  EndFrame()
    glQueryCounter(g_query[N%2 END], GL_TIMESTAMP)    // end of frame N

Frame N+1 (poll prev frame):
  if glGetQueryObjectiv(g_query[(N-1)%2 END], GL_QUERY_RESULT_AVAILABLE):
      uint64_t t0, t1;
      glGetQueryObjectui64v(g_query[(N-1)%2 START], GL_QUERY_RESULT, &t0);
      glGetQueryObjectui64v(g_query[(N-1)%2 END],   GL_QUERY_RESULT, &t1);
      gpuFrameTimeMs = (t1 - t0) / 1e6;     // GL_TIMESTAMP returns nanoseconds
```

### 4.3 平台兼容矩阵 (已知约束)

| 平台 | GL 版本 | GPU Timer Query 支持 | 处理方式 |
|------|---------|---------------------|----------|
| Windows (Desktop) | GL3.3+ Core | ✅ ARB_timer_query (GL3.3 core) | 完整支持 |
| Linux (Desktop) | GL3.3+ Core | ✅ ARB_timer_query (GL3.3 core) | 完整支持 |
| macOS (Desktop) | GL4.1 | ✅ ARB_timer_query (GL3.3 core) | 完整支持 |
| Android | GLES 3.0+ | ⚠️ EXT_disjoint_timer_query (运行时探测) | 有则启用, 否则 CPU fallback |
| iOS | GLES 3.0 | ⚠️ EXT_disjoint_timer_query 罕见 | 大概率 CPU fallback (Apple 早就推 Metal) |
| Web | WebGL2 | ⚠️ EXT_disjoint_timer_query_webgl2 (用户激活) | 有则启用, 否则 CPU fallback |

**结论**: 核心实现走 GL 标准 query, 移动端 + Web 用 ext 字符串探测; 不支持时静默 fallback CPU.

---

## 5. 关键决策点 (智能决策记录)

| 决策点 | 选择 | 理由 |
|--------|------|------|
| Profiler 库 | GL 标准 GL_TIMESTAMP query | 无外部依赖; 跨 6 平台用 ext fallback 路径 |
| Timer 粒度 | 整帧 (BeginFrame → EndFrame) | 用户选定; 简单可靠; F.1.6+ 可拆 pass |
| Async 模式 | 双 query ping-pong | 避免 stall; 与 PBO async readback 同模式 |
| Capability 模式 | 模仿 SupportsTAA / SupportsGPUSkinning | 项目惯例; 一行 if 即 capability gate |
| Fallback 策略 | 静默 fallback 到 F.1.4 CPU 路径 | 零回归; 用户无感; stats 表通过 source 字段透露 |
| 不支持时 stats | gpuFrameTimeMs=0, source="cpu" | 明确语义; 调用方可识别 |
| 移动端 ext 探测 | 用 glGetString(GL_EXTENSIONS) substring 查 EXT_disjoint_timer_query | 标准做法 |
| WebGL2 ext | 用 emscripten ctx attribute / SDL_GL ext query | 同移动端路径 |
| LegacyBackend | 不实现, 默认基类 no-op (return false) | 不阻塞老 GL2 设备 |

---

## 6. 疑问澄清 / 已自动决策

### 6.1 已决策项 (基于项目惯例 + 业界标准)

- ✅ Backend 接口名: `Begin/End/PollGpuTimer` (动宾, 与 `BeginFrame/DrawTAAPass` 一致)
- ✅ 数据返回单位: 毫秒 (与 F.1.4 `avgFrameTimeMs` 一致, 避免 unit conversion 错误)
- ✅ 决策周期: 与 F.1.4 滑动窗口同 (push GPU 时间到现有 ringbuffer; 不另起 GPU 专用窗口)
- ✅ Stats source 字段类型: `const char*` ("cpu" / "gpu" / "none") — 与现有 `GetUpscalePreset()` 模式一致
- ✅ Disjoint event 处理: GLES3 query 数据可能因 GPU clock disjoint 失效 — 检测 `GL_GPU_DISJOINT_EXT` 时丢弃 + 重置

### 6.2 待用户确认 (无, 已通过用户预选确认)

无. 用户在前置问题已选 "最务实方案", 后续设计纯技术决策.

---

## 7. 需要的支持 / 配置

### 7.1 编译

无新增 #define / CMake 标志. F.1.5 编入 (默认 capability=false 在不支持平台).

### 7.2 SDK / 库依赖

无外部依赖. 仅用 OpenGL 标准 query.

### 7.3 Header

无新增. 沿用 render_gl33.cpp 已有的 `<glad/gl.h> / <GLES3/gl3.h> / <OpenGLES/ES3/gl.h>` 路径.

### 7.4 配置文件

无.

---

## 8. 文档版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — 用户预选 "全平台 + 整帧粒度" 后启动 |
