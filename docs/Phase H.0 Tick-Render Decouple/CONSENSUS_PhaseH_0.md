# Phase H.0 Tick-Render 解耦 — CONSENSUS 共识文档

> **阶段**: 6A Workflow — 阶段 1 Align (终段) — 共识固化
> **基线**: ALIGNMENT_PhaseH_0.md v1.0 (4 项决策已确认)
> **状态**: ✅ 用户决策无歧义, 直接进入 DESIGN

---

## 1. 明确需求描述

实现"Tick (固定频率逻辑) ↔ Render (显示刷新率) 解耦":

1. 引擎主循环每帧累积 wall-clock 时间, 当累积量 ≥ 固定 dt (默认 1/60 s) 时触发若干次 `OnFixedUpdate(dt=fixedDt)`.
2. Render 阶段调用一次 `OnRender(alpha, dt)`, 其中 alpha ∈ [0, 1) 表示距离上次 fixed update 的进度 (用于客户端做状态 lerp).
3. Box2D / Bullet 物理 World 注册到全局列表后, 引擎在 FixedUpdate 阶段自动调 `Step(fixedDt)`.
4. 全平台 (含 Web) 行为一致; Web 走 `emscripten_set_main_loop_arg`, 内部 accumulator 同等执行.
5. 旧 `Window:Update(dt)` / `Window:Draw()` 回调保留, **零回归**; 老 sample 不必修改.

---

## 2. 技术实现方案

### 2.1 模块划分

| 模块 | 职责 | 文件 |
|------|------|------|
| `LT::Time` (C++) | accumulator + spiral guard + 配置 / 查询 | `light_time.h` / `light_time.cpp` |
| `Light.Time` (Lua) | API 暴露层 | 注册到 `lumen-master/src/lumen/lumen.cpp` 的全局表 |
| Window 主循环 | accumulator 推进, 调度 FixedUpdate / Update / OnRender / Draw | `light_ui.cpp::l_Window_Call` |
| 物理 auto-step | 全局 World 列表 + auto-step flag + FixedUpdate 内调用 | `light_physics.cpp` / `light_physics3d.cpp` |
| Web 主循环 | emscripten_set_main_loop_arg 回调内执行单步 | `light_ui.cpp` (新增 Web 分支) + `platform_window_sdl3.cpp::RunMainLoop` |

### 2.2 数据结构

```cpp
// LT::Time (内部状态)
struct TimeState {
    double fixedDt              = 1.0 / 60.0;  // 固定逻辑 dt (s)
    int    maxFixedStepsPerFrame = 8;          // spiral guard 上限
    double frameTimeClamp       = 0.25;        // 单帧最大 frameTime (s)
    double accumulator          = 0.0;
    double lastTime             = 0.0;         // 上次帧起点 wall-clock
    double alpha                = 0.0;         // 最近 alpha (供查询)
    int    lastFixedStepCount   = 0;           // 最近一帧实际 step 次数 (调试)
};
```

### 2.3 主循环逻辑 (light_ui.cpp 内)

```cpp
double now = GetTime();
double frameTime = now - state.lastTime;
state.lastTime = now;

if (frameTime > state.frameTimeClamp) frameTime = state.frameTimeClamp;
state.accumulator += frameTime;

// === Fixed timestep 阶段 ===
state.lastFixedStepCount = 0;
while (state.accumulator >= state.fixedDt &&
       state.lastFixedStepCount < state.maxFixedStepsPerFrame) {
    // 1) 物理 auto-step (Box2D + Bullet)
    PhysicsAutoStepAll(state.fixedDt);

    // 2) Lua 回调 OnFixedUpdate
    CallLuaCallback(L, "OnFixedUpdate", state.fixedDt);

    state.accumulator -= state.fixedDt;
    state.lastFixedStepCount++;
}
// spiral 后即使触顶, 累积仍可能超过 fixedDt → 强制丢
if (state.accumulator > state.fixedDt * 4) state.accumulator = state.fixedDt * 4;

state.alpha = state.accumulator / state.fixedDt;

// === Render 阶段 ===
BeginFrame ... TAA::ApplyJitter
CallLuaCallback(L, "Draw");                   // 旧路径
CallLuaCallback(L, "Update", frameTime);      // 旧路径 (wall-clock dt)
CallLuaCallback(L, "OnRender", state.alpha, frameTime);   // 新路径 (alpha + dt)
EndFrame ... SwapBuffers
```

**Lua 回调存在性策略**:
- `OnFixedUpdate` 不存在 → 静默跳过 (但物理 auto-step 仍执行).
- `OnRender` 不存在 → 静默跳过 (老 sample 走 Draw + Update 即可).
- 老 sample (无 OnFixedUpdate / OnRender, 仅 Update + Draw): 物理 auto-step **默认禁用** (避免双 Step), 详见 §2.5.

### 2.4 Lua API 设计

```lua
-- 时间配置 (一次性设定)
Light.Time.SetFixedTimestep(60)            -- fps; 内部 fixedDt = 1/60
Light.Time.SetFixedTimestep(120)           -- 120 fps fixed
local fps = Light.Time.GetFixedTimestep()  -- 60 / 120 / ...

Light.Time.SetMaxFixedStepsPerFrame(8)     -- spiral guard 上限
Light.Time.SetFrameTimeClamp(0.25)         -- 单帧最大 dt

-- 查询当前帧状态 (供高级用户)
local alpha = Light.Time.GetAlpha()        -- [0, 1) 距上次 fixed 的进度
local acc   = Light.Time.GetAccumulator()  -- 累积器值 (s)
local n     = Light.Time.GetLastStepCount()  -- 上一帧实际 step 数

-- 物理 auto-step 控制 (新)
Light.Physics.SetAutoStep(world, true)         -- Box2D
Light.Physics3D.SetAutoStep(world, true)       -- Bullet
local on = Light.Physics.GetAutoStep(world)
```

**Window 回调约定**:

```lua
function Window:OnFixedUpdate(dt)   -- dt = fixedDt 永远固定
    -- 物理 / 网络同步 / 状态机 / 决定性逻辑
end

function Window:OnRender(alpha, dt) -- alpha ∈ [0, 1), dt = wall-clock
    -- 状态 lerp + 渲染参数
    local x = lerp(prev.x, curr.x, alpha)
    Light.Graphics.DrawRect(x, y, w, h)
end

-- 兼容回调 (老 sample 继续用)
function Window:Update(dt)  -- dt = wall-clock (不变)
function Window:Draw()      -- 不变
```

### 2.5 物理 auto-step 默认值与冲突防御

| 场景 | 默认 SetAutoStep | 用户行为 | 引擎行为 |
|------|-----------------|---------|---------|
| 新建 World, 不调 SetAutoStep | **false** | 用户手动 World:Step (老模式) | 不自动 Step |
| 新建 World, 显式 SetAutoStep(true) | true | 用户不再 Step | 引擎在 FixedUpdate 自动 Step(fixedDt) |
| 用户混合 (auto + 手动) | true | 也调 World:Step | **每帧打 warning log**, 双 Step 物理震荡 |

**默认 false** — 避免破坏 32 个老 sample. 用户主动 opt-in 才启用 auto-step. 这与决策点 3 的"自动 Step"语义一致, **但默认不启用是为零回归**, 由用户在新 sample 显式开启.

### 2.6 顺序定义 (确定性)

每个 `Window:__call` 执行如下严格顺序:

```
1. 计算 frameTime
2. accumulator += frameTime
3. while accumulator >= fixedDt && step < maxStep:
     a. PhysicsAutoStepAll(fixedDt)        # Box2D 全部 + Bullet 全部
     b. Lua: Window:OnFixedUpdate(fixedDt)
     c. accumulator -= fixedDt
4. alpha = accumulator / fixedDt
5. BeginFrame + AssetLoader::Tick + Batch/HDR/TAA::Begin + TAA::ApplyJitter
6. Lua: Window:Draw(self)                  # 旧
7. Lua: Window:Update(self, frameTime)     # 旧
8. Lua: Window:OnRender(self, alpha, frameTime)  # 新
9. Batch/HDR/TAA::End + g_render->EndFrame
10. RecordTickHook + DrawRecordOSD
11. SwapBuffers
```

注意第 6/7/8 顺序: **Draw 在 Update 前** 是为了**保留与 F.1 系列已交付的顺序兼容**, OnRender 紧跟 Update 后 (用最新逻辑状态绘渲染插值).

---

## 3. 验收标准 (具体可测试)

### 3.1 功能验证 ✅

| # | 检查 | 验证手段 |
|---|------|---------|
| F1 | 默认 fixedDt = 1/60 s | smoke `Light.Time.GetFixedTimestep() == 60` |
| F2 | SetFixedTimestep(120) 生效 | smoke set + get round-trip |
| F3 | OnFixedUpdate 在 60Hz 显示器上每帧调 1 次 | smoke 计数器 + 1 秒后 ≈ 60 ± 5 |
| F4 | OnFixedUpdate 在 144Hz 显示器上每帧调 0 或 1 次 (因为 fixedDt > frameTime) | demo HUD 显示 `lastStepCount` |
| F5 | OnFixedUpdate 在低 FPS (10Hz) 触发多 Step | smoke 模拟 frameTime=0.15 → step ≈ 9 (但被 maxStep=8 限制) |
| F6 | alpha ∈ [0, 1) | smoke 多帧检查 |
| F7 | Spiral guard: frameTime 0.5s clamp 到 0.25s | smoke 模拟 |
| F8 | 旧 Update + Draw 仍正常工作 (零回归) | 32 个老 sample 不修改; 至少 1 个 sample CI smoke |
| F9 | 物理 auto-step 默认 false | smoke `Light.Physics.GetAutoStep(world) == false` |
| F10 | SetAutoStep(world, true) 后 FixedUpdate 自动 Step | smoke + demo |
| F11 | 同时有 Update / OnRender / OnFixedUpdate 三回调时全部触发 | smoke 计数器 |
| F12 | OnRender 不存在时 Draw 仍调度 (优雅降级) | smoke |

### 3.2 跨平台验证 ✅

| 平台 | 期待 | 验证 |
|------|------|------|
| Windows / Linux / macOS | 全功能 + VSync 60/144Hz | CI build |
| Android / iOS | 全功能 + 后台暂停时 accumulator 不爆 | CI build (运行时 audit 留 TODO) |
| Web (Emscripten) | emscripten_set_main_loop 内嵌 accumulator | CI build + 浏览器手动测 |

### 3.3 性能要求 ✅

| 项 | 要求 |
|---|------|
| OnFixedUpdate Lua 调用边界开销 | < 30 ns (与 G.1.7.P3.1 测得 ~27 ns 一致) |
| 主循环 accumulator 计算开销 | < 100 ns/frame (浮点 4 次 + while 1 次) |
| 物理 auto-step 列表遍历 (空列表) | < 50 ns |
| 总开销 | < 0.01 ms/frame (60Hz 下 ~0.6%) |

### 3.4 内存增量 ✅

| 位置 | 增量 |
|------|------|
| `LT::TimeState` (全局单例) | ~64 B |
| Box2D World struct + auto_step bool | +1 B (有 padding) |
| Bullet World struct + auto_step bool | +1 B |
| 全局 World 注册列表 | std::vector, 默认空 → 0 B |
| **总计** | < 256 B (整个进程) |

### 3.5 兼容性 ✅

| 项 | 要求 |
|---|------|
| 32 个老 sample 不修改, CI 全绿 | F.1.5 已通过的 sample 全部继续工作 |
| Lua API 命名空间 `Light.Time` 全新 | 不冲突 |
| `Light.Physics.SetAutoStep` 新 API | 老 sample 不调用即可保持原行为 |
| Hot reload (G.0) 与新回调互通 | OnFixedUpdate / OnRender 走 Preserve 路径自动恢复 |

### 3.6 文档与 CI ✅

| 项 | 要求 |
|---|------|
| 6A 7 件套全部完成 | ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO |
| smoke `tick_render.lua` 至少 20 子用例 | API 完整性 + 频率 + alpha + spiral + 物理 auto |
| `samples/demo_tick_render/` 新增 sample + README | HUD 显示 fixedHz / actualFPS / alpha / step count |
| CI 6 平台 build success | Run #XX 全绿 |

---

## 4. 任务边界与限制

### 4.1 IN

- 上文 §2 全部模块改动
- §3 全部验收点

### 4.2 OUT

| 项 | 原因 |
|---|------|
| Tick 多线程 (Lua VM 跨线程) | 复杂度爆炸, 不在用户决策中 |
| Network sync hooks (输入预测 / rollback) | 上层应用 |
| Frame Generation (DLSS3 / FSR3) | 不属于 Tick-Render 解耦核心 |
| Animation system 自动 Update | 不在用户决策, ECS 已封装够用 |
| iOS / Android pause 状态机 | 平台特定, 留 TODO |
| 32 个老 sample 修改 | 零回归保证 |

### 4.3 Hard Constraints (不可妥协)

1. **零回归**: 32 个老 sample 不必改一行代码, CI 6 平台全部继续 build success.
2. **API 加性增长**: 不删 / 不重命名任何已有 Lua API.
3. **Lua VM 单线程**: OnFixedUpdate / OnRender 都在主线程 Lua VM 上调.
4. **Spiral 防御**: 任何场景 (alt-tab, debug pause, 慢机) 不能让累积器无限增长.

---

## 5. 已确认假设

| # | 假设 | 验证手段 |
|---|------|---------|
| A1 | `PlatformWindow::GetTime()` 返单调递增 wall-clock | platform_window_sdl3.cpp 用 SDL_GetPerformanceCounter (单调) ✅ |
| A2 | Lua `lua_pcall` 在 OnFixedUpdate 抛错不影响下个 step | 同 Update 现有行为 (CC::Log + lua_pop), 已稳定 ✅ |
| A3 | Box2D `b2World::Step` 是阻塞同步调用 | Box2D 文档 + 现有 l_World_Step 实现 ✅ |
| A4 | Bullet `btDynamicsWorld::stepSimulation` 内部已有 sub-step (但我们不依赖它) | 我们用 maxSubSteps=1, 在外层显式 accumulator ✅ |
| A5 | Web emscripten_set_main_loop fps=0 走 requestAnimationFrame | Emscripten 文档 ✅ |
| A6 | Hot reload Preserve(key, factory) 跨 reload 保留 OnFixedUpdate / OnRender 注册 | G.0 已稳定 ✅ |

---

## 6. 工时估算

| 任务 | 估时 |
|------|------|
| T1 LT::Time C++ 模块 | 1.5h |
| T2 Light.Time Lua API + lumen 注册 | 1h |
| T3 主循环重构 (light_ui.cpp) | 1.5h |
| T4 物理 auto-step (Box2D + Bullet) | 2.5h |
| T5 Web 主循环重构 | 1.5h |
| T6 smoke `tick_render.lua` (20+ 用例) | 1.5h |
| T7 demo_tick_render sample + README | 2h |
| T8 6A 文档 (DESIGN/TASK/ACCEPTANCE/FINAL/TODO) | 2h |
| T9 CI 验证 + 修问题 | 1h |
| **合计** | **~14.5h** |

落在用户决策时给出的 8-15h 区间 (上沿).

---

## 7. 文档版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — 用户 4 决策已 100% 共识, 直接进 DESIGN |
