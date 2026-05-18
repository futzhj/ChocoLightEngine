/**
 * @file light_graphics_shader.cpp
 * @brief Light.Graphics.Shader 模块 — 用户自定义 Shader 支持
 *
 * 仅在 GL33 后端可用 (Legacy 返回失败)
 *
 * 约定:
 *   - 顶点属性位置: aPos(0), aTexCoord(1), aColor(2)  — 与引擎 VAO 一致
 *   - 内置 uniform: uMVP (mat4, 引擎自动上传)
 *   - 用户可自由添加其它 uniform (time/resolution 等)
 *
 * Lua API:
 *   local shader = Light.Graphics.Shader.New(vsSrc, fsSrc)   -- 失败返回 nil, errLog
 *   shader:Use()                                             -- 激活
 *   shader:SetFloat("uTime", 1.5)
 *   shader:SetVec2("uRes", 800, 600)
 *   shader:SetVec3("uColor", 1, 0.5, 0.2)
 *   shader:SetVec4("uTint", 1, 1, 1, 0.8)
 *   shader:SetInt("uMode", 2)
 *   shader:SetMat4("uMat", {m00, m01, ..., m33})
 *   Light.Graphics.Shader.UseDefault()                       -- 恢复引擎默认
 *   shader:Delete()                                          -- 主动释放
 *   Light.Graphics.Shader.IsSupported()                      -- 查询是否支持
 */

#include "light.h"
#include "light_lua_helpers.h"  // Phase G.1.7.1 — 类型安全 helpers + magic
#include "render_backend.h"

// Shader userdata 元表名
static const char* SHADER_MT = "Light.Graphics.Shader";

/// Phase G.1.7.1: 首字段 magic 防止 type-confusion (双保险: metatable 名 + magic)
struct ShaderUserdata {
    uint32_t magic;       // 必须 = LT_MAGIC_SHADER (首字段)
    uint32_t programId;   // GL program ID (0 = 无效)
};

/// Phase G.1.7.1: magic 双保险
static ShaderUserdata* CheckShader(lua_State* L, int idx) {
    auto* ud = (ShaderUserdata*)luaL_checkudata(L, idx, SHADER_MT);
    if (ud && ud->magic != LT::LT_MAGIC_SHADER) {
        luaL_error(L, "Light.Graphics.Shader: type confusion at arg #%d (magic mismatch)", idx);
    }
    return ud;
}

// ==================== Shader.New(vsSrc, fsSrc) ====================
// 成功: 返回 shader userdata
// 失败: 返回 nil, errLog
static int l_Shader_New(lua_State* L) {
    const char* vs = luaL_checkstring(L, 1);
    const char* fs = luaL_checkstring(L, 2);

    if (!g_render || !g_render->SupportsShaders()) {
        lua_pushnil(L);
        lua_pushstring(L, "Render backend does not support user shaders (need GL 3.3+)");
        return 2;
    }

    char errLog[1024] = {0};
    uint32_t prog = g_render->CreateShader(vs, fs, errLog, (int)sizeof(errLog));
    if (!prog) {
        lua_pushnil(L);
        lua_pushstring(L, errLog[0] ? errLog : "shader compile/link failed");
        return 2;
    }

    // 创建 userdata
    ShaderUserdata* ud = (ShaderUserdata*)lua_newuserdata(L, sizeof(ShaderUserdata));
    ud->magic     = LT::LT_MAGIC_SHADER;  // Phase G.1.7.1 — type tag
    ud->programId = prog;
    luaL_getmetatable(L, SHADER_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// shader:Use()
static int l_Shader_Use(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    if (!g_render || !ud->programId) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, g_render->UseShader(ud->programId) ? 1 : 0);
    return 1;
}

// Shader.UseDefault()
static int l_Shader_UseDefault(lua_State* L) {
    if (g_render) g_render->UseDefaultShader();
    return 0;
}

// Shader.IsSupported()
static int l_Shader_IsSupported(lua_State* L) {
    lua_pushboolean(L, (g_render && g_render->SupportsShaders()) ? 1 : 0);
    return 1;
}

// 辅助: 获取 uniform location (失败返回 -1)
static int GetUniformLoc(lua_State* L, ShaderUserdata* ud, const char* name) {
    if (!g_render || !ud->programId) return -1;
    return g_render->GetUniformLocation(ud->programId, name);
}

// shader:SetFloat(name, v)
static int l_Shader_SetFloat(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float v = (float)luaL_checknumber(L, 3);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform1f(loc, v);
    return 0;
}

// shader:SetVec2(name, x, y)
static int l_Shader_SetVec2(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float x = (float)luaL_checknumber(L, 3);
    float y = (float)luaL_checknumber(L, 4);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform2f(loc, x, y);
    return 0;
}

// shader:SetVec3(name, x, y, z)
static int l_Shader_SetVec3(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float x = (float)luaL_checknumber(L, 3);
    float y = (float)luaL_checknumber(L, 4);
    float z = (float)luaL_checknumber(L, 5);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform3f(loc, x, y, z);
    return 0;
}

// shader:SetVec4(name, x, y, z, w)
static int l_Shader_SetVec4(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    float x = (float)luaL_checknumber(L, 3);
    float y = (float)luaL_checknumber(L, 4);
    float z = (float)luaL_checknumber(L, 5);
    float w = (float)luaL_checknumber(L, 6);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform4f(loc, x, y, z, w);
    return 0;
}

// shader:SetInt(name, v)
static int l_Shader_SetInt(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int v = (int)luaL_checkinteger(L, 3);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform1i(loc, v);
    return 0;
}

// shader:SetMat4(name, {m00, m01, ..., m33})  // 16 个 float
static int l_Shader_SetMat4(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    float m[16];
    for (int i = 0; i < 16; i++) {
        lua_rawgeti(L, 3, i + 1);
        m[i] = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
    }
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniformMat4(loc, m);
    return 0;
}

// ==================== Phase AS.1 — Shader uniform 扩展 (6 setter) ====================

// shader:SetMat3(name, {m00, m01, m02, m10, m11, m12, m20, m21, m22})  // 9 个 float
static int l_Shader_SetMat3(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    float m[9];
    for (int i = 0; i < 9; i++) {
        lua_rawgeti(L, 3, i + 1);
        m[i] = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
    }
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniformMat3(loc, m);
    return 0;
}

// shader:SetIVec2(name, x, y)
static int l_Shader_SetIVec2(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int x = (int)luaL_checkinteger(L, 3);
    int y = (int)luaL_checkinteger(L, 4);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform2i(loc, x, y);
    return 0;
}

// shader:SetIVec3(name, x, y, z)
static int l_Shader_SetIVec3(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int x = (int)luaL_checkinteger(L, 3);
    int y = (int)luaL_checkinteger(L, 4);
    int z = (int)luaL_checkinteger(L, 5);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform3i(loc, x, y, z);
    return 0;
}

// shader:SetIVec4(name, x, y, z, w)
static int l_Shader_SetIVec4(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    int x = (int)luaL_checkinteger(L, 3);
    int y = (int)luaL_checkinteger(L, 4);
    int z = (int)luaL_checkinteger(L, 5);
    int w = (int)luaL_checkinteger(L, 6);
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform4i(loc, x, y, z, w);
    return 0;
}

// shader:SetFloatArray(name, {v1, v2, ...})
// 软限制: 单次最多 256 个元素 (避免 Lua 端误传巨大表导致拷贝爆栈)
static int l_Shader_SetFloatArray(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    int count = (int)lua_objlen(L, 3);
    if (count <= 0) return 0;
    if (count > 256) {
        CC::Log(CC::LOG_WARN, "Shader:SetFloatArray(%s): count=%d exceeds 256, truncated", name, count);
        count = 256;
    }
    float values[256];
    for (int i = 0; i < count; i++) {
        lua_rawgeti(L, 3, i + 1);
        values[i] = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
    }
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform1fv(loc, count, values);
    return 0;
}

// shader:SetVec2Array(name, {x1, y1, x2, y2, ...})
// 平铺存储 (2 个 lua 数 = 1 个 vec2), table 长度必须为偶数
// 软限制: 最多 256 个 vec2 (即 512 个 lua 数)
static int l_Shader_SetVec2Array(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TTABLE);

    int total = (int)lua_objlen(L, 3);
    if (total <= 0 || (total % 2) != 0) {
        CC::Log(CC::LOG_WARN, "Shader:SetVec2Array(%s): table length %d not multiple of 2", name, total);
        return 0;
    }
    if (total > 512) {
        CC::Log(CC::LOG_WARN, "Shader:SetVec2Array(%s): %d floats exceeds 512, truncated", name, total);
        total = 512;
    }
    int count = total / 2;
    float values[512];
    for (int i = 0; i < total; i++) {
        lua_rawgeti(L, 3, i + 1);
        values[i] = (float)luaL_optnumber(L, -1, 0.0);
        lua_pop(L, 1);
    }
    int loc = GetUniformLoc(L, ud, name);
    if (loc >= 0) g_render->SetUniform2fv(loc, count, values);
    return 0;
}

// shader:SetTexture(name, tex_id, [slot])
//   tex_id: number (来自 canvas:GetTextureId() 或 image:GetTextureId())
//   slot:   可选, 默认 1 (slot=0 引擎专用)
static int l_Shader_SetTexture(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    const char* name = luaL_checkstring(L, 2);
    uint32_t texId = (uint32_t)luaL_checkinteger(L, 3);
    int slot = (int)luaL_optinteger(L, 4, 1);

    if (!g_render || !ud->programId || !texId) return 0;
    int loc = g_render->GetUniformLocation(ud->programId, name);
    if (loc < 0) return 0;

    g_render->SetUniformSampler(loc, slot, texId);
    return 0;
}

// shader:Delete() / __gc
// Phase G.1.7.1: __gc 后 设 magic = DEAD, 防 use-after-free
static int l_Shader_Delete(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    if (ud->programId && g_render) {
        g_render->DeleteShader(ud->programId);
        ud->magic = LT::LT_MAGIC_DEAD;  // Phase G.1.7.1 — 释放后不可再访问
        ud->programId = 0;
    }
    return 0;
}

// __tostring
static int l_Shader_tostring(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    lua_pushfstring(L, "Light.Graphics.Shader(id=%d)", (int)ud->programId);
    return 1;
}

// ==================== luaopen_Light_Graphics_Shader ====================

// 复用 light_graphics_spriteanimation 的 EnsureGraphicsTable 模式
static void EnsureGraphicsTableForShader(lua_State* L) {
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
}

int luaopen_Light_Graphics_Shader(lua_State* L) {
    // 注册 userdata 元表
    if (luaL_newmetatable(L, SHADER_MT)) {
        static const luaL_Reg methods[] = {
            {"Use",           l_Shader_Use},
            {"SetFloat",      l_Shader_SetFloat},
            {"SetVec2",       l_Shader_SetVec2},
            {"SetVec3",       l_Shader_SetVec3},
            {"SetVec4",       l_Shader_SetVec4},
            {"SetInt",        l_Shader_SetInt},
            {"SetMat4",       l_Shader_SetMat4},
            // Phase AS.1 — 6 个新 setter
            {"SetMat3",       l_Shader_SetMat3},
            {"SetIVec2",      l_Shader_SetIVec2},
            {"SetIVec3",      l_Shader_SetIVec3},
            {"SetIVec4",      l_Shader_SetIVec4},
            {"SetFloatArray", l_Shader_SetFloatArray},
            {"SetVec2Array",  l_Shader_SetVec2Array},
            {"SetTexture",    l_Shader_SetTexture},
            {"Delete",        l_Shader_Delete},
            {"__gc",          l_Shader_Delete},
            {"__tostring",    l_Shader_tostring},
            {nullptr, nullptr}
        };
        luaL_setfuncs(L, methods, 0);
        // __index = 自身 (让实例可调用元表方法)
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);  // 弹出元表

    // 注册 Light.Graphics.Shader = { New, UseDefault, IsSupported }
    EnsureGraphicsTableForShader(L);

    lua_pushstring(L, "Shader");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Shader");
        lua_createtable(L, 0, 3);
        static const luaL_Reg shader_funcs[] = {
            {"New",         l_Shader_New},
            {"UseDefault",  l_Shader_UseDefault},
            {"IsSupported", l_Shader_IsSupported},
            {nullptr, nullptr}
        };
        luaL_setfuncs(L, shader_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Shader");
        lua_rawget(L, -2);
    }

    lua_remove(L, -2);  // 移除 Graphics 表, 保留 Shader
    return 1;
}
