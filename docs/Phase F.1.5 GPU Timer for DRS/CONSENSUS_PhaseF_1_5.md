# Phase F.1.5 GPU Timer for DRS — CONSENSUS 文档

> **阶段**: 6A Workflow — 阶段 1 Align (最终共识)
> **基线**: ALIGNMENT_PhaseF_1_5.md

---

## 1. 明确需求描述

为 F.1.4 DRS 决策提供真实 GPU 帧时间, 替代当前包含整个主循环的 CPU `dt`. 跨 6 平台支持: 桌面 (Windows/Linux/macOS) GL3.3 core 必有; 移动端/Web 运行时探测 ext, 不支持时静默 fallback CPU.

---

## 2. 技术实现方案 (高层)

### 2.1 新增 4 个 backend 虚接口 (`render_backend.h`)

```cpp
// 默认实现全部 no-op (legacy / 不支持平台静默退化)
virtual bool SupportsGpuTimer() const { return false; }
virtual void BeginGpuTimer() {}
virtual void EndGpuTimer() {}
virtual bool PollGpuTimer(double* outMs) {
    if (outMs) *outMs = 0.0;
    return false;
}
```

### 2.2 GL33Backend 实现要点

- 双 query ping-pong: `m_gpuTimer_query[2][2]` (2 帧 × start/end)
- 运行时探测 + capability bit: `m_gpuTimerSupported`
- BeginFrame 自动调 BeginGpuTimer (透明) — 用户无感
- EndFrame 自动调 EndGpuTimer
- PollGpuTimer 在 TAARenderer Process 路径或 UpdateDRS 时显式调

### 2.3 TAARenderer State 字段扩展 (3 字段)

```cpp
// Phase F.1.5 — GPU timer for DRS
double  drsGpuFrameTimeMs   = 0.0;     // 上一帧 GPU 时间 (ms); 0 = 未取到
int     drsLastSource       = 0;       // 0=none / 1=cpu / 2=gpu
bool    drsPreferGpuSource  = true;    // 用户开关 (默认 true; 关后强制 CPU 路径)
```

### 2.4 UpdateDRS 决策路径升级

```cpp
// F.1.5 优先级链
double srcMs = 0.0;
const char* sourceName = "none";
if (g.drsPreferGpuSource && g.backend && g.backend->SupportsGpuTimer()) {
    if (g.backend->PollGpuTimer(&srcMs) && srcMs > 0) {
        g.drsGpuFrameTimeMs = srcMs;
        g.drsLastSource = 2;
        sourceName = "gpu";
    }
}
if (g.drsLastSource != 2) {
    srcMs = drsAvgFrameTimeMs_();      // F.1.4 路径 fallback
    g.drsLastSource = (srcMs > 0) ? 1 : 0;
    sourceName = (srcMs > 0) ? "cpu" : "none";
}
// 后续决策不变 (ratio = srcMs / targetMs)
```

---

## 3. 验收标准

### 3.1 API 完整性 ✅

| API | 类型 | 说明 |
|-----|------|------|
| `RenderBackend::SupportsGpuTimer()` | C++ virtual | 默认 false |
| `RenderBackend::BeginGpuTimer()` | C++ virtual | 默认 no-op |
| `RenderBackend::EndGpuTimer()` | C++ virtual | 默认 no-op |
| `RenderBackend::PollGpuTimer(double*)` | C++ virtual | 默认 false |
| `GL33Backend::*` 4 个 override | C++ override | 真实 GL_TIMESTAMP query |
| `Light.Graphics.TAA.SetPreferGpuSource(bool)` | Lua | 用户开关 |
| `Light.Graphics.TAA.GetPreferGpuSource()` | Lua | 查询 |
| `Light.Graphics.TAA.GetDynamicStats()` 扩 2 字段 | Lua | gpuFrameTimeMs / source |

### 3.2 默认值 ✅

| 字段 | 默认 |
|------|------|
| backend supports gpuTimer (legacy/不支持平台) | false |
| backend supports gpuTimer (GL3.3+ 桌面) | true |
| TAARenderer drsPreferGpuSource | true |
| TAARenderer drsGpuFrameTimeMs | 0.0 |
| TAARenderer drsLastSource | 0 (none) |

### 3.3 功能验证 ✅

| 项 | 验证 |
|----|------|
| 桌面平台 GPU timer 启用, source="gpu" | smoke §15 / 实机 demo |
| 不支持平台静默 fallback, source="cpu" | smoke §15 (mock backend) |
| F.1.4 行为零回归 (drsPreferGpuSource=false 强制 CPU) | smoke §15 |
| 双 query ping-pong 不 stall | 性能测试 (帧率不应下降 > 1%) |
| Disjoint event 处理 (GPU clock 跳变) | GLES 实机验证 / 不可在 headless 测 |
| GetDynamicStats 新增 2 字段 (gpuFrameTimeMs / source) | smoke §15 字段检查 |
| Multi-instance 隔离 (每 instance 独立 source) | smoke §15 / per-instance state |

### 3.4 性能要求 ✅

| 项 | 目标 |
|----|------|
| 帧时间开销 (GL_TIMESTAMP query) | < 0.05 ms (GL native, sub-microsecond) |
| GL fence stall 风险 | 0 (双 query ping-pong, N+1 帧 poll) |
| 内存增量 | < 64 byte/instance (3 字段 + 4 GL query handle in backend) |

### 3.5 类型 / 边界

| 项 | 处理 |
|----|------|
| `Set/PollGpuTimer` outMs=nullptr | safely return false |
| 同帧多次调 BeginGpuTimer | 后调覆盖前调 (driver 行为) |
| 不调 EndGpuTimer 直接 Poll | return false (query 未结束) |
| Disjoint=true 时 poll | 丢弃此次结果 + return false |

### 3.6 CI 与文档

| 项 | 标准 |
|----|------|
| CI 6 平台全绿 | Windows/Linux/macOS/Android/iOS/Web |
| smoke §15 覆盖 6+ 子检查点 | 见 TASK |
| demo_taau HUD 显示 source | 用户可视 |
| 6A 文档 7 件套 | ALIGN/CONS/DESIGN/TASK/ACCEPT/FINAL/TODO |

---

## 4. 任务边界

### 4.1 IN-SCOPE

- Backend 4 虚接口 + GL33Backend 实现
- TAARenderer 3 State 字段 + UpdateDRS 升级
- Lua bridge 3 新 API (SetPreferGpuSource / GetPreferGpuSource / 扩展 GetDynamicStats)
- smoke §15
- demo HUD source 行
- 6A 7 件套文档

### 4.2 OUT-OF-SCOPE (留 F.1.6+)

- NSight Aftermath / RenderDoc API
- Per-pass 拆分计时
- Vulkan / Metal backend
- 实时 GPU profiler UI

---

## 5. 不变量

- 默认行为零回归 (drsPreferGpuSource=true 但 backend 不支持时 = F.1.4 CPU 路径)
- TAARenderer 不直接调 GL (经 backend)
- 跨 6 平台编译 PASS

---

## 版本历史

| v1.0 | 2026-05-19 | 初稿 |
