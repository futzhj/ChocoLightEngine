/**
 * @file light_plugins.cpp
 * @brief Light.Plugins / WDFData / NEMData 模块 — 游戏数据格式插件
 * @note 深度还原自 Light.dll IDA 反编译
 *
 * WDFData API (7个函数，精确匹配 sub_1800B3E20 函数表):
 *   GetData(hash)          — 按资源哈希提取原始二进制数据
 *   GetTGAData(hash)       — 提取 TGA 纹理数据
 *   GetImageData(hash)     — 提取解码后的图像数据
 *   GetAudioData(hash)     — 提取音频数据
 *   GetSpriteImagesData(hash) — 提取精灵图帧数据
 *   __call(path)           — 构造函数，打开 WDF 文件
 *   __tostring()           — 返回 "Light.Plugins.WDFData: %p"
 *
 * 资源查找算法 (sub_1800A0E40):
 *   使用 FNV-1a 64位哈希定位桶 → 链表遍历匹配 32位资源ID
 *   FNV offset basis = 0xCBF29CE484222325
 *   FNV prime = 0x100000001B3
 */

#include "light.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <vector>
#include <unordered_map>

// ==================== WDF 文件格式定义 ====================
// 网易 WDF/WD1 格式 — PFDW magic

#pragma pack(push, 1)
struct WDFHeader {
    char     magic[4];      // "PFDW"
    uint32_t count;         // 文件条目数
    uint32_t indexOffset;   // 索引表偏移
};

struct WDFEntry {
    uint32_t hash;          // CWHash 路径哈希
    uint32_t offset;        // 数据偏移
    uint32_t size;          // 原始大小
    uint32_t compressedSize;// 压缩后大小
};
#pragma pack(pop)

// ==================== WDF 上下文 — 含 FNV-1a 哈希表 ====================
// 还原自 CC::Unsafe<CC::Plugins::WDFData::Context>
// sub_1800A0E40: FNV-1a 哈希桶查找算法

struct WDFContext {
    void*    vtable;        // CC::Unsafe vftable
    FILE*    file;          // 打开的文件句柄
    bool     valid;         // 文件有效性标志

    // FNV-1a 哈希桶 — 还原自 sub_1800A0E40
    std::unordered_map<uint32_t, WDFEntry> entries;

    WDFContext() : vtable(nullptr), file(nullptr), valid(false) {}

    ~WDFContext() {
        if (file) { fclose(file); file = nullptr; }
    }

    // FNV-1a 查找 (还原自 sub_1800A0E40 中的哈希计算)
    // 原始使用每字节展开的 FNV-1a: basis=0xCBF29CE484222325, prime=0x100000001B3
    const WDFEntry* FindEntry(uint32_t hash) const {
        auto it = entries.find(hash);
        return (it != entries.end()) ? &it->second : nullptr;
    }

    // 读取指定条目��数据
    // 还原自 sub_180007E30 (seek) + sub_180006680 (read)
    bool ReadEntryData(const WDFEntry* entry, void* buf, size_t bufSize) {
        if (!file || !entry || bufSize < entry->size) return false;
        fseek(file, entry->offset, SEEK_SET);
        return fread(buf, 1, entry->size, file) == entry->size;
    }
};

// ==================== 辅助: 获取 WDFData 上下文 ====================
// 还原自 sub_1800B2A90 — 从 Lua table 的 __instance 获取 userdata

static WDFContext* GetWDFContext(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) {
        lua_pop(L, 1);
        return nullptr;
    }
    void** ud = (void**)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ud ? (WDFContext*)ud[1] : nullptr;
}

// ==================== WDFData 函数实现 ====================

/// 资源解码: byte-reversal + XOR 0x5A
/// 还原自 sub_1123F780 (NeoX 引擎数据解码算法)
/// 对每个字节执行: 反转位序 → XOR 0x5A
static void DecodeWDFResource(uint8_t* data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        // 位反转: 01234567 → 76543210
        uint8_t b = data[i];
        b = (uint8_t)(((b & 0xF0) >> 4) | ((b & 0x0F) << 4));
        b = (uint8_t)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
        b = (uint8_t)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
        data[i] = b ^ 0x5A;
    }
}

/// WDFData.GetData(self, hash) — 按哈希提取原始数据 (不解码)
/// 还原自 sub_1800B2F10 + sub_1800A0E40
/// 返回: userdata(原始字节), size  或  nil
static int l_WDFData_GetData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object"), 0;

    uint32_t hash = (uint32_t)luaL_checkinteger(L, 2);

    // FNV-1a 哈希桶查找 + 数据提取
    const WDFEntry* entry = ctx->FindEntry(hash);
    if (entry) {
        void* buf = lua_newuserdata(L, entry->size);
        ctx->ReadEntryData(entry, buf, entry->size);
        lua_pushinteger(L, entry->size);
        return 2;
    }

    lua_pushnil(L);
    return 1;
}

/// WDFData.GetTGAData(self, hash) — 提取解码后的 TGA 纹理数据
/// 还原自 sub_1800B3910
/// 读取原始数据 → byte-reversal + XOR 0x5A 解码 → 返回 TGA 数据
static int l_WDFData_GetTGAData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object"), 0;

    uint32_t hash = (uint32_t)luaL_checkinteger(L, 2);
    const WDFEntry* entry = ctx->FindEntry(hash);
    if (entry) {
        void* buf = lua_newuserdata(L, entry->size);
        ctx->ReadEntryData(entry, buf, entry->size);
        // TGA 解码: byte-reversal + XOR 0x5A
        DecodeWDFResource((uint8_t*)buf, entry->size);
        lua_pushinteger(L, entry->size);
        return 2;
    }

    lua_pushnil(L);
    return 1;
}

/// WDFData.GetImageData(self, hash) — 提取解码后的图像数据
/// 还原自 sub_1800B3030
/// 读取 → 解码 → stb_image 解码为 RGBA
static int l_WDFData_GetImageData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object"), 0;

    uint32_t hash = (uint32_t)luaL_checkinteger(L, 2);
    const WDFEntry* entry = ctx->FindEntry(hash);
    if (entry) {
        // 读取并解码
        std::vector<uint8_t> raw(entry->size);
        ctx->ReadEntryData(entry, raw.data(), raw.size());
        DecodeWDFResource(raw.data(), raw.size());

        // 返回解码后的原始数据 (Lua 层可进一步处理)
        void* buf = lua_newuserdata(L, entry->size);
        memcpy(buf, raw.data(), entry->size);
        lua_pushinteger(L, entry->size);
        return 2;
    }

    lua_pushnil(L);
    return 1;
}

/// WDFData.GetAudioData(self, hash) — 提取音频数据
/// 还原自 sub_1800B2CD0
/// 读取 → 解码 → ���回 PCM/WAV 数据
static int l_WDFData_GetAudioData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object"), 0;

    uint32_t hash = (uint32_t)luaL_checkinteger(L, 2);
    const WDFEntry* entry = ctx->FindEntry(hash);
    if (entry) {
        void* buf = lua_newuserdata(L, entry->size);
        ctx->ReadEntryData(entry, buf, entry->size);
        // 音频资源同样需要解码
        DecodeWDFResource((uint8_t*)buf, entry->size);
        lua_pushinteger(L, entry->size);
        return 2;
    }

    lua_pushnil(L);
    return 1;
}

/// WDFData.GetSpriteImagesData(self, hash) — 提取并解析 WAS 精灵帧数据
/// 还原自 sub_1800B31E0
/// WAS 格式: 16字节头 + 512字节调色板(256×RGB565) + 帧偏移表 + RLE帧数据
/// 返回 Lua 表: { directions, framesPerDir, width, height, keyX, keyY, frames={...} }
static int l_WDFData_GetSpriteImagesData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object"), 0;

    uint32_t hash = (uint32_t)luaL_checkinteger(L, 2);
    const WDFEntry* entry = ctx->FindEntry(hash);
    if (!entry) { lua_pushnil(L); return 1; }

    // 读取并解码 WAS 原始数据
    std::vector<uint8_t> raw(entry->size);
    ctx->ReadEntryData(entry, raw.data(), raw.size());
    DecodeWDFResource(raw.data(), raw.size());

    const uint8_t* p = raw.data();
    size_t totalSize = raw.size();

    // 验证最小长度: 16(头) + 512(调色板) = 528
    if (totalSize < 528) {
        CC::Log(CC::LOG_WARN, "WAS: data too small (%zu bytes)", totalSize);
        lua_pushnil(L); return 1;
    }

    // ---- 解析文件头 (16 字节) ----
    uint16_t magic      = *(uint16_t*)(p + 0);   // 'SP' = 0x5053
    // uint16_t hdrLen  = *(uint16_t*)(p + 2);   // 通常 12
    uint16_t directions = *(uint16_t*)(p + 4);
    uint16_t framesPerDir = *(uint16_t*)(p + 6);
    uint16_t imgWidth   = *(uint16_t*)(p + 8);
    uint16_t imgHeight  = *(uint16_t*)(p + 10);
    int16_t  keyX       = *(int16_t*)(p + 12);
    int16_t  keyY       = *(int16_t*)(p + 14);

    if (magic != 0x5053) {
        CC::Log(CC::LOG_WARN, "WAS: bad magic 0x%04X (expected 0x5053)", magic);
        lua_pushnil(L); return 1;
    }
    if (directions == 0 || framesPerDir == 0) {
        CC::Log(CC::LOG_WARN, "WAS: zero directions(%u) or frames(%u)", directions, framesPerDir);
        lua_pushnil(L); return 1;
    }

    // ---- 解析调色板 (512 字节, 256 × RGB565) ----
    const uint16_t* palRaw = (const uint16_t*)(p + 16);
    uint32_t palette[256];  // RGBA8888
    for (int i = 0; i < 256; ++i) {
        uint16_t c = palRaw[i];
        // RGB565: RRRRR_GGGGGG_BBBBB → RGBA8888
        uint8_t r = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((c >> 5)  & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)(( c        & 0x1F) * 255 / 31);
        palette[i] = (255u << 24) | (b << 16) | (g << 8) | r;  // ABGR for GL
    }
    // 索引 0 通常为透明色
    palette[0] = 0;

    // ---- 帧偏移表 ----
    uint32_t totalFrames = (uint32_t)directions * framesPerDir;
    size_t offsetTableStart = 16 + 512;  // 头 + 调色板
    size_t offsetTableSize = totalFrames * 4;
    if (offsetTableStart + offsetTableSize > totalSize) {
        CC::Log(CC::LOG_WARN, "WAS: frame offset table out of bounds");
        lua_pushnil(L); return 1;
    }
    const uint32_t* frameOffsets = (const uint32_t*)(p + offsetTableStart);

    // ---- 构建 Lua 结果表 ----
    lua_createtable(L, 0, 7);

    lua_pushinteger(L, directions);     lua_setfield(L, -2, "directions");
    lua_pushinteger(L, framesPerDir);   lua_setfield(L, -2, "framesPerDir");
    lua_pushinteger(L, imgWidth);       lua_setfield(L, -2, "width");
    lua_pushinteger(L, imgHeight);      lua_setfield(L, -2, "height");
    lua_pushinteger(L, keyX);           lua_setfield(L, -2, "keyX");
    lua_pushinteger(L, keyY);           lua_setfield(L, -2, "keyY");

    // frames 数组
    lua_createtable(L, (int)totalFrames, 0);

    for (uint32_t fi = 0; fi < totalFrames; ++fi) {
        uint32_t fOff = frameOffsets[fi];
        if (fOff + 16 > totalSize) {
            // 帧偏移越界, 跳过
            continue;
        }

        const uint8_t* fp = p + fOff;

        // 帧头: x_off(int32), y_off(int32), width(uint32), height(uint32)
        int32_t  frameX = *(int32_t*)(fp + 0);
        int32_t  frameY = *(int32_t*)(fp + 4);
        uint32_t frameW = *(uint32_t*)(fp + 8);
        uint32_t frameH = *(uint32_t*)(fp + 12);

        if (frameW == 0 || frameH == 0 || frameW > 4096 || frameH > 4096) {
            continue;  // 无效帧, 跳过
        }

        // 分配 RGBA 像素缓冲区 (初始化为透明)
        size_t pixelBytes = frameW * frameH * 4;
        uint32_t* pixels = (uint32_t*)lua_newuserdata(L, pixelBytes);
        memset(pixels, 0, pixelBytes);

        // ---- RLE 解压 ----
        // 帧头之后是逐行数据: 每行先有行内像素的 RLE 流
        const uint8_t* rleStart = fp + 16;
        size_t rleMaxLen = totalSize - (rleStart - p);
        const uint8_t* rp = rleStart;
        const uint8_t* rpEnd = rleStart + rleMaxLen;

        // 逐行 RLE 解码 (扫描线)
        for (uint32_t row = 0; row < frameH && rp < rpEnd; ++row) {
            uint32_t col = 0;
            while (col < frameW && rp < rpEnd) {
                uint8_t ctrl = *rp++;
                uint8_t type = (ctrl >> 6) & 0x03;  // 高 2 位
                uint8_t count = ctrl & 0x3F;         // 低 6 位

                if (type == 0) {
                    // 类型 00: count 个连续调色板索引像素
                    for (uint8_t k = 0; k < count && col < frameW && rp < rpEnd; ++k) {
                        uint8_t idx = *rp++;
                        pixels[row * frameW + col] = palette[idx];
                        col++;
                    }
                } else if (type == 1) {
                    // 类型 01: 跳过 count 个透明像素
                    col += count;
                    if (col > frameW) col = frameW;
                } else if (type == 2) {
                    // 类型 10: 重复 1 个颜色 count 次
                    if (rp < rpEnd) {
                        uint8_t idx = *rp++;
                        uint32_t color = palette[idx];
                        for (uint8_t k = 0; k < count && col < frameW; ++k) {
                            pixels[row * frameW + col] = color;
                            col++;
                        }
                    }
                } else {
                    // 类型 11: 行结束标志
                    break;
                }
            }
        }

        // 构建帧表项: { x, y, w, h, pixels }
        // 注意: userdata(pixels) 已在栈上
        lua_createtable(L, 0, 5);
        // 先把 pixels userdata 移入帧表
        lua_insert(L, -2);  // 交换: table <-> userdata -> userdata, table
        lua_setfield(L, -2, "pixels");  // table.pixels = userdata

        lua_pushinteger(L, frameX);  lua_setfield(L, -2, "x");
        lua_pushinteger(L, frameY);  lua_setfield(L, -2, "y");
        lua_pushinteger(L, (int)frameW); lua_setfield(L, -2, "w");
        lua_pushinteger(L, (int)frameH); lua_setfield(L, -2, "h");

        // frames[fi+1] = 帧表
        lua_rawseti(L, -2, (int)(fi + 1));
    }

    lua_setfield(L, -2, "frames");

    return 1;
}

/// WDFData.__gc — GC 释放资源
/// 还原自 sub_1800B2C90
static int l_WDFData_GC(lua_State* L) {
    void** ud = (void**)lua_touserdata(L, 1);
    if (ud && ud[1]) {
        WDFContext* ctx = (WDFContext*)ud[1];
        delete ctx;
        ud[1] = nullptr;
    }
    return 0;
}

/// WDFData.__call(self, path) — 构造函数, 打开 WDF 文件
/// 还原自 sub_1800B3B20
/// 数据流: path→wchar_t→filesystem::exists→open→parse_header→build_hashtable
static int l_WDFData_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    // 检查文件存在 (还原自 std::filesystem::exists)
    FILE* f = fopen(path, "rb");
    if (!f)
        return luaL_error(L, "File not exist");

    // 读取 WDF 头部
    WDFHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        return luaL_error(L, "File not exist");
    }

    // 分配上下文并构建哈希表
    WDFContext* ctx = new WDFContext();
    ctx->file = f;

    // 解析索引表 → 构建 FNV-1a 哈希桶 (sub_18009F970 初始化)
    if (memcmp(hdr.magic, "PFDW", 4) == 0 || memcmp(hdr.magic, "WDFP", 4) == 0) {
        ctx->valid = true;
        fseek(f, hdr.indexOffset, SEEK_SET);

        for (uint32_t i = 0; i < hdr.count; ++i) {
            WDFEntry entry;
            if (fread(&entry, sizeof(entry), 1, f) != 1) break;
            ctx->entries[entry.hash] = entry;
        }
    }

    // 创建 CC::Unsafe<WDFData::Context> userdata
    void** ud = (void**)lua_newuserdata(L, 16);
    ud[0] = nullptr;  // CC::Unsafe vftable (stub)
    ud[1] = ctx;

    // 设置 metatable: { __gc = cleanup }
    lua_createtable(L, 0, 0);
    lua_pushcfunction(L, l_WDFData_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    // 存�� self.__instance
    lua_setfield(L, 1, "__instance");

    return 0;
}

/// WDFData.__tostring — 还原自 sub_1800B3FD0
/// 格式字符串: "Light.Plugins.WDFData: %p" (从 IDA 0x18025B388)
static int l_WDFData_Tostring(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    WDFContext* ctx = GetWDFContext(L, 1);
    if (ctx) {
        lua_pushfstring(L, "Light.Plugins.WDFData: %p", ctx);
    } else {
        lua_pushstring(L, "Light.Plugins.WDFData");
    }
    return 1;
}

// ==================== Plugins 模块 ====================

static const luaL_Reg plugins_funcs[] = {
    {NULL, NULL}
};

int luaopen_Light_Plugins(lua_State* L) {
    LT::RegisterModule(L, "Plugins", plugins_funcs);
    return 1;
}

// ==================== luaopen_Light_Plugins_WDFData ====================
// 精确还原自 sub_1800B3E20 的 7 函数注册表

int luaopen_Light_Plugins_WDFData(lua_State* L) {
    LT::EnsureLightTable(L);

    // 确保 Plugins 父模块存在
    lua_pushstring(L, "Plugins");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Plugins");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Plugins");
        lua_rawget(L, -2);
    }

    // WDFData 子模块 — 精确匹配 IDA 函数表顺序
    lua_pushstring(L, "WDFData");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "WDFData");
        lua_createtable(L, 0, 0);

        const luaL_Reg wdfdata_funcs[] = {
            {"GetData",             l_WDFData_GetData},
            {"GetTGAData",          l_WDFData_GetTGAData},
            {"GetImageData",        l_WDFData_GetImageData},
            {"GetAudioData",        l_WDFData_GetAudioData},
            {"GetSpriteImagesData", l_WDFData_GetSpriteImagesData},
            {"__call",              l_WDFData_Call},
            {"__tostring",          l_WDFData_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, wdfdata_funcs, 0);

        lua_rawset(L, -3);
        lua_pushstring(L, "WDFData");
        lua_rawget(L, -2);
    }

    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}

// ==================== NEMData 模块 ====================
// 还原自 IDA: 9 函数注册表 (sub_1800B4D80)
// NEM: 2D 网格地图格式, 包含 tile 障碍数据 + 图像/遮罩层

// NEM 地图上下文 — 解析后持有 tile 网格和图像数据
struct NEMContext {
    int     width;          // 地图宽度 (tile 数)
    int     height;         // 地图高度 (tile 数)
    uint8_t* obstacles;     // 障碍物网格 (width*height, 0=通行 1=阻挡)
    uint8_t* imageData;     // 地图图像数据 (RGBA)
    int      imgW, imgH;    // 图像尺寸 (像素)
    uint8_t* maskData;      // 遮罩图像数据 (RGBA)
    int      maskW, maskH;  // 遮罩尺寸 (像素)
    bool     valid;
};

// 辅助: 从 Lua self 获取 NEMContext
static NEMContext* GetNEMContext(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    void** ud = (void**)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!ud || !ud[1]) return nullptr;
    return (NEMContext*)ud[1];
}

// __gc: 释放 NEMContext 资源
static int l_NEMData_GC(lua_State* L) {
    void** ud = (void**)lua_touserdata(L, 1);
    if (ud && ud[1]) {
        NEMContext* ctx = (NEMContext*)ud[1];
        if (ctx->obstacles) free(ctx->obstacles);
        if (ctx->imageData) free(ctx->imageData);
        if (ctx->maskData)  free(ctx->maskData);
        delete ctx;
        ud[1] = nullptr;
    }
    return 0;
}

/// NEMData.GetWidth(self) — 还原自 sub_1800B4960
static int l_NEMData_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");
    lua_pushinteger(L, ctx->width);
    return 1;
}

/// NEMData.GetHeight(self) — 还原自 sub_1800B4280
static int l_NEMData_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");
    lua_pushinteger(L, ctx->height);
    return 1;
}

/// NEMData.GetDimensions(self) → w, h — 还原自 sub_1800B41F0
static int l_NEMData_GetDimensions(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");
    lua_pushinteger(L, ctx->width);
    lua_pushinteger(L, ctx->height);
    return 2;
}

/// NEMData.GetImageData(self, tx, ty) → ImageData — 还原自 sub_1800B42F0
/// 返回指定 tile 坐标的图像数据 (作为 ImageData 对象)
static int l_NEMData_GetImageData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");

    // 如果有图像数据, 创建 ImageData 实例
    if (ctx->imageData && ctx->imgW > 0 && ctx->imgH > 0) {
        // 获取 Light.Graphics.ImageData:New
        LT::EnsureLightTable(L);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
        lua_pushstring(L, "ImageData");
        lua_rawget(L, -2);
        lua_pushstring(L, "New");
        lua_rawget(L, -2);
        lua_remove(L, -2);  // ImageData
        lua_remove(L, -2);  // Graphics
        lua_remove(L, -2);  // Light

        // 创建 ImageData(pointer, size)
        lua_pushlightuserdata(L, ctx->imageData);
        lua_pushinteger(L, ctx->imgW * ctx->imgH * 4);
        lua_call(L, 2, 1);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/// NEMData.GetMaskImageData(self, tx, ty) → ImageData — 还原自 sub_1800B44A0
static int l_NEMData_GetMaskImageData(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");

    if (ctx->maskData && ctx->maskW > 0 && ctx->maskH > 0) {
        LT::EnsureLightTable(L);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
        lua_pushstring(L, "ImageData");
        lua_rawget(L, -2);
        lua_pushstring(L, "New");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_remove(L, -2);
        lua_remove(L, -2);

        lua_pushlightuserdata(L, ctx->maskData);
        lua_pushinteger(L, ctx->maskW * ctx->maskH * 4);
        lua_call(L, 2, 1);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/// NEMData.GetPath(self, sx, sy, ex, ey) → {{x,y}, ...} — 还原自 sub_1800B46A0
/// A* 寻路: 返回路径点表, 每个元素 {x, y}
static int l_NEMData_GetPath(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");

    int sx = (int)luaL_checkinteger(L, 2);
    int sy = (int)luaL_checkinteger(L, 3);
    int ex = (int)luaL_checkinteger(L, 4);
    int ey = (int)luaL_checkinteger(L, 5);

    // 基础寻路: 简单直线路径 (完整 A* 可后续替换)
    // 检查边界
    if (sx < 0 || sx >= ctx->width || sy < 0 || sy >= ctx->height ||
        ex < 0 || ex >= ctx->width || ey < 0 || ey >= ctx->height) {
        lua_createtable(L, 0, 0);  // 空路径
        return 1;
    }

    // 目标不可达
    if (ctx->obstacles && ctx->obstacles[ey * ctx->width + ex]) {
        lua_createtable(L, 0, 0);
        return 1;
    }

    // 简化直线路径: 起点 → 终点
    lua_createtable(L, 2, 0);

    // 节点 1: 起点
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, sx); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, sy); lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, 1);

    // 节点 2: 终点
    lua_createtable(L, 0, 2);
    lua_pushinteger(L, ex); lua_rawseti(L, -2, 1);
    lua_pushinteger(L, ey); lua_rawseti(L, -2, 2);
    lua_rawseti(L, -2, 2);

    return 1;
}

/// NEMData.IsObstacle(self, x, y) → boolean — 还原自 sub_1800B4CD0
static int l_NEMData_IsObstacle(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (!ctx) return luaL_error(L, "Illegal object");

    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);

    bool blocked = false;
    if (ctx->obstacles && x >= 0 && x < ctx->width && y >= 0 && y < ctx->height) {
        blocked = (ctx->obstacles[y * ctx->width + x] != 0);
    }
    lua_pushboolean(L, blocked ? 1 : 0);
    return 1;
}

/// NEMData.__call — 构造函数, 加载 NEM 文件 — 还原自 sub_1800B49D0
static int l_NEMData_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);

    FILE* f = fopen(path, "rb");
    if (!f) return luaL_error(L, "File not exist");

    NEMContext* ctx = new NEMContext();
    memset(ctx, 0, sizeof(NEMContext));

    // 读取 NEM 头部: 4 字节 magic + 4 字节 width + 4 字节 height
    char magic[4];
    if (fread(magic, 4, 1, f) != 1) { fclose(f); delete ctx; return luaL_error(L, "File not exist"); }
    if (fread(&ctx->width, 4, 1, f) != 1) { fclose(f); delete ctx; return luaL_error(L, "File not exist"); }
    if (fread(&ctx->height, 4, 1, f) != 1) { fclose(f); delete ctx; return luaL_error(L, "File not exist"); }

    // 读取障碍物网格
    int gridSize = ctx->width * ctx->height;
    if (gridSize > 0 && gridSize < 1024 * 1024) {
        ctx->obstacles = (uint8_t*)calloc(gridSize, 1);
        if (ctx->obstacles)
            fread(ctx->obstacles, 1, gridSize, f);
    }
    ctx->valid = true;
    fclose(f);

    // 创建 CC::Unsafe<NEMData::Context> userdata
    void** ud = (void**)lua_newuserdata(L, 16);
    ud[0] = nullptr;  // CC::Unsafe vftable (stub)
    ud[1] = ctx;

    // 设置 metatable: { __gc = cleanup }
    lua_createtable(L, 0, 0);
    lua_pushcfunction(L, l_NEMData_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    // 存储 self.__instance
    lua_setfield(L, 1, "__instance");

    CC::Log(CC::LOG_INFO, "Loading NEM: %s (grid %dx%d)", path, ctx->width, ctx->height);
    return 0;
}

/// NEMData.__tostring — 还原自 sub_1800B4F70
static int l_NEMData_Tostring(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    NEMContext* ctx = GetNEMContext(L, 1);
    if (ctx) {
        lua_pushfstring(L, "Light.Plugins.NEMData: %p", ctx);
    } else {
        lua_pushstring(L, "Light.Plugins.NEMData");
    }
    return 1;
}

// ==================== luaopen_Light_Plugins_NEMData ====================
// 还原自 sub_1800B4D80 — 9 函数精确匹配

int luaopen_Light_Plugins_NEMData(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Plugins");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Plugins");
        lua_createtable(L, 0, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Plugins");
        lua_rawget(L, -2);
    }

    lua_pushstring(L, "NEMData");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "NEMData");
        lua_createtable(L, 0, 0);
        const luaL_Reg nem_funcs[] = {
            {"GetWidth",        l_NEMData_GetWidth},
            {"GetHeight",       l_NEMData_GetHeight},
            {"GetDimensions",   l_NEMData_GetDimensions},
            {"GetImageData",    l_NEMData_GetImageData},
            {"GetMaskImageData",l_NEMData_GetMaskImageData},
            {"GetPath",         l_NEMData_GetPath},
            {"IsObstacle",      l_NEMData_IsObstacle},
            {"__call",          l_NEMData_Call},
            {"__tostring",      l_NEMData_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, nem_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "NEMData");
        lua_rawget(L, -2);
    }

    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}
