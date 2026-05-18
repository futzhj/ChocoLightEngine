# Phase F.1.5 GPU Timer for DRS — TODO 待办与配置

> **目的**: 列出本阶段交付完成后, 仍待用户/团队跟进的未尽事宜与可选增强.
> **基线**: 阶段已完整交付; 以下均为"非阻塞"的后续优化方向.

---

## 1. 用户配置项 — 无 (零配置)

本阶段不引入任何新的:
- 环境变量
- API 密钥
- 配置文件 (.env / config.json)
- 构建参数
- 平台依赖

桌面平台**自动启用** GPU timer (开箱即用); 移动端/Web 自动 fallback CPU 路径. 无需用户操作.

---

## 2. 推荐试用 / 验证 (用户操作)

### 2.1 实机验证 GPU timer (Windows)

```powershell
# 1. 拉取最新代码并构建
git pull
cmake --build build --config Release

# 2. 运行 demo_taau, 观察 HUD
.\build\samples\demo_taau\Release\demo_taau.exe

# 3. 在 demo 内按键:
#    N : 启用 DRS
#    G : 切换 GPU/CPU 源
#    F : 设置目标 FPS
#    观察 HUD "Source:" 行显示 "gpu" / "cpu"
```

**期待**: 桌面 (NVIDIA / AMD / Intel) 显示 `Source: gpu (prefer=gpu) gpuMs=2.34`.

### 2.2 实机验证 fallback (Android / iOS / Web)

```bash
# Android
.\gradlew assembleRelease
adb install -r build/.../demo_taau.apk

# iOS
xcodebuild -project ChocoLight.xcodeproj -target demo_taau

# Web
emcmake cmake -B build-emcc; cmake --build build-emcc
# 浏览器打开 build-emcc/samples/demo_taau/demo_taau.html
```

**期待**: 这 3 平台显示 `Source: cpu (prefer=gpu)` (preferGpuSource=true 但 backend 不支持, 静默 fallback CPU).

### 2.3 Lua 用户脚本集成示例

```lua
local TAA = Light.Graphics.TAA

-- 启用 DRS (GPU timer 自动启用, 桌面默认)
TAA.SetDynamicEnabled(true)
TAA.SetDynamicTarget(60)

-- 调试时可以强制 CPU 路径对比效果
-- TAA.SetPreferGpuSource(false)

-- 每帧驱动 (在 OnUpdate 内)
function OnUpdate(dt)
    TAA.UpdateDRS(dt)

    -- 监控决策源
    local s = TAA.GetDynamicStats()
    if s.enabled then
        print(string.format("DRS: scale=%.2f source=%s gpuMs=%.2f",
              s.currentScale, s.source, s.gpuFrameTimeMs))
    end
end
```

---

## 3. 已知限制 (设计权衡, 不影响主流程)

### 3.1 GLES3 / WebGL2 不启用 GPU timer

**原因**: `glQueryCounterEXT` / `glGetQueryObjectui64vEXT` 是 EXT 扩展函数, 需 `eglGetProcAddress` 加载. 当前实现为简化跨平台构建, 在 GLES3 路径直接 `m_gpuTimerSupported=false`.

**影响**: 移动端 + Web 用 CPU 路径 (零回归; 与 F.1.4 行为相同).

**绕过**: 见 §4.1 "未来增强" 的 EXT 加载方案.

### 3.2 整帧粒度 (不支持 sub-frame region timer)

**原因**: 用户在 ALIGNMENT 阶段选定 "整帧" 粒度.

**影响**: 不能精确测量 Pre-pass / TAA / Post / Blit 各 stage 时间; 但对 DRS 整体决策足够.

**绕过**: 见 §4.2 "Region 级别 timer".

### 3.3 滞后 1-2 帧

**原因**: 双 query ping-pong 本质特征; PollGpuTimer 取 N-1 或 N-2 帧的 GPU 时间.

**影响**: 对 DRS 决策几乎无影响 (cooldown=60 帧远大于此).

**绕过**: 不必绕过; 业界标准做法.

---

## 4. 未来增强候选 (按优先级)

### 4.1 GLES3 / WebGL2 EXT 函数加载 (~4-6h, 中优先)

**方案**:
```cpp
// render_gl33.cpp 内补充
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
    // 通过 eglGetProcAddress / SDL_GL_GetProcAddress 加载 EXT 函数
    static auto pfnGlQueryCounterEXT = (PFNGLQUERYCOUNTEREXTPROC)
        eglGetProcAddress("glQueryCounterEXT");
    static auto pfnGlGetQueryObjectui64vEXT = (PFNGLGETQUERYOBJECTUI64VEXTPROC)
        eglGetProcAddress("glGetQueryObjectui64vEXT");
    // ...
    if (pfnGlQueryCounterEXT && pfnGlGetQueryObjectui64vEXT) {
        // 启用 GPU timer
    }
#endif
```

**收益**: 移动端 + Web 全平台启用 GPU timer.

**风险**: Android driver 兼容性测试增加; 需 disjoint event 处理.

### 4.2 Region 级别 timer (~6-8h, 中优先)

**方案**: 扩展 RenderBackend 加 `BeginGpuRegion(name)` / `EndGpuRegion()`, 用 query pool 测量子区段 (Pre-pass / TAA / Post).

**收益**: 性能 profiler 可视化; 帮助找性能瓶颈.

**用例**: 开发者 HUD 显示各 stage 时间百分比.

### 4.3 GPU timer histogram (~1-2h, 低优先)

**方案**: 在 dev HUD 加 60 帧 GPU 时间环形 buffer, 绘制 sparkline 图.

**收益**: 视觉化 GPU 时间波动; 帮助调优 DRS 参数.

### 4.4 Disjoint event 详细日志 (~2-3h, 低优先)

**方案**: GLES3 路径若启用 EXT timer (需 4.1 完成), 增 disjoint event 检测 + 详细 log (报告 driver bug 时有用).

---

## 5. 不会做的事 (Out of Scope)

- ❌ Vulkan / Metal / DX12 backend (当前引擎仅 GL backend)
- ❌ 第三方 profiler 集成 (Tracy / RGP / RenderDoc) — 用户可单独接入
- ❌ Real-time GPU memory tracking — 不属于 timer 范畴
- ❌ Cross-thread profiler — 引擎 single-threaded GL

---

## 6. 文档状态

| 文档 | 状态 | 更新日期 |
|------|------|---------|
| ALIGNMENT_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| CONSENSUS_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| DESIGN_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| TASK_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| ACCEPTANCE_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| FINAL_PhaseF_1_5.md | ✅ 完成 | 2026-05-19 |
| TODO_PhaseF_1_5.md | ✅ 完成 (本文) | 2026-05-19 |

7 件套齐全, 6A 工作流 100% 完整执行.

---

## 7. 联系方式 / 问题反馈

如发现:
- 桌面平台 GPU timer 行为异常 (e.g., 时间值明显错误 / driver crash)
- 移动端 / Web 不再 silent fallback (i.e., 报错 / panic)
- DRS 决策行为偏离 F.1.4 (回归问题)

请按 issue 模板提交至 [GitHub Issues](https://github.com/futzhj/ChocoLightEngine/issues) — 标 `phase-f.1.5`.

---

## 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — 7 件套收尾 |
