-- Phase AQ smoke: TextInput + IME (Light.UI.Window 6 fns + Light.UI 13 consts)
--
-- 在 CI runner 上不能创建窗口 (无显示设备), 关键验证:
--   * Light.UI 表上 13 个 TextInput 常量全部存在
--   * Light.UI.Window 表上 6 个 TextInput 方法全部是 function
--   * 在 g_mainWindow == nullptr 状态下调用所有 6 个方法不崩, 优雅返回 false/0
--   * StartTextInput 支持空参 / table 参 / 字符串枚举 / 整数枚举多种风格
--   * 常量数值与 SDL_TEXTINPUT_TYPE_* / SDL_CAPITALIZE_* 一致

local function pass(msg) print(msg) end
local function fail(msg) error("FAIL: " .. tostring(msg), 0) end

-- ==================== 1) require Light.UI / Light.UI.Window ====================

local ok, UI = pcall(require, "Light.UI")
if not ok then fail("require(Light.UI) failed: " .. tostring(UI)) end
if type(UI) ~= "table" then fail("Light.UI not a table") end

local ok2, Window = pcall(require, "Light.UI.Window")
if not ok2 then fail("require(Light.UI.Window) failed: " .. tostring(Window)) end
if type(Window) ~= "table" then fail("Light.UI.Window not a table") end

pass("Light.UI / Light.UI.Window loaded")

-- ==================== 2) 6 fns on Window table ====================

local fn_names = {
    "StartTextInput", "StopTextInput", "IsTextInputActive",
    "SetTextInputArea", "ClearComposition", "IsScreenKeyboardShown",
}
for _, k in ipairs(fn_names) do
    if type(Window[k]) ~= "function" then
        fail("Light.UI.Window." .. k .. " is not a function (got " ..
             type(Window[k]) .. ")")
    end
end
pass("Light.UI.Window TextInput fns ok (" .. #fn_names .. " methods)")

-- ==================== 3) 13 constants on Light.UI ====================

local expected_consts = {
    TEXTINPUT_TYPE_TEXT                    = 0,
    TEXTINPUT_TYPE_NAME                    = 1,
    TEXTINPUT_TYPE_EMAIL                   = 2,
    TEXTINPUT_TYPE_USERNAME                = 3,
    TEXTINPUT_TYPE_PASSWORD_HIDDEN         = 4,
    TEXTINPUT_TYPE_PASSWORD_VISIBLE        = 5,
    TEXTINPUT_TYPE_NUMBER                  = 6,
    TEXTINPUT_TYPE_NUMBER_PASSWORD_HIDDEN  = 7,
    TEXTINPUT_TYPE_NUMBER_PASSWORD_VISIBLE = 8,
    CAPITALIZATION_NONE                    = 0,
    CAPITALIZATION_SENTENCES               = 1,
    CAPITALIZATION_WORDS                   = 2,
    CAPITALIZATION_LETTERS                 = 3,
}
for k, expected in pairs(expected_consts) do
    if type(UI[k]) ~= "number" then
        fail("Light.UI." .. k .. " not a number (got " .. type(UI[k]) .. ")")
    end
    if UI[k] ~= expected then
        fail("Light.UI." .. k .. " expected " .. expected .. ", got " ..
             tostring(UI[k]))
    end
end
pass("Light.UI TextInput constants ok (13 consts)")

-- ==================== 4) Headless 行为: 无窗口时安全调用 ====================
-- 此 stage 在 CI 没有 GL 上下文/无显示设备的情况下,
-- g_mainWindow == nullptr, 实现应优雅返回 false/0 而不崩

-- 构造一个空 self table (Window:Method(...) 的方法接收 self 作 arg 1, 我们
-- 不依赖它的内容, 只需类型为 table 即可避免 luaL_checktype 失败)
local self_stub = {}

-- StartTextInput 无参: 应返回 false (因 g_mainWindow == nullptr)
local r = Window.StartTextInput(self_stub)
if type(r) ~= "boolean" then
    fail("StartTextInput() should return boolean, got " .. type(r))
end
if r ~= false then
    -- 如果 CI 实际有窗口, 也允许 true; 但 sandbox 模式默认 false
    pass("StartTextInput() returned true (CI has active window?)")
else
    pass("StartTextInput() returned false (no active window) - expected")
end

-- StopTextInput 不返回值, 仅验证不崩
Window.StopTextInput(self_stub)
pass("StopTextInput() ok (no crash)")

-- IsTextInputActive() 应返回 boolean
local active = Window.IsTextInputActive(self_stub)
if type(active) ~= "boolean" then
    fail("IsTextInputActive() should return boolean, got " .. type(active))
end
pass("IsTextInputActive() = " .. tostring(active))

-- SetTextInputArea(x,y,w,h) - 4 args 形式
Window.SetTextInputArea(self_stub, 100, 200, 300, 40)
pass("SetTextInputArea(100,200,300,40) ok")

-- SetTextInputArea(x,y,w,h,cursor) - 5 args 形式
Window.SetTextInputArea(self_stub, 100, 200, 300, 40, 5)
pass("SetTextInputArea(100,200,300,40, cursor=5) ok")

-- ClearComposition - 不返回值, 仅验证不崩
Window.ClearComposition(self_stub)
pass("ClearComposition() ok (no crash)")

-- IsScreenKeyboardShown() 应返回 boolean
local shown = Window.IsScreenKeyboardShown(self_stub)
if type(shown) ~= "boolean" then
    fail("IsScreenKeyboardShown() should return boolean, got " .. type(shown))
end
pass("IsScreenKeyboardShown() = " .. tostring(shown))

-- ==================== 5) StartTextInput with table props ====================
-- 测试 props table 解析: 字符串枚举 / 整数枚举 / 布尔字段 / 部分字段

-- 5.1 字符串枚举
local r1 = Window.StartTextInput(self_stub, {
    type           = "email",
    capitalization = "sentences",
    autocorrect    = true,
    multiline      = false,
})
if type(r1) ~= "boolean" then
    fail("StartTextInput(props) should return boolean, got " .. type(r1))
end
pass("StartTextInput({type='email', cap='sentences', ...}) = " .. tostring(r1))

-- 5.2 整数枚举 (用常量)
local r2 = Window.StartTextInput(self_stub, {
    type           = UI.TEXTINPUT_TYPE_NUMBER,
    capitalization = UI.CAPITALIZATION_NONE,
})
if type(r2) ~= "boolean" then
    fail("StartTextInput(int_props) should return boolean, got " .. type(r2))
end
pass("StartTextInput({type=NUMBER, cap=NONE}) = " .. tostring(r2))

-- 5.3 部分字段 (只设 type)
local r3 = Window.StartTextInput(self_stub, { type = "password" })
if type(r3) ~= "boolean" then
    fail("StartTextInput({type='password'}) should return boolean")
end
pass("StartTextInput({type='password'}) = " .. tostring(r3))

-- 5.4 空 props table (相当于无参)
local r4 = Window.StartTextInput(self_stub, {})
if type(r4) ~= "boolean" then
    fail("StartTextInput({}) should return boolean")
end
pass("StartTextInput({}) = " .. tostring(r4))

-- 5.5 未知字符串枚举: 不崩 (会被 ParseXxx 返回 -1, 等于不设置)
local r5 = Window.StartTextInput(self_stub, {
    type           = "unknown_garbage_value_xx",
    capitalization = "also_garbage",
})
if type(r5) ~= "boolean" then
    fail("StartTextInput(garbage_props) should return boolean")
end
pass("StartTextInput({type=garbage, cap=garbage}) = " .. tostring(r5) ..
     " (no crash on unknown enum)")

-- ==================== 6) Light.Keyboard 仍可用 (不破坏现有模块) ====================

local ok3, Kb = pcall(require, "Light.Keyboard")
if not ok3 then fail("require(Light.Keyboard) failed: " .. tostring(Kb)) end
if type(Kb.HasScreenKeyboardSupport) ~= "function" then
    fail("Light.Keyboard.HasScreenKeyboardSupport missing - regression!")
end
pass("Light.Keyboard.HasScreenKeyboardSupport still present (no regression)")

print("textinput smoke ok")
