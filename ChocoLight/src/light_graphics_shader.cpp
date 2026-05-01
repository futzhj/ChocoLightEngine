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
#include "render_backend.h"

// Shader userdata 元表名
static const char* SHADER_MT = "Light.Graphics.Shader";

// userdata 结构: 固定大小, 避免跨模块依赖
struct ShaderUserdata {
    uint32_t programId;   // GL program ID (0 = 无效)
};

static ShaderUserdata* CheckShader(lua_State* L, int idx) {
    return (ShaderUserdata*)luaL_checkudata(L, idx, SHADER_MT);
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

// shader:Delete() / __gc
static int l_Shader_Delete(lua_State* L) {
    ShaderUserdata* ud = CheckShader(L, 1);
    if (ud->programId && g_render) {
        g_render->DeleteShader(ud->programId);
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
            {"Use",        l_Shader_Use},
            {"SetFloat",   l_Shader_SetFloat},
            {"SetVec2",    l_Shader_SetVec2},
            {"SetVec3",    l_Shader_SetVec3},
            {"SetVec4",    l_Shader_SetVec4},
            {"SetInt",     l_Shader_SetInt},
            {"SetMat4",    l_Shader_SetMat4},
            {"Delete",     l_Shader_Delete},
            {"__gc",       l_Shader_Delete},
            {"__tostring", l_Shader_tostring},
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
