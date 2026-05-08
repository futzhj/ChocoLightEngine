/**
 * @file light_gamepad.cpp
 * @brief Light.Gamepad - SDL_gamepad.h 完整覆盖 (Phase G 基础 + Phase AJ 扩展, ~64 fns)
 *
 * 高层手柄 API (vs Light.Joystick 的原始 input). 仅当设备符合"console-style"
 * gamepad 布局 (面键 ABXY + 双扳机 + 双摇杆 + dpad + start/back) 时才会被
 * SDL3 识别为 gamepad. 不符合的(如方向盘/飞行摇杆)需用 Light.Joystick.
 *
 * Lua API:
 *
 *  Discovery (no handle):
 *    HasGamepad()                                -> bool
 *    GetGamepads()                               -> {instance_id, ...}, err
 *    IsGamepad(id)                               -> bool
 *    GetNameForID(id)                            -> string | nil
 *    GetPathForID(id)                            -> string | nil
 *    GetPlayerIndexForID(id)                     -> int (-1 if unset)
 *    GetGUIDForID(id)                            -> string (33-char hex)
 *    GetVendorForID(id)                          -> uint16
 *    GetProductForID(id)                         -> uint16
 *    GetProductVersionForID(id)                  -> uint16
 *    GetTypeForID(id)                            -> int (TYPE_* enum)
 *    GetRealTypeForID(id)                        -> int (TYPE_* enum)
 *    SetEventsEnabled(b) / EventsEnabled() -> bool / Update()
 *
 *  Mapping (db override):
 *    AddMapping(s)                               -> int (1 added, 0 updated, -1 err), err
 *    AddMappingsFromFile(path)                   -> int (count or -1), err
 *    ReloadMappings()                            -> bool, err
 *    GetMappings()                               -> {string, ...}, err
 *    GetMappingForGUID(guid_str)                 -> string | nil, err
 *    GetMapping(handle)                          -> string | nil, err
 *    SetMapping(id, s)                           -> bool, err
 *    GetMappingForID(id)                         -> string | nil, err
 *
 *  Open / lookup / close:
 *    Open(id)                                    -> handle | nil, err
 *    GetGamepadFromID(id)                        -> handle | nil
 *    GetGamepadFromPlayerIndex(idx)              -> handle | nil
 *    Close(handle)                               -> bool, err
 *
 *  Per-handle metadata:
 *    GetID(handle)                               -> instance_id, err
 *    GetName(handle)                             -> string, err
 *    GetPath(handle)                             -> string | nil, err
 *    GetType(handle)                             -> type_string  (legacy)
 *    GetRealType(handle)                         -> int (TYPE_* enum)
 *    GetVendor(handle)                           -> uint16
 *    GetProduct(handle)                          -> uint16
 *    GetProductVersion(handle)                   -> uint16
 *    GetFirmwareVersion(handle)                  -> uint16
 *    GetSerial(handle)                           -> string | nil
 *    GetSteamHandle(handle)                      -> number (uint64, lua_Number lossy)
 *    GetPlayerIndex(handle)                      -> int
 *    SetPlayerIndex(handle, idx)                 -> bool, err
 *    GetProperties(handle)                       -> propid (uint32)
 *    GetJoystick(handle)                         -> joystick_lightuserdata | nil
 *    IsConnected(handle)                         -> bool                     (legacy)
 *    GetConnectionState(handle)                  -> "wired"|"wireless"|...   (legacy)
 *    GetPowerInfo(handle)                        -> state_str, percent       (legacy)
 *
 *  Type/Button/Axis string conversion:
 *    GetTypeFromString(s)                        -> int (TYPE_* or 0)
 *    GetStringForType(t)                         -> string | nil
 *    GetButtonFromString(s)                      -> int (BUTTON_* or -1)
 *    GetStringForButton(b)                       -> string | nil
 *    GetAxisFromString(s)                        -> int (AXIS_* or -1)
 *    GetStringForAxis(a)                         -> string | nil
 *    GetButtonLabel(handle, btn_name)            -> int (BUTTON_LABEL_*)
 *    GetButtonLabelForType(typenum, btn_name)    -> int (BUTTON_LABEL_*)
 *
 *  Polling (legacy, accept string name):
 *    HasButton(handle, name)                     -> bool
 *    GetButton(handle, name)                     -> bool
 *    HasAxis(handle, name)                       -> bool
 *    GetAxis(handle, name)                       -> int16
 *
 *  Touchpad:
 *    GetNumTouchpads(handle)                     -> int
 *    GetNumTouchpadFingers(handle, touchpad)     -> int
 *    GetTouchpadFinger(handle, touchpad, finger) -> {down, x, y, pressure}, err
 *
 *  Sensor (gyro/accel embedded in gamepad, e.g. PS5 / Switch Pro):
 *    HasSensor(handle, sensor_type)              -> bool
 *    SetSensorEnabled(handle, sensor_type, bool) -> bool, err
 *    SensorEnabled(handle, sensor_type)          -> bool
 *    GetSensorDataRate(handle, sensor_type)      -> number (Hz)
 *    GetSensorData(handle, sensor_type, n)       -> {float, ...}, err
 *      sensor_type: "accel"|"gyro"|"accel_l"|"accel_r"|"gyro_l"|"gyro_r"
 *
 *  Effects:
 *    Rumble(h, low, high, ms)                    -> bool, err  (legacy)
 *    RumbleTriggers(h, l, r, ms)                 -> bool, err
 *    SetLED(h, r, g, b)                          -> bool, err  (DualShock4/5 lightbar etc.)
 *    SendEffect(h, data_string)                  -> bool, err  (raw bytes, 设备特定)
 *
 * Constants:
 *    TYPE_UNKNOWN/STANDARD/XBOX360/XBOXONE/PS3/PS4/PS5/
 *    NINTENDO_SWITCH_PRO/NINTENDO_SWITCH_JOYCON_LEFT/RIGHT/PAIR
 *    (TYPE_GAMECUBE 在 SDL3 v3.2.30 不可用, 已省略)
 *    BUTTON_SOUTH/EAST/WEST/NORTH/BACK/GUIDE/START/
 *    LEFT_STICK/RIGHT_STICK/LEFT_SHOULDER/RIGHT_SHOULDER/
 *    DPAD_UP/DPAD_DOWN/DPAD_LEFT/DPAD_RIGHT/
 *    MISC1..6/RIGHT_PADDLE1/LEFT_PADDLE1/RIGHT_PADDLE2/LEFT_PADDLE2/TOUCHPAD
 *    AXIS_LEFTX/LEFTY/RIGHTX/RIGHTY/LEFT_TRIGGER/RIGHT_TRIGGER
 *    BUTTON_LABEL_UNKNOWN/A/B/X/Y/CROSS/CIRCLE/SQUARE/TRIANGLE
 *    CONNECTION_INVALID/UNKNOWN/WIRED/WIRELESS  (复用 Joystick)
 *    AXIS_MAX = 32767, AXIS_MIN = -32768
 *
 * NOT bound:
 *    SDL_AddGamepadMappingsFromIO         - 依赖 IOStream, 用 AddMappingsFromFile
 *    SDL_GetGamepadBindings               - SDL_GamepadBinding 复杂结构, 罕用
 *    SDL_GetGamepadAppleSFSymbolsName*    - Apple-platform-specific, 罕用
 *
 * Lazy init: SDL_INIT_GAMEPAD 在首次设备列举/打开时初始化.
 *
 * Lifetime: handle = SDL_Gamepad* 用 lightuserdata 暴露; Close 后失效.
 *           GetGamepadFromID/PlayerIndex 返回的是借用句柄 (无所有权), 不要 Close.
 *
 * 平台覆盖: Win/Mac/Linux/Android (HID), Web (Gamepad API), iOS 部分.
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <cstring>
#include <cctype>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ============================================================
// Lazy SDL_INIT_GAMEPAD
// ============================================================
static bool g_padSubsysInited = false;
static bool EnsureGamepadSubsystem() {
    if (g_padSubsysInited) return true;
    if (SDL_WasInit(SDL_INIT_GAMEPAD) != 0) {
        g_padSubsysInited = true;
        return true;
    }
    if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) return false;
    g_padSubsysInited = true;
    return true;
}

// 内部: 校验 lightuserdata 转 SDL_Gamepad*; 返回 nullptr 时压栈 err
static SDL_Gamepad* CheckGamepad(lua_State* L, int idx) {
    if (lua_islightuserdata(L, idx)) {
        return (SDL_Gamepad*)lua_touserdata(L, idx);
    }
    return nullptr;
}

// 内部: sensor type 字符串 -> SDL_SensorType 枚举
//   "accel"|"gyro"|"accel_l"|"accel_r"|"gyro_l"|"gyro_r" -> SDL_SENSOR_*
//   未知值返回 SDL_SENSOR_INVALID (0xFFFFFFFF in cast, 实际为 -1)
static SDL_SensorType SensorTypeFromStr(const char* s) {
    if (!s) return SDL_SENSOR_INVALID;
    if (strcmp(s, "accel")   == 0) return SDL_SENSOR_ACCEL;
    if (strcmp(s, "gyro")    == 0) return SDL_SENSOR_GYRO;
    if (strcmp(s, "accel_l") == 0) return SDL_SENSOR_ACCEL_L;
    if (strcmp(s, "accel_r") == 0) return SDL_SENSOR_ACCEL_R;
    if (strcmp(s, "gyro_l")  == 0) return SDL_SENSOR_GYRO_L;
    if (strcmp(s, "gyro_r")  == 0) return SDL_SENSOR_GYRO_R;
    return SDL_SENSOR_INVALID;
}

// 内部: 把 SDL_GUID 推为 33 字符 hex 字符串
static void PushGUIDString(lua_State* L, SDL_GUID guid) {
    char buf[33];
    SDL_GUIDToString(guid, buf, sizeof(buf));
    lua_pushstring(L, buf);
}

// 内部: SDL_PowerState -> 字符串
static const char* PowerStateToStr(SDL_PowerState s) {
    switch (s) {
        case SDL_POWERSTATE_ERROR:      return "error";
        case SDL_POWERSTATE_UNKNOWN:    return "unknown";
        case SDL_POWERSTATE_ON_BATTERY: return "on_battery";
        case SDL_POWERSTATE_NO_BATTERY: return "no_battery";
        case SDL_POWERSTATE_CHARGING:   return "charging";
        case SDL_POWERSTATE_CHARGED:    return "charged";
        default:                        return "unknown";
    }
}

// 内部: SDL_JoystickConnectionState -> 字符串
static const char* ConnStateToStr(SDL_JoystickConnectionState s) {
    switch (s) {
        case SDL_JOYSTICK_CONNECTION_INVALID:  return "invalid";
        case SDL_JOYSTICK_CONNECTION_UNKNOWN:  return "unknown";
        case SDL_JOYSTICK_CONNECTION_WIRED:    return "wired";
        case SDL_JOYSTICK_CONNECTION_WIRELESS: return "wireless";
        default:                               return "unknown";
    }
}

// ==================== Light.Gamepad.GetGamepads ====================

static int l_Gamepad_GetGamepads(lua_State* L) {
    int count = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&count);
    if (!ids) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushinteger(L, (lua_Integer)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Gamepad.Open / Close ====================

static int l_Gamepad_Open(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    SDL_Gamepad* gp = SDL_OpenGamepad(id);
    if (!gp) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, gp);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_Close(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid gamepad handle");
        return 2;
    }
    SDL_CloseGamepad(gp);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Gamepad.GetID / GetName / GetType ====================

static int l_Gamepad_GetID(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_JoystickID id = SDL_GetGamepadID(gp);
    if (id == 0) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushinteger(L, (lua_Integer)id);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetName(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    const char* name = SDL_GetGamepadName(gp);
    if (!name) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetType(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_GamepadType t = SDL_GetGamepadType(gp);
    const char* s = SDL_GetGamepadStringForType(t);
    lua_pushstring(L, s ? s : "unknown");
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Gamepad.IsConnected / GetConnectionState ====================

static int l_Gamepad_IsConnected(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadConnected(gp) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetConnectionState(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushstring(L, "invalid"); return 1; }
    lua_pushstring(L, ConnStateToStr(SDL_GetGamepadConnectionState(gp)));
    return 1;
}

// ==================== Light.Gamepad.GetPowerInfo ====================

static int l_Gamepad_GetPowerInfo(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushstring(L, "error"); lua_pushnil(L); return 2; }
    int percent = -1;
    SDL_PowerState s = SDL_GetGamepadPowerInfo(gp, &percent);
    lua_pushstring(L, PowerStateToStr(s));
    if (percent >= 0) {
        lua_pushinteger(L, percent);
    } else {
        lua_pushnil(L);  // 不支持电池查询时为 nil
    }
    return 2;
}

// ==================== Light.Gamepad.HasButton / GetButton ====================

static int l_Gamepad_HasButton(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadHasButton(gp, b) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetButton(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GetGamepadButton(gp, b) ? 1 : 0);
    return 1;
}

// ==================== Light.Gamepad.HasAxis / GetAxis ====================

static int l_Gamepad_HasAxis(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
    if (a == SDL_GAMEPAD_AXIS_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadHasAxis(gp, a) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetAxis(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    const char* name = luaL_checkstring(L, 2);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(name);
    if (a == SDL_GAMEPAD_AXIS_INVALID) { lua_pushinteger(L, 0); return 1; }
    Sint16 v = SDL_GetGamepadAxis(gp, a);
    lua_pushinteger(L, (lua_Integer)v);
    return 1;
}

// ==================== Light.Gamepad.Rumble ====================

static int l_Gamepad_Rumble(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid gamepad handle");
        return 2;
    }
    lua_Integer low_i  = luaL_checkinteger(L, 2);
    lua_Integer high_i = luaL_checkinteger(L, 3);
    lua_Integer dur_i  = luaL_checkinteger(L, 4);
    if (low_i  < 0 || low_i  > 65535) {
        lua_pushboolean(L, 0); lua_pushstring(L, "low must be 0..65535"); return 2;
    }
    if (high_i < 0 || high_i > 65535) {
        lua_pushboolean(L, 0); lua_pushstring(L, "high must be 0..65535"); return 2;
    }
    if (dur_i < 0) {
        lua_pushboolean(L, 0); lua_pushstring(L, "duration_ms must be >= 0"); return 2;
    }
    bool ok = SDL_RumbleGamepad(gp, (Uint16)low_i, (Uint16)high_i, (Uint32)dur_i);
    lua_pushboolean(L, ok);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

// ============================================================
// Phase AJ extensions: SDL_gamepad.h 完整覆盖
// ============================================================

// ---------- Discovery (no handle) ----------

static int l_Gamepad_HasGamepad(lua_State* L) {
    EnsureGamepadSubsystem();
    lua_pushboolean(L, SDL_HasGamepad() ? 1 : 0);
    return 1;
}

static int l_Gamepad_IsGamepad(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushboolean(L, SDL_IsGamepad(id) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetNameForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    const char* s = SDL_GetGamepadNameForID(id);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetPathForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    const char* s = SDL_GetGamepadPathForID(id);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetPlayerIndexForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, SDL_GetGamepadPlayerIndexForID(id));
    return 1;
}

static int l_Gamepad_GetGUIDForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    PushGUIDString(L, SDL_GetGamepadGUIDForID(id));
    return 1;
}

static int l_Gamepad_GetVendorForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, SDL_GetGamepadVendorForID(id));
    return 1;
}

static int l_Gamepad_GetProductForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, SDL_GetGamepadProductForID(id));
    return 1;
}

static int l_Gamepad_GetProductVersionForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, SDL_GetGamepadProductVersionForID(id));
    return 1;
}

static int l_Gamepad_GetTypeForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadTypeForID(id));
    return 1;
}

static int l_Gamepad_GetRealTypeForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    lua_pushinteger(L, (lua_Integer)SDL_GetRealGamepadTypeForID(id));
    return 1;
}

// ---------- Mapping (db override) ----------

static int l_Gamepad_AddMapping(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    EnsureGamepadSubsystem();
    int r = SDL_AddGamepadMapping(s);
    lua_pushinteger(L, r);
    if (r < 0) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_AddMappingsFromFile(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    EnsureGamepadSubsystem();
    int r = SDL_AddGamepadMappingsFromFile(path);
    lua_pushinteger(L, r);
    if (r < 0) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_ReloadMappings(lua_State* L) {
    EnsureGamepadSubsystem();
    bool ok = SDL_ReloadGamepadMappings();
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetMappings(lua_State* L) {
    EnsureGamepadSubsystem();
    int count = 0;
    char** arr = SDL_GetGamepadMappings(&count);
    if (!arr) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; ++i) {
        lua_pushstring(L, arr[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(arr);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetMappingForGUID(lua_State* L) {
    const char* guid_s = luaL_checkstring(L, 1);
    SDL_GUID guid = SDL_StringToGUID(guid_s);
    EnsureGamepadSubsystem();
    char* m = SDL_GetGamepadMappingForGUID(guid);
    if (!m) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_pushstring(L, m);
    SDL_free(m);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetMapping(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    char* m = SDL_GetGamepadMapping(gp);
    if (!m) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, m);
    SDL_free(m);
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_SetMapping(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    const char* s = luaL_checkstring(L, 2);
    EnsureGamepadSubsystem();
    bool ok = SDL_SetGamepadMapping(id, s);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetMappingForID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    EnsureGamepadSubsystem();
    char* m = SDL_GetGamepadMappingForID(id);
    if (!m) { lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushstring(L, m);
    SDL_free(m);
    lua_pushnil(L);
    return 2;
}

// ---------- Lookup ----------

static int l_Gamepad_GetGamepadFromID(lua_State* L) {
    SDL_JoystickID id = (SDL_JoystickID)luaL_checkinteger(L, 1);
    SDL_Gamepad* gp = SDL_GetGamepadFromID(id);
    if (!gp) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, gp);
    return 1;
}

static int l_Gamepad_GetGamepadFromPlayerIndex(lua_State* L) {
    int idx = (int)luaL_checkinteger(L, 1);
    SDL_Gamepad* gp = SDL_GetGamepadFromPlayerIndex(idx);
    if (!gp) { lua_pushnil(L); return 1; }
    lua_pushlightuserdata(L, gp);
    return 1;
}

// ---------- Per-handle metadata ----------

static int l_Gamepad_GetPath(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    const char* s = SDL_GetGamepadPath(gp);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetRealType(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, SDL_GAMEPAD_TYPE_UNKNOWN); return 1; }
    lua_pushinteger(L, (lua_Integer)SDL_GetRealGamepadType(gp));
    return 1;
}

static int l_Gamepad_GetVendor(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, SDL_GetGamepadVendor(gp));
    return 1;
}

static int l_Gamepad_GetProduct(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, SDL_GetGamepadProduct(gp));
    return 1;
}

static int l_Gamepad_GetProductVersion(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, SDL_GetGamepadProductVersion(gp));
    return 1;
}

static int l_Gamepad_GetFirmwareVersion(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, SDL_GetGamepadFirmwareVersion(gp));
    return 1;
}

static int l_Gamepad_GetSerial(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); return 1; }
    const char* s = SDL_GetGamepadSerial(gp);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetSteamHandle(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnumber(L, 0); return 1; }
    Uint64 h = SDL_GetGamepadSteamHandle(gp);
    // 用 lua_pushnumber 保留尽可能多的精度; lua_Integer (int64 in 5.3+, int32 in 5.1)
    // 5.1 下大于 2^53 的值会有损失, 但 Steam handle 通常远小于 2^53
    lua_pushnumber(L, (lua_Number)h);
    return 1;
}

static int l_Gamepad_GetPlayerIndex(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, -1); return 1; }
    lua_pushinteger(L, SDL_GetGamepadPlayerIndex(gp));
    return 1;
}

static int l_Gamepad_SetPlayerIndex(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    int idx = (int)luaL_checkinteger(L, 2);
    bool ok = SDL_SetGamepadPlayerIndex(gp, idx);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_GetProperties(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadProperties(gp));
    return 1;
}

static int l_Gamepad_GetJoystick(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); return 1; }
    SDL_Joystick* j = SDL_GetGamepadJoystick(gp);
    if (!j) { lua_pushnil(L); return 1; }
    // 注意: Light.Joystick 模块目前用自定义 LJoystick userdata + metatable.
    // 这里返回 lightuserdata, Lua 端不能直接用 Light.Joystick.* 方法.
    // 是底层 escape hatch, 主要用于跨模块互操作场景.
    lua_pushlightuserdata(L, j);
    return 1;
}

// ---------- Events ----------

static int l_Gamepad_SetEventsEnabled(lua_State* L) {
    bool b = lua_toboolean(L, 1) ? true : false;
    EnsureGamepadSubsystem();
    SDL_SetGamepadEventsEnabled(b);
    return 0;
}

static int l_Gamepad_EventsEnabled(lua_State* L) {
    EnsureGamepadSubsystem();
    lua_pushboolean(L, SDL_GamepadEventsEnabled() ? 1 : 0);
    return 1;
}

static int l_Gamepad_Update(lua_State* L) {
    (void)L;
    EnsureGamepadSubsystem();
    SDL_UpdateGamepads();
    return 0;
}

// ---------- Type/Button/Axis string conversion ----------

static int l_Gamepad_GetTypeFromString(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadTypeFromString(s));
    return 1;
}

static int l_Gamepad_GetStringForType(lua_State* L) {
    SDL_GamepadType t = (SDL_GamepadType)luaL_checkinteger(L, 1);
    const char* s = SDL_GetGamepadStringForType(t);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetButtonFromString(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadButtonFromString(s));
    return 1;
}

static int l_Gamepad_GetStringForButton(lua_State* L) {
    SDL_GamepadButton b = (SDL_GamepadButton)luaL_checkinteger(L, 1);
    const char* s = SDL_GetGamepadStringForButton(b);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetAxisFromString(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadAxisFromString(s));
    return 1;
}

static int l_Gamepad_GetStringForAxis(lua_State* L) {
    SDL_GamepadAxis a = (SDL_GamepadAxis)luaL_checkinteger(L, 1);
    const char* s = SDL_GetGamepadStringForAxis(a);
    if (!s) { lua_pushnil(L); return 1; }
    lua_pushstring(L, s);
    return 1;
}

static int l_Gamepad_GetButtonLabel(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN); return 1; }
    const char* name = luaL_checkstring(L, 2);
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushinteger(L, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN); return 1; }
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadButtonLabel(gp, b));
    return 1;
}

static int l_Gamepad_GetButtonLabelForType(lua_State* L) {
    SDL_GamepadType t = (SDL_GamepadType)luaL_checkinteger(L, 1);
    const char* name = luaL_checkstring(L, 2);
    SDL_GamepadButton b = SDL_GetGamepadButtonFromString(name);
    if (b == SDL_GAMEPAD_BUTTON_INVALID) { lua_pushinteger(L, SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN); return 1; }
    lua_pushinteger(L, (lua_Integer)SDL_GetGamepadButtonLabelForType(t, b));
    return 1;
}

// ---------- Touchpad ----------

static int l_Gamepad_GetNumTouchpads(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    lua_pushinteger(L, SDL_GetNumGamepadTouchpads(gp));
    return 1;
}

static int l_Gamepad_GetNumTouchpadFingers(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushinteger(L, 0); return 1; }
    int touchpad = (int)luaL_checkinteger(L, 2);
    lua_pushinteger(L, SDL_GetNumGamepadTouchpadFingers(gp, touchpad));
    return 1;
}

static int l_Gamepad_GetTouchpadFinger(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    int touchpad = (int)luaL_checkinteger(L, 2);
    int finger   = (int)luaL_checkinteger(L, 3);
    bool down = false; float x = 0, y = 0, p = 0;
    if (!SDL_GetGamepadTouchpadFinger(gp, touchpad, finger, &down, &x, &y, &p)) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_createtable(L, 0, 4);
    lua_pushboolean(L, down ? 1 : 0); lua_setfield(L, -2, "down");
    lua_pushnumber(L,  x);            lua_setfield(L, -2, "x");
    lua_pushnumber(L,  y);            lua_setfield(L, -2, "y");
    lua_pushnumber(L,  p);            lua_setfield(L, -2, "pressure");
    lua_pushnil(L);
    return 2;
}

// ---------- Sensor (gyro/accel embedded in gamepad) ----------

static int l_Gamepad_HasSensor(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_SensorType t = SensorTypeFromStr(luaL_checkstring(L, 2));
    if (t == SDL_SENSOR_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadHasSensor(gp, t) ? 1 : 0);
    return 1;
}

static int l_Gamepad_SetSensorEnabled(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_SensorType t = SensorTypeFromStr(luaL_checkstring(L, 2));
    if (t == SDL_SENSOR_INVALID) {
        lua_pushboolean(L, 0); lua_pushstring(L, "invalid sensor_type"); return 2;
    }
    bool b = lua_toboolean(L, 3) ? true : false;
    bool ok = SDL_SetGamepadSensorEnabled(gp, t, b);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_SensorEnabled(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); return 1; }
    SDL_SensorType t = SensorTypeFromStr(luaL_checkstring(L, 2));
    if (t == SDL_SENSOR_INVALID) { lua_pushboolean(L, 0); return 1; }
    lua_pushboolean(L, SDL_GamepadSensorEnabled(gp, t) ? 1 : 0);
    return 1;
}

static int l_Gamepad_GetSensorDataRate(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnumber(L, 0); return 1; }
    SDL_SensorType t = SensorTypeFromStr(luaL_checkstring(L, 2));
    if (t == SDL_SENSOR_INVALID) { lua_pushnumber(L, 0); return 1; }
    lua_pushnumber(L, SDL_GetGamepadSensorDataRate(gp, t));
    return 1;
}

static int l_Gamepad_GetSensorData(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushnil(L); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    SDL_SensorType t = SensorTypeFromStr(luaL_checkstring(L, 2));
    if (t == SDL_SENSOR_INVALID) {
        lua_pushnil(L); lua_pushstring(L, "invalid sensor_type"); return 2;
    }
    lua_Integer n_i = luaL_optinteger(L, 3, 3);
    if (n_i < 1 || n_i > 16) {
        lua_pushnil(L); lua_pushstring(L, "n must be 1..16"); return 2;
    }
    int n = (int)n_i;
    float buf[16] = {0};
    if (!SDL_GetGamepadSensorData(gp, t, buf, n)) {
        lua_pushnil(L); lua_pushstring(L, SDL_GetError()); return 2;
    }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_pushnumber(L, buf[i]);
        lua_rawseti(L, -2, i + 1);
    }
    lua_pushnil(L);
    return 2;
}

// ---------- Effects (RumbleTriggers / SetLED / SendEffect) ----------

static int l_Gamepad_RumbleTriggers(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    lua_Integer l_i = luaL_checkinteger(L, 2);
    lua_Integer r_i = luaL_checkinteger(L, 3);
    lua_Integer d_i = luaL_checkinteger(L, 4);
    if (l_i < 0 || l_i > 65535) { lua_pushboolean(L, 0); lua_pushstring(L, "left must be 0..65535"); return 2; }
    if (r_i < 0 || r_i > 65535) { lua_pushboolean(L, 0); lua_pushstring(L, "right must be 0..65535"); return 2; }
    if (d_i < 0)                { lua_pushboolean(L, 0); lua_pushstring(L, "duration_ms must be >= 0"); return 2; }
    bool ok = SDL_RumbleGamepadTriggers(gp, (Uint16)l_i, (Uint16)r_i, (Uint32)d_i);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_SetLED(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    lua_Integer r = luaL_checkinteger(L, 2);
    lua_Integer g = luaL_checkinteger(L, 3);
    lua_Integer b = luaL_checkinteger(L, 4);
    if (r < 0 || r > 255) { lua_pushboolean(L, 0); lua_pushstring(L, "r must be 0..255"); return 2; }
    if (g < 0 || g > 255) { lua_pushboolean(L, 0); lua_pushstring(L, "g must be 0..255"); return 2; }
    if (b < 0 || b > 255) { lua_pushboolean(L, 0); lua_pushstring(L, "b must be 0..255"); return 2; }
    bool ok = SDL_SetGamepadLED(gp, (Uint8)r, (Uint8)g, (Uint8)b);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

static int l_Gamepad_SendEffect(lua_State* L) {
    SDL_Gamepad* gp = CheckGamepad(L, 1);
    if (!gp) { lua_pushboolean(L, 0); lua_pushstring(L, "invalid gamepad handle"); return 2; }
    size_t sz = 0;
    const char* data = luaL_checklstring(L, 2, &sz);
    if (sz == 0) {
        lua_pushboolean(L, 0); lua_pushstring(L, "data must be non-empty string"); return 2;
    }
    bool ok = SDL_SendGamepadEffect(gp, data, (int)sz);
    lua_pushboolean(L, ok ? 1 : 0);
    if (!ok) { lua_pushstring(L, SDL_GetError()); return 2; }
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Gamepad ====================

#define LG_PUSH(name, val) do {                       \
    lua_pushinteger(L, (lua_Integer)(val));           \
    lua_setfield(L, -2, name);                        \
} while (0)

extern "C" LIGHT_API int luaopen_Light_Gamepad(lua_State* L) {
    static const luaL_Reg fns[] = {
        // Discovery (no handle)
        { "HasGamepad",                   l_Gamepad_HasGamepad                },
        { "GetGamepads",                  l_Gamepad_GetGamepads               },
        { "IsGamepad",                    l_Gamepad_IsGamepad                 },
        { "GetNameForID",                 l_Gamepad_GetNameForID              },
        { "GetPathForID",                 l_Gamepad_GetPathForID              },
        { "GetPlayerIndexForID",          l_Gamepad_GetPlayerIndexForID       },
        { "GetGUIDForID",                 l_Gamepad_GetGUIDForID              },
        { "GetVendorForID",               l_Gamepad_GetVendorForID            },
        { "GetProductForID",              l_Gamepad_GetProductForID           },
        { "GetProductVersionForID",       l_Gamepad_GetProductVersionForID    },
        { "GetTypeForID",                 l_Gamepad_GetTypeForID              },
        { "GetRealTypeForID",             l_Gamepad_GetRealTypeForID          },
        // Mapping
        { "AddMapping",                   l_Gamepad_AddMapping                },
        { "AddMappingsFromFile",          l_Gamepad_AddMappingsFromFile       },
        { "ReloadMappings",               l_Gamepad_ReloadMappings            },
        { "GetMappings",                  l_Gamepad_GetMappings               },
        { "GetMappingForGUID",            l_Gamepad_GetMappingForGUID         },
        { "GetMapping",                   l_Gamepad_GetMapping                },
        { "SetMapping",                   l_Gamepad_SetMapping                },
        { "GetMappingForID",              l_Gamepad_GetMappingForID           },
        // Open / lookup / close
        { "Open",                         l_Gamepad_Open                      },
        { "GetGamepadFromID",             l_Gamepad_GetGamepadFromID          },
        { "GetGamepadFromPlayerIndex",    l_Gamepad_GetGamepadFromPlayerIndex },
        { "Close",                        l_Gamepad_Close                     },
        // Per-handle metadata (legacy + new)
        { "GetID",                        l_Gamepad_GetID                     },
        { "GetName",                      l_Gamepad_GetName                   },
        { "GetPath",                      l_Gamepad_GetPath                   },
        { "GetType",                      l_Gamepad_GetType                   },
        { "GetRealType",                  l_Gamepad_GetRealType               },
        { "GetVendor",                    l_Gamepad_GetVendor                 },
        { "GetProduct",                   l_Gamepad_GetProduct                },
        { "GetProductVersion",            l_Gamepad_GetProductVersion         },
        { "GetFirmwareVersion",           l_Gamepad_GetFirmwareVersion        },
        { "GetSerial",                    l_Gamepad_GetSerial                 },
        { "GetSteamHandle",               l_Gamepad_GetSteamHandle            },
        { "GetPlayerIndex",               l_Gamepad_GetPlayerIndex            },
        { "SetPlayerIndex",               l_Gamepad_SetPlayerIndex            },
        { "GetProperties",                l_Gamepad_GetProperties             },
        { "GetJoystick",                  l_Gamepad_GetJoystick               },
        { "IsConnected",                  l_Gamepad_IsConnected               },
        { "GetConnectionState",           l_Gamepad_GetConnectionState        },
        { "GetPowerInfo",                 l_Gamepad_GetPowerInfo              },
        // Events
        { "SetEventsEnabled",             l_Gamepad_SetEventsEnabled          },
        { "EventsEnabled",                l_Gamepad_EventsEnabled             },
        { "Update",                       l_Gamepad_Update                    },
        // Type/Button/Axis string conversion
        { "GetTypeFromString",            l_Gamepad_GetTypeFromString         },
        { "GetStringForType",             l_Gamepad_GetStringForType          },
        { "GetButtonFromString",          l_Gamepad_GetButtonFromString       },
        { "GetStringForButton",           l_Gamepad_GetStringForButton        },
        { "GetAxisFromString",            l_Gamepad_GetAxisFromString         },
        { "GetStringForAxis",             l_Gamepad_GetStringForAxis          },
        { "GetButtonLabel",               l_Gamepad_GetButtonLabel            },
        { "GetButtonLabelForType",        l_Gamepad_GetButtonLabelForType     },
        // Polling (legacy, accept string name)
        { "HasButton",                    l_Gamepad_HasButton                 },
        { "GetButton",                    l_Gamepad_GetButton                 },
        { "HasAxis",                      l_Gamepad_HasAxis                   },
        { "GetAxis",                      l_Gamepad_GetAxis                   },
        // Touchpad
        { "GetNumTouchpads",              l_Gamepad_GetNumTouchpads           },
        { "GetNumTouchpadFingers",        l_Gamepad_GetNumTouchpadFingers     },
        { "GetTouchpadFinger",            l_Gamepad_GetTouchpadFinger         },
        // Sensor
        { "HasSensor",                    l_Gamepad_HasSensor                 },
        { "SetSensorEnabled",             l_Gamepad_SetSensorEnabled          },
        { "SensorEnabled",                l_Gamepad_SensorEnabled             },
        { "GetSensorDataRate",            l_Gamepad_GetSensorDataRate         },
        { "GetSensorData",                l_Gamepad_GetSensorData             },
        // Effects
        { "Rumble",                       l_Gamepad_Rumble                    },
        { "RumbleTriggers",               l_Gamepad_RumbleTriggers            },
        { "SetLED",                       l_Gamepad_SetLED                    },
        { "SendEffect",                   l_Gamepad_SendEffect                },
        { nullptr,                        nullptr                             },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);

    // SDL_GamepadType
    LG_PUSH("TYPE_UNKNOWN",                       SDL_GAMEPAD_TYPE_UNKNOWN);
    LG_PUSH("TYPE_STANDARD",                      SDL_GAMEPAD_TYPE_STANDARD);
    LG_PUSH("TYPE_XBOX360",                       SDL_GAMEPAD_TYPE_XBOX360);
    LG_PUSH("TYPE_XBOXONE",                       SDL_GAMEPAD_TYPE_XBOXONE);
    LG_PUSH("TYPE_PS3",                           SDL_GAMEPAD_TYPE_PS3);
    LG_PUSH("TYPE_PS4",                           SDL_GAMEPAD_TYPE_PS4);
    LG_PUSH("TYPE_PS5",                           SDL_GAMEPAD_TYPE_PS5);
    LG_PUSH("TYPE_NINTENDO_SWITCH_PRO",           SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
    LG_PUSH("TYPE_NINTENDO_SWITCH_JOYCON_LEFT",   SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT);
    LG_PUSH("TYPE_NINTENDO_SWITCH_JOYCON_RIGHT",  SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT);
    LG_PUSH("TYPE_NINTENDO_SWITCH_JOYCON_PAIR",   SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
    // TYPE_GAMECUBE: SDL3 v3.2.30 未包含 (在更新版本中添加), 故省略

    // SDL_GamepadButton
    LG_PUSH("BUTTON_SOUTH",          SDL_GAMEPAD_BUTTON_SOUTH);
    LG_PUSH("BUTTON_EAST",           SDL_GAMEPAD_BUTTON_EAST);
    LG_PUSH("BUTTON_WEST",           SDL_GAMEPAD_BUTTON_WEST);
    LG_PUSH("BUTTON_NORTH",          SDL_GAMEPAD_BUTTON_NORTH);
    LG_PUSH("BUTTON_BACK",           SDL_GAMEPAD_BUTTON_BACK);
    LG_PUSH("BUTTON_GUIDE",          SDL_GAMEPAD_BUTTON_GUIDE);
    LG_PUSH("BUTTON_START",          SDL_GAMEPAD_BUTTON_START);
    LG_PUSH("BUTTON_LEFT_STICK",     SDL_GAMEPAD_BUTTON_LEFT_STICK);
    LG_PUSH("BUTTON_RIGHT_STICK",    SDL_GAMEPAD_BUTTON_RIGHT_STICK);
    LG_PUSH("BUTTON_LEFT_SHOULDER",  SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    LG_PUSH("BUTTON_RIGHT_SHOULDER", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    LG_PUSH("BUTTON_DPAD_UP",        SDL_GAMEPAD_BUTTON_DPAD_UP);
    LG_PUSH("BUTTON_DPAD_DOWN",      SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    LG_PUSH("BUTTON_DPAD_LEFT",      SDL_GAMEPAD_BUTTON_DPAD_LEFT);
    LG_PUSH("BUTTON_DPAD_RIGHT",     SDL_GAMEPAD_BUTTON_DPAD_RIGHT);
    LG_PUSH("BUTTON_MISC1",          SDL_GAMEPAD_BUTTON_MISC1);
    LG_PUSH("BUTTON_RIGHT_PADDLE1",  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1);
    LG_PUSH("BUTTON_LEFT_PADDLE1",   SDL_GAMEPAD_BUTTON_LEFT_PADDLE1);
    LG_PUSH("BUTTON_RIGHT_PADDLE2",  SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2);
    LG_PUSH("BUTTON_LEFT_PADDLE2",   SDL_GAMEPAD_BUTTON_LEFT_PADDLE2);
    LG_PUSH("BUTTON_TOUCHPAD",       SDL_GAMEPAD_BUTTON_TOUCHPAD);
    LG_PUSH("BUTTON_MISC2",          SDL_GAMEPAD_BUTTON_MISC2);
    LG_PUSH("BUTTON_MISC3",          SDL_GAMEPAD_BUTTON_MISC3);
    LG_PUSH("BUTTON_MISC4",          SDL_GAMEPAD_BUTTON_MISC4);
    LG_PUSH("BUTTON_MISC5",          SDL_GAMEPAD_BUTTON_MISC5);
    LG_PUSH("BUTTON_MISC6",          SDL_GAMEPAD_BUTTON_MISC6);

    // SDL_GamepadAxis
    LG_PUSH("AXIS_LEFTX",         SDL_GAMEPAD_AXIS_LEFTX);
    LG_PUSH("AXIS_LEFTY",         SDL_GAMEPAD_AXIS_LEFTY);
    LG_PUSH("AXIS_RIGHTX",        SDL_GAMEPAD_AXIS_RIGHTX);
    LG_PUSH("AXIS_RIGHTY",        SDL_GAMEPAD_AXIS_RIGHTY);
    LG_PUSH("AXIS_LEFT_TRIGGER",  SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
    LG_PUSH("AXIS_RIGHT_TRIGGER", SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);

    // SDL_GamepadButtonLabel
    LG_PUSH("BUTTON_LABEL_UNKNOWN",  SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN);
    LG_PUSH("BUTTON_LABEL_A",        SDL_GAMEPAD_BUTTON_LABEL_A);
    LG_PUSH("BUTTON_LABEL_B",        SDL_GAMEPAD_BUTTON_LABEL_B);
    LG_PUSH("BUTTON_LABEL_X",        SDL_GAMEPAD_BUTTON_LABEL_X);
    LG_PUSH("BUTTON_LABEL_Y",        SDL_GAMEPAD_BUTTON_LABEL_Y);
    LG_PUSH("BUTTON_LABEL_CROSS",    SDL_GAMEPAD_BUTTON_LABEL_CROSS);
    LG_PUSH("BUTTON_LABEL_CIRCLE",   SDL_GAMEPAD_BUTTON_LABEL_CIRCLE);
    LG_PUSH("BUTTON_LABEL_SQUARE",   SDL_GAMEPAD_BUTTON_LABEL_SQUARE);
    LG_PUSH("BUTTON_LABEL_TRIANGLE", SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE);

    // Joystick connection state (复用 Joystick 的常量名空间)
    LG_PUSH("CONNECTION_INVALID",  SDL_JOYSTICK_CONNECTION_INVALID);
    LG_PUSH("CONNECTION_UNKNOWN",  SDL_JOYSTICK_CONNECTION_UNKNOWN);
    LG_PUSH("CONNECTION_WIRED",    SDL_JOYSTICK_CONNECTION_WIRED);
    LG_PUSH("CONNECTION_WIRELESS", SDL_JOYSTICK_CONNECTION_WIRELESS);

    // Axis range sentinels
    LG_PUSH("AXIS_MAX",  32767);
    LG_PUSH("AXIS_MIN", -32768);

    return 1;
}
