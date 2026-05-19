# Phase H.0 Tick-Render 解耦 — TODO 文档

> **范围**: 本阶段 (H.0) 完成后已知的待办、配置、限制和增强候选
> **状态**: T1~T9 已完成, CI 6/6 PASS; 以下条目为后续可选项

---

## 1. 用户配置项 (无)

H.0 默认配置开箱即用, **无需任何用户配置**:
- `fixedHz=60`, `maxFixedStepsPerFrame=8`, `frameTimeClamp=0.25s`
- 物理 `autoStep=false` (老 sample 零回归)
- 老 `Window:Update(dt)` / `Window:Draw()` 仍正常工作

如希望自定义, 在 Lua 启动时调:
```lua
Light.Time.SetFixedTimestep(60)         -- 30/60/120/144 都常见
Light.Time.SetMaxFixedStepsPerFrame(8)  -- spiral guard, 默认 8
Light.Time.SetFrameTimeClamp(0.25)      -- 单帧最大 dt (s)
```

---

## 2. 推荐验证 (实机)

smoke + CI 已覆盖 API 表面 + 多平台 build, 但以下行为需实机验证:

### 2.1 60Hz 显示器 + 60Hz fixedHz
```
现象: alpha 每帧 ≈ 0; steps/frame = 1
HUD: alpha=0.0XX, steps=1
```

### 2.2 144Hz 显示器 + 60Hz fixedHz
```
现象: alpha ∈ [0, 1) 持续变化; steps/frame 在 0/1 之间交替
HUD: alpha=0.42 ... 0.83 ... 0, steps=0/1
平均: 60 step / 144 frame ≈ 0.42 step/frame
```
**直观验收点**: 左方块 (无 alpha) 看见跳动, 右方块 (有 alpha) 平滑.

### 2.3 低 FPS (锁 30 帧)
```
现象: steps/frame ≈ 2 (60/30); accumulator < fixedDt
HUD: steps=2
```

### 2.4 卡顿 (sleep 1s)
```
现象: 触发 spiral guard, 单帧吞掉 maxStep=8 step, 累积器残留;
      log: "[LT::TickRender] spiral guard ON" → "OFF"
```

### 2.5 物理 auto-step
```lua
local Physics = require "Light.Physics"
local world = setmetatable({}, {__index = Physics.World})
Physics.World(world)
world:SetAutoStep(true)             -- 启用
-- ... CreateBody / 添加 fixture ...
-- demo 内不调 world:Step, 弹球依然落地 + 反弹
```

### 2.6 demo 实机
```
cd e:\jinyiNew\Light
.\samples\demo_tick_render\main.lua    # 用引擎可执行加载
```
按键 1/2/3/4 切 fixedHz; A 切 alpha 插值; P 切 Box2D auto-step; HUD 实时显示.

---

## 3. 已知限制

### 3.1 Web 主循环走 ASYNCIFY (非 emscripten_set_main_loop)
**当前**: `light_ui.cpp` while 循环 + `emscripten_sleep` 复用方案.
**影响**: 浏览器后台标签页能耗略高; CPU 用量比 native 主循环高 5~10%.
**升级**: 见 §5.1 — 4~6h 切换到 `emscripten_set_main_loop_arg`.

### 3.2 iOS/Android 后台 pause 状态机缺失
**当前**: 移动端切到后台时, `clock` 仍走, 切回前台时 `frameTime` 可能很大 → 触发 frameTimeClamp (clamp 到 0.25s) → spiral guard log 一次, 然后正常恢复.
**影响**: 切回前台后第一帧 1 step (受 clamp 保护), 不会 crash, 但 log 可能刷一行 warn.
**升级**: 见 §5.2 — 2~3h 实现 `Pause()` / `Resume()` 清零 accumulator.

### 3.3 物理双 Step 检测缺失
**当前**: 用户启用 `world:SetAutoStep(true)` 后再调 `world:Step(dt)`, 引擎不报错 → 该 world 一帧 step 两次, 模拟速度翻倍.
**影响**: 物理 bug, 用户难定位.
**升级**: 见 §5.3 — 1h 在 `l_World_Step` 内 `if (autoStep) Log_Warn`.

### 3.4 fixedHz 极端值的实测节奏
**当前**: smoke clamp 测试覆盖 `Set(0)→1` / `Set(99999)→1000`, 但 fixedHz=1000 在低 FPS 显示器上节奏未实测.
**影响**: 理论上 maxStep=8 仍 clamp 安全, 但 step 频次需观察.
**建议**: 业务方使用 `[30, 240]` 内的常见值即可.

### 3.5 OnFixedUpdate Lua 错误中断 step 链
**当前**: `lua_pcall` 报错 → log + 跳出该次 step 循环 (consume 不变), 下一帧继续累积.
**影响**: 严重 Lua 错误时, 累积器可能爆炸 → spiral guard 介入 clamp.
**结论**: 已用 spiral guard + frameTimeClamp 双重保护, 无需改动.

---

## 4. 推荐操作指引

### 4.1 验证 6 平台 build (已 PASS)
```bash
gh run list --limit 1 --branch main
gh run view <id> --json status,conclusion,jobs
```

### 4.2 本地跑 smoke (Windows)
```powershell
cd e:\jinyiNew\Light
# 假设引擎已构建到 build\Release\Light.exe
.\build\Release\Light.exe scripts\smoke\tick_render.lua
# 期待: PASS x 30+ / "ALL TESTS PASSED"
```

### 4.3 本地跑 demo (Windows)
```powershell
.\build\Release\Light.exe samples\demo_tick_render\main.lua
# 按 1/2/3/4 切 fixedHz, A 切 alpha 插值, P 切 Box2D auto-step
```

### 4.4 集成到现有项目 (老 sample 不需改)
不需任何改动. 老 `Update(dt)` / `Draw()` 自动继续工作.

### 4.5 集成到新项目 (推荐用 OnFixedUpdate / OnRender)
```lua
local M = {}
function M:OnFixedUpdate(dt)        -- dt = 1/60 永远固定
    -- 物理 / 网络 / 决定性逻辑写这里
end
function M:OnRender(alpha, dt)      -- alpha ∈ [0, 1), dt = wall-clock
    -- 状态 lerp / 动画 / UI 写这里
end
Light(M):Open(800, 600, "My Game")
while Light.UI.Loop() do Light.UI.Resume() end
```

---

## 5. 未来增强候选 (按 ROI 排序)

### 5.1 emscripten_set_main_loop_arg 真集成 ✅ 已完成 (Phase H.0.2)
**实际**: ~1.6h. 加性 API `Light.UI.RunBrowserMainLoop`.
- Web: `emscripten_set_main_loop_arg(BrowserMainLoopFrame_, L, 0, 1)`.
- Native: 阻塞 while 等价老 `while UI.Loop() do UI.Resume() end`.
- 老 sample (不调新 API) 完全走 ASYNCIFY 路径, 零回归.
**详见**: `docs/Phase H.0.2 Web Browser Main Loop/`

### 5.2 iOS/Android pause 状态机 ✅ 已完成 (Phase H.0.3)
**实际**: ~1.5h. TickRender Pause/Resume + skipNextDt + SDL3 BG/FG 事件 hook.
- `LT::TickRender::Pause()` / `Resume()` / `IsPaused()` C++ API.
- `Light.Time.Pause` / `Resume` / `IsPaused` Lua wrapper.
- `Window:OnAppEnterBackground` / `OnAppEnterForeground` Lua callback.
- SDL_EVENT_DID_ENTER_BACKGROUND/WILL_ENTER_FOREGROUND 自动驱动 TickRender 状态.
- iOS/Android/Web (SDL3 抽象) 统一路径.
**详见**: `docs/Phase H.0.3 Pause-Resume State Machine/`

### 5.3 物理双 Step 检测 ✅ 已完成 (Phase H.0.1, commit 待确认)
**实际**: 1h. Box2D + Bullet 各自 `l_World_Step` 内插入 throttle warn (limit 3 次).
**详见**: `docs/Phase H.0.1 Tick-Render Polish/`

### 5.4 GPU 端 alpha 插值 helper ⭐⭐
**估时**: 3h
**收益**: Lua 用户少写 lerp 代码; 引擎自动管理 prev/curr Transform.
**实现要点**:
- 新增 `Light.Time.LerpTransform(prev, curr, alpha)` (返回 4x4 矩阵).
- 或 Component 系统自动维护 `Transform.prev` (类似 E.13 Motion Vector).

### 5.5 HUD overlay ✅ 已完成 (Phase H.0.1)
**实际**: 2h. `Light.Time.DrawHUD()` (Lua 实现) + Set/Get HUDEnabled/HUDPosition (C++).
**详见**: `docs/Phase H.0.1 Tick-Render Polish/`

### 5.6 实机性能基准 ⚠️ 部分完成 (Phase H.0.1 加 smoke perf budget)
**已做**: smoke §10 perf budget assertion (accumulator < fixedDt * 2).
**未做**: 60Hz / 144Hz 显示器实机直方图 (需要两台显示器手动跑, 留 dev 验证).

---

## 6. 性能监控建议

### 6.1 持续性能基线 (CI 集成)
- smoke 内加 perf budget assertion: `accumulator < fixedDt * 2` (确保 spiral guard 不长时触发).
- demo HUD 输出 JSON 到 stdout, CI 解析跌幅 > 5% 报警.

### 6.2 Production 用法
- 用户在 release 版隐藏 HUD; debug 版打开.
- 长时运行 (> 1h) 后 `accumulator` 应稳定 < `fixedDt`, 否则有泄漏.

---

## 7. 文档状态

| 6A 件 | 状态 | 文件 |
|-------|------|------|
| ALIGNMENT | ✅ | `ALIGNMENT_PhaseH_0.md` |
| CONSENSUS | ✅ | `CONSENSUS_PhaseH_0.md` |
| DESIGN | ✅ | `DESIGN_PhaseH_0.md` |
| TASK | ✅ | `TASK_PhaseH_0.md` |
| ACCEPTANCE | ✅ | `ACCEPTANCE_PhaseH_0.md` |
| FINAL | ✅ | `FINAL_PhaseH_0.md` |
| TODO | ✅ | 本文 |

---

## 8. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — 列出限制 / 增强 / 验证指引 |
