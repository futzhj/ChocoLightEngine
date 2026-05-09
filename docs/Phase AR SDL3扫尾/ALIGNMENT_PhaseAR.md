# Phase AR — SDL3 扫尾 — 对齐文档

> **6A 工作流**: Align → Architect → Atomize → Approve → Automate → Assess

---

## 1. 项目上下文分析

### 1.1 已绑 Light.* 模块清单 (78 个)

通过 `lumen-master/src/light/light.cpp:g_lightModules[]` 表权威核对,引擎已绑定 78 个 Light.* 模块。SDL3 主要子系统覆盖情况:

| SDL3 子系统 | 状态 | Phase |
|---|---|---|
| Sensor | ✅ Light.Sensor (17 fns + 9 const) | AK + AL |
| Audio | ✅ Light.Audio (51 fns + 16 const + 4 callback) | AM + AN |
| Joystick / Gamepad / Haptic | ✅ Light.Joystick / Gamepad / Haptic | AG/AI/AK 系列 |
| HIDAPI | ✅ Light.Hidapi | J |
| Touch | ✅ Light.Touch | M |
| Keyboard / Mouse | ✅ Light.Keyboard / Mouse (15+/8 fns) | AG/AH |
| **TextInput / IME** | ✅ Light.UI.Window 6 fns + 13 const | **AQ (刚完成)** |
| Locale / Power / Time | ✅ Light.Locale / Power / Time | C/D/O |
| CPUInfo / Endian / Atomic / Mutex | ✅ Light.CPUInfo / Endian / Atomic / Mutex | U/P/AC/AD |
| Surface / Pixels / BlendMode / Display / Cursor | ✅ Light.Surface / Pixels / BlendMode / Display / Cursor | AF/R/S/F |
| Filesystem / IOStream / Storage | ✅ Light.Filesystem / IOStream / Storage | T/AE/I |
| Process / Camera / MessageBox / Dialog / Tray | ✅ Light.Process / Camera / MessageBox / Dialog / Tray | E/L/I |
| Clipboard / URL / Misc | ✅ Light.Clipboard / URL / Misc | F/AA |
| Guid / Properties / Error / Hints / Version / Log | ✅ 6 utility 模块 | H/Y/V/N/X/Q |
| Loadso / Rect | ✅ Light.Loadso / Rect | W/AB |
| Physics (Box2D) | ✅ Light.Physics + Light.Physics.World (122 fns) | AO + AP |

### 1.2 SDL3 剩余未绑模块盘点

经审视 SDL3 公共头文件,**真正未绑且有价值**的子系统只有 3 个:

| 头文件 | 函数数 | 是否适合 Phase AR |
|---|---|---|
| `SDL_pen.h` | **0 (纯事件)** | ✅ 适合 — 仅事件 dispatch + 常量, 无线程问题 |
| `SDL_events.h` | 19 | ✅ 适合(选 P0 子集 9 个) — PushEvent/HasEvent/RegisterEvents 等 |
| `SDL_timer.h` 扩展 | 10 (5 已绑) | ✅ 适合 — AddTimer/RemoveTimer 回调式定时器 |
| `SDL_thread.h` | 14 | ❌ **不适合 AR** — Lua 多状态架构需独立 Phase |
| `SDL_mutex.h` Sema/Cond | 20 | ❌ **不适合 AR** — 依赖 Thread 才有意义 |
| `SDL_video.h` 多窗口扩展 | ~10 | ⚠️ 推迟 — 单窗口足够多数游戏,扩展 Phase |
| `SDL_gpu.h` | 大工程 | ❌ Phase AS 范畴 |
| `SDL_render.h` | 不需要 | ❌ 引擎用 Light.Graphics 高级 API |
| `SDL_metal.h` / `SDL_vulkan.h` | 不需要 | ❌ 引擎用 GL |
| `SDL_assert.h` | 1 fn | ❌ 极少游戏需要 |
| `SDL_main.h` | 入口 | ❌ 引擎已处理 |

### 1.3 用户原话核对

用户说 "SDL3 扫尾 (Dialog/MessageBox/Camera/Pen/Process)" — 经核对:
- **Dialog ✅** 已绑 (Phase L `Light.Dialog`)
- **MessageBox ✅** 已绑 (Phase E `Light.MessageBox`)
- **Camera ✅** 已绑 (Phase E `Light.Camera`)
- **Process ✅** 已绑 (Phase E `Light.Process`)
- **Pen ❌** 未绑 ← **本 Phase 的核心**

---

## 2. 任务范围确认

### 2.1 In-Scope (本 Phase 必做)

#### 2.1.1 Pen 数字笔事件支持 (核心)

**SDL_pen.h 是纯事件驱动头**, 0 个函数, 仅常量 + 事件类型:

| Event 类型 | 触发场景 | 字段 |
|---|---|---|
| `SDL_EVENT_PEN_PROXIMITY_IN` | 笔靠近板 | which (PenID) |
| `SDL_EVENT_PEN_PROXIMITY_OUT` | 笔离开板 | which |
| `SDL_EVENT_PEN_DOWN` | 笔接触板 | which, x, y, eraser |
| `SDL_EVENT_PEN_UP` | 笔抬起 | which, x, y, eraser |
| `SDL_EVENT_PEN_BUTTON_DOWN` | 笔身按钮按下 | which, button, x, y |
| `SDL_EVENT_PEN_BUTTON_UP` | 笔身按钮松开 | which, button, x, y |
| `SDL_EVENT_PEN_MOTION` | 笔移动 | which, x, y |
| `SDL_EVENT_PEN_AXIS` | 压感/倾斜/距离变化 | which, axis, value |

**实现**:
- `platform_window.h` Event 加 `PenProximity` / `PenDown` / `PenUp` / `PenButton` / `PenMotion` / `PenAxis` 6 个类型 + 字段 (penId, button, axis, axisValue, eraser)
- `platform_window_sdl3.cpp` 加 8 个 case (in/out 共用 PenProximity, down/up button_down/up 共用类型 + action 字段)
- `light_ui.cpp` 加 6 个 dispatch + 6 个 hook (`Window:OnPenProximity` / `OnPenDown` / `OnPenUp` / `OnPenButton` / `OnPenMotion` / `OnPenAxis`)
- `Light.UI` 加 15 个常量 (`PEN_INPUT_*` 7 + `PEN_AXIS_*` 8)

#### 2.1.2 Light.Event 模块 (新)

绑 SDL_events.h 的 P0 子集 (9 个常用 fn):

| 函数 | 用途 |
|---|---|
| `Light.Event.HasEvent(type)` | 队列中是否有指定类型事件 |
| `Light.Event.HasEvents(min, max)` | 队列中是否有 [min,max] 类型事件 |
| `Light.Event.FlushEvent(type)` | 清除指定类型事件 |
| `Light.Event.FlushEvents(min, max)` | 清除范围事件 |
| `Light.Event.Push(type, [extra...])` | 推用户自定义事件到队列 |
| `Light.Event.Pump()` | 强制刷新事件队列 (主线程用) |
| `Light.Event.SetEnabled(type, enabled)` | 启/禁用某类事件 |
| `Light.Event.IsEnabled(type) -> bool` | 查询启用状态 |
| `Light.Event.Register(n) -> int` | 注册 N 个用户事件 ID, 返回起始 ID |

**不做** (P1):
- `PeepEvents` — 复杂 buffer 操作, Lua API 设计成本高
- `SetEventFilter` / `AddEventWatch` — 持久化 Lua callback, 内存泄漏风险
- `PollEvent` / `WaitEvent` — 引擎主循环内部使用, 不暴露到 Lua

#### 2.1.3 Light.Time 扩展 (回调式定时器)

| 新增 fn | 用途 |
|---|---|
| `Light.Time.AddTimer(ms, fn) -> id` | 注册延时/周期回调, 返回 timer id |
| `Light.Time.RemoveTimer(id) -> bool` | 取消定时器 |

**实现要点 (避免线程问题)**:
- SDL_AddTimer 的回调在 SDL 内部线程触发 → 不能直接调 Lua (lua_State 非线程安全)
- 解决方案: SDL 回调内 push 自定义事件 (`SDL_USEREVENT + timer_id`) 到队列
- 主循环 `Light.UI.Resume` 中 dispatch user event → 调对应的 Lua callback
- 取消时清理 ref 表

### 2.2 Out-of-Scope (本 Phase 不做, 留作后续 Phase)

#### 2.2.1 Thread / Semaphore / Condition (独立 Phase 待规划)

**为什么不做**:
- Lua 5.1 的 `lua_State` **非线程安全**, 一个 state 不能在多线程间共享
- 1:1 绑 SDL_CreateThread 会立即 segfault
- 正确实现需要:
  1. 每个 worker thread 独立 `lua_newstate`
  2. 主 state 与 worker state 间用消息队列 IPC
  3. 限制 worker 不能调 Light.* 模块 (它们假设主 state)
  4. 设计 thread userdata 生命周期管理
- 这是**专门的 Lua 并发架构 Phase**, 非简单 SDL3 绑定

**推荐放在**: 未来 Phase **"Lua 多状态并发"** (独立设计,可能 1 个月)

#### 2.2.2 多窗口 / 全屏模式扩展

`SDL_video.h` 还有 ~10 个 Window 相关 fn 没绑 (SetWindowFullscreen / SetWindowIcon / SetWindowAlwaysOnTop / GetWindows / RaiseWindow 等)。

**推迟原因**:
- Light.UI.Window 当前是单例 (`g_mainWindow`), 不支持多窗口对象
- 引入多窗口需要重构 `g_mainWindow` 为 userdata 数组 + 修改 Resume/__call 适配多窗口
- 工程量大, 优先级低 (多数 2D 游戏用单窗口)

**推荐放在**: Phase **"多窗口 + Window 扩展"** (可独立 1 周)

#### 2.2.3 SDL_render.h / SDL_gpu.h / SDL_metal.h / SDL_vulkan.h

引擎用 OpenGL + Light.Graphics 高级 API, **不直接绑 SDL 渲染层**。

---

## 3. 工作量估算

| 子模块 | 新 fns | C++ 行 | Lua hook | smoke 行 |
|---|---|---|---|---|
| Pen 事件 dispatch (platform + ui) | 0 | ~250 | 6 | ~80 |
| Light.UI 常量 (PEN_INPUT_/AXIS_) | 0 | ~30 | - | (合并 textinput.lua 验证) |
| Light.Event (新模块) | 9 | ~280 | - | ~120 |
| Light.Time AddTimer 扩展 | 2 | ~180 (含 user event id 管理) | 1 | ~80 |
| **合计** | **11 fns** | **~740** | **7** | **~280** |

预期工程量 (含文档/CI): 1 commit, ~5 工作小时, 6 平台 CI 全绿验收。

---

## 4. 现有项目模式对齐

### 4.1 复用模式

- **Event 扩展** 沿用 Phase AQ 的方式: `platform_window.h` 加 Event::Type + 字段, `platform_window_sdl3.cpp` 加 case, `light_ui.cpp` 加 dispatch + hook
- **新模块注册** 沿用现有 `LT::RegisterModule(L, "Event", funcs)` + 加入 `g_lightModules` 映射表 + `lumen-master/src/light/light.cpp` 注册
- **userdata 引用管理** 沿用 Light.HotReload / Light.Audio 现有的 `luaL_ref(LUA_REGISTRYINDEX)` 模式
- **smoke 风格** 沿用 `keyboard.lua` / `textinput.lua` 风格 (pcall require → assert fns/consts → 边界路径)

### 4.2 关键约束

- **不破坏 Event POD layout**: Pen 字段只追加在末尾 (与 Phase AQ 同样原则)
- **不引入新依赖**: 仅用 SDL3 已链接的 API
- **回调安全**: Timer 回调走 SDL_PushEvent 中转, 主线程 dispatch 时调 Lua, 永不在 SDL 内部线程调 lua_*
- **headless 友好**: 无窗口时所有 fn 优雅返回 false/0/nil 不崩 (Pen 事件本身就只在有窗口时触发)

---

## 5. 智能决策预案

### Q1: Pen 事件的 Lua hook 签名

**方案 A** (推荐): 6 个独立 hook
```lua
function window:OnPenProximity(penId, in_or_out)  -- in_or_out: 1=进入, 0=离开
function window:OnPenDown(penId, x, y, eraser)
function window:OnPenUp(penId, x, y, eraser)
function window:OnPenButton(penId, button, action, x, y)  -- action: 1=按下, 0=松开
function window:OnPenMotion(penId, x, y)
function window:OnPenAxis(penId, axis, value)  -- axis: 0=PRESSURE..7=TANGENTIAL_PRESSURE
```

**方案 B**: 单一 `OnPen(eventType, ...)` 通用 hook,用户用 if/else 分发  
**方案 C**: 把 Pen 当扩展鼠标,合并到 OnMouse* (eraser 标志用 button=99)

**A 的优势**: 与 OnKey/OnMouseButton 风格一致,语义清晰,签名稳定

### Q2: Light.Event.Push 的事件 payload 形式

**方案 A** (推荐): 位置参数 — `Push(type, code, data1, data2)` 4 参数 (匹配 SDL_UserEvent 字段)  
**方案 B**: table — `Push(type, {code=..., data1=..., data2=...})` (灵活但开销高)

**A 的优势**: 性能更好 (不创建临时 table),与 SDL_UserEvent 1:1 映射

### Q3: Light.Time.AddTimer 的回调来源

**方案 A** (推荐): SDL_AddTimer + SDL_PushEvent 中转 — 在主线程 dispatch  
**方案 B**: 主循环 tick 累计模式 (纯主线程, 不用 SDL_AddTimer) — 精度低但简单

**A 的优势**: 借助 SDL 真定时器精度更高;**风险**: 需要 RegisterEvents 配合,实现较复杂  
**B 的优势**: 简单纯主线程, 但精度受限于帧率 (60Hz=16.67ms, 不能做 5ms 高精度)

### Q4: Light.Event 与 Light.UI 的关系

**方案 A** (推荐): Light.Event 是独立顶级模块 (与 Light.UI 平级)  
**方案 B**: 挂在 Light.UI.Event 子模块下

**A 的优势**: SDL_events.h 独立性强, 不与 UI Window 强耦合; 与 Light.Process / Light.System 平级

### Q5: 用户自定义事件 ID 分配 (Light.Event.Register)

**方案 A** (推荐): 直接转发 SDL_RegisterEvents — 返回起始 ID (UInt32)  
**方案 B**: 引擎内部维护一个 ID 池, 抽象掉 SDL ID

**A 的优势**: 与 SDL 行为一致, 实现简单 (单层 wrapper); ID 空间充足 (32 位)

---

## 6. 验收标准

- [ ] `Light.UI.Window` 元表新增 6 个 Pen hook 名 (OnPenProximity 等)
- [ ] `Light.UI` 模块表新增 15 个 Pen 常量 (PEN_INPUT_* x7 + PEN_AXIS_* x8)
- [ ] `Light.Event` 模块加载且 9 fns 全部 type==function
- [ ] `Light.Time` 新增 AddTimer / RemoveTimer 是 function
- [ ] `scripts/smoke/pen_event_timer.lua` 通过 lightc -p 和 Windows runtime
- [ ] 6 平台 CI 全绿 (Windows 含 runtime smoke)
- [ ] 不引入对 SDL_thread.h 的依赖

---

## 7. 关键决策待确认

5 个 Q 都给出推荐方案 (A)。本次同样请用户在以下选项中选:

1. **全部采用推荐方案 (Q1~Q5 全 A)**  
2. **除 Q3 外用推荐, Q3 选 B (主循环 tick)** — 不依赖 SDL_AddTimer + RegisterEvents,简化但精度低
3. **去掉 AddTimer (本 Phase 仅做 Pen + Light.Event)** — 工程量缩 30%, AddTimer 推迟到下一 Phase
4. **分别详细选择**
