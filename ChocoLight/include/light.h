/**
 * @file light.h
 * @brief ChocoLight 框架公共头文件 — 从 Light.dll v0.2.3 逆向还原
 * @author Jakit Liang (原作者)
 * @note 代码由 IDA Pro 反编译还原，保持与原始二进制的符号兼容性
 */

#pragma once

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdint>
#include <iostream>
#include <string>
#include <cstdarg>
#include <vector>
#include <algorithm>

// ==================== Lua 头文件 ====================

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

// ==================== 导出宏 ====================

#ifdef LIGHT_EXPORTS
#define LIGHT_API __declspec(dllexport)
#else
#define LIGHT_API __declspec(dllimport)
#endif

// ==================== CC 命名空间 (核心工具) ====================

namespace CC {

/// @brief 断言失败时打印消息并 abort
/// @param condition 条件值
/// @param msg 消息
/// @param file 文件名
/// @param line 行号
LIGHT_API void Assert(bool condition, const char* msg, const char* file, int line);

/// @brief 日志级别
enum LogLevel {
    LOG_INFO  = 0,  // [I] — 白色
    LOG_WARN  = 1,  // [W] — 黄色
    LOG_ERROR = 2,  // [E] — 红色
};

/// @brief 核心日志函数 — 带时间戳和颜色输出
/// @param level 日志级别 (0=Info, 1=Warn, 2=Error)
/// @param fmt 格式字符串
void Log(LogLevel level, const char* fmt, ...);

/// @brief 安全指针模板 (引用计数 + vtable)
template<typename T>
class Safe {
public:
    void* vtable;       // 虚函数表
    uint8_t flag;       // 标志位
    T* ptr;             // 指向的数据
    
    Safe();
    ~Safe();
    const char* c_str() const;  // 获取内部字符串指针
    void release();             // 释放资源
};

} // namespace CC

// ==================== 模块注册辅助 ====================

namespace LT {

/// @brief 确保 Light 全局表已在 Lua 栈中创建
/// 不存在时加载并执行初始化脚本，创建 Light 表
void EnsureLightTable(lua_State* L);

/// @brief 标准模块注册模板
/// 检查子表是否存在于 Light 表中，不存在则创建并注册函数
/// @param L Lua状态
/// @param name 模块名 (如 "Debug", "Graphics")
/// @param funcs 函数注册表
void RegisterModule(lua_State* L, const char* name, const luaL_Reg* funcs);

/// @brief 网络空通知
namespace Network {
    LIGHT_API void NotifyEmpty();
    LIGHT_API void NotifyMainEmpty();
}

} // namespace LT

// ==================== 字体字形查询接口 ====================
// 跨编译单元桥接函数 — 从 FontContext userdata 获取字形信息

struct FontGlyphResult {
    float u0, v0, u1, v1;  // 图集 UV
    float xoff, yoff;      // 位置偏移
    float width, height;   // 像素尺寸
    float xadvance;        // 水平步进
    int   found;           // 0=未找到, 1=找到
};

// 获取字形 (自动触发动态烘焙)
void FontGetGlyph(void* fontCtx, int codepoint, FontGlyphResult* out);

// ==================== Lua 模块导出 ====================

extern "C" {

// 核心模块
LIGHT_API int luaopen_Light(lua_State* L);
LIGHT_API int luaopen_Light_Debug(lua_State* L);
LIGHT_API int luaopen_Light_Data(lua_State* L);
LIGHT_API int luaopen_Light_Math(lua_State* L);

// UI 模块
LIGHT_API int luaopen_Light_UI(lua_State* L);
LIGHT_API int luaopen_Light_UI_Window(lua_State* L);

// 图形模块
LIGHT_API int luaopen_Light_Graphics(lua_State* L);
LIGHT_API int luaopen_Light_Graphics_Canvas(lua_State* L);
LIGHT_API int luaopen_Light_Graphics_Image(lua_State* L);
LIGHT_API int luaopen_Light_Graphics_ImageData(lua_State* L);
LIGHT_API int luaopen_Light_Graphics_Font(lua_State* L);
LIGHT_API int luaopen_Light_Graphics_PixelFormat(lua_State* L);

// 音视频模块
LIGHT_API int luaopen_Light_AV(lua_State* L);
LIGHT_API int luaopen_Light_AV_Audio(lua_State* L);
LIGHT_API int luaopen_Light_AV_AudioData(lua_State* L);
LIGHT_API int luaopen_Light_AV_Video(lua_State* L);

// 数据库模块
LIGHT_API int luaopen_Light_DB(lua_State* L);
LIGHT_API int luaopen_Light_DB_SQLite(lua_State* L);

// 网络模块
LIGHT_API int luaopen_Light_Network(lua_State* L);
LIGHT_API int luaopen_Light_Network_Http(lua_State* L);
LIGHT_API int luaopen_Light_Network_HttpServer(lua_State* L);
LIGHT_API int luaopen_Light_Network_Web(lua_State* L);

// 录制模块
LIGHT_API int luaopen_Light_Record(lua_State* L);

// 插件模块
LIGHT_API int luaopen_Light_Plugins(lua_State* L);
LIGHT_API int luaopen_Light_Plugins_WDFData(lua_State* L);
LIGHT_API int luaopen_Light_Plugins_NEMData(lua_State* L);

// SQLite3 扩展导出 (原始DLL静态链接SQLite后暴露的扩展接口)
LIGHT_API int sqlite3_carray_init(void* db, char** pzErrMsg, void* pApi);
LIGHT_API int sqlite3_fileio_init(void* db, char** pzErrMsg, void* pApi);
LIGHT_API int sqlite3_regexp_init(void* db, char** pzErrMsg, void* pApi);
LIGHT_API int sqlite3_series_init(void* db, char** pzErrMsg, void* pApi);
LIGHT_API int sqlite3_shathree_init(void* db, char** pzErrMsg, void* pApi);
LIGHT_API int sqlite3_uuid_init(void* db, char** pzErrMsg, void* pApi);

} // extern "C"
