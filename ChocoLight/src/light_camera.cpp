/**
 * @file light_camera.cpp
 * @brief Light.Camera 模块 - 摄像头采集 (基于 SDL_Camera, SDL3.2.x)
 *
 * Lua API:
 *   Light.Camera.GetCameras() -> { id1, id2, ... }, err
 *     - id 为 SDL_CameraID (Uint32) 数值
 *
 *   Light.Camera.GetName(id) -> name_string, err
 *
 *   Light.Camera.Open(id) -> handle (light userdata), err
 *     - 仅请求开启, 实际权限可能仍 pending, 需轮询 GetPermissionState
 *
 *   Light.Camera.GetPermissionState(handle) -> int, err
 *     - 1 = approved, 0 = pending, -1 = denied
 *
 *   Light.Camera.AcquireFrame(handle) -> {w,h,format,pitch,timestamp_ns,pixels=str} | nil, err
 *     - 阻塞拷贝当前帧像素至 Lua string 后立即 release, Lua 端无需关心 surface 生命周期
 *     - 无新帧可用时返回 nil + err="no frame available"
 *
 *   Light.Camera.Close(handle) -> ok, err
 *
 * 平台覆盖: Win/Mac/Linux/Android/iOS/Web 均有不同程度支持; CI 桌面环境多无相机, 需测试边界路径.
 */
#include "light.h"

#include <SDL3/SDL.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// ==================== Light.Camera.GetCameras ====================

static int l_Camera_GetCameras(lua_State* L) {
    int count = 0;
    SDL_CameraID* ids = SDL_GetCameras(&count);
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

// ==================== Light.Camera.GetName ====================

static int l_Camera_GetName(lua_State* L) {
    SDL_CameraID id = (SDL_CameraID)luaL_checkinteger(L, 1);
    const char* name = SDL_GetCameraName(id);
    if (!name) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushstring(L, name);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Camera.Open ====================

static int l_Camera_Open(lua_State* L) {
    SDL_CameraID id = (SDL_CameraID)luaL_checkinteger(L, 1);
    // spec=NULL: 让 SDL 选择默认格式; 高级用户可后续扩展接受 spec table
    SDL_Camera* cam = SDL_OpenCamera(id, nullptr);
    if (!cam) {
        lua_pushnil(L);
        lua_pushstring(L, SDL_GetError());
        return 2;
    }
    lua_pushlightuserdata(L, cam);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Camera.GetPermissionState ====================

static int l_Camera_GetPermissionState(lua_State* L) {
    SDL_Camera* cam = (SDL_Camera*)lua_touserdata(L, 1);
    if (!cam) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid camera handle");
        return 2;
    }
    int state = SDL_GetCameraPermissionState(cam);
    lua_pushinteger(L, state);
    lua_pushnil(L);
    return 2;
}

// ==================== Light.Camera.AcquireFrame ====================

static int l_Camera_AcquireFrame(lua_State* L) {
    SDL_Camera* cam = (SDL_Camera*)lua_touserdata(L, 1);
    if (!cam) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid camera handle");
        return 2;
    }
    Uint64 ts_ns = 0;
    SDL_Surface* surf = SDL_AcquireCameraFrame(cam, &ts_ns);
    if (!surf) {
        // 无新帧 != 错误; 返回 nil + 简短信息, 调用方可循环轮询
        lua_pushnil(L);
        lua_pushstring(L, "no frame available");
        return 2;
    }

    // 拷贝像素至 Lua string. 完成后立即 release surface, Lua 不持有 SDL 资源.
    int w = surf->w;
    int h = surf->h;
    int pitch = surf->pitch;
    Uint32 fmt = (Uint32)surf->format;
    size_t bytes = (size_t)pitch * (size_t)h;

    lua_createtable(L, 0, 6);
    lua_pushinteger(L, w);     lua_setfield(L, -2, "w");
    lua_pushinteger(L, h);     lua_setfield(L, -2, "h");
    lua_pushinteger(L, pitch); lua_setfield(L, -2, "pitch");
    lua_pushinteger(L, (lua_Integer)fmt); lua_setfield(L, -2, "format");
    lua_pushnumber(L, (lua_Number)ts_ns); lua_setfield(L, -2, "timestamp_ns");
    if (surf->pixels && bytes > 0) {
        lua_pushlstring(L, (const char*)surf->pixels, bytes);
    } else {
        lua_pushlstring(L, "", 0);
    }
    lua_setfield(L, -2, "pixels");

    SDL_ReleaseCameraFrame(cam, surf);

    lua_pushnil(L);
    return 2;
}

// ==================== Light.Camera.Close ====================

static int l_Camera_Close(lua_State* L) {
    SDL_Camera* cam = (SDL_Camera*)lua_touserdata(L, 1);
    if (!cam) {
        lua_pushboolean(L, 0);
        lua_pushstring(L, "invalid camera handle");
        return 2;
    }
    SDL_CloseCamera(cam);
    lua_pushboolean(L, 1);
    lua_pushnil(L);
    return 2;
}

// ==================== luaopen_Light_Camera ====================

extern "C" LIGHT_API int luaopen_Light_Camera(lua_State* L) {
    static const luaL_Reg fns[] = {
        { "GetCameras",          l_Camera_GetCameras          },
        { "GetName",             l_Camera_GetName             },
        { "Open",                l_Camera_Open                },
        { "GetPermissionState",  l_Camera_GetPermissionState  },
        { "AcquireFrame",        l_Camera_AcquireFrame        },
        { "Close",               l_Camera_Close               },
        { nullptr,               nullptr                      },
    };
    lua_newtable(L);
    luaL_register(L, nullptr, fns);
    return 1;
}
