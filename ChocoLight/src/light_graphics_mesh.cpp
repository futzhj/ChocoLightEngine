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
#include "light_lua_helpers.h"  // Phase G.1.7.1 — 类型安全 helpers + magic
#include "render_backend.h"
#include "asset_loader.h"

#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "cgltf.h"   // Phase AS.3 — glTF 2.0 解析 (single-header in third_party/)
#include "stb_image.h"  // Phase AS.4.x — 解码 PNG/JPG/etc. 到 RGBA pixels
}

// 前向声明: 创建 Material userdata (Phase AS.4.x glTF 材质提取)
extern "C" int luaopen_Light_Graphics_Material(lua_State* L);

// 每顶点 floats 数 (固定 12 = pos3+normal3+uv2+color4)
static constexpr int FLOATS_PER_VERTEX = 12;

// userdata 元表名
static const char* MESH_MT = "Light.Graphics.Mesh";

/// Phase G.1.7.1: 首字段 magic 防止 type-confusion (双保险: metatable 名 + magic)
struct MeshUserdata {
    uint32_t magic;        // 必须 = LT_MAGIC_MESH (首字段)
    uint32_t meshId;       // RenderBackend 返回的 mesh id (0 = invalid)
    int      vertexCount;  // 顶点数 (用于 GetVertexCount)
    int      indexCount;   // 索引数
};

/// Phase G.1.7.1: magic 双保险
static MeshUserdata* CheckMesh(lua_State* L, int idx) {
    auto* ud = (MeshUserdata*)luaL_checkudata(L, idx, MESH_MT);
    if (ud && ud->magic != LT::LT_MAGIC_MESH) {
        luaL_error(L, "Light.Graphics.Mesh: type confusion at arg #%d (magic mismatch)", idx);
    }
    return ud;
}

static bool ReadMat4FromTable(lua_State* L, int idx, float* outMat) {
    if (lua_type(L, idx) != LUA_TTABLE) return false;
    for (int i = 0; i < 16; ++i) {
        lua_rawgeti(L, idx, i + 1);
        if (lua_type(L, -1) != LUA_TNUMBER) {
            lua_pop(L, 1);
            return false;
        }
        outMat[i] = (float)lua_tonumber(L, -1);
        lua_pop(L, 1);
    }
    return true;
}

static bool TrySetPreviousModelMatrix(lua_State* L, int idx) {
    if (!g_render || lua_type(L, idx) != LUA_TTABLE) return false;
    float prevModel[16];
    if (!ReadMat4FromTable(L, idx, prevModel)) {
        luaL_error(L, "previous model matrix must be a 16-element table");
        return false;
    }
    g_render->SetNextPreviousModelMatrix(prevModel);
    return true;
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
    ud->magic       = LT::LT_MAGIC_MESH;  // Phase G.1.7.1 — type tag
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

// ==================== Phase AS.4.x — glTF 材质提取 ====================

// userdata 元表名 (与 light_graphics_material.cpp 一致, 用于创建 Material userdata)
// Phase G.1.7 P2.1: 保留供调试参考, 实际创建走 PushNewMaterialUserdata helper
[[maybe_unused]] static const char* MATERIAL_MT_NAME = "Light.Graphics.Material";

// Phase G.1.7 P2.1 — forward decl: Material userdata wrapper helpers (实现在 light_graphics_material.cpp)
extern "C" const MaterialDesc* CheckMaterialUserdata(lua_State* L, int idx);
extern "C" MaterialDesc*       PushNewMaterialUserdata(lua_State* L);

// 取 .gltf 文件所在目录 (含尾部分隔符), 用于解析相对纹理 URI
static std::string GetGLTFDirectory(const char* gltfPath) {
    if (!gltfPath) return "";
    std::string p = gltfPath;
    // 找最后一个 '/' 或 '\\'
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return p.substr(0, pos + 1);  // 含分隔符
}

// 加载 glTF 中的一张 image, 返回 GPU texId (0=失败). 不让 LoadGLTF 整体失败.
//
// 来源优先级:
//   1. image->buffer_view 非空 -> embedded GLB chunk 数据
//   2. image->uri 是 "data:" URI -> base64 解码
//   3. image->uri 是文件路径 -> 拼 gltfDir + uri 读文件
//
// Phase G.1.5 T3 — 入参从 cgltf_image* 改为 cgltf_texture* 以读取 sampler 字段.
// tex->sampler 可能为 NULL (glTF 未指定), 此时用 glTF 2.0 默认值.
static uint32_t LoadGLTFImage(const cgltf_texture* tex, const std::string& gltfDir) {
    if (!tex || !tex->image || !g_render) return 0;
    const cgltf_image* img = tex->image;

    const uint8_t* imgData = nullptr;
    size_t         imgSize = 0;

    // case 1: GLB embedded buffer_view
    std::vector<uint8_t> ownedData;  // 负责生命周期管理 (case 2/3 下)
    if (img->buffer_view) {
        const uint8_t* bvData = cgltf_buffer_view_data(img->buffer_view);
        if (!bvData) return 0;
        imgData = bvData;
        imgSize = img->buffer_view->size;
    } else if (img->uri) {
        if (strncmp(img->uri, "data:", 5) == 0) {
            // case 2: data URI (data:image/png;base64,XXX)
            const char* commaPos = strchr(img->uri, ',');
            if (!commaPos) return 0;
            const char* b64 = commaPos + 1;
            size_t b64Len = strlen(b64);
            // 估计解码后大小 (cgltf_load_buffer_base64 需要确切 size)
            size_t decodedSize = (b64Len / 4) * 3;
            // 减去 padding '=' (1 个 = -> -1 字节, 2 个 == -> -2 字节)
            if (b64Len >= 2 && b64[b64Len - 1] == '=') {
                decodedSize--;
                if (b64[b64Len - 2] == '=') decodedSize--;
            }
            if (decodedSize == 0) return 0;
            cgltf_options opts = {};
            void* dec = nullptr;
            cgltf_result r = cgltf_load_buffer_base64(&opts, decodedSize, b64, &dec);
            if (r != cgltf_result_success || !dec) return 0;
            ownedData.assign((uint8_t*)dec, (uint8_t*)dec + decodedSize);
            free(dec);  // cgltf 默认用 malloc
            imgData = ownedData.data();
            imgSize = decodedSize;
        } else {
            // case 3: 相对文件路径
            std::string fullPath = gltfDir + img->uri;
            FILE* fp = fopen(fullPath.c_str(), "rb");
            if (!fp) return 0;
            fseek(fp, 0, SEEK_END);
            long sz = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            if (sz <= 0) { fclose(fp); return 0; }
            ownedData.resize((size_t)sz);
            size_t readBytes = fread(ownedData.data(), 1, (size_t)sz, fp);
            fclose(fp);
            if (readBytes != (size_t)sz) return 0;
            imgData = ownedData.data();
            imgSize = (size_t)sz;
        }
    } else {
        return 0;  // 既没 buffer_view 也没 uri
    }

    // 用 stb_image 解码到 RGBA pixels
    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load_from_memory(imgData, (int)imgSize, &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return 0;
    }

    uint32_t texId = g_render->CreateTexture(w, h, 4, pixels);
    stbi_image_free(pixels);
    if (!texId) return 0;
    // Phase G.1.2 — VRAM Tracking (GLTF material texture; Untrack 留到 Mesh GC 时, 现 mesh GC 路径分散, v1.3 改进)
    LT::GpuMem::Track("Mesh texture", "RGBA8", w, h);

    // Phase G.1.5 T2 + T3 — 提取 cgltf sampler + mipmap-aware 生成 + 透传 sampler.
    // sampler 可能为 NULL (未指定), 此时按 glTF 2.0 默认: mag=LINEAR / min=LINEAR_MIPMAP_LINEAR / wrap=REPEAT.
    const cgltf_sampler* sampler = tex->sampler;
    const int magF = sampler ? (int)sampler->mag_filter : 0;
    const int minF = sampler ? (int)sampler->min_filter : 0;
    const int wrS  = sampler ? (int)sampler->wrap_s     : 0;
    const int wrT  = sampler ? (int)sampler->wrap_t     : 0;
    // GL_NEAREST_MIPMAP_NEAREST=0x2700 / GL_LINEAR_MIPMAP_NEAREST=0x2701
    // GL_NEAREST_MIPMAP_LINEAR=0x2702 / GL_LINEAR_MIPMAP_LINEAR=0x2703
    // 0 = 未指定 (用默认 = LINEAR_MIPMAP_LINEAR), 同样生成 mipmap.
    const bool needsMipmap = (minF == 0) ||
                             (minF == 0x2700) || (minF == 0x2701) ||
                             (minF == 0x2702) || (minF == 0x2703);
    if (needsMipmap) g_render->GenerateMipmap2D(texId);
    g_render->SetTexture2DSampler(texId, magF, minF, wrS, wrT);
    return texId;
}

// 提取 cgltf material 到 MaterialDesc. material 可为 NULL (用默认 PBR).
static void ExtractMaterial(MaterialDesc& d, const cgltf_material* mat, const std::string& gltfDir) {
    // 默认 PBR + 白底
    memset(&d, 0, sizeof(d));
    d.mode = 1;  // PBR
    d.color[0] = d.color[1] = d.color[2] = d.color[3] = 1.0f;
    d.metallic = 0.0f;
    d.roughness = 1.0f;
    d.normalScale = 1.0f;
    d.occlusionStrength = 1.0f;
    d.alphaMode = 0;
    d.alphaCutoff = 0.5f;
    d.doubleSided = 0;

    if (!mat) return;  // primitive 无 material 时返回默认值

    d.mode = mat->unlit ? 0 : 1;

    if (mat->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
        d.color[0]  = pbr.base_color_factor[0];
        d.color[1]  = pbr.base_color_factor[1];
        d.color[2]  = pbr.base_color_factor[2];
        d.color[3]  = pbr.base_color_factor[3];
        d.metallic  = pbr.metallic_factor;
        d.roughness = pbr.roughness_factor;
        if (pbr.base_color_texture.texture) {
            d.texBaseColor = LoadGLTFImage(pbr.base_color_texture.texture, gltfDir);
        }
        if (pbr.metallic_roughness_texture.texture) {
            d.texMetallicRoughness = LoadGLTFImage(pbr.metallic_roughness_texture.texture, gltfDir);
        }
    }

    // normal texture (scale 字段)
    if (mat->normal_texture.texture) {
        d.texNormal = LoadGLTFImage(mat->normal_texture.texture, gltfDir);
        d.normalScale = mat->normal_texture.scale;
    }

    // occlusion texture (cgltf 共享 cgltf_texture_view, scale 字段实为 strength)
    if (mat->occlusion_texture.texture) {
        d.texOcclusion = LoadGLTFImage(mat->occlusion_texture.texture, gltfDir);
        d.occlusionStrength = mat->occlusion_texture.scale;
    }

    // emissive
    d.emissive[0] = mat->emissive_factor[0];
    d.emissive[1] = mat->emissive_factor[1];
    d.emissive[2] = mat->emissive_factor[2];
    if (mat->emissive_texture.texture) {
        d.texEmissive = LoadGLTFImage(mat->emissive_texture.texture, gltfDir);
    }

    // alphaMode
    switch (mat->alpha_mode) {
        case cgltf_alpha_mode_mask:  d.alphaMode = 2; break;
        case cgltf_alpha_mode_blend: d.alphaMode = 1; break;
        case cgltf_alpha_mode_opaque:
        default:                     d.alphaMode = 0; break;
    }
    d.alphaCutoff = mat->alpha_cutoff;
    d.doubleSided = mat->double_sided ? 1 : 0;
}

// ==================== Mesh.LoadGLTF(path, [primitive_index=0], [with_material=false]) ====================
// Phase AS.4.x: with_material=true 时返回 (Mesh, Material), 否则只返回 Mesh (向后兼容).
static int l_Mesh_LoadGLTF(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int primIdx = (int)luaL_optinteger(L, 2, 0);
    bool withMaterial = lua_toboolean(L, 3) != 0;
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

    // Backend 检查 (g_render + Supports3D), 在 cgltf_free 前
    if (!g_render) {
        cgltf_free(data);
        lua_pushnil(L);
        lua_pushstring(L, "no render backend (window not opened?)");
        return 2;
    }
    if (!g_render->Supports3D()) {
        cgltf_free(data);
        lua_pushnil(L);
        lua_pushstring(L, "render backend does not support 3D mesh (need GL 3.3+)");
        return 2;
    }

    // 创建 GPU mesh (失败时也要 free cgltf)
    uint32_t meshId = g_render->CreateMesh(verts.data(), (int)verts.size(),
                                            indices.data(), (int)indices.size());
    if (!meshId) {
        cgltf_free(data);
        lua_pushnil(L);
        lua_pushstring(L, "CreateMesh failed (GPU upload error)");
        return 2;
    }

    // Phase AS.4.x: with_material=true 时, 提取 material + 加载贴图
    MaterialDesc matDesc = {};
    if (withMaterial) {
        std::string gltfDir = GetGLTFDirectory(path);
        ExtractMaterial(matDesc, prim->material, gltfDir);
    }

    cgltf_free(data);  // prim 指针之后不可访问

    // 创建 mesh userdata + 元表
    MeshUserdata* ud = (MeshUserdata*)lua_newuserdata(L, sizeof(MeshUserdata));
    ud->magic       = LT::LT_MAGIC_MESH;  // Phase G.1.7.1 — type tag
    ud->meshId      = meshId;
    ud->vertexCount = (int)verts.size();
    ud->indexCount  = (int)indices.size();
    luaL_getmetatable(L, MESH_MT);
    lua_setmetatable(L, -2);

    // Phase AS.4.x: 附加 Material userdata
    // Phase G.1.7 P2.1: 走 PushNewMaterialUserdata 以启用 magic 防御
    if (withMaterial) {
        MaterialDesc* matUd = PushNewMaterialUserdata(L);
        *matUd = matDesc;
        return 2;  // (Mesh, Material)
    }
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

// Phase G.1.5 — Mesh result pusher: with_material=true 时 push (mesh, material), 返 2;
// 否则 push mesh 单值返 1. 错误路径 push nil 返 1.
//
// MaterialDesc 通过 char[128] POD 序列化在 worker thread 完成 (asset_loader.cpp),
// 此处反序列化创建 Material userdata.
static int MeshPushResult_(void* L_, AssetLoader::FutureState* state) {
    lua_State* L = (lua_State*)L_;
    if (!L) return 0;
    if (!state || state->status.load() != (int)AssetLoader::FutureStatus::Ready || !state->resMeshId) {
        lua_pushnil(L);
        return 1;
    }
    // 1) push Mesh userdata
    MeshUserdata* ud = (MeshUserdata*)lua_newuserdata(L, sizeof(MeshUserdata));
    ud->magic       = LT::LT_MAGIC_MESH;  // Phase G.1.7.1 — type tag
    ud->meshId      = state->resMeshId;
    ud->vertexCount = state->gltfVertCount;
    ud->indexCount  = state->gltfIdxCount;
    state->resMeshId = 0;
    luaL_getmetatable(L, MESH_MT);
    lua_setmetatable(L, -2);

    // 2) Phase G.1.5: with_material=true 时 push Material userdata (反序列化 MaterialDesc 字节)
    // Phase G.1.7 P2.1: 走 PushNewMaterialUserdata 以启用 magic 防御
    if (state->gltfWithMaterial) {
        static_assert(sizeof(MaterialDesc) <= sizeof(state->gltfMaterialDesc),
                      "MaterialDesc exceeds gltfMaterialDesc[128] buffer; bump capacity in asset_loader.h");
        MaterialDesc* matUd = PushNewMaterialUserdata(L);
        memcpy(matUd, state->gltfMaterialDesc, sizeof(MaterialDesc));
        return 2;   // (Mesh, Material)
    }
    return 1;   // (Mesh) 仅 mesh, 与 G.1.0 行为一致
}

// Phase G.1.5 — Mesh callback dispatcher: with_material 时 cb 收 (mesh, material, err) 3 参;
// 否则 cb 收 (mesh, err) 2 参 (与 G.1.0 行为一致, 现有脚本零回归).
static void MeshAsyncDispatcher_(void* L_, AssetLoader::FutureState* state, int cbLuaRef) {
    lua_State* L = (lua_State*)L_;
    if (!L || !state || cbLuaRef < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbLuaRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    const bool withMat = state->gltfWithMaterial;
    int        nArgs   = 0;
    if (state->status.load() == (int)AssetLoader::FutureStatus::Ready) {
        // MeshPushResult_ push 1 或 2 个 (mesh, optionally material)
        int pushed = MeshPushResult_(L, state);
        // 补齐: with_material 时 push 失败兜底 push nil 作为 material
        if (withMat && pushed < 2) {
            lua_pushnil(L);
            ++pushed;
        }
        lua_pushnil(L);   // err = nil
        nArgs = pushed + 1;
    } else {
        // 错误路径: with_material 时 (nil, nil, err); 否则 (nil, err)
        lua_pushnil(L);
        if (withMat) lua_pushnil(L);
        lua_pushstring(L, state->errorMsg.empty() ? "unknown error" : state->errorMsg.c_str());
        nArgs = withMat ? 3 : 2;
    }
    if (lua_pcall(L, nArgs, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "Mesh LoadGLTFAsync cb error: %s", err ? err : "(none)");
        lua_pop(L, 1);
    }
}

// Phase G.1.5 — l_Mesh_LoadGLTFAsync 灵活参数解析:
//   Mesh.LoadGLTFAsync(path)                              -- Future, mesh only
//   Mesh.LoadGLTFAsync(path, primIdx)                     -- Future, mesh only
//   Mesh.LoadGLTFAsync(path, primIdx, true)               -- Future, with material
//   Mesh.LoadGLTFAsync(path, cb)                          -- cb(mesh, err), mesh only
//   Mesh.LoadGLTFAsync(path, primIdx, cb)                 -- cb(mesh, err), mesh only
//   Mesh.LoadGLTFAsync(path, primIdx, true, cb)           -- cb(mesh, material, err)
//   Mesh.LoadGLTFAsync(path, primIdx, false, cb)          -- cb(mesh, err) 显式 false
static int l_Mesh_LoadGLTFAsync(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    // 灵活参数解析: 依次扫 args[2..4], 按类型识别 primIdx (number) / withMaterial (bool) / cb (function)
    int  primIdx      = 0;
    bool withMaterial = false;
    int  cbStackIdx   = 0;   // 0 = 无 cb, 否则 Lua 栈索引

    int top = lua_gettop(L);
    int seen = 0;   // 已识别参数槽位: 1=primIdx, 2=withMaterial, 4=cb
    for (int i = 2; i <= top; ++i) {
        int t = lua_type(L, i);
        if (t == LUA_TNUMBER && !(seen & 1)) {
            primIdx = (int)lua_tointeger(L, i);
            seen |= 1;
        } else if (t == LUA_TBOOLEAN && !(seen & 2)) {
            withMaterial = lua_toboolean(L, i) != 0;
            seen |= 2;
        } else if (t == LUA_TFUNCTION && !(seen & 4)) {
            cbStackIdx = i;
            seen |= 4;
        }
        // 未知/重复 类型静默忽略 (保持向后兼容)
    }

    int cbRef = -1;
    if (cbStackIdx > 0) {
        lua_pushvalue(L, cbStackIdx);
        cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }

    auto state = AssetLoader::LoadGLTFAsync(path, primIdx, withMaterial);
    AssetLoader::RegisterResultPusher(state, MeshPushResult_);
    if (cbRef >= 0) {
        AssetLoader::RegisterCallback(state, MeshAsyncDispatcher_, L, cbRef);
        if (state->status.load() != (int)AssetLoader::FutureStatus::Pending) {
            MeshAsyncDispatcher_(L, state.get(), cbRef);
            luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
            state->dispatcher = nullptr;
            state->cbLuaRef = -1;
            state->cbLuaState = nullptr;
        }
    }
    return AssetLoader::PushAsyncFuture(L, std::move(state));
}

// ==================== mesh:Draw([textureId | material]) ====================
// Phase AS.4: 自动判断参数类型
//   - integer / nil 缺省 -> 老路径 (DrawMesh + textureId)
//   - userdata (Material) -> 新路径 (DrawMeshMaterial)
// (forward decl 已提前到文件顶部 — G.1.7 P2.1)

static int l_Mesh_Draw(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    if (!g_render || !ud->meshId) return 0;

    int t = lua_type(L, 2);
    if (t == LUA_TTABLE) {
        TrySetPreviousModelMatrix(L, 2);
        g_render->DrawMesh(ud->meshId, 0);
    } else if (t == LUA_TUSERDATA) {
        // 新路径: material 路径
        const MaterialDesc* desc = CheckMaterialUserdata(L, 2);
        if (desc) {
            TrySetPreviousModelMatrix(L, 3);
            g_render->DrawMeshMaterial(ud->meshId, desc);
        } else {
            // 不是 Material userdata, 报错
            return luaL_error(L, "mesh:Draw expects integer textureId or Material userdata");
        }
    } else {
        // 老路径: integer / nil
        TrySetPreviousModelMatrix(L, 3);
        uint32_t texId = (t == LUA_TNUMBER || t == LUA_TSTRING) ? (uint32_t)lua_tointeger(L, 2) : 0;
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
// Phase G.1.7.1: __gc 后 设 magic = DEAD, 防 use-after-free
static int l_Mesh_Delete(lua_State* L) {
    MeshUserdata* ud = CheckMesh(L, 1);
    if (ud->meshId && g_render) {
        g_render->DeleteMesh(ud->meshId);
        ud->meshId = 0;
        ud->magic = LT::LT_MAGIC_DEAD;  // Phase G.1.7.1 — 释放后不可再访问
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
    // Phase AS.4.x — 确保 Material 元表已注册 (LoadGLTF with_material 需要)
    // 调一次 luaopen_Light_Graphics_Material, 之后弹掉返回值, 不影响后续栈操作
    int top = lua_gettop(L);
    luaopen_Light_Graphics_Material(L);
    lua_settop(L, top);

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
            { "LoadGLTFAsync",      l_Mesh_LoadGLTFAsync },
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
