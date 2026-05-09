/**
 * @file light_graphics_mesh.cpp
 * @brief Light.Graphics.Mesh — 3D mesh userdata (Phase AS.2)
 *
 * Lua API:
 *   Light.Graphics.Mesh.New(vertices, indices) -> Mesh|nil
 *     vertices: flat float table, 12 floats/vertex (pos.x,y,z, normal.x,y,z, uv.x,y, color.r,g,b,a)
 *     indices:  int table (1-indexed values, 0-based stored internally)
 *     失败返回 nil + err string
 *   Light.Graphics.Mesh.GetVertexFormat() -> "pos3, normal3, uv2, color4"
 *
 *   mesh:Draw([textureId])
 *   mesh:GetVertexCount() -> int
 *   mesh:GetIndexCount()  -> int
 *   mesh:Delete()         -- 也通过 __gc 自动调用
 *
 * 内部:
 *   userdata 存 mesh GPU id (uint32) + 顶点/索引计数 (用于 GetVertexCount/GetIndexCount).
 *   __gc 调 g_render->DeleteMesh 释放 GPU 资源.
 */

#include "light.h"
#include "render_backend.h"

#include <vector>
#include <cstring>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

// 每顶点 floats 数 (固定 12 = pos3+normal3+uv2+color4)
static constexpr int FLOATS_PER_VERTEX = 12;

// userdata 元表名
static const char* MESH_MT = "Light.Graphics.Mesh";

struct MeshUserdata {
    uint32_t meshId;       // RenderBackend 返回的 mesh id (0 = invalid)
    int      vertexCount;  // 顶点数 (用于 GetVertexCount)
    int      indexCount;   // 索引数
};

static MeshUserdata* CheckMesh(lua_State* L, int idx) {
    return (MeshUserdata*)luaL_checkudata(L, idx, MESH_MT);
}

// ==================== Mesh.New(vertices, indices) ====================
// 失败: 返回 nil + err
static int l_Mesh_New(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checktype(L, 2, LUA_TTABLE);

    int vertexFloats = (int)lua_objlen(L, 1);
    int indexCount   = (int)lua_objlen(L, 2);

    if (vertexFloats <= 0 || (vertexFloats % FLOATS_PER_VERTEX) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "vertex table length %d not multiple of %d (pos3+normal3+uv2+color4)",
                        vertexFloats, FLOATS_PER_VERTEX);
        return 2;
    }
    if (indexCount <= 0 || (indexCount % 3) != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "index count %d must be > 0 and multiple of 3 (triangles)", indexCount);
        return 2;
    }

    int vertexCount = vertexFloats / FLOATS_PER_VERTEX;

    // 软上限保护 (避免超大 table 拖垮 Lua 栈)
    if (vertexCount > 1000000) {
        lua_pushnil(L);
        lua_pushfstring(L, "vertex count %d exceeds 1M soft limit", vertexCount);
        return 2;
    }
    if (indexCount > 3000000) {
        lua_pushnil(L);
        lua_pushfstring(L, "index count %d exceeds 3M soft limit", indexCount);
        return 2;
    }

    // 提取顶点数据到 RenderVertex3D 数组
    std::vector<RenderVertex3D> verts(vertexCount);
    for (int i = 0; i < vertexCount; i++) {
        int base = i * FLOATS_PER_VERTEX;
        RenderVertex3D& v = verts[i];
        // 12 floats: pos3 + normal3 + uv2 + color4
        for (int j = 0; j < FLOATS_PER_VERTEX; j++) {
            lua_rawgeti(L, 1, base + j + 1);
        }
        v.x  = (float)luaL_optnumber(L, -12, 0.0);
        v.y  = (float)luaL_optnumber(L, -11, 0.0);
        v.z  = (float)luaL_optnumber(L, -10, 0.0);
        v.nx = (float)luaL_optnumber(L,  -9, 0.0);
        v.ny = (float)luaL_optnumber(L,  -8, 1.0);  // 默认法线 +y
        v.nz = (float)luaL_optnumber(L,  -7, 0.0);
        v.u  = (float)luaL_optnumber(L,  -6, 0.0);
        v.v  = (float)luaL_optnumber(L,  -5, 0.0);
        v.r  = (float)luaL_optnumber(L,  -4, 1.0);
        v.g  = (float)luaL_optnumber(L,  -3, 1.0);
        v.b  = (float)luaL_optnumber(L,  -2, 1.0);
        v.a  = (float)luaL_optnumber(L,  -1, 1.0);
        lua_pop(L, FLOATS_PER_VERTEX);
    }

    // 提取索引数据 (Lua 端 1-indexed, 转为 0-indexed 给 GL)
    std::vector<uint32_t> indices(indexCount);
    for (int i = 0; i < indexCount; i++) {
        lua_rawgeti(L, 2, i + 1);
        lua_Integer idx = luaL_optinteger(L, -1, 0);
        // 转为 0-indexed
        idx--;
        if (idx < 0) idx = 0;
        if (idx >= vertexCount) idx = vertexCount - 1;
        indices[i] = (uint32_t)idx;
        lua_pop(L, 1);
    }

    // 调 RenderBackend 创建 GPU mesh
    if (!g_render) {
        lua_pushnil(L);
        lua_pushstring(L, "no render backend (window not opened?)");
        return 2;
    }
    if (!g_render->Supports3D()) {
        lua_pushnil(L);
        lua_pushstring(L, "render backend does not support 3D mesh (need GL 3.3+)");
        return 2;
    }

    uint32_t meshId = g_render->CreateMesh(verts.data(), vertexCount,
                                            indices.data(), indexCount);
    if (!meshId) {
        lua_pushnil(L);
        lua_pushstring(L, "CreateMesh failed (GPU upload error)");
        return 2;
    }

    // 创建 userdata + 元表
    MeshUserdata* ud = (MeshUserdata*)lua_newuserdata(L, sizeof(MeshUserdata));
    ud->meshId      = meshId;
    ud->vertexCount = vertexCount;
    ud->indexCount  = indexCount;
    luaL_getmetatable(L, MESH_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== Mesh.GetVertexFormat() ====================
static int l_Mesh_GetVertexFormat(lua_State* L) {
    lua_pushstring(L, "pos3, normal3, uv2, color4");
    return 1;
}

// ==================== mesh:Draw([textureId]) ====================
static int l_Mesh_Draw(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    uint32_t texId = (uint32_t)luaL_optinteger(L, 2, 0);
    if (g_render && ud->meshId) {
        g_render->DrawMesh(ud->meshId, texId);
    }
    return 0;
}

// ==================== mesh:GetVertexCount() ====================
static int l_Mesh_GetVertexCount(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    lua_pushinteger(L, ud->vertexCount);
    return 1;
}

// ==================== mesh:GetIndexCount() ====================
static int l_Mesh_GetIndexCount(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    lua_pushinteger(L, ud->indexCount);
    return 1;
}

// ==================== mesh:Delete() / __gc ====================
static int l_Mesh_Delete(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    if (ud->meshId && g_render) {
        g_render->DeleteMesh(ud->meshId);
        ud->meshId = 0;
    }
    return 0;
}

// ==================== mesh:__tostring ====================
static int l_Mesh_Tostring(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    lua_pushfstring(L, "Light.Graphics.Mesh(id=%d, verts=%d, idx=%d)",
                    (int)ud->meshId, ud->vertexCount, ud->indexCount);
    return 1;
}

// ==================== Module registration ====================

extern "C" LIGHT_API int luaopen_Light_Graphics_Mesh(lua_State* L) {
    // 注册 userdata 元表
    if (luaL_newmetatable(L, MESH_MT)) {
        static const luaL_Reg methods[] = {
            { "Draw",            l_Mesh_Draw },
            { "GetVertexCount",  l_Mesh_GetVertexCount },
            { "GetIndexCount",   l_Mesh_GetIndexCount },
            { "Delete",          l_Mesh_Delete },
            { "__gc",            l_Mesh_Delete },
            { "__tostring",      l_Mesh_Tostring },
            { nullptr, nullptr },
        };
        luaL_setfuncs(L, methods, 0);
        // __index = self (让实例方法可被调用)
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);

    // 注册 Light.Graphics.Mesh 模块表 (含 New + GetVertexFormat 静态方法)
    LT::EnsureLightTable(L);
    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        // Graphics 不存在, 创建空表 (luaopen_Light_Graphics 不会被自动调用)
        lua_pop(L, 1);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, "Mesh");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        // 创建 Light.Graphics.Mesh 表
        lua_pop(L, 1);
        lua_pushstring(L, "Mesh");
        lua_createtable(L, 0, 2);
        static const luaL_Reg mesh_funcs[] = {
            { "New",             l_Mesh_New },
            { "GetVertexFormat", l_Mesh_GetVertexFormat },
            { nullptr, nullptr },
        };
        luaL_setfuncs(L, mesh_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Mesh");
        lua_rawget(L, -2);
    }
    // 栈: [Light, Graphics, Mesh]
    lua_remove(L, -2);  // 移除 Graphics
    lua_remove(L, -2);  // 移除 Light, 留 Mesh
    return 1;
}
