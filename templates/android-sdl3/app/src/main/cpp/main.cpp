/**
 * @file main.cpp
 * @brief ChocoLight Android 入口 — SDL3 主函数
 *
 * SDL3 的 SDLActivity 通过 JNI 调用此 main(), 自动管理:
 * - GL Surface 创建 (GLES 3.0)
 * - 事件循环 (触摸/键盘/生命周期)
 * - 音频设备
 *
 * 流程: main() → 创建 Lua VM → 注册 Light 模块 → 执行 main.lua
 */

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <android/log.h>

// Lumen (Lua 5.1 兼容)
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

// ChocoLight 模块注册 (声明在 light.h 中)
#include "light.h"
#include "light_platform_net.h"

// 脚本解密支持
#include "choco_crypt.h"

#define TAG "ChocoLight"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 自定义 print: 输出到 logcat
static int l_print(lua_State* L) {
    int n = lua_gettop(L);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);
    for (int i = 1; i <= n; i++) {
        const char* s = lua_tostring(L, i);
        if (i > 1) luaL_addchar(&buf, '\t');
        luaL_addstring(&buf, s ? s : "(null)");
    }
    luaL_pushresult(&buf);
    LOGI("%s", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 0;
}

// 从 SDL 基路径加载 Lua 脚本 (assets 目录)
static int LoadScript(lua_State* L, const char* filename) {
    // Android: SDL_GetBasePath 映射到 assets
    char path[512];
    snprintf(path, sizeof(path), "lua/%s", filename);

    // 尝试通过 SDL_IOStream 从 assets 读取
    size_t dataLen = 0;
    void* data = SDL_LoadFile(path, &dataLen);
    if (!data) {
        LOGE("Failed to load script: %s (%s)", path, SDL_GetError());
        return -1;
    }

    // 检查是否为加密脚本 (.enc 或 magic 头)
    const uint8_t* bytes = (const uint8_t*)data;
    bool encrypted = (dataLen > 4 && bytes[0] == 'C' && bytes[1] == 'L'
                      && bytes[2] == 'P' && bytes[3] == 'K');

    int result;
    if (encrypted) {
        // CLPK 解密
        uint8_t* dec = nullptr;
        size_t decLen = 0;
        if (choco_decrypt(bytes, dataLen, &dec, &decLen) == 0 && dec) {
            result = luaL_loadbuffer(L, (const char*)dec, decLen, filename);
            free(dec);
        } else {
            LOGE("Script decryption failed: %s", filename);
            SDL_free(data);
            return -1;
        }
    } else {
        result = luaL_loadbuffer(L, (const char*)data, dataLen, filename);
    }

    SDL_free(data);

    if (result != 0) {
        LOGE("Lua load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }

    // 执行脚本
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        LOGE("Lua runtime error: %.1024s", err ? err : "(unknown error)");
        lua_pop(L, 1);
        return -1;
    }

    return 0;
}

int main(int argc, char* argv[]) {
    (void)argc; (void)argv;
    LOGI("ChocoLight Engine starting (SDL3 + GLES3)");

    // 网络子系统初始化
    LOGI("[DIAG] PlatformNet::Init...");
    PlatformNet::Init();

    // SDL3 初始化 (Video 子系统, Audio 由 miniaudio 管理)
    LOGI("[DIAG] SDL_Init...");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        LOGE("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    // 创建 Lua VM
    lua_State* L = luaL_newstate();
    if (!L) {
        LOGE("Failed to create Lua state");
        SDL_Quit();
        return 1;
    }

    luaL_openlibs(L);

    // 覆盖 print → logcat
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    // 注册 ChocoLight 所有模块
    luaopen_Light(L); lua_pop(L, 1);
    luaopen_Light_Debug(L); lua_pop(L, 1);
    luaopen_Light_Data(L); lua_pop(L, 1);
    luaopen_Light_Math(L); lua_pop(L, 1);
    luaopen_Light_UI(L); lua_pop(L, 1);
    luaopen_Light_UI_Window(L); lua_pop(L, 1);
    luaopen_Light_Graphics(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Canvas(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Image(L); lua_pop(L, 1);
    luaopen_Light_Graphics_ImageData(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Font(L); lua_pop(L, 1);
    luaopen_Light_Graphics_PixelFormat(L); lua_pop(L, 1);
    luaopen_Light_AV(L); lua_pop(L, 1);
    luaopen_Light_AV_Audio(L); lua_pop(L, 1);
    luaopen_Light_AV_AudioData(L); lua_pop(L, 1);
    luaopen_Light_AV_Video(L); lua_pop(L, 1);
    luaopen_Light_DB(L); lua_pop(L, 1);
    luaopen_Light_DB_SQLite(L); lua_pop(L, 1);
    luaopen_Light_Network(L); lua_pop(L, 1);
    luaopen_Light_Network_Http(L); lua_pop(L, 1);
    luaopen_Light_Network_HttpServer(L); lua_pop(L, 1);
    luaopen_Light_Network_Web(L); lua_pop(L, 1);
    // Phase 2 逐步排查: 仅 ECS
    LOGI("[DIAG] Phase2: ECS only...");
    luaopen_Light_ECS(L); lua_pop(L, 1);
    luaopen_Light_Record(L); lua_pop(L, 1);
    luaopen_Light_Plugins(L); lua_pop(L, 1);
    luaopen_Light_Plugins_WDFData(L); lua_pop(L, 1);
    luaopen_Light_Plugins_NEMData(L); lua_pop(L, 1);

    LOGI("Lua VM initialized, loading main.lua...");

    // 执行主脚本
    int status = LoadScript(L, "main.lua");
    if (status != 0) {
        LOGE("Failed to execute main.lua");
    }

    // 清理
    lua_close(L);
    PlatformNet::Shutdown();
    SDL_Quit();

    LOGI("ChocoLight Engine shutdown");
    return status;
}
