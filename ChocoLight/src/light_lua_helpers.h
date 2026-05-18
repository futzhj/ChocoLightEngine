/**
 * @file   light_lua_helpers.h
 * @brief  Phase G.1.7 — Lua API 容错 audit: 通用类型校验 + 错误返回 helpers
 *
 * 本头文件仅在 ChocoLight 内部使用 (private, 不暴露给用户)
 *
 * 设计目标:
 *   - 消除 Lua binding 层的 type-confusion crash 风险 (用户构造 fake __instance)
 *   - 提供统一的 magic header 校验机制
 *   - 提供严格类型校验 (拒绝 number 隐式转 string 等)
 *   - 提供 nil+err / boolean+err 返回辅助
 *
 * 使用范例:
 * @code
 *   // 1) ctx struct 增加 magic 首字段
 *   constexpr uint32_t LT_MAGIC_IMAGE = LT::Magic4('I','M','G','E');
 *
 *   struct ImageContext {
 *       uint32_t     magic;          // ← 必须为首字段, 必须为 LT_MAGIC_IMAGE
 *       unsigned int texId;
 *       int          width;
 *       int          height;
 *       // ... 其他字段
 *   };
 *
 *   // 2) 创建路径设置 magic
 *   ImageContext* ctx = (ImageContext*)lua_newuserdata(L, sizeof(ImageContext));
 *   ctx->magic = LT_MAGIC_IMAGE;
 *   ctx->texId = ...;
 *
 *   // 3) Get/Check 函数走 LT::CheckInstance
 *   static ImageContext* CheckImage(lua_State* L, int idx) {
 *       return LT::CheckInstance<ImageContext>(L, idx, LT_MAGIC_IMAGE, "Light.Image");
 *   }
 *
 *   // 4) __gc 析构标记 (use-after-free 检测)
 *   static int l_Image_gc(lua_State* L) {
 *       ImageContext* ctx = LT::TryCheckInstance<ImageContext>(L, 1, LT_MAGIC_IMAGE);
 *       if (ctx) {
 *           // ... 真实析构 ...
 *           ctx->magic = LT_MAGIC_DEAD;  // 设为已析构
 *       }
 *       return 0;
 *   }
 * @endcode
 *
 * 详见 docs/Phase G.1.7 Lua API Robustness Audit/{ALIGNMENT,DESIGN}_PhaseG_1_7.md
 *
 * 作者: ChocoLight Engine
 * 版本: Phase G.1.7
 */

#pragma once

#include "light.h"

#include <cstdarg>
#include <cstdint>

namespace LT {

// ============================================================
//  Magic 编码 (跨平台一致)
// ============================================================

/// @brief 4 字符 magic 编码
/// @note 用 constexpr 编译期求值, LE/BE 平台读写都用同一 macro 故一致
constexpr uint32_t Magic4(char a, char b, char c, char d) noexcept {
    return  static_cast<uint32_t>(static_cast<unsigned char>(a))
         | (static_cast<uint32_t>(static_cast<unsigned char>(b)) << 8)
         | (static_cast<uint32_t>(static_cast<unsigned char>(c)) << 16)
         | (static_cast<uint32_t>(static_cast<unsigned char>(d)) << 24);
}

/// @brief 已析构 ctx 的 magic (use-after-free 检测)
/// @note __gc 路径设置 ctx->magic = LT_MAGIC_DEAD; 后续 access 抛 type confusion 错误
constexpr uint32_t LT_MAGIC_DEAD = Magic4('D','E','A','D');


// ============================================================
//  全局 Magic 常量表 (Phase G.1.7 各子阶段)
// ============================================================

// G.1.7.0 — 第一批高风险文件
constexpr uint32_t LT_MAGIC_IMAGE      = Magic4('I','M','G','E');  // light_graphics_image.cpp
constexpr uint32_t LT_MAGIC_CANVAS     = Magic4('C','N','V','S');  // light_graphics_canvas.cpp
constexpr uint32_t LT_MAGIC_FONT       = Magic4('F','O','N','T');  // light_graphics.cpp (FontCtxHeader)
constexpr uint32_t LT_MAGIC_DB         = Magic4('S','Q','L','I');  // light_db.cpp
constexpr uint32_t LT_MAGIC_AV         = Magic4('A','V','C','T');  // light_av.cpp (AVContext)
constexpr uint32_t LT_MAGIC_VIDEO      = Magic4('V','I','D','O');  // light_av.cpp (VideoWrapper)
constexpr uint32_t LT_MAGIC_DATABUF    = Magic4('D','B','U','F');  // light_data.cpp
constexpr uint32_t LT_MAGIC_HTTPCTX    = Magic4('H','T','T','P');  // light_network.cpp
constexpr uint32_t LT_MAGIC_EMITTER    = Magic4('E','M','I','T');  // light_particles.cpp
constexpr uint32_t LT_MAGIC_TILEMAP    = Magic4('T','L','M','P');  // light_tilemap.cpp

// G.1.7.1 — Graphics 子系统 (后续会话填充)
constexpr uint32_t LT_MAGIC_MESH       = Magic4('M','E','S','H');  // light_graphics_mesh.cpp
constexpr uint32_t LT_MAGIC_SHADER     = Magic4('S','H','D','R');  // light_graphics_shader.cpp
constexpr uint32_t LT_MAGIC_MATERIAL   = Magic4('M','A','T','L');  // light_graphics_material.cpp
constexpr uint32_t LT_MAGIC_SPRITE     = Magic4('S','P','R','T');  // light_graphics_spriteanimation.cpp
constexpr uint32_t LT_MAGIC_LIGHT2D    = Magic4('L','I','T','2');  // light_lighting2d.cpp
constexpr uint32_t LT_MAGIC_CAMERA     = Magic4('C','A','M','R');  // light_camera.cpp
constexpr uint32_t LT_MAGIC_SURFACE    = Magic4('S','R','F','C');  // light_surface.cpp
constexpr uint32_t LT_MAGIC_CURSOR     = Magic4('C','R','S','R');  // light_cursor.cpp

// G.1.7.2 — Audio + Network 子系统
constexpr uint32_t LT_MAGIC_AUDIO_SRC  = Magic4('A','S','R','C');  // light_audio_sound.cpp
constexpr uint32_t LT_MAGIC_AUDIO_GRP  = Magic4('A','G','R','P');  // light_audio_group.cpp
constexpr uint32_t LT_MAGIC_AUDIO_FX   = Magic4('A','F','X','0');  // light_audio_effect.cpp
constexpr uint32_t LT_MAGIC_NET_UDP    = Magic4('N','U','D','P');  // light_network_udp.cpp
constexpr uint32_t LT_MAGIC_NET_RPC    = Magic4('N','R','P','C');  // light_network_rpc.cpp
constexpr uint32_t LT_MAGIC_NET_ROOM   = Magic4('N','R','O','M');  // light_network_room.cpp
constexpr uint32_t LT_MAGIC_NET_WEB    = Magic4('N','W','E','B');  // light_network_web.cpp
constexpr uint32_t LT_MAGIC_IOSTREAM   = Magic4('I','O','S','T');  // light_iostream.cpp

// G.1.7.3 — Physics + Animation + ECS
constexpr uint32_t LT_MAGIC_WORLD      = Magic4('W','R','L','D');  // light_physics.cpp
constexpr uint32_t LT_MAGIC_BODY       = Magic4('B','O','D','Y');  // light_physics.cpp
constexpr uint32_t LT_MAGIC_SHAPE      = Magic4('S','H','P','E');  // light_physics.cpp
constexpr uint32_t LT_MAGIC_FIXTURE    = Magic4('F','X','T','R');  // light_physics.cpp
constexpr uint32_t LT_MAGIC_JOINT      = Magic4('J','N','T','I');  // light_physics.cpp
constexpr uint32_t LT_MAGIC_PHY3D_W    = Magic4('P','3','W','D');  // light_physics3d.cpp World
constexpr uint32_t LT_MAGIC_PHY3D_B    = Magic4('P','3','B','D');  // light_physics3d.cpp Body
constexpr uint32_t LT_MAGIC_CHARACTER  = Magic4('C','H','3','D');  // light_physics3d.cpp Character3D
constexpr uint32_t LT_MAGIC_VEHICLE    = Magic4('V','H','3','D');  // light_physics3d.cpp Vehicle3D
constexpr uint32_t LT_MAGIC_SOFTBODY   = Magic4('S','B','3','D');  // light_physics3d.cpp SoftBody3D
constexpr uint32_t LT_MAGIC_ECS_ENT    = Magic4('E','N','T','Y');  // light_ecs.cpp Entity
// Animation 模块 (light_animation.cpp)
constexpr uint32_t LT_MAGIC_SKELETON   = Magic4('S','K','E','L');
constexpr uint32_t LT_MAGIC_ANIMCLIP   = Magic4('A','C','L','P');
constexpr uint32_t LT_MAGIC_ANIMATOR   = Magic4('A','N','M','R');
constexpr uint32_t LT_MAGIC_SKINMESH   = Magic4('S','K','M','H');

// G.1.7.4 — 系统类剩余
constexpr uint32_t LT_MAGIC_NEM        = Magic4('N','E','M','C');  // light_plugins.cpp NEMContext
constexpr uint32_t LT_MAGIC_WDF        = Magic4('W','D','F','C');  // light_plugins.cpp WDFContext
constexpr uint32_t LT_MAGIC_PROCESS    = Magic4('P','R','O','C');  // light_process.cpp
constexpr uint32_t LT_MAGIC_TRAY       = Magic4('T','R','A','Y');  // light_tray.cpp
constexpr uint32_t LT_MAGIC_HIDDEV     = Magic4('H','I','D','D');  // light_hidapi.cpp
constexpr uint32_t LT_MAGIC_SENSOR     = Magic4('S','E','N','S');  // light_sensor.cpp
constexpr uint32_t LT_MAGIC_HAPTIC     = Magic4('H','A','P','T');  // light_haptic.cpp
constexpr uint32_t LT_MAGIC_LOADSO     = Magic4('S','O','L','D');  // light_loadso.cpp


// ============================================================
//  通用 instance ctx 验证
// ============================================================

/// @brief 验证 OOP `__instance` userdata + magic 首字段
///
/// 模式:
///   Lua: setmetatable({__instance = userdata}, OOP_MT)
///   C++: getfield(idx, "__instance") → cast → check magic[0]
///
/// 失败时直接抛 luaL_error (longjmp), 不返回. 业务代码可假定非空.
///
/// @tparam T  ctx struct 类型, 必须以 `uint32_t magic` 作为首字段
/// @param L  Lua 状态
/// @param idx  栈索引 (positive 或 negative)
/// @param expectedMagic  期望的 magic 值 (e.g. LT_MAGIC_IMAGE)
/// @param typeName  错误信息中的类型名 (e.g. "Light.Image")
/// @return 非空 T*; 失败时已 luaL_error 抛出
template <typename T>
inline T* CheckInstance(lua_State* L, int idx, uint32_t expectedMagic, const char* typeName) {
    if (!lua_istable(L, idx)) {
        luaL_typerror(L, idx, typeName);
        return nullptr;  // unreachable (luaL_typerror longjmp)
    }
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        luaL_error(L, "%s: expected instance table with __instance userdata at arg #%d", typeName, idx);
        return nullptr;
    }
    void* raw = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!raw) {
        luaL_error(L, "%s: __instance userdata is null at arg #%d", typeName, idx);
        return nullptr;
    }
    // magic 在 ctx struct 首字段; 用 uint32_t 对齐读取
    uint32_t actualMagic = *static_cast<const uint32_t*>(raw);
    if (actualMagic == LT_MAGIC_DEAD) {
        luaL_error(L, "%s: use-after-free detected at arg #%d (object already destroyed)", typeName, idx);
        return nullptr;
    }
    if (actualMagic != expectedMagic) {
        luaL_error(L,
            "%s: type confusion detected at arg #%d (magic mismatch: got 0x%08X, expected 0x%08X)",
            typeName, idx, actualMagic, expectedMagic);
        return nullptr;
    }
    return static_cast<T*>(raw);
}

/// @brief 不抛错版本 (返 nullptr); 用于可选参数 / 老代码迁移过渡期
///
/// 比 CheckInstance 多 magic 校验, 但失败仅返 nullptr 不抛. 调用者负责处理 nullptr.
///
/// @return T* 或 nullptr (任何检查失败均返 nullptr)
template <typename T>
inline T* TryCheckInstance(lua_State* L, int idx, uint32_t expectedMagic) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    void* raw = lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!raw) return nullptr;
    uint32_t actualMagic = *static_cast<const uint32_t*>(raw);
    if (actualMagic != expectedMagic) return nullptr;
    return static_cast<T*>(raw);
}


// ============================================================
//  严格类型校验 (拒绝 number 隐式转 string 等)
// ============================================================

/// @brief 严格 string 校验 (拒绝 number, lua_tostring 对 number 返非 nullptr 但语义混淆)
inline const char* CheckStringStrict(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TSTRING) {
        luaL_typerror(L, idx, "string");
        return nullptr;  // unreachable
    }
    return lua_tostring(L, idx);
}

/// @brief 可选 string (nil → def, number 拒绝)
inline const char* OptStringStrict(lua_State* L, int idx, const char* def) {
    int t = lua_type(L, idx);
    if (t == LUA_TNONE || t == LUA_TNIL) return def;
    if (t != LUA_TSTRING) {
        luaL_typerror(L, idx, "string or nil");
        return nullptr;
    }
    return lua_tostring(L, idx);
}


// ============================================================
//  错误返回辅助
// ============================================================

/// @brief 推 nil + err 字符串到栈 (nil+err API 风格); 总返 2
///
/// 用法:
/// @code
///   if (somethingFailed) {
///       return LT::PushNilError(L, "operation failed: %s", reason);
///   }
/// @endcode
inline int PushNilError(lua_State* L, const char* fmt, ...) {
    lua_pushnil(L);
    va_list ap;
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return 2;
}

/// @brief 推 false + err 字符串到栈 (boolean+err API 风格); 总返 2
inline int PushBooleanError(lua_State* L, const char* fmt, ...) {
    lua_pushboolean(L, 0);
    va_list ap;
    va_start(ap, fmt);
    lua_pushvfstring(L, fmt, ap);
    va_end(ap);
    return 2;
}

}  // namespace LT
