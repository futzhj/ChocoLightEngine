# Phase H.0.3 Pause-Resume State Machine — 6A 轻量合并文档

> **基线**: H.0 + H.0.1 + H.0.2 已交付 (commits `8b7837d` / `63f75ba` / `82871bc`)
> **类型**: H.0 TODO §5.2 落地 — iOS/Android pause 状态机 (Web 后台亦受益)
> **估时**: 2-3h
> **风险**: 状态机要正确处理 background-foreground 切换, 避免 dt 长跳触发 spiral guard

---

## 1. Align — 项目对齐

### 1.1 来源
H.0 TODO §5.2:
> 移动端切回前台 0 卡顿; log 不再刷.
> - `LT::TickRender::Pause()` 设 paused=true, 记录 `pauseTime`.
> - `LT::TickRender::Resume()` 设 paused=false, 让 `BeginFrame` 跳过本次 dt 计算 (置 0 不累积).
> - iOS: hook `applicationWillResignActive` / `applicationDidBecomeActive`.
> - Android: hook `onPause` / `onResume`.

### 1.2 现状

| 模块 | 状态 |
|------|------|
| `SDL_EVENT_DID_ENTER_BACKGROUND` / `_WILL_ENTER_FOREGROUND` 常量 | 已在 `light_event.cpp` 暴露给 Lua |
| `PlatformWindow::Event` 类型 | **未定义** 对应的 BG/FG 事件类型 |
| `PlatformWindow::PollEvent` SDL → Event 转换 | **未处理** BG/FG; SDL 事件被 default 分支吃掉 (返 None) |
| `LT::TickRender::Pause/Resume` | **未实现** |
| Lua `Window:OnAppEnterBackground/Foreground` callback | **未支持** |

### 1.3 用户决策 (轻量自决)

| 问 | 决策 | 理由 |
|---|---|---|
| 几个 SDL 事件 hook? | **2 个: DID_BG + WILL_FG** | iOS 标准: DID_BG=立刻停, WILL_FG=即将恢复. WILL_BG / DID_FG 留给 Lua 自己监听 SDL 事件值 |
| Lua callback 是否暴露? | **是** (`OnAppEnterBackground` / `OnAppEnterForeground`) | 给应用层保存进度的钩子 |
| Lua API 暴露 Pause/Resume? | **是** (`Light.Time.Pause` / `.Resume` / `.IsPaused`) | 手动测试 / 编辑器场景 / 单元测试需要 |
| Resume 后第一帧策略 | **skipNextDt = true** (单帧 dt=0) | 避免 background 期间 wall-clock 长跳触发 spiral |
| pauseTime 是否记录? | **否** | 没有消费方; 留 TODO 待真有用 case 再加 |

### 1.4 边界

- **IN**: TickRender Pause/Resume 状态机 + skipNextDt + 2 个 PlatformWindow Event + PollEvent 转换 + DispatchEvents hook + Lua callback (`OnAppEnterBackground/Foreground`) + 3 个 Lua API (`Pause`/`Resume`/`IsPaused`) + smoke §13 + demo 加 P key 模拟 pause.
- **OUT**: 修改主循环结构 / Web emscripten visibilitychange (浏览器自己暂停 callback, 与 H.0.2 配合) / iOS/Android 平台特有 hook (SDL3 已抽象为统一事件) / pauseTime 记录.

---

## 2. Architect — 设计

### 2.1 状态机

```
                       ┌──────────────┐
                       │   Running    │ ◄── Init (默认)
                       │  paused=F    │
                       │skipNextDt=F  │
                       └──────────────┘
                          │      ▲
            DID_ENTER_BG  │      │  WILL_ENTER_FG
            OR Pause()    │      │  OR Resume()
                          ▼      │
                       ┌──────────────┐
                       │   Paused     │
                       │  paused=T    │
                       │skipNextDt=T  │ (Resume 时设)
                       └──────────────┘
```

### 2.2 BeginFrame 修改 (核心)

```cpp
void BeginFrame() {
    const double now = SDL_GetTicksNS() / 1e9;

    if (g_paused) {
        g_lastTime = now;        // 保持最新 wall-clock, 防止 Resume 后第一帧仍长跳
        g_lastStepCount = 0;     // accumulator 不变, 不累积
        return;
    }

    double dt = (g_lastTime > 0) ? (now - g_lastTime) : 0.0;
    g_lastTime = now;

    if (g_skipNextDt) {          // Resume 后第一帧 dt=0
        dt = 0.0;
        g_skipNextDt = false;
    }

    // 原有逻辑: clamp(dt, 0, frameTimeClamp); accumulator += dt; ...
}
```

### 2.3 Pause/Resume API

```cpp
void Pause() {
    if (g_paused) return;
    g_paused = true;
    CC::Log(CC::LOG_INFO, "TickRender::Pause()");
}

void Resume() {
    if (!g_paused) return;
    g_paused = false;
    g_skipNextDt = true;
    g_lastTime = SDL_GetTicksNS() / 1e9;  // 重置, 确保下一帧 dt 计算从新基线
    CC::Log(CC::LOG_INFO, "TickRender::Resume() (skipping next dt)");
}

bool IsPaused() { return g_paused; }
```

### 2.4 PlatformWindow::Event 新增类型

```cpp
enum Type {
    ...
    AppEnterBackground = 25,   // SDL_EVENT_DID_ENTER_BACKGROUND
    AppEnterForeground = 26,   // SDL_EVENT_WILL_ENTER_FOREGROUND
};
```

### 2.5 PollEvent 转换 (SDL → Event)

```cpp
case SDL_EVENT_DID_ENTER_BACKGROUND:
    out->type = Event::AppEnterBackground;
    return true;

case SDL_EVENT_WILL_ENTER_FOREGROUND:
    out->type = Event::AppEnterForeground;
    return true;
```

### 2.6 DispatchEvents hook (light_ui.cpp)

```cpp
case PlatformWindow::Event::AppEnterBackground:
    LT::TickRender::Pause();
    DispatchOnAppEnterBackground(L);   // Lua callback
    break;
case PlatformWindow::Event::AppEnterForeground:
    LT::TickRender::Resume();
    DispatchOnAppEnterForeground(L);
    break;
```

### 2.7 Lua callback (新)

```lua
function Window:OnAppEnterBackground()
    -- 用户钩子: 保存进度 / 暂停音乐 / etc
end

function Window:OnAppEnterForeground()
    -- 用户钩子: 恢复音乐 / 检查 token / etc
end
```

### 2.8 Lua API (Light.Time)

```lua
Light.Time.Pause()       -- 等价 LT::TickRender::Pause()
Light.Time.Resume()      -- 等价 LT::TickRender::Resume()
Light.Time.IsPaused()    -- 返 bool
```

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输入 | 输出 |
|------|------|------|------|
| **T1** TickRender Pause/Resume/IsPaused + BeginFrame skip 逻辑 | 0.5h | light_time.h / .cpp | API + state |
| **T2** PlatformWindow Event 2 类型 + PollEvent 转换 | 0.3h | platform_window.h / sdl3.cpp | enum + switch case |
| **T3** light_ui.cpp DispatchEvents hook + Lua callback dispatcher | 0.5h | light_ui.cpp | 2 个 DispatchOn* fn + switch case |
| **T4** Lua wrappers + 注册 | 0.3h | light_time.cpp | 3 个 wrapper + kTimeReg 加 3 项 |
| **T5** smoke §13 (API surface + round-trip + BeginFrame skip 行为) | 0.5h | tick_render.lua | 段加 ~30 LOC |
| **T6** demo `P` 键模拟 pause + 4 件 6A 文档 + 更新 H.0 TODO | 0.5h | demo_tick_render/main.lua + 4 md | - |
| **T7** CI 验证 | 0.5h | git push + GH | 6/6 PASS |

**总计**: ~3.1h (估时 2-3h, ≈准)

---

## 4. 验收标准

| 项 | 验证 |
|----|------|
| ✅ `Light.Time.Pause/Resume/IsPaused` 函数存在 | smoke §13 |
| ✅ Pause → IsPaused=true; Resume → IsPaused=false | smoke §13 round-trip |
| ✅ Resume 后第一帧不长跳 (skipNextDt 已设) | smoke §13 间接 (查 accumulator 不暴增) |
| ✅ DID_ENTER_BACKGROUND 事件触发自动 Pause | 实机 iOS/Android (留实测) |
| ✅ WILL_ENTER_FOREGROUND 事件触发自动 Resume | 实机 iOS/Android |
| ✅ Lua `OnAppEnterBackground/Foreground` callback 可绑 | smoke 间接 + demo P 键模拟 |
| ✅ 6/6 平台 CI PASS | GH Actions |

### 4.1 零回归
- 默认 paused=false (与 H.0 行为完全一致).
- skipNextDt 仅在 Resume 时设, 不影响普通帧.
- 32+ 老 sample 不调 Pause/Resume → 路径完全不变.

---

## 5. 风险

| 风险 | 缓解 |
|------|------|
| Pause 后第一次 Resume dt 不为 0 → spiral guard | skipNextDt + g_lastTime 重置双保险 |
| 多次 Pause / Resume 嵌套 | 状态机用 if (g_paused) return 守卫, 不计数 |
| iOS Background 时仍调主循环 (SDL3 在某些平台下不暂停) | Pause 内 g_lastTime = now 保持新, accumulator 不累积 |
| Web 主循环已被浏览器暂停 (H.0.2 已解决) | 与本期独立; 浏览器后台事件靠 visibilitychange (留 TODO) |

---

## 6. 6A 7 件套精简映射

**本文 = ALIGNMENT + CONSENSUS + DESIGN + TASK 合并**, 后续:
- ACCEPTANCE_PhaseH_0_3.md
- FINAL_PhaseH_0_3.md
- TODO_PhaseH_0_3.md

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — H.0.3 启动 |
