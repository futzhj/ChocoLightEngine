# Phase AR — SDL3 扫尾 (Pen + Event + Timer) — 验收文档

> **状态**: ✅ **已完成** — 6 平台 CI 全绿
>
> GitHub Actions run: [25595205241](https://github.com/futzhj/ChocoLightEngine/actions/runs/25595205241) (修复 commit `c309325` 之后)
> 注: 首次 run 25595048581 因 `extern "C"` 在函数内书写错误 (C2598) 导致 5/6 平台编译失败, 1 行修复后全绿。

---

## 一、实施概览

| 维度 | 数量 / 内容 |
|---|---|
| 新增 Lua 模块 | 1 (`Light.Event`) |
| 新增 Lua 函数 | 11 (Light.Event 9 + Light.Time 2) |
| 新增 Lua 回调 hook (Window:OnXxx) | 6 (Pen Proximity/Down/Up/Button/Motion/Axis) |
| 新增 Lua 常量 (Light.UI Pen) | 15 (PEN_INPUT_* 7 + PEN_AXIS_* 8) |
| 新增 Lua 常量 (Light.Event 事件类型) | 17 (FIRST/QUIT/KEY_*/MOUSE_*/PEN_*/USER/LAST 等) |
| 新增 Event::Type | 7 (PenProximity/Down/Up/Button/Motion/Axis + Timer 内部) |
| 新增 Event 字段 | 6 (penId, penButton, penAxis, penAxisValue, penEraser, penAction) |
| 修改文件 | 5 (platform_window.h, platform_window_sdl3.cpp, light_ui.cpp, light_time.cpp, build-templates.yml) |
| 新增文件 | 6 (light_event.cpp, pen_event_timer.lua, ALIGNMENT/CONSENSUS/ACCEPTANCE 3 docs, light.h 1 行) |
| 注册表更新 | 2 (lumen-master/light.cpp + ChocoLight/CMakeLists.txt) |

---

## 二、API 详细列表

### 2.1 Pen 数字笔事件 hooks (6 个, 挂在 `Light.UI.Window`)

```lua
function window:OnPenProximity(penId, action)        -- action: 1=in, 0=out
function window:OnPenDown(penId, x, y, eraser)       -- eraser: 0/1
function window:OnPenUp(penId, x, y, eraser)
function window:OnPenButton(penId, button, action, x, y)  -- action: 1=down, 0=up
function window:OnPenMotion(penId, x, y)
function window:OnPenAxis(penId, axis, value)        -- axis: 0..7, value: float
```

### 2.2 Pen 常量 (15 个, 挂在 `Light.UI`)

```lua
-- Pen 输入状态位 (与 SDL_PEN_INPUT_* 一致)
Light.UI.PEN_INPUT_DOWN       = 1
Light.UI.PEN_INPUT_BUTTON_1   = 2
Light.UI.PEN_INPUT_BUTTON_2   = 4
Light.UI.PEN_INPUT_BUTTON_3   = 8
Light.UI.PEN_INPUT_BUTTON_4   = 16
Light.UI.PEN_INPUT_BUTTON_5   = 32
Light.UI.PEN_INPUT_ERASER_TIP = 1073741824   -- 1 << 30

-- Pen 轴 ID (与 SDL_PenAxis 枚举一致)
Light.UI.PEN_AXIS_PRESSURE            = 0
Light.UI.PEN_AXIS_XTILT               = 1
Light.UI.PEN_AXIS_YTILT               = 2
Light.UI.PEN_AXIS_DISTANCE            = 3
Light.UI.PEN_AXIS_ROTATION            = 4
Light.UI.PEN_AXIS_SLIDER              = 5
Light.UI.PEN_AXIS_TANGENTIAL_PRESSURE = 6
Light.UI.PEN_AXIS_COUNT               = 7
```

### 2.3 Light.Event 模块 (新增, 9 fns + 17 const)

```lua
Light.Event.HasEvent(type) -> bool
Light.Event.HasEvents(min_type, max_type) -> bool
Light.Event.FlushEvent(type) -> ()
Light.Event.FlushEvents(min_type, max_type) -> ()
Light.Event.Push(type, [code], [data1], [data2]) -> bool
Light.Event.Pump() -> ()
Light.Event.SetEnabled(type, enabled) -> ()
Light.Event.IsEnabled(type) -> bool
Light.Event.Register(num) -> int                      -- 起始 ID, 0=失败

-- 常用事件类型常量 (17 个): FIRST, QUIT, TERMINATING, LOW_MEMORY,
-- WILL_ENTER_BACKGROUND, DID_ENTER_BACKGROUND, WILL_ENTER_FOREGROUND,
-- DID_ENTER_FOREGROUND, LOCALE_CHANGED, KEY_DOWN, KEY_UP, TEXT_INPUT,
-- TEXT_EDITING, MOUSE_MOTION, MOUSE_BUTTON_DOWN, MOUSE_BUTTON_UP,
-- MOUSE_WHEEL, PEN_PROXIMITY_IN, PEN_PROXIMITY_OUT, PEN_DOWN, PEN_UP,
-- PEN_BUTTON_DOWN, PEN_BUTTON_UP, PEN_MOTION, PEN_AXIS, USER, LAST
```

### 2.4 Light.Time 扩展 (2 fns)

```lua
Light.Time.AddTimer(ms, fn) -> id        -- 单次触发, 0=失败
Light.Time.RemoveTimer(id) -> bool

-- 重复定时器: Lua 端自己 wrap (避免 SDL 子线程↔主线程同步复杂度)
local function periodic(ms, fn)
    Light.Time.AddTimer(ms, function()
        fn()
        periodic(ms, fn)
    end)
end
```

---

## 三、关键设计决策 (与 CONSENSUS 对齐)

| Q | 决策 | 实施细节 |
|---|---|---|
| Q1 Pen hook 签名 | A — 6 独立 hooks | 与 OnKey/OnMouseButton 风格一致 |
| Q2 Event.Push payload | A — 位置参数 (type, code, data1, data2) | SDL_UserEvent 1:1 映射 |
| Q3 AddTimer 回调机制 | A — SDL_AddTimer + SDL_PushEvent 中转, 单次触发 | 避免跨线程 lua_State 访问 |
| Q4 Light.Event 归属 | A — 独立顶级模块 | 与 Light.Process / Light.System 平级 |
| Q5 RegisterEvents | A — 转发 SDL_RegisterEvents | 单层 wrapper |

---

## 四、Smoke 测试覆盖

`scripts/smoke/pen_event_timer.lua` (5 个 stage):

1. **Pen 常量校验**: 15 个常量数值与 SDL 一致
2. **Light.Event 模块加载**: 9 fns + 8 个事件类型常量
3. **Light.Event 行为**: HasEvent/HasEvents 返回 boolean, Pump/Flush 不崩, SetEnabled/IsEnabled round-trip, Register(N) 返回 number, Push 返回 boolean, Push+HasEvent 验证事件入队
4. **Light.Time AddTimer/RemoveTimer**: 注册 60s 定时器返回 >0 timer_id, RemoveTimer 返回 true; 无效 id 返回 false 不崩
5. **兼容性**: Light.Time.GetTicks/GetPerformanceCounter 仍可用

---

## 五、跨线程安全设计 (Timer 关键)

**问题**: SDL_AddTimer 回调在 SDL 内部线程触发, 但 Lua 5.1 的 `lua_State` 非线程安全。

**解决方案**:
1. SDL 子线程回调 `TimerCallback` **仅**调用 `SDL_PushEvent` 推送一个用户事件 (类型=`g_timerEventType`, code=timer_id), 不访问 lua_State
2. SDL 子线程回调返回 0 → SDL 不重复触发 (单次定时器)
3. 主线程 `PollEvent` 通过 `Time_GetTimerEventType()` 识别该用户事件, 转换为 `Event::Timer`
4. 主线程 `DispatchEvents` 调用 `Time_OnTimerEvent(L, timer_id)`, 这里读取 `g_timerCallbackRef` 表, lua_pcall callback, 然后释放 ref
5. `g_timerMutex` 仅保护 `g_nextTimerId++` (防止多线程并发 AddTimer)
6. `g_timerCallbackRef` / `g_timerSdlMap` **只在主线程访问** (从 AddTimer/RemoveTimer/Time_OnTimerEvent), 因此无需锁

**重复定时器**: 留给 Lua 端 wrap (CONSENSUS Q3 A 方案), 避免 SDL 间隔 < 主循环 dispatch 时间时事件堆积。

---

## 六、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `Event` POD layout | 新增 6 字段, 全部追加在末尾, 无 ABI 兼容性问题 |
| `light_ui.cpp` DispatchEvents | 新增 7 case (6 Pen + 1 Timer), 不影响现有 case |
| `Light.UI` 表 | 仅追加 15 个常量字段 |
| `Light.Time` | 仅追加 2 fns + 内部 Timer 基础设施 (静态变量, 模块作用域) |
| `Light.Keyboard / Mouse / Joystick` | **零修改** |
| `Light.Touch / Gamepad` | **零修改** |
| `platform_window.h` | Event enum 加 7 个值, 字段加 6 个 (在末尾) |

---

## 七、未实施项 (明确不做, 留作后续 Phase)

- **SDL_thread.h** — Lua 多状态架构需独立 Phase (~1 周设计)
- **SDL_mutex.h Sema/Cond** — 依赖 Thread 才有意义
- **SDL_video.h 多窗口** — 需重构 g_mainWindow 单例
- **SDL_events.h: PeepEvents / SetEventFilter / AddEventWatch** — 持久 Lua callback, 内存泄漏风险
- **SDL_timer.h: AddTimerNS** — 高精度版本暂不需要 (毫秒级足够多数游戏)
- **重复定时器原生支持** — Lua 端 wrap 已可达成, 减少 C++ 复杂度
- **SDL_gpu.h / render.h / metal.h / vulkan.h** — 引擎使用 GL, 不绑

---

## 八、CI 验收标准

- [x] `lightc -p scripts/smoke/pen_event_timer.lua` Exit=0 (本地)
- [x] GitHub Actions `Build Templates (All Platforms)` **全绿** (run 25595205241):
  - [x] Windows x64: 编译 + Windows runtime smoke (含 pen_event_timer.lua) ✅
  - [x] Linux x64: 编译 + 语法检查 ✅
  - [x] macOS Universal: 编译 + 语法检查 ✅
  - [x] Android arm64+x86_64: 编译 ✅
  - [x] iOS arm64: 编译 ✅
  - [x] Web WASM: 编译 ✅

**Phase AR 最终交付完成。**

### 八.1 修复经验记录

首次 run 失败的根因: 在 `platform_window_sdl3.cpp` 的函数体内写了 `extern "C" Uint32 Time_GetTimerEventType();` 声明,触发 MSVC C2598 (linkage specification must be at global scope)。

**修复**: 把 `extern "C"` 声明上移到文件顶部 namespace 之外,函数内调用时用 `::Time_GetTimerEventType()` 显式全局解析。

**经验**: 跨编译单元的 C 链接函数声明应统一在文件顶部声明,绝不在函数/类作用域内写 `extern "C"` 块。下个 Phase (AS/AT) 应注意同样陷阱。
