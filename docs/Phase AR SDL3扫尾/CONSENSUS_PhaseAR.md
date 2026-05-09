# Phase AR — SDL3 扫尾 — 共识文档

> **状态**: ✅ 决策已锁定 (Q1~Q5 全 A 推荐方案), 进入实施阶段

---

## 1. 锁定范围

### 1.1 Pen 数字笔事件支持 (核心)

| 项 | 细节 |
|---|---|
| **新 Event 类型** | 6 (PenProximity / PenDown / PenUp / PenButton / PenMotion / PenAxis) |
| **新 Event 字段** | 5 (penId, penButton, penAxis, penAxisValue, penEraser) |
| **新 Lua hooks** | 6 (`Window:OnPenProximity` / `OnPenDown` / `OnPenUp` / `OnPenButton` / `OnPenMotion` / `OnPenAxis`) |
| **新 Lua 常量** | 15 (`Light.UI.PEN_INPUT_*` x7 + `PEN_AXIS_*` x8) |

### 1.2 Light.Event 模块 (新, 9 fns)

```lua
Light.Event.HasEvent(type) -> bool
Light.Event.HasEvents(min_type, max_type) -> bool
Light.Event.FlushEvent(type) -> ()
Light.Event.FlushEvents(min_type, max_type) -> ()
Light.Event.Push(type, code, data1, data2) -> bool   -- code/data1/data2 可选 int
Light.Event.Pump() -> ()
Light.Event.SetEnabled(type, enabled) -> ()
Light.Event.IsEnabled(type) -> bool
Light.Event.Register(num) -> int                      -- 返回起始 ID
```

### 1.3 Light.Time 扩展 (2 新 fns)

```lua
Light.Time.AddTimer(ms, fn) -> id        -- ms 后回调 fn(id), 返回非 0 表示成功; fn 返回数字 = 重复间隔, nil = 单次
Light.Time.RemoveTimer(id) -> bool
```

---

## 2. 关键决策 (Q1~Q5 全 A)

| Q | 决策 | 实施细节 |
|---|---|---|
| Q1 Pen hook 签名 | A — 6 独立 hooks | 与 OnKey/OnMouseButton 风格一致 |
| Q2 Event.Push payload | A — 位置参数 (type, code, data1, data2) | SDL_UserEvent 1:1 映射,性能高 |
| Q3 AddTimer 回调机制 | A — SDL_AddTimer + SDL_PushEvent 中转 | 主线程 dispatch user event → Lua callback |
| Q4 Light.Event 归属 | A — 独立顶级模块 (Light.Event) | 与 Light.Process / Light.System 平级 |
| Q5 RegisterEvents | A — 直接转发 SDL_RegisterEvents | 单层 wrapper |

---

## 3. 技术方案

### 3.1 Event POD 字段扩展 (向后兼容)

`platform_window.h` Event 结构追加字段(末尾, 不破坏 ABI):
```cpp
int   penId        = 0;     // PenProximity/Down/Up/Button/Motion/Axis: SDL_PenID
int   penButton    = 0;     // PenButton: 按钮 ID (1~5)
int   penAxis      = 0;     // PenAxis: 轴 ID (0=PRESSURE..7)
float penAxisValue = 0.0f;  // PenAxis: 轴值
int   penEraser    = 0;     // PenDown/Up: 1=橡皮擦末端
int   penAction    = 0;     // PenProximity/Button: 1=in/down, 0=out/up
```

### 3.2 Pen 事件 Lua 签名

```lua
function window:OnPenProximity(penId, action)        -- action: 1=in, 0=out
function window:OnPenDown(penId, x, y, eraser)       -- eraser: 0/1
function window:OnPenUp(penId, x, y, eraser)
function window:OnPenButton(penId, button, action, x, y)  -- action: 1=down, 0=up
function window:OnPenMotion(penId, x, y)
function window:OnPenAxis(penId, axis, value)        -- axis: 0..7, value: float
```

### 3.3 AddTimer 实现要点

1. **启动**: 在 `luaopen_Light_Time` 中 `SDL_RegisterEvents(1)` 注册一个引擎专属事件 ID `g_timerEventType`
2. **AddTimer**: 把 Lua callback `luaL_ref` 到注册表,记录 timer_id → ref 映射 (内部 std::unordered_map)
3. **SDL_AddTimer 回调** (在 SDL 内部线程触发): 调 `SDL_PushEvent(g_timerEventType, code=timer_id)`
4. **DispatchEvents (主线程)**: 收到 g_timerEventType 事件 → 查表找 ref → `lua_rawgeti` + `lua_pcall(0,1)` → 检查返回值, 数字=重复间隔, nil=取消
5. **RemoveTimer**: SDL_RemoveTimer + 释放 ref

### 3.4 跨模块协作

- `light_event.cpp` 新增 → 加入 `ChocoLight/CMakeLists.txt` 源文件列表
- `lumen-master/src/light/light.cpp` `g_lightModules` 加 `{"Light.Event", "luaopen_Light_Event"}`
- `light.h` 声明 `extern "C" int luaopen_Light_Event(lua_State*);`
- `platform_window.h` Event 内 enum 末尾加 6 个 PenXxx (类型号 18~23)
- `platform_window_sdl3.cpp` `PollEvent` 加 8 个 case (in/out 共用 PenProximity, button_down/up 共用 PenButton)

---

## 4. 验收标准 (与 ALIGNMENT 一致)

- [ ] `Light.UI.Window` 元表 6 个 Pen hook 可被覆盖 (string key 检查)
- [ ] `Light.UI` 表 15 个 Pen 常量数值正确
- [ ] `Light.Event` 模块 9 fns 全部 type==function
- [ ] `Light.Time.AddTimer`/`RemoveTimer` 是 function
- [ ] `Light.Event.Register(N)` 返回 number, `Push(type, ...)` 返回 boolean
- [ ] 在无窗口/headless 下所有 fn 安全调用不崩
- [ ] `scripts/smoke/pen_event_timer.lua` lightc 语法 OK + Windows runtime 通过
- [ ] 6 平台 CI 全绿

---

## 5. 已确认的边界

- **不引入 SDL_thread.h** — Lua 多状态架构留独立 Phase
- **不动 g_mainWindow 单例** — 多窗口扩展留独立 Phase
- **不绑 PeepEvents/SetEventFilter/AddEventWatch** — 工程量与价值不匹配, 留 Phase AR.2 (如需)
- **smoke 在 headless 下也要全绿** — 不依赖物理 Pen / 实际 Timer 触发

---

## 6. 实施顺序 (单 commit, 最优路径)

1. `platform_window.h` Event 字段扩展 + 6 个新 enum
2. `platform_window_sdl3.cpp` 8 个 Pen case
3. `light_ui.cpp` 6 dispatch + 6 hooks + 15 const + DispatchEvents 加 case
4. `ChocoLight/src/light_event.cpp` 新模块 9 fns
5. `light.h` / `light_module.cpp` / `lumen/light.cpp` 注册新模块
6. `light_time.cpp` 扩展 AddTimer/RemoveTimer + Timer 事件 ID + DispatchEvents 接管
7. `ChocoLight/CMakeLists.txt` 加 light_event.cpp
8. `scripts/smoke/pen_event_timer.lua` 5 阶段 smoke
9. `.github/workflows/build-templates.yml` 加 pen_event_timer.lua
10. `ACCEPTANCE_PhaseAR.md`
11. commit + push + 等 CI 6 平台全绿
