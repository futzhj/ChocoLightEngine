# Phase AQ — TextInput + IME — 验收文档

> **状态**: 已完成 (本地实施 + lightc 语法检查通过, 待 GitHub Actions CI 全平台验证)

---

## 一、实施概览

| 项 | 数量 / 内容 |
|---|---|
| 新增 C++ 函数 (PlatformWindow 抽象层) | 7 (Start/StartWithProps/Stop/IsActive/SetArea/ClearComposition/IsScreenKeyboardShown) |
| 新增 Lua 函数 (Light.UI.Window 方法) | 6 (StartTextInput/StopTextInput/IsTextInputActive/SetTextInputArea/ClearComposition/IsScreenKeyboardShown) |
| 新增 Lua 回调 hook (Window:OnXxx) | 2 (OnTextInput / OnTextEditing) |
| 新增 Lua 常量 (Light.UI.*) | 13 (9 个 TEXTINPUT_TYPE_* + 4 个 CAPITALIZATION_*) |
| 新增 Event::Type | 1 (TextEditing = 17) |
| 新增 Event 字段 | 2 (text_start, text_length) |
| Event::text 缓冲区 | 32 bytes → 128 bytes |
| 字符串枚举 (StartTextInput props) | 13 (9 type + 4 capitalization) |
| Smoke 阶段 | 6 (require / 6 fns / 13 consts / 无窗口安全 / 5 种 props 形式 / 兼容性) |
| 修改文件数 | 4 (platform_window.h, platform_window_sdl3.cpp, light_ui.cpp, build-templates.yml) |
| 新增文件数 | 5 (textinput.lua, ALIGNMENT/CONSENSUS/ACCEPTANCE/TODO 文档) |

---

## 二、API 详细列表

### 2.1 `Light.UI.Window` 新增方法 (6 个)

| Lua 方法 | 签名 | 行为 |
|---|---|---|
| `window:StartTextInput()` | `() -> bool` | 启动文本输入,移动端弹软键盘 |
| `window:StartTextInput(props)` | `(table) -> bool` | 同上,但带属性 (type/capitalization/autocorrect/multiline) |
| `window:StopTextInput()` | `()` | 停止文本输入,移动端收起软键盘 |
| `window:IsTextInputActive()` | `() -> bool` | 查询是否在接收文本输入 |
| `window:SetTextInputArea(x,y,w,h, [cursor])` | `(int,int,int,int, int?) -> ()` | 指定 IME 候选词窗口对齐区域 (像素) |
| `window:ClearComposition()` | `()` | 取消当前 IME 组合态 |
| `window:IsScreenKeyboardShown()` | `() -> bool` | 软键盘是否显示中 |

#### `props` table 字段

| 字段 | 类型 | 字符串枚举值 | 整数枚举值 |
|---|---|---|---|
| `type` | string \| integer | `"text"` `"name"` `"email"` `"username"` `"password"` `"password_visible"` `"number"` `"number_password"` `"number_password_visible"` | 0~8 (`Light.UI.TEXTINPUT_TYPE_*`) |
| `capitalization` | string \| integer | `"none"` `"sentences"` `"words"` `"letters"` | 0~3 (`Light.UI.CAPITALIZATION_*`) |
| `autocorrect` | boolean | `true` / `false` | - |
| `multiline` | boolean | `true` / `false` | - |

未设置的字段沿用平台默认。所有字段都可选。

### 2.2 `Light.UI` 新增常量 (13 个)

```lua
-- TEXTINPUT_TYPE_* (与 SDL_TEXTINPUT_TYPE_* 一致)
Light.UI.TEXTINPUT_TYPE_TEXT                    = 0
Light.UI.TEXTINPUT_TYPE_NAME                    = 1
Light.UI.TEXTINPUT_TYPE_EMAIL                   = 2
Light.UI.TEXTINPUT_TYPE_USERNAME                = 3
Light.UI.TEXTINPUT_TYPE_PASSWORD_HIDDEN         = 4
Light.UI.TEXTINPUT_TYPE_PASSWORD_VISIBLE        = 5
Light.UI.TEXTINPUT_TYPE_NUMBER                  = 6
Light.UI.TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN  = 7
Light.UI.TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE = 8

-- CAPITALIZATION_* (与 SDL_CAPITALIZE_* 一致)
Light.UI.CAPITALIZATION_NONE      = 0
Light.UI.CAPITALIZATION_SENTENCES = 1
Light.UI.CAPITALIZATION_WORDS     = 2
Light.UI.CAPITALIZATION_LETTERS   = 3
```

### 2.3 Lua 回调 (2 个 hook,挂在 Window 表上由游戏脚本设置)

```lua
function window:OnTextInput(text)
    -- text: UTF-8 string, IME 已提交的文本
end

function window:OnTextEditing(text, start, length)
    -- text:   UTF-8 string, IME 当前组合态
    -- start:  组合区光标起点 (UTF-8 字节偏移)
    -- length: 组合区选区长度 (UTF-8 字节)
end
```

### 2.4 PlatformWindow 抽象层 C++ API (内部, 7 个)

```cpp
// platform_window.h
struct TextInputProps { int type, capitalization, autocorrect, multiline; };

bool StartTextInput(void* win);
bool StartTextInputWithProps(void* win, const TextInputProps& props);
void StopTextInput(void* win);
bool IsTextInputActive(void* win);
void SetTextInputArea(void* win, int x, int y, int w, int h, int cursor);
void ClearComposition(void* win);
bool IsScreenKeyboardShown(void* win);
```

---

## 三、关键设计决策 (与 CONSENSUS 对齐)

| 决策 | 实现 |
|---|---|
| Q1 API 归属 | `window:StartTextInput()` 等 6 方法挂在 `Light.UI.Window` 元表 |
| Q2 缓冲区 | `Event::text[32]` → `[128]` 容纳长 IME 短语 |
| Q3 回调签名 | `OnTextEditing(text, start, length)` 位置参数,与 OnKey 风格一致 |
| Q4 Props 形式 | Lua table → SDL_PropertiesID,所有默认走快速路径免创建 props |
| Q5 SetTextInputArea | `(x, y, w, h, cursor)` 像素参数 |

---

## 四、Smoke 测试覆盖

`scripts/smoke/textinput.lua` (6 个 stage):

1. **Module 加载**: `require("Light.UI")` + `require("Light.UI.Window")` 都成功
2. **6 个 Window 方法注册**: 全部 `type == "function"`
3. **13 个 UI 常量**: 类型为 number,数值与预期一致
4. **无窗口安全行为**: 6 个方法在 `g_mainWindow == nullptr` 时全部不崩,优雅返回 false/0
5. **props 5 种形式**: 字符串枚举 / 整数枚举 / 部分字段 / 空 table / 未知字符串 (容错)
6. **兼容性**: `Light.Keyboard` 仍可加载,`HasScreenKeyboardSupport` 仍存在

---

## 五、本地验证

```pwsh
# 语法检查
lumen-master\build\src\lightc\Release\lightc.exe -p scripts\smoke\textinput.lua
# Exit=0 ✓

# CI 编译 + 运行 (待 push 触发)
```

---

## 六、回归影响评估

| 受影响模块 | 影响 |
|---|---|
| `Event` POD layout | 新增字段,但只在末尾追加,无 ABI 兼容性问题 |
| `Event::text` 缓冲区 | 32 → 128 bytes, 旧调用者 (Lua dispatch) 不受影响 |
| 已注册的 13 个 Window 方法 | 不变,仅追加 6 个新方法 |
| `Light.UI` 表 | 仅追加 13 个常量字段 |
| `light_keyboard.cpp` | **零修改**, 现有 15 fns 不变 |
| `light_input.cpp` | **零修改**, TextInput 不进入 polling state (走 hook 模式) |

---

## 七、未实施项 (明确不做)

- **Light.Properties 通用模块**: SDL3 Properties 通用 binding,工程量大,推迟到独立 Phase
- **EVENT_TEXT_EDITING_CANDIDATES**: SDL3 候选词列表事件,极少游戏需要,放弃
- **UI 控件库 TextField 组件**: 属于 UI 库范畴,本 Phase 只做底层绑定
- **iOS/Android 软键盘外观自定义**: SDL3 平台限制,不可控

---

## 八、已知 LSP 警告

`scripts/smoke/textinput.lua` 无 LSP 警告。`scripts/smoke/physics.lua` 仍有 32+ 个 "需要判空" 警告 — 这些是 Phase AP 遗留的 LSP 误报 (`fail()` 是 noreturn 函数,LSP 不识别),与本 Phase 无关,运行时无影响。

---

## 九、CI 验收标准

- [x] `lightc -p scripts/smoke/textinput.lua` Exit=0 (本地)
- [ ] GitHub Actions `Build Templates (All Platforms)` 全绿:
  - [ ] Windows x64: 编译 + Windows runtime smoke (含 textinput.lua)
  - [ ] Linux x64: 编译 + 语法检查
  - [ ] macOS Universal: 编译 + 语法检查
  - [ ] Android arm64+x86_64: 编译
  - [ ] iOS arm64: 编译
  - [ ] Web WASM: 编译

CI 全绿后此 Phase 才算最终交付完成。
