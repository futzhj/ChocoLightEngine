# Phase AQ — TextInput + IME — 对齐文档

> 目标: **文本输入能力**, SDL3 TextInput/TextEditing + IME 候选词事件绑定。
>
> 前置: Phase AL/AM/AN/AO/AP 已完成。Event::TextInput 已定义且 SDL3 已解析,但 **未 dispatch 到 Lua**。

---

## 一、项目上下文分析

### 1.1 现有基础设施

| 位置 | 现状 |
|---|---|
| `@e:\jinyiNew\Light\ChocoLight\include\platform_window.h:49` | `Event::TextInput = 12` 类型已定义 |
| `@e:\jinyiNew\Light\ChocoLight\include\platform_window.h:69` | `char text[32]` 字段存放 UTF-8 |
| `@e:\jinyiNew\Light\ChocoLight\src\platform_window_sdl3.cpp:380-383` | `SDL_EVENT_TEXT_INPUT` 已转换为 `Event::TextInput` |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp:222-224` | `DispatchEvents` 的 default 分支 **吞掉了 TextInput** — ⚠️ 缺口 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_keyboard.cpp:7-11` | 注释明确说 "StartTextInput 等 Window-coupled API 延后到 shared Window type 后" — 时机到了 |

### 1.2 缺失的 SDL3 API (按重要性)

**🔴 必须 (P0)**:
- `SDL_StartTextInput(window)` / `SDL_StopTextInput(window)` / `SDL_TextInputActive(window)`
- `SDL_EVENT_TEXT_EDITING` (IME 组合态,含 text/start/length) — 当前完全未处理
- 将 `Event::TextInput` / `Event::TextEditing` dispatch 到 Lua 回调

**🟡 重要 (P1)**:
- `SDL_SetTextInputArea(window, rect, cursor)` — IME 候选框定位 (中日韩输入关键)
- `SDL_ClearComposition(window)` — 取消当前 IME 组合
- `SDL_ScreenKeyboardShown(window)` — 移动端软键盘状态
- `SDL_StartTextInputWithProperties(window, props)` — 指定 TYPE_NUMBER/EMAIL/PASSWORD,移动端控制键盘类型

**🟢 可选 (P2)**:
- `SDL_EVENT_TEXT_EDITING_CANDIDATES` — 候选词列表事件 (用于自定义 IME UI)

### 1.3 体系冲突/一致性

- **事件缓冲区**: 当前 `Event::text[32]` 固定大小。IME 合成的日文长短语会 >32 byte,需扩容到 **128 byte** 或改为 `std::string`。
- **Event::TextInput 已有**,但 Event::TextEditing 还没有对应类型,需要在 `platform_window.h::Event::Type` 加一个新值。
- **回调模式**: 现有事件走 `light_ui.cpp` 的 hook 注册模式 (`OnKey/OnMouseButton/OnFrame`),新事件应保持一致 → `OnTextInput(fn)` / `OnTextEditing(fn)`。

---

## 二、需求理解

### 2.1 目标使用场景

1. **文本输入框** (UI 控件): 玩家在游戏内填角色名、聊天、设置
2. **中日韩 IME**: 玩家打中文,IME 候选词窗口位置正确
3. **移动端软键盘**: iOS/Android 点击输入框自动弹软键盘, `type=email` 时弹邮箱键盘
4. **代码编辑器/调试 Console**: (可选) 游戏内 debug tty

### 2.2 估算工作量

| 部分 | fns | 备注 |
|---|---|---|
| `platform_window.h/cpp` 扩展 Event::TextEditing + 缓冲区扩容 | - | 基础设施改造 |
| `light_ui.cpp` dispatch 2 种新事件 + 注册 2 个 hook | 2 | OnTextInput/OnTextEditing |
| `Light.Keyboard` 新方法: Start/Stop/IsActive/SetArea/ClearComposition/ScreenKeyboardShown | 6 | window-coupled, 改用 Window userdata |
| `Light.Keyboard.StartTextInputWithProperties` | 1 | 10 个 props field 作为 options table |
| 常量: TEXTINPUT_TYPE_*, CAPITALIZATION_* 等 | ~12 常量 | |
| 修改 `light_ui.cpp::DispatchEvents` 添加 2 个 case | - | |
| Smoke: IME 模拟 + 回调触发 + Props 分支 | +4 stages | |
| 文档 + 常量说明 | - | |
| **总计** | **~9 fns + 12 常量 + 2 hooks** | |

### 2.3 边界确认 — 任务范围

**✅ 包含**:
- 基础 Start/Stop TextInput 绑定 (P0)
- Event::TextInput / TextEditing 的 Lua dispatch (P0)
- IME 候选框区域设置 (P1)
- Screen Keyboard 状态查询 (P1)
- `StartTextInputWithProperties` 配置项 (P1)

**❌ 排除** (明确不做):
- 自定义 IME 候选词 UI (P2: EVENT_TEXT_EDITING_CANDIDATES — 极少游戏需要,放弃)
- UI 控件库中的文本框组件 (属于 UI 库范畴,本 Phase 只做底层绑定)
- iOS/Android 键盘自定义外观 (平台限制, SDL3 无法控制)

---

## 三、疑问澄清 — 关键决策点

### ⚠️ Q1: Window-coupled API 的归属

`SDL_StartTextInput(window)` 需要 `SDL_Window*` 参数。如何绑定到 Lua?

| 方案 | API 风格 | 优缺点 |
|---|---|---|
| **A (推荐)** `window:StartTextInput()` — UI.Window 方法 | `local win = Light.UI.Window.Create(...); win:StartTextInput()` | ✅ 与 SDL3 签名一致 ✅ 面向对象 ❌ 必须从 `light_ui.cpp::LightWindow` userdata 取 SDL_Window |
| **B** `Light.Keyboard.StartTextInput(window)` — 显式参数 | `Light.Keyboard.StartTextInput(win)` | ✅ 与 `Light.Keyboard` 其他 API 一致 ❌ 需要从 Keyboard 访问 Window userdata |
| **C** `Light.Keyboard.StartTextInput()` — 隐式当前 Window | `Light.Keyboard.StartTextInput()` — 内部用 `cc_core` 的 g_window | ✅ 最简单 ❌ 不支持多窗口 |

**推荐**: **A**, 新 6 个方法挂到 `light_ui.cpp` 里 `LightWindow` 的 method 表 (`window:XxxTextInput`)。常量和事件 dispatch 还在 `Light.Keyboard` / `light_ui.cpp` 的 hook 注册。

---

### ⚠️ Q2: Event 缓冲区扩容方式

现 `Event::text[32]` 不够 IME 长短语。

| 方案 | 优缺点 |
|---|---|
| **A (推荐)** 扩容为 `char text[128]` | ✅ 栈上, POD, 简单 ✅ 128 byte 已覆盖 99% IME 短语 ❌ 极少长短语会截断 |
| **B** `std::string text` | ✅ 无上限 ❌ `Event` 从 POD 变为有构造函数, 影响 ABI |
| **C** `char* text` + 手工 dup/free | ✅ 无上限 ❌ 生命周期复杂, 易泄漏 |

**推荐**: **A**, 保持 POD, 128 已足够。

---

### ⚠️ Q3: TextEditing 事件派发参数

IME 组合态事件含 3 个字段: `text`, `start`, `length`。Lua 回调签名?

| 方案 | 签名 |
|---|---|
| **A (推荐)** | `fn(text, cursor_start, cursor_length)` — 位置参数, 与现有 OnKey 风格一致 |
| **B** | `fn({text=..., start=..., length=...})` — table, 未来易扩展 |

**推荐**: **A**, 与 `OnKey(key, scancode, action, mods)` 一致。

---

### ⚠️ Q4: StartTextInputWithProperties 的 props 形式

`SDL_StartTextInputWithProperties` 用 SDL 的 PropertiesID (底层是 `hashmap<string, prop>`)。Lua 侧接收?

| 方案 | 调用形式 |
|---|---|
| **A (推荐)** | `win:StartTextInput({type="email", capitalization="sentences", multiline=false, autocorrect=true})` — 自动建 SDL_PropertiesID → 设置 → 传入 → 销毁 |
| **B** | 暴露 `Light.Properties` 模块 (SDL3 通用 Properties 绑定) — 大工程, 跨模块 |

**推荐**: **A**, 仅这一个 API 用 table → C++ 内部转 PropertiesID,不展开 SDL_Properties 全模块 (否则要单独一个 Phase)。

---

### ⚠️ Q5: `SetTextInputArea` 的坐标单位

SDL3 该 API 要求 SDL_Rect (像素)。

| 方案 | 签名 |
|---|---|
| **A (推荐)** | `win:SetTextInputArea(x, y, w, h, cursor?)` — 像素, 与 UI 绘制坐标一致 |
| **B** | `win:SetTextInputArea(rect_table)` — `{x, y, w, h}` |

**推荐**: **A**, 与 `Light.Graphics.DrawRect(x, y, w, h)` 一致。

---

## 四、验收标准

- 9 个新 fns + 12 常量全部注册,`ping` 测试通过
- `window:StartTextInput()` 后, 键盘输入触发 `OnTextInput` 回调, text 为 UTF-8
- `OnTextEditing` 在中文输入法合成期触发, start/length 正确
- `win:SetTextInputArea(100, 200, 300, 40)` 后候选词窗口定位到该区域 (手工验证, smoke 仅验证不崩)
- `StartTextInputWithProperties({type="number"})` 在移动端弹数字键盘 (移动端手工, Windows 仅验证不崩)
- 6 平台 CI 全绿,Windows runtime smoke `textinput.lua` 通过

---

## 五、下一步

等待用户对 **Q1~Q5** 的确认,然后进入 Consensus 阶段固化决策,再进入 Architect/Atomize/Automate。
