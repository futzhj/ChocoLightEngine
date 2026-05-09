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
#include <string>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "cgltf.h"   // Phase AS.3 — glTF 2.0 解析 (single-header in third_party/)
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

// ==================== Phase AS.3 — cgltf glTF 加载辅助 ====================

// 解析 .gltf/.glb 文件; 失败时填充 errOut. 返回 cgltf_data* 或 nullptr.
// 调用方负责 cgltf_free(data).
static cgltf_data* GLTF_ParseAndLoad(const char* path, std::string& errOut) {
    cgltf_options options = {};  // zero-init: 默认行为
    cgltf_data* data = nullptr;

    cgltf_result r = cgltf_parse_file(&options, path, &data);
    if (r != cgltf_result_success) {
        errOut = "parse failed (cgltf err " + std::to_string((int)r) + ")";
        return nullptr;
    }
    r = cgltf_load_buffers(&options, data, path);
    if (r != cgltf_result_success) {
        errOut = "buffer load failed (cgltf err " + std::to_string((int)r) + ")";
        cgltf_free(data);
        return nullptr;
    }
    return data;
}

// 计算总 primitive 数 (跨所有 mesh)
static size_t GLTF_TotalPrimitives(const cgltf_data* data) {
    size_t total = 0;
    for (size_t i = 0; i < data->meshes_count; i++) {
        total += data->meshes[i].primitives_count;
    }
    return total;
}

// 取第 N 个 primitive (跨 mesh 计数, 0-indexed). 越界返回 nullptr.
static const cgltf_primitive* GLTF_FindPrimitive(const cgltf_data* data, size_t primIdx) {
    size_t cursor = 0;
    for (size_t i = 0; i < data->meshes_count; i++) {
        const cgltf_mesh& m = data->meshes[i];
        if (primIdx < cursor + m.primitives_count) {
            return &m.primitives[primIdx - cursor];
        }
        cursor += m.primitives_count;
    }
    return nullptr;
}

// 提取 primitive 顶点 + 索引到 RenderVertex3D + uint32 数组. 失败填充 errOut.
static bool GLTF_ExtractPrimitive(const cgltf_primitive* prim,
                                   std::vector<RenderVertex3D>& outVerts,
                                   std::vector<uint32_t>& outIndices,
                                   std::string& errOut) {
    // 找各 attribute (POSITION 必须, 其他可选)
    const cgltf_accessor* accPos   = nullptr;
    const cgltf_accessor* accNorm  = nullptr;
    const cgltf_accessor* accUV    = nullptr;
    const cgltf_accessor* accColor = nullptr;

    for (size_t a = 0; a < prim->attributes_count; a++) {
        const cgltf_attribute& attr = prim->attributes[a];
        if (attr.index != 0) continue;  // 仅取第 0 通道 (POSITION_0/NORMAL_0/...)
        switch (attr.type) {
            case cgltf_attribute_type_position: accPos   = attr.data; break;
            case cgltf_attribute_type_normal:   accNorm  = attr.data; break;
            case cgltf_attribute_type_texcoord: accUV    = attr.data; break;
            case cgltf_attribute_type_color:    accColor = attr.data; break;
            default: break;
        }
    }

    if (!accPos) {
        errOut = "primitive has no POSITION attribute";
        return false;
    }
    size_t vCount = accPos->count;
    if (vCount == 0) {
        errOut = "primitive has 0 vertices";
        return false;
    }
    if (vCount > 1000000) {
        errOut = "primitive vertex count " + std::to_string(vCount) + " exceeds 1M soft limit";
        return false;
    }

    // 提取 POSITION (vec3 必填)
    std::vector<float> posData(vCount * 3);
    cgltf_accessor_unpack_floats(accPos, posData.data(), vCount * 3);

    // NORMAL (可选, vec3)
    std::vector<float> normData;
    if (accNorm && accNorm->count == vCount && cgltf_num_components(accNorm->type) == 3) {
        normData.resize(vCount * 3);
        cgltf_accessor_unpack_floats(accNorm, normData.data(), vCount * 3);
    }

    // TEXCOORD_0 (可选, vec2)
    std::vector<float> uvData;
    if (accUV && accUV->count == vCount && cgltf_num_components(accUV->type) == 2) {
        uvData.resize(vCount * 2);
        cgltf_accessor_unpack_floats(accUV, uvData.data(), vCount * 2);
    }

    // COLOR_0 (可选, vec3 或 vec4)
    std::vector<float> colorData;
    int colorComp = 0;
    if (accColor && accColor->count == vCount) {
        size_t comp = cgltf_num_components(accColor->type);
        if (comp == 3 || comp == 4) {
            colorComp = (int)comp;
            colorData.resize(vCount * comp);
            cgltf_accessor_unpack_floats(accColor, colorData.data(), vCount * comp);
        }
    }

    // 构造 RenderVertex3D 数组
    outVerts.resize(vCount);
    for (size_t i = 0; i < vCount; i++) {
        RenderVertex3D& v = outVerts[i];
        v.x = posData[i * 3 + 0];
        v.y = posData[i * 3 + 1];
        v.z = posData[i * 3 + 2];
        if (!normData.empty()) {
            v.nx = normData[i * 3 + 0];
            v.ny = normData[i * 3 + 1];
            v.nz = normData[i * 3 + 2];
        } else {
            v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f;
        }
        if (!uvData.empty()) {
            v.u = uvData[i * 2 + 0];
            v.v = uvData[i * 2 + 1];
        } else {
            v.u = 0.0f; v.v = 0.0f;
        }
        if (!colorData.empty()) {
            v.r = colorData[i * colorComp + 0];
            v.g = colorData[i * colorComp + 1];
            v.b = colorData[i * colorComp + 2];
            v.a = (colorComp == 4) ? colorData[i * 4 + 3] : 1.0f;
        } else {
            v.r = v.g = v.b = v.a = 1.0f;
        }
    }

    // 索引: 有则提取转 uint32, 无则自动生成 (drawArray 模式)
    if (prim->indices) {
        size_t iCount = prim->indices->count;
        if (iCount > 3000000) {
            errOut = "index count " + std::to_string(iCount) + " exceeds 3M soft limit";
            return false;
        }
        outIndices.resize(iCount);
        cgltf_accessor_unpack_indices(prim->indices, outIndices.data(),
                                      sizeof(uint32_t), iCount);
    } else {
        // 自动生成 0, 1, 2, ... 序列
        outIndices.resize(vCount);
        for (size_t i = 0; i < vCount; i++) {
            outIndices[i] = (uint32_t)i;
        }
    }

    return true;
}

// ==================== Mesh.LoadGLTF(path, [primitive_index=0]) ====================
static int l_Mesh_LoadGLTF(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int primIdx = (int)luaL_optinteger(L, 2, 0);
    if (primIdx < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "primitive_index must be >= 0");
        return 2;
    }

    std::string err;
    cgltf_data* data = GLTF_ParseAndLoad(path, err);
    if (!data) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }

    size_t totalPrims = GLTF_TotalPrimitives(data);
    if ((size_t)primIdx >= totalPrims) {
        lua_pushnil(L);
        lua_pushfstring(L, "primitive index %d out of range (have %d)", primIdx, (int)totalPrims);
        cgltf_free(data);
        return 2;
    }

    const cgltf_primitive* prim = GLTF_FindPrimitive(data, (size_t)primIdx);
    std::vector<RenderVertex3D> verts;
    std::vector<uint32_t> indices;
    if (!GLTF_ExtractPrimitive(prim, verts, indices, err)) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        cgltf_free(data);
        return 2;
    }
    cgltf_free(data);

    // 调 backend 创建 GPU mesh
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
    uint32_t meshId = g_render->CreateMesh(verts.data(), (int)verts.size(),
                                            indices.data(), (int)indices.size());
    if (!meshId) {
        lua_pushnil(L);
        lua_pushstring(L, "CreateMesh failed (GPU upload error)");
        return 2;
    }

    // 创建 userdata + 元表
    MeshUserdata* ud = (MeshUserdata*)lua_newuserdata(L, sizeof(MeshUserdata));
    ud->meshId      = meshId;
    ud->vertexCount = (int)verts.size();
    ud->indexCount  = (int)indices.size();
    luaL_getmetatable(L, MESH_MT);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== Mesh.GetGLTFMeshCount(path) ====================
// 返回 .gltf/.glb 中的 primitive 总数 (跨所有 mesh). 失败返回 nil + err.
static int l_Mesh_GetGLTFMeshCount(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    std::string err;
    cgltf_data* data = GLTF_ParseAndLoad(path, err);
    if (!data) {
        lua_pushnil(L);
        lua_pushstring(L, err.c_str());
        return 2;
    }
    size_t total = GLTF_TotalPrimitives(data);
    cgltf_free(data);
    lua_pushinteger(L, (lua_Integer)total);
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
            { "New",                l_Mesh_New },
            { "GetVertexFormat",    l_Mesh_GetVertexFormat },
            { "LoadGLTF",           l_Mesh_LoadGLTF },          // Phase AS.3
            { "GetGLTFMeshCount",   l_Mesh_GetGLTFMeshCount },  // Phase AS.3
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
