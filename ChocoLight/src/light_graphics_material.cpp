/**
 * @file light_graphics_material.cpp
 * @brief Light.Graphics.Material — PBR/Unlit 材质资源 (Phase AS.4)
 *
 * Lua API:
 *   Light.Graphics.Material.New([mode = "pbr"]) -> Material
 *     mode: "pbr" | "unlit"
 *
 *   mat:SetMode("pbr"|"unlit") / mat:GetMode() -> string
 *   mat:SetColor(r,g,b,[a=1]) / mat:GetColor() -> r,g,b,a
 *   mat:SetEmissive(r,g,b) / mat:GetEmissive() -> r,g,b
 *   mat:SetMetallic(f) / mat:GetMetallic() -> f          (0..1, PBR)
 *   mat:SetRoughness(f) / mat:GetRoughness() -> f         (0..1, PBR)
 *   mat:SetNormalScale(f) / mat:GetNormalScale() -> f
 *   mat:SetOcclusionStrength(f) / mat:GetOcclusionStrength() -> f
 *   mat:SetTexture(name, textureId) / mat:GetTexture(name) -> textureId|0
 *     name: "baseColor" | "metallicRoughness" | "normal" | "emissive" | "occlusion"
 *   mat:SetDoubleSided(bool) / mat:GetDoubleSided() -> bool
 *   mat:SetAlphaMode(s) / mat:GetAlphaMode() -> s
 *     s: "opaque" | "blend" | "mask"
 *   mat:SetAlphaCutoff(f) / mat:GetAlphaCutoff() -> f
 *   mat:Delete()                       -- POD, no-op (留接口对称性)
 *   mat:__tostring                     -- "Material(pbr, color=(1,0.5,0.2,1))"
 *
 * 内部:
 *   userdata 直接存 MaterialDesc (POD), 无需 GPU 资源, 无需 __gc.
 *   引擎在 mesh:Draw(material) 时把 desc 副本传给 backend->DrawMeshMaterial.
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7 P2.1 — magic 常量
#include "render_backend.h"

#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// userdata 元表名
static const char* MATERIAL_MT = "Light.Graphics.Material";

/**
 * Phase G.1.7 P2.1 — Material wrapper struct
 *
 * 背景: MaterialDesc 是 RenderBackend 核心 POD struct, 定义在 render_backend.h,
 * 改其 layout 会破坏整个引擎 ABI. 但 G.1.7 安全审计要求所有 Lua-facing
 * userdata 都有 magic 防御.
 *
 * 解决: 包一层 wrapper, userdata 外包是 MaterialUserdata, 内部嵌 MaterialDesc.
 * 所有 Check 函数返回 &ud->desc, 调用点透明 (不需修改 RenderBackend).
 */
struct MaterialUserdata {
    uint32_t     magic;   // = LT_MAGIC_MATERIAL
    MaterialDesc desc;
};

// 模式字符串 -> 数值
static int ParseMode(const char* s) {
    if (!s) return 1;  // PBR 默认
    if (strcmp(s, "unlit") == 0) return 0;
    return 1;  // 其他都视为 pbr
}
static const char* ModeString(int mode) {
    return (mode == 0) ? "unlit" : "pbr";
}

// alphaMode 字符串 <-> 数值
static int ParseAlphaMode(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "blend") == 0) return 1;
    if (strcmp(s, "mask") == 0) return 2;
    return 0;  // opaque
}
static const char* AlphaModeString(int m) {
    switch (m) {
        case 1: return "blend";
        case 2: return "mask";
        default: return "opaque";
    }
}

// 把一个 MaterialDesc 设为默认值
static void InitDefault(MaterialDesc* d, int mode) {
    memset(d, 0, sizeof(*d));
    d->mode = mode;
    d->color[0] = d->color[1] = d->color[2] = d->color[3] = 1.0f;
    d->emissive[0] = d->emissive[1] = d->emissive[2] = 0.0f;
    d->metallic = 0.0f;
    d->roughness = 1.0f;
    d->normalScale = 1.0f;
    d->occlusionStrength = 1.0f;
    d->texBaseColor = 0;
    d->texMetallicRoughness = 0;
    d->texNormal = 0;
    d->texEmissive = 0;
    d->texOcclusion = 0;
    d->alphaMode = 0;
    d->alphaCutoff = 0.5f;
    d->doubleSided = 0;
}

// Phase G.1.7 P2.1: 双保险 (metatable + magic)
static MaterialDesc* CheckMaterial(lua_State* L, int idx) {
    auto* ud = (MaterialUserdata*)luaL_checkudata(L, idx, MATERIAL_MT);
    if (ud && ud->magic != LT::LT_MAGIC_MATERIAL) {
        luaL_error(L, "Light.Graphics.Material: type confusion at arg #%d", idx);
        return nullptr;
    }
    return ud ? &ud->desc : nullptr;
}

// ==================== Material.New([mode]) ====================
static int l_Material_New(lua_State* L) {
    const char* modeStr = luaL_optstring(L, 1, "pbr");
    int mode = ParseMode(modeStr);

    auto* ud = (MaterialUserdata*)lua_newuserdata(L, sizeof(MaterialUserdata));
    ud->magic = LT::LT_MAGIC_MATERIAL;
    InitDefault(&ud->desc, mode);
    luaL_getmetatable(L, MATERIAL_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== mode ====================
static int l_Material_SetMode(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    const char* s = luaL_checkstring(L, 2);
    d->mode = ParseMode(s);
    return 0;
}
static int l_Material_GetMode(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushstring(L, ModeString(d->mode));
    return 1;
}

// ==================== color (baseColor) ====================
static int l_Material_SetColor(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->color[0] = (float)luaL_checknumber(L, 2);
    d->color[1] = (float)luaL_checknumber(L, 3);
    d->color[2] = (float)luaL_checknumber(L, 4);
    d->color[3] = (float)luaL_optnumber(L, 5, 1.0);
    return 0;
}
static int l_Material_GetColor(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->color[0]);
    lua_pushnumber(L, d->color[1]);
    lua_pushnumber(L, d->color[2]);
    lua_pushnumber(L, d->color[3]);
    return 4;
}

// ==================== emissive ====================
static int l_Material_SetEmissive(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->emissive[0] = (float)luaL_checknumber(L, 2);
    d->emissive[1] = (float)luaL_checknumber(L, 3);
    d->emissive[2] = (float)luaL_checknumber(L, 4);
    return 0;
}
static int l_Material_GetEmissive(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->emissive[0]);
    lua_pushnumber(L, d->emissive[1]);
    lua_pushnumber(L, d->emissive[2]);
    return 3;
}

// ==================== metallic / roughness / normalScale / occlusionStrength ====================

// clamp 到 [0,1]
static float Clamp01(float v) {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

static int l_Material_SetMetallic(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->metallic = Clamp01((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Material_GetMetallic(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->metallic);
    return 1;
}
static int l_Material_SetRoughness(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->roughness = Clamp01((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Material_GetRoughness(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->roughness);
    return 1;
}
static int l_Material_SetNormalScale(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->normalScale = (float)luaL_checknumber(L, 2);
    return 0;
}
static int l_Material_GetNormalScale(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->normalScale);
    return 1;
}
static int l_Material_SetOcclusionStrength(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->occlusionStrength = Clamp01((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Material_GetOcclusionStrength(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->occlusionStrength);
    return 1;
}

// ==================== textures ====================
// 把纹理 slot 名映射到 MaterialDesc 字段指针
static uint32_t* TexSlotPtr(MaterialDesc* d, const char* name) {
    if (!name) return nullptr;
    if (strcmp(name, "baseColor") == 0)         return &d->texBaseColor;
    if (strcmp(name, "metallicRoughness") == 0) return &d->texMetallicRoughness;
    if (strcmp(name, "normal") == 0)            return &d->texNormal;
    if (strcmp(name, "emissive") == 0)          return &d->texEmissive;
    if (strcmp(name, "occlusion") == 0)         return &d->texOcclusion;
    return nullptr;
}

static int l_Material_SetTexture(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    const char* name = luaL_checkstring(L, 2);
    uint32_t texId = (uint32_t)luaL_optinteger(L, 3, 0);
    uint32_t* slot = TexSlotPtr(d, name);
    if (!slot) {
        return luaL_error(L, "unknown texture slot '%s' (valid: baseColor, metallicRoughness, normal, emissive, occlusion)", name);
    }
    *slot = texId;
    return 0;
}
static int l_Material_GetTexture(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    const char* name = luaL_checkstring(L, 2);
    uint32_t* slot = TexSlotPtr(d, name);
    if (!slot) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)*slot);
    return 1;
}

// ==================== doubleSided / alphaMode / alphaCutoff ====================
static int l_Material_SetDoubleSided(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->doubleSided = lua_toboolean(L, 2) ? 1 : 0;
    return 0;
}
static int l_Material_GetDoubleSided(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushboolean(L, d->doubleSided ? 1 : 0);
    return 1;
}
static int l_Material_SetAlphaMode(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    const char* s = luaL_checkstring(L, 2);
    d->alphaMode = ParseAlphaMode(s);
    return 0;
}
static int l_Material_GetAlphaMode(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushstring(L, AlphaModeString(d->alphaMode));
    return 1;
}
static int l_Material_SetAlphaCutoff(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    d->alphaCutoff = Clamp01((float)luaL_checknumber(L, 2));
    return 0;
}
static int l_Material_GetAlphaCutoff(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushnumber(L, d->alphaCutoff);
    return 1;
}

// ==================== Delete / __gc / __tostring ====================
static int l_Material_Delete(lua_State* L) {
    // POD, 无需释放 GPU 资源 (留接口对称性, no-op)
    (void)L;
    return 0;
}

static int l_Material_Tostring(lua_State* L) {
    MaterialDesc* d = CheckMaterial(L, 1);
    lua_pushfstring(L, "Light.Graphics.Material(%s, color=(%.2f,%.2f,%.2f,%.2f))",
                    ModeString(d->mode),
                    d->color[0], d->color[1], d->color[2], d->color[3]);
    return 1;
}

// ==================== Module registration ====================

extern "C" LIGHT_API int luaopen_Light_Graphics_Material(lua_State* L) {
    // 注册 userdata 元表
    if (luaL_newmetatable(L, MATERIAL_MT)) {
        static const luaL_Reg methods[] = {
            { "SetMode",                l_Material_SetMode },
            { "GetMode",                l_Material_GetMode },
            { "SetColor",               l_Material_SetColor },
            { "GetColor",               l_Material_GetColor },
            { "SetEmissive",            l_Material_SetEmissive },
            { "GetEmissive",            l_Material_GetEmissive },
            { "SetMetallic",            l_Material_SetMetallic },
            { "GetMetallic",            l_Material_GetMetallic },
            { "SetRoughness",           l_Material_SetRoughness },
            { "GetRoughness",           l_Material_GetRoughness },
            { "SetNormalScale",         l_Material_SetNormalScale },
            { "GetNormalScale",         l_Material_GetNormalScale },
            { "SetOcclusionStrength",   l_Material_SetOcclusionStrength },
            { "GetOcclusionStrength",   l_Material_GetOcclusionStrength },
            { "SetTexture",             l_Material_SetTexture },
            { "GetTexture",             l_Material_GetTexture },
            { "SetDoubleSided",         l_Material_SetDoubleSided },
            { "GetDoubleSided",         l_Material_GetDoubleSided },
            { "SetAlphaMode",           l_Material_SetAlphaMode },
            { "GetAlphaMode",           l_Material_GetAlphaMode },
            { "SetAlphaCutoff",         l_Material_SetAlphaCutoff },
            { "GetAlphaCutoff",         l_Material_GetAlphaCutoff },
            { "Delete",                 l_Material_Delete },
            { "__gc",                   l_Material_Delete },
            { "__tostring",             l_Material_Tostring },
            { nullptr, nullptr },
        };
        luaL_setfuncs(L, methods, 0);
        // __index = self
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    // 注册 Light.Graphics.Material 模块表 (含 New 静态方法)
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        // Graphics 不存在, 创建空表
        lua_pop(L, 1);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, "Material");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        // 创建 Light.Graphics.Material 表
        lua_pop(L, 1);
        lua_pushstring(L, "Material");
        lua_createtable(L, 0, 1);
        static const luaL_Reg material_funcs[] = {
            { "New", l_Material_New },
            { nullptr, nullptr },
        };
        luaL_setfuncs(L, material_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Material");
        lua_rawget(L, -2);
    }
    // 栈: [Light, Graphics, Material]
    lua_remove(L, -2);  // 移除 Graphics
    lua_remove(L, -2);  // 移除 Light
    return 1;
}

// ==================== 内部 C++ helper (供 light_graphics_mesh.cpp 用) ====================

// Phase G.1.7 P2.1: 公开 helper 给 light_graphics_mesh.cpp 等调用.
// 返回只读 MaterialDesc 指针 (wrapper 透明), 失败返 nullptr (不抛错以保持兑心 API).
extern "C" const MaterialDesc* CheckMaterialUserdata(lua_State* L, int idx) {
    if (lua_type(L, idx) != LUA_TUSERDATA) return nullptr;
    auto* ud = (MaterialUserdata*)luaL_testudata(L, idx, MATERIAL_MT);
    if (!ud) return nullptr;
    if (ud->magic != LT::LT_MAGIC_MATERIAL) return nullptr;  // magic 拒绝 type-confusion
    return &ud->desc;
}

// Phase G.1.7 P2.1: 供 light_graphics_mesh.cpp 等需要 *创建* Material userdata 的地方调用.
// 返回可写的 MaterialDesc* (在 wrapper 内), 调用者 memcpy / 赋值 均可.
// userdata 已 push 到栈顶, 调用者负责当作返值 (或按需处理栈).
extern "C" MaterialDesc* PushNewMaterialUserdata(lua_State* L) {
    auto* ud = (MaterialUserdata*)lua_newuserdata(L, sizeof(MaterialUserdata));
    ud->magic = LT::LT_MAGIC_MATERIAL;
    memset(&ud->desc, 0, sizeof(MaterialDesc));
    luaL_getmetatable(L, MATERIAL_MT);
    lua_setmetatable(L, -2);
    return &ud->desc;
}
