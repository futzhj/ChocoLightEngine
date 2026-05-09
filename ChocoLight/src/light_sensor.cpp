/**
 * @file light_sensor.cpp
 * @brief Light.Sensor 模块 - 加速度/陀螺仪传感器 (基于 SDL_sensor) - Phase AL 完整覆盖
 *
 * Lua API (Phase G + Phase AL 共 17 fns):
 *
 *   [Phase G] 高级简化 API (5 fns) - 单例 Accel + 单例 Gyro
 *     OpenAccel()                  -> ok, err
 *     OpenGyro()                   -> ok, err
 *     GetAccel()                   -> x, y, z       (m/s^2, 含重力)
 *     GetGyro()                    -> x, y, z       (rad/s)
 *     Close()                      -> ok            (关 accel+gyro 单例)
 *
 *   [Phase AL] 底层 SDL_sensor API (12 fns) - lightuserdata 句柄, 多 sensor 并行
 *     GetSensors()                 -> array<id>     (Uint32 instance ids)
 *     GetSensorNameForID(id)       -> string, err
 *     GetSensorTypeForID(id)       -> string        ('invalid'/'unknown'/'accel'/'gyro'/...)
 *     GetSensorNonPortableTypeForID(id) -> int, err
 *     OpenSensorByID(id)           -> handle, err   (lightuserdata SDL_Sensor*)
 *     GetSensorFromID(id)          -> handle, err   (返回已打开的句柄, 不存在=nil)
 *     CloseSensor(handle)          -> ok
 *     GetSensorName(handle)        -> string, err
 *     GetSensorType(handle)        -> string, err
 *     GetSensorID(handle)          -> id, err
 *     GetSensorData(handle, n)     -> v1, v2, ..., err  (1 <= n <= 16, clamp)
 *     UpdateSensors()              -> ok            (event 关时手动 poll)
 *
 * 常量 (9):
 *   SENSOR_INVALID = -1, SENSOR_UNKNOWN = 0
 *   SENSOR_ACCEL = 1, SENSOR_GYRO = 2
 *   SENSOR_ACCEL_L = 3, SENSOR_GYRO_L = 4
 *   SENSOR_ACCEL_R = 5, SENSOR_GYRO_R = 6
 *   STANDARD_GRAVITY = 9.80665
 *
 * 设计要点:
 *   - Phase G 旧 5 fns 内部 C++ 单例 (g_accel/g_gyro), 与 Phase AL 句柄系统**互不干扰**
 *   - Phase AL 句柄通过 lightuserdata 暴露 (与 Joystick/Gamepad/Haptic 协议一致)
 *   - 所有路径走 EnsureSensorSubsystem() 懒加载 SDL_INIT_SENSOR
 *   - SensorType <-> string 双向映射, 保持 GetSensorType / GetSensorTypeForID 一致
 *   - GetSensorData 按 SDL3 文档默认最多 16 axis, Lua 端 clamp 防越界
 *   - 桌面平台通常无传感器, 所有 fns 在 Open 失败时优雅返回 (false/nil + err) 不崩溃
 *
 * Thread-safety: SDL_sensor APIs 必须在 main thread 调用 (与 SDL3 保持一致)
 */
#include "light.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {
    // ---------- Phase G 单例状态 ----------
    SDL_Sensor* g_accel = nullptr;
    SDL_Sensor* g_gyro  = nullptr;
    bool        g_subsysInited = false;

    // ---------- 公共: 懒加载 sensor subsystem ----------
    bool EnsureSensorSubsystem() {
        if (g_subsysInited) return true;
        if (SDL_WasInit(SDL_INIT_SENSOR) != 0) {
            g_subsysInited = true;
            return true;
        }
        if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) return false;
        g_subsysInited = true;
        return true;
    }

    // ---------- SDL_SensorType <-> string ----------
    const char* SensorTypeName(SDL_SensorType t) {
        switch (t) {
            case SDL_SENSOR_INVALID:  return "invalid";
            case SDL_SENSOR_UNKNOWN:  return "unknown";
            case SDL_SENSOR_ACCEL:    return "accel";
            case SDL_SENSOR_GYRO:     return "gyro";
            case SDL_SENSOR_ACCEL_L:  return "accel_l";
            case SDL_SENSOR_GYRO_L:   return "gyro_l";
            case SDL_SENSOR_ACCEL_R:  return "accel_r";
            case SDL_SENSOR_GYRO_R:   return "gyro_r";
            default:                  return "unknown";
        }
    }

    // ---------- handle 检查器 ----------
    SDL_Sensor* CheckSensor(lua_State* L, int idx) {
        if (lua_type(L, idx) != LUA_TLIGHTUSERDATA) return nullptr;
        return static_cast<SDL_Sensor*>(lua_touserdata(L, idx));
    }

    // ---------- Phase G 私有 helper ----------
    SDL_SensorID FindFirstByType(SDL_SensorType wanted) {
        int count = 0;
        SDL_SensorID* ids = SDL_GetSensors(&count);
        if (!ids) return 0;
        SDL_SensorID found = 0;
        for (int i = 0; i < count; ++i) {
            if (SDL_GetSensorTypeForID(ids[i]) == wanted) { found = ids[i]; break; }
        }
        SDL_free(ids);
        return found;
    }

    int OpenSensorByType(lua_State* L, SDL_SensorType wanted, SDL_Sensor** slot) {
        if (*slot) {
            lua_pushboolean(L, 1);
            lua_pushnil(L);
            return 2;
        }
        if (!EnsureSensorSubsystem()) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, SDL_GetError());
            return 2;
        }
        SDL_SensorID id = FindFirstByType(wanted);
        if (!id) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, "no matching sensor on this device");
            return 2;
        }
        *slot = SDL_OpenSensor(id);
        if (!*slot) {
            lua_pushboolean(L, 0);
            lua_pushstring(L, SDL_GetError());
            return 2;
        }
        lua_pushboolean(L, 1);
        lua_pushnil(L);
        return 2;
    }

    int ReadSensorXYZ(lua_State* L, SDL_Sensor* sensor, const char* errMsg) {
        if (!sensor) {
            lua_pushnil(L);
            lua_pushstring(L, errMsg);
            return 2;
        }
        float data[3] = { 0.0f, 0.0f, 0.0f };
        if (!SDL_GetSensorData(sensor, data, 3)) {
            lua_pushnil(L);
            lua_pushstring(L, SDL_GetError());
            return 2;
        }
        lua_pushnumber(L, data[0]);
        lua_pushnumber(L, data[1]);
        lua_pushnumber(L, data[2]);
        return 3;
    }
}

// ============================================================
// Phase G: 高级简化 API
// ============================================================

static int l_Sensor_OpenAccel(lua_State* L) {
    return OpenSensorByType(L, SDL_SENSOR_ACCEL, &g_accel);
}

static int l_Sensor_OpenGyro(lua_State* L) {
    return OpenSensorByType(L, SDL_SENSOR_GYRO, &g_gyro);
}

static int l_Sensor_GetAccel(lua_State* L) {
    return ReadSensorXYZ(L, g_accel, "accelerometer not opened");
}

static int l_Sensor_GetGyro(lua_State* L) {
    return ReadSensorXYZ(L, g_gyro, "gyroscope not opened");
}

static int l_Sensor_Close(lua_State* L) {
    if (g_accel) { SDL_CloseSensor(g_accel); g_accel = nullptr; }
    if (g_gyro)  { SDL_CloseSensor(g_gyro);  g_gyro  = nullptr; }
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// Phase AL: 底层 sensor API
// ============================================================

// Light.Sensor.GetSensors() -> array<id>
static int l_Sensor_GetSensors(lua_State* L) {
    if (!EnsureSensorSubsystem()) {
        lua_newtable(L);
        return 1;
    }
    int count = 0;
    SDL_SensorID* ids = SDL_GetSensors(&count);
    lua_newtable(L);
    if (!ids || count <= 0) {
        if (ids) SDL_free(ids);
        return 1;
    }
    for (int i = 0; i < count; ++i) {
        lua_pushnumber(L, (lua_Number)ids[i]);
        lua_rawseti(L, -2, i + 1);
    }
    SDL_free(ids);
    return 1;
}

// Light.Sensor.GetSensorNameForID(id) -> string, err
static int l_Sensor_GetSensorNameForID(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureSensorSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "sensor subsystem unavailable");
        return 2;
    }
    SDL_SensorID id = (SDL_SensorID)lua_tonumber(L, 1);
    const char* name = SDL_GetSensorNameForID(id);
    if (!name) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "invalid sensor id");
        return 2;
    }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.GetSensorTypeForID(id) -> string
static int l_Sensor_GetSensorTypeForID(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushstring(L, "invalid");
        return 1;
    }
    if (!EnsureSensorSubsystem()) {
        lua_pushstring(L, "invalid");
        return 1;
    }
    SDL_SensorID id = (SDL_SensorID)lua_tonumber(L, 1);
    SDL_SensorType t = SDL_GetSensorTypeForID(id);
    lua_pushstring(L, SensorTypeName(t));
    return 1;
}

// Light.Sensor.GetSensorNonPortableTypeForID(id) -> int, err
static int l_Sensor_GetSensorNonPortableTypeForID(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureSensorSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "sensor subsystem unavailable");
        return 2;
    }
    SDL_SensorID id = (SDL_SensorID)lua_tonumber(L, 1);
    int v = SDL_GetSensorNonPortableTypeForID(id);
    if (v == -1) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "invalid sensor id");
        return 2;
    }
    lua_pushinteger(L, v);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.OpenSensorByID(id) -> handle, err
static int l_Sensor_OpenSensorByID(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureSensorSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "sensor subsystem unavailable");
        return 2;
    }
    SDL_SensorID id = (SDL_SensorID)lua_tonumber(L, 1);
    SDL_Sensor* s = SDL_OpenSensor(id);
    if (!s) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "failed to open sensor");
        return 2;
    }
    lua_pushlightuserdata(L, s);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.GetSensorFromID(id) -> handle, err
static int l_Sensor_GetSensorFromID(lua_State* L) {
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushstring(L, "id must be a number");
        return 2;
    }
    if (!EnsureSensorSubsystem()) {
        lua_pushnil(L);
        lua_pushstring(L, "sensor subsystem unavailable");
        return 2;
    }
    SDL_SensorID id = (SDL_SensorID)lua_tonumber(L, 1);
    SDL_Sensor* s = SDL_GetSensorFromID(id);
    if (!s) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "sensor not opened");
        return 2;
    }
    lua_pushlightuserdata(L, s);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.CloseSensor(handle) -> ok
static int l_Sensor_CloseSensor(lua_State* L) {
    SDL_Sensor* s = CheckSensor(L, 1);
    if (!s) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid sensor handle");
        return 2;
    }
    // 防御性: 若 handle 与 Phase G 单例同一指针, 同时清空单例
    if (s == g_accel) g_accel = nullptr;
    if (s == g_gyro)  g_gyro  = nullptr;
    SDL_CloseSensor(s);
    lua_pushboolean(L, 1);
    return 1;
}

// Light.Sensor.GetSensorName(handle) -> string, err
static int l_Sensor_GetSensorName(lua_State* L) {
    SDL_Sensor* s = CheckSensor(L, 1);
    if (!s) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid sensor handle");
        return 2;
    }
    const char* name = SDL_GetSensorName(s);
    if (!name) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "no name");
        return 2;
    }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.GetSensorType(handle) -> string, err
static int l_Sensor_GetSensorType(lua_State* L) {
    SDL_Sensor* s = CheckSensor(L, 1);
    if (!s) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid sensor handle");
        return 2;
    }
    SDL_SensorType t = SDL_GetSensorType(s);
    lua_pushstring(L, SensorTypeName(t));
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.GetSensorID(handle) -> id, err
static int l_Sensor_GetSensorID(lua_State* L) {
    SDL_Sensor* s = CheckSensor(L, 1);
    if (!s) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid sensor handle");
        return 2;
    }
    SDL_SensorID id = SDL_GetSensorID(s);
    if (!id) {
        lua_pushnil(L);
        const char* e = SDL_GetError();
        lua_pushstring(L, (e && *e) ? e : "failed to get sensor id");
        return 2;
    }
    lua_pushnumber(L, (lua_Number)id);
    lua_pushnil(L);
    return 2;
}

// Light.Sensor.GetSensorData(handle, num_values) -> v1, v2, ..., err
static int l_Sensor_GetSensorData(lua_State* L) {
    SDL_Sensor* s = CheckSensor(L, 1);
    if (!s) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid sensor handle");
        return 2;
    }
    int n = 3;
    if (lua_type(L, 2) == LUA_TNUMBER) {
        n = (int)lua_tointeger(L, 2);
    }
    if (n < 1) n = 1;
    if (n > 16) n = 16;  // SDL3 实际不超过 16 axis, 保守上限
    float buf[16] = { 0 };
    if (!SDL_GetSensorData(s, buf, n)) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    for (int i = 0; i < n; ++i) {
        lua_pushnumber(L, (lua_Number)buf[i]);
    }
    return n;
}

// Light.Sensor.UpdateSensors() -> ok
static int l_Sensor_UpdateSensors(lua_State* L) {
    if (!EnsureSensorSubsystem()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    SDL_UpdateSensors();
    lua_pushboolean(L, 1);
    return 1;
}

// ============================================================
// luaopen_Light_Sensor
// ============================================================

static const luaL_Reg kSensorReg[] = {
    // Phase G
    { "OpenAccel",                     l_Sensor_OpenAccel                     },
    { "OpenGyro",                      l_Sensor_OpenGyro                      },
    { "GetAccel",                      l_Sensor_GetAccel                      },
    { "GetGyro",                       l_Sensor_GetGyro                       },
    { "Close",                         l_Sensor_Close                         },
    // Phase AL
    { "GetSensors",                    l_Sensor_GetSensors                    },
    { "GetSensorNameForID",            l_Sensor_GetSensorNameForID            },
    { "GetSensorTypeForID",            l_Sensor_GetSensorTypeForID            },
    { "GetSensorNonPortableTypeForID", l_Sensor_GetSensorNonPortableTypeForID },
    { "OpenSensorByID",                l_Sensor_OpenSensorByID                },
    { "GetSensorFromID",               l_Sensor_GetSensorFromID               },
    { "CloseSensor",                   l_Sensor_CloseSensor                   },
    { "GetSensorName",                 l_Sensor_GetSensorName                 },
    { "GetSensorType",                 l_Sensor_GetSensorType                 },
    { "GetSensorID",                   l_Sensor_GetSensorID                   },
    { "GetSensorData",                 l_Sensor_GetSensorData                 },
    { "UpdateSensors",                 l_Sensor_UpdateSensors                 },
    { nullptr, nullptr },
};

extern "C" LIGHT_API int luaopen_Light_Sensor(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kSensorReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }

    // Constants: SensorType enum
    lua_pushinteger(L, SDL_SENSOR_INVALID); lua_setfield(L, -2, "SENSOR_INVALID");
    lua_pushinteger(L, SDL_SENSOR_UNKNOWN); lua_setfield(L, -2, "SENSOR_UNKNOWN");
    lua_pushinteger(L, SDL_SENSOR_ACCEL);   lua_setfield(L, -2, "SENSOR_ACCEL");
    lua_pushinteger(L, SDL_SENSOR_GYRO);    lua_setfield(L, -2, "SENSOR_GYRO");
    lua_pushinteger(L, SDL_SENSOR_ACCEL_L); lua_setfield(L, -2, "SENSOR_ACCEL_L");
    lua_pushinteger(L, SDL_SENSOR_GYRO_L);  lua_setfield(L, -2, "SENSOR_GYRO_L");
    lua_pushinteger(L, SDL_SENSOR_ACCEL_R); lua_setfield(L, -2, "SENSOR_ACCEL_R");
    lua_pushinteger(L, SDL_SENSOR_GYRO_R);  lua_setfield(L, -2, "SENSOR_GYRO_R");

    // Standard gravity (m/s^2)
    lua_pushnumber(L, (lua_Number)SDL_STANDARD_GRAVITY);
    lua_setfield(L, -2, "STANDARD_GRAVITY");

    return 1;
}
