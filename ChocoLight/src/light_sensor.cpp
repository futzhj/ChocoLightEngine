/**
 * @file light_sensor.cpp
 * @brief Light.Sensor 模块 - 移动设备加速度/陀螺仪传感器 (基于 SDL_Sensor)
 *
 * Lua API:
 *   Light.Sensor.OpenAccel() -> ok, err     首次调用时 lazy init SDL_INIT_SENSOR
 *   Light.Sensor.OpenGyro()  -> ok, err
 *   Light.Sensor.GetAccel()  -> x, y, z      (m/s², 含重力)
 *   Light.Sensor.GetGyro()   -> x, y, z      (rad/s)
 *   Light.Sensor.Close()                     关闭所有 sensor 句柄
 *
 * 设计要点:
 *   - 单例 Accel + 单例 Gyro 句柄, 简化常见使用 (FPS 控制/重力游戏)
 *   - 打开第一个匹配类型的传感器, 不支持多 sensor 并行
 *   - 桌面平台通常无传感器, Open 返回失败 (用户应优雅降级)
 *   - SDL3 在 Open 时若 SDL_INIT_SENSOR 未启用会自动 init
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace {
    SDL_Sensor* g_accel = nullptr;
    SDL_Sensor* g_gyro  = nullptr;
    bool        g_subsysInited = false;

    bool EnsureSensorSubsystem() {
        if (g_subsysInited) return true;
        if (!SDL_InitSubSystem(SDL_INIT_SENSOR)) return false;
        g_subsysInited = true;
        return true;
    }

    // 在已枚举的 SensorID 列表中找第一个匹配类型的 ID
    SDL_SensorID FindFirstByType(SDL_SensorType wanted) {
        int count = 0;
        SDL_SensorID* ids = SDL_GetSensors(&count);
        if (!ids) return 0;

        SDL_SensorID found = 0;
        for (int i = 0; i < count; ++i) {
            if (SDL_GetSensorTypeForID(ids[i]) == wanted) {
                found = ids[i];
                break;
            }
        }
        SDL_free(ids);
        return found;
    }

    int OpenSensorByType(lua_State* L, SDL_SensorType wanted, SDL_Sensor** slot) {
        if (*slot) {
            // 已打开, 视为成功
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

// ==================== Light.Sensor.OpenAccel ====================

static int l_Sensor_OpenAccel(lua_State* L) {
    return OpenSensorByType(L, SDL_SENSOR_ACCEL, &g_accel);
}

// ==================== Light.Sensor.OpenGyro ====================

static int l_Sensor_OpenGyro(lua_State* L) {
    return OpenSensorByType(L, SDL_SENSOR_GYRO, &g_gyro);
}

// ==================== Light.Sensor.GetAccel ====================

static int l_Sensor_GetAccel(lua_State* L) {
    return ReadSensorXYZ(L, g_accel, "accelerometer not opened");
}

// ==================== Light.Sensor.GetGyro ====================

static int l_Sensor_GetGyro(lua_State* L) {
    return ReadSensorXYZ(L, g_gyro, "gyroscope not opened");
}

// ==================== Light.Sensor.Close ====================

static int l_Sensor_Close(lua_State* L) {
    if (g_accel) {
        SDL_CloseSensor(g_accel);
        g_accel = nullptr;
    }
    if (g_gyro) {
        SDL_CloseSensor(g_gyro);
        g_gyro = nullptr;
    }
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== luaopen_Light_Sensor ====================

extern "C" LIGHT_API int luaopen_Light_Sensor(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "OpenAccel", l_Sensor_OpenAccel },
        { "OpenGyro",  l_Sensor_OpenGyro  },
        { "GetAccel",  l_Sensor_GetAccel  },
        { "GetGyro",   l_Sensor_GetGyro   },
        { "Close",     l_Sensor_Close     },
        { nullptr,     nullptr            },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
