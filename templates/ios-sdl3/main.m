/**
 * @file main.m
 * @brief ChocoLight iOS 入口 — SDL3 主函数
 *
 * SDL3 的 UIApplicationDelegate 桥接自动管理:
 * - GL Surface (GLES 3.0)
 * - 事件循环 (触摸/加速度/生命周期)
 *
 * 流程: main() → SDL_main() → 创建 Lua VM → 执行 main.lua
 */

#import <UIKit/UIKit.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "light.h"
#include "light_platform_net.h"
#include "choco_crypt.h"

// 自定义 print: 输出到 NSLog
static int l_ios_print(lua_State *L) {
    int n = lua_gettop(L);
    NSMutableString *line = [NSMutableString string];
    for (int i = 1; i <= n; i++) {
        const char *s = lua_tostring(L, i);
        if (i > 1) [line appendString:@"\t"];
        [line appendString:s ? [NSString stringWithUTF8String:s] : @"(null)"];
    }
    NSLog(@"[Lua] %@", line);
    return 0;
}

// 从 Bundle 加载 Lua 脚本
static int LoadScript(lua_State *L, const char *filename) {
    // SDL3 on iOS: SDL_GetBasePath 返回 app bundle 路径
    size_t dataLen = 0;
    char path[512];
    snprintf(path, sizeof(path), "lua/%s", filename);

    void *data = SDL_LoadFile(path, &dataLen);
    if (!data) {
        // 回退: 直接从 NSBundle 加载
        NSString *baseName = [[NSString stringWithUTF8String:filename] stringByDeletingPathExtension];
        NSString *ext = [[NSString stringWithUTF8String:filename] pathExtension];
        NSString *bundlePath = [[NSBundle mainBundle] pathForResource:baseName ofType:ext];
        if (bundlePath) {
            NSData *nsdata = [NSData dataWithContentsOfFile:bundlePath];
            if (nsdata) {
                int r = luaL_loadbuffer(L, (const char *)[nsdata bytes], [nsdata length], filename);
                if (r != 0) {
                    NSLog(@"[ChocoLight] Lua load error: %s", lua_tostring(L, -1));
                    lua_pop(L, 1);
                    return -1;
                }
                if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
                    NSLog(@"[ChocoLight] Lua error: %s", lua_tostring(L, -1));
                    lua_pop(L, 1);
                    return -1;
                }
                return 0;
            }
        }
        NSLog(@"[ChocoLight] Failed to load: %s (%s)", filename, SDL_GetError());
        return -1;
    }

    // CLPK 加密检查
    const uint8_t *bytes = (const uint8_t *)data;
    int result;
    if (dataLen > 4 && bytes[0] == 'C' && bytes[1] == 'L' && bytes[2] == 'P' && bytes[3] == 'K') {
        uint8_t *dec = NULL;
        size_t decLen = 0;
        if (choco_decrypt(bytes, dataLen, &dec, &decLen) == 0 && dec) {
            result = luaL_loadbuffer(L, (const char *)dec, decLen, filename);
            free(dec);
        } else {
            SDL_free(data);
            return -1;
        }
    } else {
        result = luaL_loadbuffer(L, (const char *)data, dataLen, filename);
    }
    SDL_free(data);

    if (result != 0) {
        NSLog(@"[ChocoLight] Lua load error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        NSLog(@"[ChocoLight] Lua error: %s", lua_tostring(L, -1));
        lua_pop(L, 1);
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    NSLog(@"[ChocoLight] Engine starting (SDL3 + GLES3)");

    // 网络子系统初始化
    PlatformNet::Init();

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        NSLog(@"[ChocoLight] SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    lua_State *L = luaL_newstate();
    if (!L) {
        NSLog(@"[ChocoLight] Failed to create Lua state");
        SDL_Quit();
        return 1;
    }

    luaL_openlibs(L);
    lua_pushcfunction(L, l_ios_print);
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
    luaopen_Light_Input(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Particles(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Tilemap(L); lua_pop(L, 1);
    luaopen_Light_Physics(L); lua_pop(L, 1);
    luaopen_Light_Physics_World(L); lua_pop(L, 1);
    luaopen_Light_ECS(L); lua_pop(L, 1);
    luaopen_Light_Scene(L); lua_pop(L, 1);
    luaopen_Light_Graphics_SpriteAnimation(L); lua_pop(L, 1);
    luaopen_Light_Graphics_Shader(L); lua_pop(L, 1);
    luaopen_Light_HotReload(L); lua_pop(L, 1);
    luaopen_Light_UI_Widget(L); lua_pop(L, 1);
    luaopen_Light_Crypto(L); lua_pop(L, 1);
    luaopen_Light_Record(L); lua_pop(L, 1);
    luaopen_Light_Plugins(L); lua_pop(L, 1);
    luaopen_Light_Plugins_WDFData(L); lua_pop(L, 1);
    luaopen_Light_Plugins_NEMData(L); lua_pop(L, 1);

    NSLog(@"[ChocoLight] Loading main.lua...");
    int status = LoadScript(L, "main.lua");

    lua_close(L);
    PlatformNet::Shutdown();
    SDL_Quit();

    NSLog(@"[ChocoLight] Engine shutdown (status=%d)", status);
    return status;
}
