/**
 * @file light_input.cpp
 * @brief Light.Input 模块 — 统一输入管理器 (键盘/鼠标/触摸/手柄/虚拟动作)
 *
 * Lua API:
 *   Light.Input.IsKeyDown(key)              → bool
 *   Light.Input.IsKeyPressed(key)           → bool (当帧按下)
 *   Light.Input.IsMouseDown(button)         → bool
 *   Light.Input.GetMousePosition()          → x, y
 *   Light.Input.GetMouseWheel()             → dx, dy
 *   Light.Input.GetTouchCount()             → int
 *   Light.Input.GetTouch(index)             → id, x, y, pressure
 *   Light.Input.GetGamepadCount()           → int
 *   Light.Input.IsGamepadConnected(idx)     → bool
 *   Light.Input.GetGamepadButton(idx, btn)  → bool
 *   Light.Input.GetGamepadAxis(idx, axis)   → float (-1..1)
 *   Light.Input.GetGamepadName(idx)         → string
 *   Light.Input.AddAction(name, mapping)    → void
 *   Light.Input.IsActionDown(name)          → bool
 *   Light.Input.Update()                    → void (帧末调用, 更新 prev 状态)
 */

#include "light.h"
#include "platform_window.h"
#include <cstring>
#include <cmath>
#include <unordered_map>

// ==================== 输入状态 ====================

static constexpr int MAX_KEYS       = 512;
static constexpr int MAX_MOUSE_BTN  = 8;
static constexpr int MAX_TOUCHES    = 10;
static constexpr int MAX_GAMEPADS   = 4;
static constexpr int MAX_GP_BUTTONS = 32;
static constexpr int MAX_GP_AXES    = 8;
static constexpr float AXIS_DEADZONE = 0.1f;

struct TouchState {
    int   id;
    float x, y, pressure;
    bool  active;
};

struct GamepadState {
    bool  connected;
    int   sdlId;           // SDL instance ID
    bool  buttons[MAX_GP_BUTTONS];
    float axes[MAX_GP_AXES];
    char  name[64];
};

// 虚拟动作映射
struct ActionMapping {
    int  key;        // 键码 (-1 = 无)
    int  mouseBtn;   // 鼠标按钮 (-1 = 无)
    int  gpButton;   // 手柄按钮 (-1 = 无)
    int  gpIndex;    // 手柄索引 (默认 0)
};

static struct {
    // 键盘
    bool keyCurrent[MAX_KEYS];
    bool keyPrevious[MAX_KEYS];
    // 鼠标
    bool  mouseBtn[MAX_MOUSE_BTN];
    float mouseX, mouseY;
    float wheelDx, wheelDy;
    // 触摸
    TouchState touches[MAX_TOUCHES];
    int touchCount;
    // 手柄
    GamepadState gamepads[MAX_GAMEPADS];
    int gamepadCount;
    // 虚拟动作
    std::unordered_map<std::string, ActionMapping> actions;
} s_input = {};

// ==================== 内部: 事件处理 ====================

// 由 light_ui.cpp 中 DispatchEvents 同步调用, 或直接由 Input.Update 调用
void InputProcessEvent(const PlatformWindow::Event& ev) {
    switch (ev.type) {
        case PlatformWindow::Event::KeyDown:
            if (ev.keycode >= 0 && ev.keycode < MAX_KEYS)
                s_input.keyCurrent[ev.keycode] = true;
            break;
        case PlatformWindow::Event::KeyUp:
            if (ev.keycode >= 0 && ev.keycode < MAX_KEYS)
                s_input.keyCurrent[ev.keycode] = false;
            break;
        case PlatformWindow::Event::MouseDown:
            if (ev.button >= 0 && ev.button < MAX_MOUSE_BTN)
                s_input.mouseBtn[ev.button] = true;
            s_input.mouseX = (float)ev.x;
            s_input.mouseY = (float)ev.y;
            break;
        case PlatformWindow::Event::MouseUp:
            if (ev.button >= 0 && ev.button < MAX_MOUSE_BTN)
                s_input.mouseBtn[ev.button] = false;
            s_input.mouseX = (float)ev.x;
            s_input.mouseY = (float)ev.y;
            break;
        case PlatformWindow::Event::MouseMove:
            s_input.mouseX = (float)ev.x;
            s_input.mouseY = (float)ev.y;
            break;
        case PlatformWindow::Event::MouseWheel:
            s_input.wheelDx += (float)ev.dx;
            s_input.wheelDy += (float)ev.dy;
            break;
        case PlatformWindow::Event::TouchDown:
        case PlatformWindow::Event::TouchMove: {
            // 查找或分配触摸槽
            int slot = -1;
            for (int i = 0; i < MAX_TOUCHES; i++) {
                if (s_input.touches[i].active && s_input.touches[i].id == ev.touchId) {
                    slot = i; break;
                }
            }
            if (slot < 0 && ev.type == PlatformWindow::Event::TouchDown) {
                for (int i = 0; i < MAX_TOUCHES; i++) {
                    if (!s_input.touches[i].active) { slot = i; break; }
                }
            }
            if (slot >= 0) {
                s_input.touches[slot].id = ev.touchId;
                s_input.touches[slot].x = (float)ev.x;
                s_input.touches[slot].y = (float)ev.y;
                s_input.touches[slot].pressure = ev.pressure;
                s_input.touches[slot].active = true;
                // 更新 touchCount
                s_input.touchCount = 0;
                for (int i = 0; i < MAX_TOUCHES; i++)
                    if (s_input.touches[i].active) s_input.touchCount++;
            }
            break;
        }
        case PlatformWindow::Event::TouchUp:
            for (int i = 0; i < MAX_TOUCHES; i++) {
                if (s_input.touches[i].active && s_input.touches[i].id == ev.touchId) {
                    s_input.touches[i].active = false;
                    break;
                }
            }
            s_input.touchCount = 0;
            for (int i = 0; i < MAX_TOUCHES; i++)
                if (s_input.touches[i].active) s_input.touchCount++;
            break;
        case PlatformWindow::Event::GamepadButton: {
            // 找手柄槽
            for (int i = 0; i < MAX_GAMEPADS; i++) {
                if (s_input.gamepads[i].connected && s_input.gamepads[i].sdlId == ev.gamepadId) {
                    if (ev.gpButton >= 0 && ev.gpButton < MAX_GP_BUTTONS)
                        s_input.gamepads[i].buttons[ev.gpButton] = (ev.gpAction != 0);
                    break;
                }
            }
            break;
        }
        case PlatformWindow::Event::GamepadAxis: {
            for (int i = 0; i < MAX_GAMEPADS; i++) {
                if (s_input.gamepads[i].connected && s_input.gamepads[i].sdlId == ev.gamepadId) {
                    if (ev.gpAxis >= 0 && ev.gpAxis < MAX_GP_AXES) {
                        float v = ev.gpAxisValue;
                        // 死区过滤
                        s_input.gamepads[i].axes[ev.gpAxis] = (fabsf(v) < AXIS_DEADZONE) ? 0.0f : v;
                    }
                    break;
                }
            }
            break;
        }
        case PlatformWindow::Event::GamepadConnect: {
            if (ev.gpAction) {
                // 连接: 找空槽
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (!s_input.gamepads[i].connected) {
                        s_input.gamepads[i].connected = true;
                        s_input.gamepads[i].sdlId = ev.gamepadId;
                        memset(s_input.gamepads[i].buttons, 0, sizeof(s_input.gamepads[i].buttons));
                        memset(s_input.gamepads[i].axes, 0, sizeof(s_input.gamepads[i].axes));
                        snprintf(s_input.gamepads[i].name, sizeof(s_input.gamepads[i].name), "Gamepad %d", i);
                        s_input.gamepadCount++;
                        break;
                    }
                }
            } else {
                // 断开
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (s_input.gamepads[i].connected && s_input.gamepads[i].sdlId == ev.gamepadId) {
                        s_input.gamepads[i].connected = false;
                        s_input.gamepadCount--;
                        break;
                    }
                }
            }
            break;
        }
        default: break;
    }
}

// ==================== Lua API ====================

/// Input.IsKeyDown(key) → bool
static int l_Input_IsKeyDown(lua_State* L) {
    int key = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, (key >= 0 && key < MAX_KEYS) ? s_input.keyCurrent[key] : 0);
    return 1;
}

/// Input.IsKeyPressed(key) → bool (当帧首次按下)
static int l_Input_IsKeyPressed(lua_State* L) {
    int key = (int)luaL_checkinteger(L, 1);
    bool pressed = false;
    if (key >= 0 && key < MAX_KEYS)
        pressed = s_input.keyCurrent[key] && !s_input.keyPrevious[key];
    lua_pushboolean(L, pressed);
    return 1;
}

/// Input.IsMouseDown(button) → bool
static int l_Input_IsMouseDown(lua_State* L) {
    int btn = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, (btn >= 0 && btn < MAX_MOUSE_BTN) ? s_input.mouseBtn[btn] : 0);
    return 1;
}

/// Input.GetMousePosition() → x, y
static int l_Input_GetMousePosition(lua_State* L) {
    lua_pushnumber(L, s_input.mouseX);
    lua_pushnumber(L, s_input.mouseY);
    return 2;
}

/// Input.GetMouseWheel() → dx, dy
static int l_Input_GetMouseWheel(lua_State* L) {
    lua_pushnumber(L, s_input.wheelDx);
    lua_pushnumber(L, s_input.wheelDy);
    return 2;
}

/// Input.GetTouchCount() → int
static int l_Input_GetTouchCount(lua_State* L) {
    lua_pushinteger(L, s_input.touchCount);
    return 1;
}

/// Input.GetTouch(index) → id, x, y, pressure  (index 从 1 开始)
static int l_Input_GetTouch(lua_State* L) {
    int idx = (int)luaL_checkinteger(L, 1) - 1;
    // 遍历活跃触摸, 找第 idx 个
    int count = 0;
    for (int i = 0; i < MAX_TOUCHES; i++) {
        if (s_input.touches[i].active) {
            if (count == idx) {
                lua_pushinteger(L, s_input.touches[i].id);
                lua_pushnumber(L, s_input.touches[i].x);
                lua_pushnumber(L, s_input.touches[i].y);
                lua_pushnumber(L, s_input.touches[i].pressure);
                return 4;
            }
            count++;
        }
    }
    return 0;
}

/// Input.GetGamepadCount() → int
static int l_Input_GetGamepadCount(lua_State* L) {
    lua_pushinteger(L, s_input.gamepadCount);
    return 1;
}

/// Input.IsGamepadConnected(idx) → bool (idx 从 1 开始)
static int l_Input_IsGamepadConnected(lua_State* L) {
    int idx = (int)luaL_checkinteger(L, 1) - 1;
    lua_pushboolean(L, (idx >= 0 && idx < MAX_GAMEPADS) ? s_input.gamepads[idx].connected : 0);
    return 1;
}

/// Input.GetGamepadButton(idx, btn) → bool
static int l_Input_GetGamepadButton(lua_State* L) {
    int idx = (int)luaL_checkinteger(L, 1) - 1;
    int btn = (int)luaL_checkinteger(L, 2);
    bool down = false;
    if (idx >= 0 && idx < MAX_GAMEPADS && s_input.gamepads[idx].connected) {
        if (btn >= 0 && btn < MAX_GP_BUTTONS)
            down = s_input.gamepads[idx].buttons[btn];
    }
    lua_pushboolean(L, down);
    return 1;
}

/// Input.GetGamepadAxis(idx, axis) → float (-1..1)
static int l_Input_GetGamepadAxis(lua_State* L) {
    int idx  = (int)luaL_checkinteger(L, 1) - 1;
    int axis = (int)luaL_checkinteger(L, 2);
    float val = 0.0f;
    if (idx >= 0 && idx < MAX_GAMEPADS && s_input.gamepads[idx].connected) {
        if (axis >= 0 && axis < MAX_GP_AXES)
            val = s_input.gamepads[idx].axes[axis];
    }
    lua_pushnumber(L, val);
    return 1;
}

/// Input.GetGamepadName(idx) → string
static int l_Input_GetGamepadName(lua_State* L) {
    int idx = (int)luaL_checkinteger(L, 1) - 1;
    if (idx >= 0 && idx < MAX_GAMEPADS && s_input.gamepads[idx].connected) {
        lua_pushstring(L, s_input.gamepads[idx].name);
    } else {
        lua_pushstring(L, "");
    }
    return 1;
}

/// Input.AddAction(name, {key=K, mouseBtn=B, gamepadBtn=G, gamepadIdx=I})
static int l_Input_AddAction(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    ActionMapping m = { -1, -1, -1, 0 };

    lua_getfield(L, 2, "key");
    if (lua_isnumber(L, -1)) m.key = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "mouseBtn");
    if (lua_isnumber(L, -1)) m.mouseBtn = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "gamepadBtn");
    if (lua_isnumber(L, -1)) m.gpButton = (int)lua_tointeger(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, 2, "gamepadIdx");
    if (lua_isnumber(L, -1)) m.gpIndex = (int)lua_tointeger(L, -1) - 1;
    lua_pop(L, 1);

    s_input.actions[name] = m;
    return 0;
}

/// Input.IsActionDown(name) → bool
static int l_Input_IsActionDown(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    auto it = s_input.actions.find(name);
    if (it == s_input.actions.end()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    const auto& m = it->second;
    bool down = false;
    // 键盘
    if (m.key >= 0 && m.key < MAX_KEYS)
        down |= s_input.keyCurrent[m.key];
    // 鼠标
    if (m.mouseBtn >= 0 && m.mouseBtn < MAX_MOUSE_BTN)
        down |= s_input.mouseBtn[m.mouseBtn];
    // 手柄
    if (m.gpButton >= 0 && m.gpIndex >= 0 && m.gpIndex < MAX_GAMEPADS) {
        if (s_input.gamepads[m.gpIndex].connected && m.gpButton < MAX_GP_BUTTONS)
            down |= s_input.gamepads[m.gpIndex].buttons[m.gpButton];
    }
    lua_pushboolean(L, down);
    return 1;
}

/// Input.Update() — 帧末调用, 保存 previous 状态, 清零滚轮增量
static int l_Input_Update(lua_State* L) {
    (void)L;
    memcpy(s_input.keyPrevious, s_input.keyCurrent, sizeof(s_input.keyCurrent));
    s_input.wheelDx = 0.0f;
    s_input.wheelDy = 0.0f;
    return 0;
}

// ==================== luaopen 注册 ====================

static const luaL_Reg input_funcs[] = {
    {"IsKeyDown",           l_Input_IsKeyDown},
    {"IsKeyPressed",        l_Input_IsKeyPressed},
    {"IsMouseDown",         l_Input_IsMouseDown},
    {"GetMousePosition",    l_Input_GetMousePosition},
    {"GetMouseWheel",       l_Input_GetMouseWheel},
    {"GetTouchCount",       l_Input_GetTouchCount},
    {"GetTouch",            l_Input_GetTouch},
    {"GetGamepadCount",     l_Input_GetGamepadCount},
    {"IsGamepadConnected",  l_Input_IsGamepadConnected},
    {"GetGamepadButton",    l_Input_GetGamepadButton},
    {"GetGamepadAxis",      l_Input_GetGamepadAxis},
    {"GetGamepadName",      l_Input_GetGamepadName},
    {"AddAction",           l_Input_AddAction},
    {"IsActionDown",        l_Input_IsActionDown},
    {"Update",              l_Input_Update},
    {NULL, NULL}
};

int luaopen_Light_Input(lua_State* L) {
    // 初始化状态
    memset(&s_input, 0, sizeof(s_input));

    LT::RegisterModule(L, "Input", input_funcs);

    // 注册手柄按钮常量
    lua_pushstring(L, "Input");
    lua_rawget(L, -2);
    // SDL_GAMEPAD_BUTTON 常量 (简化名称)
    struct { const char* name; int val; } btn_consts[] = {
        {"BUTTON_A",             0},
        {"BUTTON_B",             1},
        {"BUTTON_X",             2},
        {"BUTTON_Y",             3},
        {"BUTTON_BACK",          4},
        {"BUTTON_GUIDE",         5},
        {"BUTTON_START",         6},
        {"BUTTON_LEFTSTICK",     7},
        {"BUTTON_RIGHTSTICK",    8},
        {"BUTTON_LEFTSHOULDER",  9},
        {"BUTTON_RIGHTSHOULDER", 10},
        {"BUTTON_DPAD_UP",       11},
        {"BUTTON_DPAD_DOWN",     12},
        {"BUTTON_DPAD_LEFT",     13},
        {"BUTTON_DPAD_RIGHT",    14},
        {nullptr, 0}
    };
    for (int i = 0; btn_consts[i].name; i++) {
        lua_pushinteger(L, btn_consts[i].val);
        lua_setfield(L, -2, btn_consts[i].name);
    }
    // SDL_GAMEPAD_AXIS 常量
    struct { const char* name; int val; } axis_consts[] = {
        {"AXIS_LEFTX",        0},
        {"AXIS_LEFTY",        1},
        {"AXIS_RIGHTX",       2},
        {"AXIS_RIGHTY",       3},
        {"AXIS_LEFT_TRIGGER", 4},
        {"AXIS_RIGHT_TRIGGER",5},
        {nullptr, 0}
    };
    for (int i = 0; axis_consts[i].name; i++) {
        lua_pushinteger(L, axis_consts[i].val);
        lua_setfield(L, -2, axis_consts[i].name);
    }
    lua_pop(L, 1);

    return 1;
}
