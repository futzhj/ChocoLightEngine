/**
 * ChocoLight Engine — Android JNI 桥接层
 * 初始化 Lumen Lua VM，从 assets 加载并执行 Lua 脚本
 */

#include <jni.h>
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cstdlib>
#include <cstring>

// Lumen (Lua 5.1 compatible) — C++ headers with built-in extern "C" linkage
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#define TAG "ChocoLight"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// 全局上下文（JNI 回调 print 需要）
static JNIEnv *g_env = nullptr;
static jobject g_callback = nullptr;
static jmethodID g_onOutput = nullptr;

// 自定义 print 函数：输出到 Java 回调 + logcat
static int l_print(lua_State *L) {
    int n = lua_gettop(L);
    luaL_Buffer buf;
    luaL_buffinit(L, &buf);

    for (int i = 1; i <= n; i++) {
        const char *s = luaL_tolstring(L, i, nullptr);
        if (i > 1) luaL_addchar(&buf, '\t');
        luaL_addstring(&buf, s ? s : "(null)");
        lua_pop(L, 1);
    }
    luaL_pushresult(&buf);
    const char *text = lua_tostring(L, -1);

    LOGI("%s", text);

    // 回调 Java 层显示输出
    if (g_env && g_callback && g_onOutput) {
        jstring jtext = g_env->NewStringUTF(text);
        g_env->CallVoidMethod(g_callback, g_onOutput, jtext);
        g_env->DeleteLocalRef(jtext);
    }

    lua_pop(L, 1);
    return 0;
}

// 从 Android assets 读取文件内容
static char* loadAsset(AAssetManager *mgr, const char *name, size_t *outLen) {
    AAsset *asset = AAssetManager_open(mgr, name, AASSET_MODE_BUFFER);
    if (!asset) return nullptr;

    size_t len = AAsset_getLength(asset);
    char *data = (char*)malloc(len + 1);
    AAsset_read(asset, data, len);
    data[len] = '\0';
    AAsset_close(asset);

    if (outLen) *outLen = len;
    return data;
}

extern "C"
JNIEXPORT jint JNICALL
Java_com_chocolight_engine_LightActivity_nativeRunScript(
    JNIEnv *env, jobject thiz,
    jobject assetManager, jstring scriptName, jobject callback) {

    g_env = env;
    g_callback = callback;

    // 获取回调方法 ID
    jclass cbClass = env->GetObjectClass(callback);
    g_onOutput = env->GetMethodID(cbClass, "onOutput", "(Ljava/lang/String;)V");

    // 初始化 Lua VM
    lua_State *L = luaL_newstate();
    if (!L) {
        LOGE("Failed to create Lua state");
        return -1;
    }
    luaL_openlibs(L);

    // 替换 print 函数
    lua_pushcfunction(L, l_print);
    lua_setglobal(L, "print");

    // 从 assets 加载脚本
    AAssetManager *mgr = AAssetManager_fromJava(env, assetManager);
    const char *name = env->GetStringUTFChars(scriptName, nullptr);

    size_t scriptLen = 0;
    char *scriptData = loadAsset(mgr, name, &scriptLen);

    int status = -1;
    if (scriptData) {
        LOGI("Loaded script: %s (%zu bytes)", name, scriptLen);
        status = luaL_loadbuffer(L, scriptData, scriptLen, name) || lua_pcall(L, 0, 0, 0);
        if (status != 0) {
            const char *err = lua_tostring(L, -1);
            LOGE("Lua error: %s", err ? err : "unknown");
            // 发送错误到 Java 层
            if (g_onOutput) {
                char errBuf[512];
                snprintf(errBuf, sizeof(errBuf), "[ERROR] %s", err ? err : "unknown");
                jstring jerr = env->NewStringUTF(errBuf);
                env->CallVoidMethod(callback, g_onOutput, jerr);
                env->DeleteLocalRef(jerr);
            }
            lua_pop(L, 1);
        }
        free(scriptData);
    } else {
        LOGE("Script not found: %s", name);
    }

    env->ReleaseStringUTFChars(scriptName, name);
    lua_close(L);
    g_env = nullptr;
    g_callback = nullptr;
    g_onOutput = nullptr;
    return status;
}
