/**
 * @file light_lighting2d.cpp
 * @brief Phase E.1.3 + E.1.4 — Light.Lighting2D C++ 模块 + Lua binding
 *
 * E.1.3: 维护 16 个 light slot + ambient + enabled 开关的单例状态.
 * E.1.4: 在本文件末尾追加 Lua binding (AddPointLight / AddSpotLight / ...).
 * E.1.5 会把 UploadToShader 里的 no-op 替换为对 RenderBackend 虚接口的真正调用.
 *
 * 设计约束:
 *   - POD State + 文件内 static 单例 (不分配堆内存, 不加锁, 单线程使用)
 *   - id 空间 1..MAX_LIGHTS (0 保留给"失败" 返回值), 内部 idx = id - 1
 *   - Add 挑第一个 type==INACTIVE 的 slot, 不保证 id 连续 (Remove 后的 slot 被复用)
 *   - C++ Update 用"全字段覆盖"语义; Lua UpdateLight 做"部分字段更新"语义
 *     (先复制现 slot, 再用 table 字段覆盖, 最后调 C++ Update)
 *   - innerAngle/outerAngle: Lua 端传 "度", C++ 存 cos (binding 内做一次转换)
 */

#include "light_lighting2d.h"
#include "light.h"             // LIGHT_API, CC::Log
#include "render_backend.h"    // RenderBackend::UploadLighting2D (E.1.5)

#include <cmath>                // cosf, sqrtf

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace Lighting2D {

// ==================== 单例状态 ====================
// 文件内 static, 默认初始化等价于 State{}:
//   enabled=true, 所有 lights[i].type=INACTIVE, active_count=0, ambient={0,0,0}
static State g_state;

State* GetState() {
    return &g_state;
}

// ==================== Enabled / Ambient ====================

void SetEnabled(bool v) {
    g_state.enabled = v;
}

bool IsEnabled() {
    return g_state.enabled;
}

void SetAmbient(float r, float g, float b) {
    g_state.ambient[0] = r;
    g_state.ambient[1] = g;
    g_state.ambient[2] = b;
}

void GetAmbient(float& r, float& g, float& b) {
    r = g_state.ambient[0];
    g = g_state.ambient[1];
    b = g_state.ambient[2];
}

// ==================== Add / Update / Remove / Clear ====================

int Add(const Light& l) {
    // 非法 type (INACTIVE) 直接拒绝, 否则 active_count 会与 slot.type 不一致
    if (l.type == TYPE_INACTIVE) return 0;

    // 扫描第一个空闲 slot (O(MAX_LIGHTS) = O(16), 可接受)
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        if (g_state.lights[i].type == TYPE_INACTIVE) {
            g_state.lights[i] = l;
            ++g_state.active_count;
            return i + 1;  // id = index + 1 (1..16)
        }
    }
    // 16 个 slot 全满
    return 0;
}

bool Update(int id, const Light& fields) {
    // 越界 (id=0 保留 / id>16)
    if (id < 1 || id > MAX_LIGHTS) return false;
    const int idx = id - 1;

    // slot 当前已是 INACTIVE (被 Remove/Clear 过)
    if (g_state.lights[idx].type == TYPE_INACTIVE) return false;

    // fields.type 若为 INACTIVE, 语义上等价于 Remove(id); 但为了明确职责边界,
    // Update 不应承担 Remove 语义, 直接拒绝让调用方显式调 Remove.
    if (fields.type == TYPE_INACTIVE) return false;

    g_state.lights[idx] = fields;
    return true;
}

void Remove(int id) {
    // 幂等: 越界 / 已 INACTIVE 时均 no-op
    if (id < 1 || id > MAX_LIGHTS) return;
    const int idx = id - 1;
    if (g_state.lights[idx].type == TYPE_INACTIVE) return;

    g_state.lights[idx].type = TYPE_INACTIVE;
    --g_state.active_count;
}

void Clear() {
    for (int i = 0; i < MAX_LIGHTS; ++i) {
        g_state.lights[i].type = TYPE_INACTIVE;
    }
    g_state.active_count = 0;
    // ambient 与 enabled 按设计保留 (符合 Lua ClearLights 语义: 只清 light, 不动全局配置)
}

// ==================== 查询 ====================

int GetCount() {
    return g_state.active_count;
}

int GetMax() {
    return MAX_LIGHTS;
}

// ==================== 后端上传 (E.1.5 真实实现) ====================
//
// 之前 (E.1.3): no-op 占位.
// 现在 (E.1.5): 转发到 RenderBackend::UploadLighting2D(state*) 虚接口,
//               GL33Backend 在内部 build SOA 临时数组 + glUniform*v 一次上传.
//
// programId 仍保留在签名里, 但当前未使用 — GL33Backend 持有 programLit2D 句柄,
// 不依赖外部传入. 留作未来 multi-program (e.g. 不同变体 shader) 的拓展接口.
void UploadToShader(RenderBackend* backend, uint32_t programId) {
    (void)programId;
    if (!backend) return;
    backend->UploadLighting2D(&g_state);
}

}  // namespace Lighting2D

// ============================================================================
// Phase E.1.4 — Lua binding (Light.Lighting2D)
// ============================================================================
//
// 函数签名约定:
//   - 失败统一返回 lua nil (Add* 返回 0/nil); UpdateLight 返回 boolean
//   - 所有 table 参数字段都有默认值 (Light 结构体默认初始化提供)
//   - innerAngle/outerAngle 单位 "度", 内部 cosf(deg * pi/180) 后存 innerCos/outerCos
//
// 命名规则: 与 CONSENSUS § 2.1 完全一致.

namespace {  // anonymous: 所有 Lua binding helper 仅本编译单元可见

constexpr float kPi       = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

/// 归一化 (x, y) 到单位向量; 零向量 fallback 为 (1, 0)
static inline void Normalize2D(float& x, float& y) {
    const float len2 = x * x + y * y;
    if (len2 > 1e-10f) {
        const float inv = 1.0f / std::sqrt(len2);
        x *= inv;
        y *= inv;
    } else {
        x = 1.0f;
        y = 0.0f;
    }
}

/// 从 stack[idx] 的 table 读 light 字段, 缺失字段保持 out 当前值不变
/// 字段约定 (与 Lua API 描述一致):
///   x, y         -> pos[0], pos[1]
///   dirX, dirY   -> dir[0], dir[1]  (调用方决定是否归一化)
///   color = {r, g, b}  (nested table) -> color[0..2]
///   range, intensity   -> 同名字段
///   innerAngle/outerAngle (度) -> innerCos/outerCos (本函数做一次 cosf 转换)
static void ReadLightTable(lua_State* L, int idx, Lighting2D::Light& out) {
    // x / y
    lua_getfield(L, idx, "x");
    if (!lua_isnil(L, -1)) out.pos[0] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "y");
    if (!lua_isnil(L, -1)) out.pos[1] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    // dirX / dirY (spot only; point 忽略, 但允许 update 同 slot 时设置)
    lua_getfield(L, idx, "dirX");
    if (!lua_isnil(L, -1)) out.dir[0] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "dirY");
    if (!lua_isnil(L, -1)) out.dir[1] = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    // color = nested table { r, g, b } (与 Sprite component 风格一致)
    lua_getfield(L, idx, "color");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "r");
        if (!lua_isnil(L, -1)) out.color[0] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "g");
        if (!lua_isnil(L, -1)) out.color[1] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, -1, "b");
        if (!lua_isnil(L, -1)) out.color[2] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    lua_pop(L, 1);

    // range / intensity
    lua_getfield(L, idx, "range");
    if (!lua_isnil(L, -1)) out.range = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, idx, "intensity");
    if (!lua_isnil(L, -1)) out.intensity = (float)lua_tonumber(L, -1);
    lua_pop(L, 1);

    // innerAngle / outerAngle (度) -> innerCos / outerCos
    lua_getfield(L, idx, "innerAngle");
    if (!lua_isnil(L, -1)) {
        const float deg = (float)lua_tonumber(L, -1);
        out.innerCos = std::cos(deg * kDegToRad);
    }
    lua_pop(L, 1);

    lua_getfield(L, idx, "outerAngle");
    if (!lua_isnil(L, -1)) {
        const float deg = (float)lua_tonumber(L, -1);
        out.outerCos = std::cos(deg * kDegToRad);
    }
    lua_pop(L, 1);
}

// ==================== Enabled / Ambient ====================

/// @lua_api Light.Lighting2D.SetEnabled(bool)
static int l_SetEnabled(lua_State* L) {
    Lighting2D::SetEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Lighting2D.IsEnabled() -> bool
static int l_IsEnabled(lua_State* L) {
    lua_pushboolean(L, Lighting2D::IsEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Lighting2D.SetAmbient(r, g, b)
static int l_SetAmbient(lua_State* L) {
    const float r = (float)luaL_checknumber(L, 1);
    const float g = (float)luaL_checknumber(L, 2);
    const float b = (float)luaL_checknumber(L, 3);
    Lighting2D::SetAmbient(r, g, b);
    return 0;
}

/// @lua_api Light.Lighting2D.GetAmbient() -> r, g, b
static int l_GetAmbient(lua_State* L) {
    float r, g, b;
    Lighting2D::GetAmbient(r, g, b);
    lua_pushnumber(L, r);
    lua_pushnumber(L, g);
    lua_pushnumber(L, b);
    return 3;
}

// ==================== Add / Update / Remove / Clear ====================

/// @lua_api Light.Lighting2D.AddPointLight({x, y, color={r,g,b}, range, intensity}) -> id or nil
static int l_AddPointLight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    Lighting2D::Light l;  // 默认构造 (POD 默认值)
    l.type = Lighting2D::TYPE_POINT;
    ReadLightTable(L, 1, l);
    // point 不用 dir, 但保持默认值即可

    const int id = Lighting2D::Add(l);
    if (id == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "lights full or invalid type");
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

/// @lua_api Light.Lighting2D.AddSpotLight({x, y, dirX, dirY, color, range,
///                                          innerAngle, outerAngle, intensity}) -> id or nil
static int l_AddSpotLight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    Lighting2D::Light l;
    l.type = Lighting2D::TYPE_SPOT;
    // spot 默认 innerAngle=20°, outerAngle=35° (与 DESIGN § 2.2.3 ECS 默认值一致)
    l.innerCos = std::cos(20.0f * kDegToRad);
    l.outerCos = std::cos(35.0f * kDegToRad);
    ReadLightTable(L, 1, l);
    Normalize2D(l.dir[0], l.dir[1]);  // spot 必须归一化

    const int id = Lighting2D::Add(l);
    if (id == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "lights full or invalid type");
        return 2;
    }
    lua_pushinteger(L, id);
    return 1;
}

/// @lua_api Light.Lighting2D.UpdateLight(id, fields) -> bool
/// 部分字段更新: 缺失字段保留原值. type 字段当前不允许通过 Update 切换 (Point<->Spot).
static int l_UpdateLight(lua_State* L) {
    const int id = (int)luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TTABLE);

    if (id < 1 || id > Lighting2D::MAX_LIGHTS) {
        lua_pushboolean(L, 0);
        return 1;
    }

    // 复制现 slot, 用 table 字段覆盖, 再调 C++ Update (会再做一次合法性检查)
    Lighting2D::State* st = Lighting2D::GetState();
    Lighting2D::Light fields = st->lights[id - 1];
    if (fields.type == Lighting2D::TYPE_INACTIVE) {
        lua_pushboolean(L, 0);
        return 1;
    }

    ReadLightTable(L, 2, fields);
    if (fields.type == Lighting2D::TYPE_SPOT) {
        Normalize2D(fields.dir[0], fields.dir[1]);
    }

    const bool ok = Lighting2D::Update(id, fields);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// @lua_api Light.Lighting2D.RemoveLight(id)
static int l_RemoveLight(lua_State* L) {
    const int id = (int)luaL_checkinteger(L, 1);
    Lighting2D::Remove(id);  // 幂等
    return 0;
}

/// @lua_api Light.Lighting2D.ClearLights()
static int l_ClearLights(lua_State* L) {
    (void)L;
    Lighting2D::Clear();
    return 0;
}

/// @lua_api Light.Lighting2D.GetLightCount() -> int
static int l_GetLightCount(lua_State* L) {
    lua_pushinteger(L, Lighting2D::GetCount());
    return 1;
}

/// @lua_api Light.Lighting2D.GetMaxLights() -> int (= 16)
static int l_GetMaxLights(lua_State* L) {
    lua_pushinteger(L, Lighting2D::GetMax());
    return 1;
}

// ==================== 注册表 ====================

static const luaL_Reg kLighting2DReg[] = {
    { "SetEnabled",     l_SetEnabled     },
    { "IsEnabled",      l_IsEnabled      },
    { "SetAmbient",     l_SetAmbient     },
    { "GetAmbient",     l_GetAmbient     },
    { "AddPointLight",  l_AddPointLight  },
    { "AddSpotLight",   l_AddSpotLight   },
    { "UpdateLight",    l_UpdateLight    },
    { "RemoveLight",    l_RemoveLight    },
    { "ClearLights",    l_ClearLights    },
    { "GetLightCount",  l_GetLightCount  },
    { "GetMaxLights",   l_GetMaxLights   },
    { nullptr, nullptr }
};

}  // anonymous namespace

// ==================== luaopen 入口 (LIGHT_API 导出) ====================

extern "C" LIGHT_API int luaopen_Light_Lighting2D(lua_State* L) {
    lua_newtable(L);
    for (const luaL_Reg* r = kLighting2DReg; r->name; ++r) {
        lua_pushcfunction(L, r->func);
        lua_setfield(L, -2, r->name);
    }
    // 暴露常量 (Lua 端可读)
    lua_pushinteger(L, Lighting2D::MAX_LIGHTS);
    lua_setfield(L, -2, "MAX_LIGHTS");
    lua_pushinteger(L, Lighting2D::TYPE_POINT);
    lua_setfield(L, -2, "TYPE_POINT");
    lua_pushinteger(L, Lighting2D::TYPE_SPOT);
    lua_setfield(L, -2, "TYPE_SPOT");
    return 1;
}
