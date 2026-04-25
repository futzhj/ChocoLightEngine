/**
 * @file light_tilemap.cpp
 * @brief Light.Graphics.Tilemap — 正交 Tilemap 渲染 + TMX 加载
 *
 * Lua API:
 *   map = Light(Light.Graphics.Tilemap):New()
 *   map:LoadFromData(width, height, tileW, tileH, data, image)
 *   map:SetTile(layer, x, y, id)
 *   map:GetTile(layer, x, y) → id
 *   map:Draw(offsetX, offsetY)
 *   map:DrawLayer(layer, offsetX, offsetY)
 *   map:GetMapSize() → w, h (tile 数)
 *   map:GetTileSize() → w, h (像素)
 *   map:GetLayerCount() → int
 *
 * 内部用简化 CSV 数据格式 (Tiled "CSV" 编码), 不依赖 XML 库
 * Lua 端可用辅助函数解析 .tmx XML → 传入纯数据
 */

#include "light.h"
#include "render_backend.h"
#include <cstring>
#include <cstdlib>
#include <vector>
#include <string>

extern RenderBackend* g_render;

// ==================== 数据结构 ====================

struct TileLayer {
    std::vector<int> data;   // tile ID (0=空, 1-based)
    int width, height;
    std::string name;
};

struct TilemapData {
    std::vector<TileLayer> layers;
    int mapW, mapH;          // tile 数
    int tileW, tileH;        // 像素
    uint32_t texId;          // 图集纹理 (GL texture ID)
    int texW, texH;          // 图集像素尺寸
    int columns;             // 图集列数
};

static TilemapData* CheckTilemap(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    auto* tm = (TilemapData*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return tm;
}

// ==================== 绘制 ====================

static void DrawLayer(TilemapData* tm, int layerIdx, float ox, float oy) {
    if (!tm || !g_render || layerIdx < 0 || layerIdx >= (int)tm->layers.size()) return;
    if (tm->texId == 0 || tm->columns <= 0) return;

    const TileLayer& layer = tm->layers[layerIdx];
    float tw = (float)tm->tileW;
    float th = (float)tm->tileH;
    // 图集 UV 步长
    float uStep = tw / (float)tm->texW;
    float vStep = th / (float)tm->texH;

    g_render->BindTexture(tm->texId);

    for (int y = 0; y < layer.height; y++) {
        for (int x = 0; x < layer.width; x++) {
            int idx = y * layer.width + x;
            if (idx >= (int)layer.data.size()) continue;
            int tileId = layer.data[idx];
            if (tileId <= 0) continue;

            // tile ID → 图集坐标
            int tid = tileId - 1;
            int tx = tid % tm->columns;
            int ty = tid / tm->columns;
            float u0 = tx * uStep;
            float v0 = ty * vStep;
            float u1 = u0 + uStep;
            float v1 = v0 + vStep;

            float px = ox + x * tw;
            float py = oy + y * th;

            float verts[] = { px,py,0, px+tw,py,0, px+tw,py+th,0, px,py+th,0 };
            float uvs[]   = { u0,v0, u1,v0, u1,v1, u0,v1 };
            g_render->DrawArrays(2, verts, 4, uvs, nullptr);
        }
    }
    g_render->BindTexture(0);
}

// ==================== Lua 绑定 ====================

/// Tilemap.__call — 构造
static int l_Tilemap_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    auto* tm = (TilemapData*)lua_newuserdata(L, sizeof(TilemapData));
    new (tm) TilemapData();
    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Tilemap:AddLayer(width, height, csvData)
/// csvData: 逗号分隔 tile ID 字符串
static int l_Tilemap_AddLayer(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    if (!tm) return 0;
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);
    size_t len = 0;
    const char* csv = luaL_checklstring(L, 4, &len);

    TileLayer layer;
    layer.width = w;
    layer.height = h;
    layer.data.resize(w * h, 0);

    // 解析 CSV
    int idx = 0;
    const char* p = csv;
    while (*p && idx < w * h) {
        // 跳过空白
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (!*p) break;
        layer.data[idx++] = (int)strtol(p, (char**)&p, 10);
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    }

    tm->layers.push_back(std::move(layer));
    lua_pushinteger(L, (lua_Integer)tm->layers.size());
    return 1;
}

/// Tilemap:SetMapInfo(mapW, mapH, tileW, tileH)
static int l_Tilemap_SetMapInfo(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    if (!tm) return 0;
    tm->mapW = (int)luaL_checkinteger(L, 2);
    tm->mapH = (int)luaL_checkinteger(L, 3);
    tm->tileW = (int)luaL_checkinteger(L, 4);
    tm->tileH = (int)luaL_checkinteger(L, 5);
    return 0;
}

/// Tilemap:SetTileset(textureId, texW, texH, columns)
static int l_Tilemap_SetTileset(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    if (!tm) return 0;
    tm->texId = (uint32_t)luaL_checkinteger(L, 2);
    tm->texW = (int)luaL_checkinteger(L, 3);
    tm->texH = (int)luaL_checkinteger(L, 4);
    tm->columns = (int)luaL_checkinteger(L, 5);
    return 0;
}

/// Tilemap:SetTile(layer, x, y, id) — layer/x/y 从 1 开始
static int l_Tilemap_SetTile(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    if (!tm) return 0;
    int li = (int)luaL_checkinteger(L, 2) - 1;
    int x = (int)luaL_checkinteger(L, 3) - 1;
    int y = (int)luaL_checkinteger(L, 4) - 1;
    int id = (int)luaL_checkinteger(L, 5);
    if (li >= 0 && li < (int)tm->layers.size()) {
        auto& layer = tm->layers[li];
        int idx = y * layer.width + x;
        if (idx >= 0 && idx < (int)layer.data.size())
            layer.data[idx] = id;
    }
    return 0;
}

/// Tilemap:GetTile(layer, x, y) → id
static int l_Tilemap_GetTile(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    if (!tm) { lua_pushinteger(L, 0); return 1; }
    int li = (int)luaL_checkinteger(L, 2) - 1;
    int x = (int)luaL_checkinteger(L, 3) - 1;
    int y = (int)luaL_checkinteger(L, 4) - 1;
    int id = 0;
    if (li >= 0 && li < (int)tm->layers.size()) {
        auto& layer = tm->layers[li];
        int idx = y * layer.width + x;
        if (idx >= 0 && idx < (int)layer.data.size())
            id = layer.data[idx];
    }
    lua_pushinteger(L, id);
    return 1;
}

/// Tilemap:Draw(ox, oy) — 绘制所有图层
static int l_Tilemap_Draw(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    float ox = (float)luaL_optnumber(L, 2, 0.0);
    float oy = (float)luaL_optnumber(L, 3, 0.0);
    if (tm) {
        for (int i = 0; i < (int)tm->layers.size(); i++)
            DrawLayer(tm, i, ox, oy);
    }
    return 0;
}

/// Tilemap:DrawLayer(layerIdx, ox, oy)
static int l_Tilemap_DrawLayer(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    int li = (int)luaL_checkinteger(L, 2) - 1;
    float ox = (float)luaL_optnumber(L, 3, 0.0);
    float oy = (float)luaL_optnumber(L, 4, 0.0);
    DrawLayer(tm, li, ox, oy);
    return 0;
}

/// Tilemap:GetMapSize() → w, h
static int l_Tilemap_GetMapSize(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    lua_pushinteger(L, tm ? tm->mapW : 0);
    lua_pushinteger(L, tm ? tm->mapH : 0);
    return 2;
}

/// Tilemap:GetTileSize() → w, h
static int l_Tilemap_GetTileSize(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    lua_pushinteger(L, tm ? tm->tileW : 0);
    lua_pushinteger(L, tm ? tm->tileH : 0);
    return 2;
}

/// Tilemap:GetLayerCount() → int
static int l_Tilemap_GetLayerCount(lua_State* L) {
    auto* tm = CheckTilemap(L, 1);
    lua_pushinteger(L, tm ? (lua_Integer)tm->layers.size() : 0);
    return 1;
}

static int l_Tilemap_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.Tilemap");
    return 1;
}

// ==================== luaopen 注册 ====================

static void EnsureGraphicsTable(lua_State* L) {
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

static const luaL_Reg tilemap_funcs[] = {
    {"AddLayer",      l_Tilemap_AddLayer},
    {"SetMapInfo",    l_Tilemap_SetMapInfo},
    {"SetTileset",    l_Tilemap_SetTileset},
    {"SetTile",       l_Tilemap_SetTile},
    {"GetTile",       l_Tilemap_GetTile},
    {"Draw",          l_Tilemap_Draw},
    {"DrawLayer",     l_Tilemap_DrawLayer},
    {"GetMapSize",    l_Tilemap_GetMapSize},
    {"GetTileSize",   l_Tilemap_GetTileSize},
    {"GetLayerCount", l_Tilemap_GetLayerCount},
    {"__call",        l_Tilemap_Call},
    {"__tostring",    l_Tilemap_Tostring},
    {NULL, NULL}
};

int luaopen_Light_Graphics_Tilemap(lua_State* L) {
    EnsureGraphicsTable(L);
    lua_pushstring(L, "Tilemap");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Tilemap");
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, tilemap_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Tilemap");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
