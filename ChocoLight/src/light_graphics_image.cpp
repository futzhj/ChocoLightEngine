/**
 * @file light_graphics_image.cpp
 * @brief Light.Graphics.Image + ImageData + PixelFormat + Font 模块
 * @note 深度还原自 Light.dll IDA 反编译
 *
 * Image API (6 函数, 还原自 luaopen_Light_Graphics_Image):
 *   GetWidth()        — 纹理宽度
 *   GetHeight()       — 纹理高度
 *   GetDepth()        — 纹理位深
 *   GetDimensions()   — 返回 (w, h, depth) 三个值
 *   __call(path_or_data) — 构造函数, 从文件或 ImageData 创建纹理
 *   __tostring()      — "Light.Graphics.Image"
 *   继承自 Drawable
 *
 * ImageData API (7 函数, 还原自 luaopen_Light_Graphics_ImageData):
 *   GetWidth()     — 数据宽度
 *   GetHeight()    — 数据高度
 *   GetDepth()     — 像素位深/通道数
 *   GetPointer()   — 原始像素数据指针
 *   Count()        — 像素总数
 *   __call(w,h)    — 构造函数
 *   __tostring()   — "Light.Graphics.ImageData"
 *
 * Font API (2 函数, 还原自 luaopen_Light_Graphics_Font):
 *   __call(path,size) — 构造函数, 加载字体文件
 *   __tostring()      — "Light.Graphics.Font"
 *
 * PixelFormat (56 常量枚举, 还原自 sub_1800A85B0):
 *   R8UNorm=1 ~ DXT5sRGB=56
 */

#include "light.h"
#include "render_backend.h"
#include <cstring>
#include "stb_image.h"      // 图像解码
#include "stb_truetype.h"   // 字体解码

// ==================== 共享上下文结构 ====================

struct ImageContext {
    unsigned int texId;     // OpenGL 纹理 ID
    int          width;
    int          height;
    int          channels;  // 通道数 (3=RGB, 4=RGBA)
    void*        pixels;    // 原始像素数据 (仅 ImageData 保留)
    int          pixelCount;
};

static ImageContext* GetImageCtx(lua_State* L, int idx) {
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    ImageContext* ctx = (ImageContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

// ==================== Image 函数 ====================

/// @lua_api Light.Graphics.Image.GetWidth
/// @brief 获取图像宽度
/// @return number 宽度(像素)
static int l_Image_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}

/// @lua_api Light.Graphics.Image.GetHeight
/// @brief 获取图像高度
/// @return number 高度(像素)
static int l_Image_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// @lua_api Light.Graphics.Image.GetDepth
/// @brief 获取图像通道数
/// @return number 通道数(3=RGB,4=RGBA)
static int l_Image_GetDepth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 1;
}

/// @lua_api Light.Graphics.Image.GetDimensions
/// @brief 获取图像尺寸和通道数
/// @return number,number,number w,h,depth
static int l_Image_GetDimensions(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 3;
}

/// @lua_api Light.Graphics.Image.GetTextureId
/// @brief 获取原生 GL 纹理 ID (Phase AS.1, 用于 shader:SetTexture)
/// @return number 0=未初始化
static int l_Image_GetTextureId(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? (lua_Integer)ctx->texId : 0);
    return 1;
}

/// @lua_api Light.Graphics.Image.__call
/// @brief 构造函数, 支持两种创建方式
/// @param path string 图像文件路径 (PNG/JPG/BMP/TGA)
/// @return void
/// @overload (w, h, rgba_bytes) 从 RGBA 字节直接构造 (Phase D.x.7)
///   w: number 宽度像素
///   h: number 高度像素
///   rgba_bytes: string 长度必须 = w*h*4 的 RGBA8 字节序列
/// @example
/// local img = Light(Light.Graphics.Image):New("hero.png")
/// local rgba = string.rep(string.char(255, 0, 0, 255), 32*32)  -- 32x32 纯红
/// local img2 = Light(Light.Graphics.Image):New(32, 32, rgba)
static int l_Image_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int argc = lua_gettop(L);

    ImageContext* ctx = (ImageContext*)lua_newuserdata(L, sizeof(ImageContext));
    memset(ctx, 0, sizeof(ImageContext));

    // 分支 1: argc==2 + string => 从文件加载 (现有行为)
    if (argc == 2 && lua_isstring(L, 2)) {
        const char* path = lua_tostring(L, 2);

        // stb_image 解码 (强制 RGBA 4 通道)
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);  // 不翻转 (OpenGL 2D 左上原点)
        unsigned char* pixels = stbi_load(path, &w, &h, &ch, 4);

        if (pixels && w > 0 && h > 0) {
            ctx->width    = w;
            ctx->height   = h;
            ctx->channels = 4;

            // 创建纹理 (通过渲染后端) - 防御 headless/no-GL 环境 g_render==nullptr
            ctx->texId = g_render ? g_render->CreateTexture(w, h, 4, pixels) : 0;

            CC::Log(CC::LOG_INFO, "Image loaded: %s (%dx%d, texId=%u)", path, w, h, ctx->texId);
            stbi_image_free(pixels);
        } else {
            CC::Log(CC::LOG_ERROR, "Failed to load image: %s (%s)", path,
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        }
    }
    // 分支 2: argc==4 + (number, number, string) => 从 RGBA 字节直接构造
    else if (argc == 4 && lua_isnumber(L, 2) && lua_isnumber(L, 3) && lua_isstring(L, 4)) {
        int w = (int)lua_tointeger(L, 2);
        int h = (int)lua_tointeger(L, 3);
        size_t len = 0;
        const char* bytes = lua_tolstring(L, 4, &len);

        if (w > 0 && h > 0 && bytes && len == (size_t)(w * h * 4)) {
            ctx->width    = w;
            ctx->height   = h;
            ctx->channels = 4;
            // 防御 headless/no-GL 环境 g_render==nullptr (smoke 环境常见)
            ctx->texId    = g_render ? g_render->CreateTexture(w, h, 4, (const unsigned char*)bytes) : 0;
            CC::Log(CC::LOG_INFO, "Image from bytes (%dx%d, texId=%u)", w, h, ctx->texId);
        } else {
            CC::Log(CC::LOG_ERROR, "Image from bytes: invalid w=%d h=%d byte_len=%zu (expected %d)",
                     w, h, len, w * h * 4);
        }
    }

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Image.__tostring — 还原自 sub_1800AD8B0
static int l_Image_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.Image");
    return 1;
}

// ==================== ImageData 函数 ====================

/// @lua_api Light.Graphics.ImageData.GetWidth
/// @brief 获取数据宽度
/// @return number
static int l_ImageData_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}

/// @lua_api Light.Graphics.ImageData.GetHeight
/// @brief 获取数据高度
/// @return number
static int l_ImageData_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// @lua_api Light.Graphics.ImageData.GetDepth
/// @brief 获取像素位深/通道数
/// @return number
static int l_ImageData_GetDepth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 1;
}

/// @lua_api Light.Graphics.ImageData.GetPointer
/// @brief 获取原始像素数据指针
/// @return lightuserdata,number 指针,字节数
static int l_ImageData_GetPointer(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    if (ctx && ctx->pixels) {
        lua_pushlightuserdata(L, ctx->pixels);
        lua_pushinteger(L, ctx->pixelCount * ctx->channels);
        return 2;
    }
    lua_pushnil(L);
    return 1;
}

/// @lua_api Light.Graphics.ImageData.Count
/// @brief 获取像素总数
/// @return number
static int l_ImageData_Count(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->pixelCount : 0);
    return 1;
}

/// ImageData.__gc — 析构函数, 释放像素数据
/// 还原自 sub_1800AF020
static int l_ImageData_GC(lua_State* L) {
    ImageContext* ctx = (ImageContext*)lua_touserdata(L, 1);
    if (ctx && ctx->pixels) {
        free(ctx->pixels);
        ctx->pixels = nullptr;
    }
    return 0;
}

/// @lua_api Light.Graphics.ImageData.__call
/// @brief 构造函数 (支持 3 种创建方式)
/// @param filename_or_w string|number 文件名或宽度
/// @return void
/// @note __call(self, filename) — 从文件加载
/// @note __call(self, pointer, size) — 从缓冲区创建
/// @note __call(self, w, h, depth, format) — 指定尺寸创建
static int l_ImageData_Call(lua_State* L) {
    int argc = lua_gettop(L);
    luaL_checktype(L, 1, LUA_TTABLE);

    void* bufPtr = nullptr;
    size_t bufSize = 0;
    int w = 0, h = 0, depth = 0, format = 0;
    const char* filename = nullptr;

    switch (argc) {
    case 2:  // 文件名
        filename = luaL_checkstring(L, 2);
        break;
    case 3: {
        // 缓冲区指针 + 大小 (IDA: 检查 cdata/userdata 类型)
        const char* typeName = lua_typename(L, lua_type(L, 2));
        // 仅接受 cdata 或 userdata, 否则报错
        if (strcmp(typeName, "cdata") != 0 && strcmp(typeName, "userdata") != 0) {
            luaL_error(L, "Buffer should be cdata or userdata, but give: %s", typeName);
        }
        bufPtr = (void*)lua_topointer(L, 2);
        bufSize = (size_t)luaL_checkinteger(L, 3);
        break;
    }
    case 5:  // w, h, depth, format
        w = (int)luaL_checkinteger(L, 2);
        h = (int)luaL_checkinteger(L, 3);
        depth = (int)luaL_checkinteger(L, 4);
        format = (int)luaL_checkinteger(L, 5);
        break;
    }

    // 创建 64 字节 userdata (匹配 IDA: lua_newuserdata(L, 64))
    ImageContext* ctx = (ImageContext*)lua_newuserdata(L, 64);
    memset(ctx, 0, 64);

    // 设置 __gc 元表 (IDA: lua_createtable → pushcclosure(__gc) → setmetatable)
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_ImageData_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    lua_setfield(L, 1, "__instance");

    // 根据参数类型初始化像素数据 (sub_180090CC0 / sub_180090D50 / sub_180090E20)
    int extra = argc - 2;
    if (extra == 0) {
        // 从文件名加载 (sub_180090CC0)
        if (filename) {
            CC::Log(CC::LOG_INFO, "ImageData: loading '%s'", filename);
        }
    } else if (extra == 1) {
        // 从缓冲区创建 (sub_180090D50)
        ctx->pixels = bufPtr;
        ctx->pixelCount = (int)bufSize;
    } else if (extra == 3) {
        // 指定尺寸创建 (sub_180090E20)
        ctx->width = w;
        ctx->height = h;
        ctx->channels = depth;
        ctx->pixelCount = w * h;
    } else {
        CC::Log(CC::LOG_ERROR, "ImageData: Unknown parameters");
    }

    return 1;
}

/// ImageData.__tostring — 还原自 sub_1800AE230
static int l_ImageData_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.ImageData");
    return 1;
}

// ==================== Font 函数 ====================

// 单个字形信息 — UV 坐标和排版指标
struct GlyphInfo {
    float u0, v0, u1, v1;  // 图集 UV
    float xoff, yoff;      // 位置偏移 (相对基线)
    float width, height;   // 像素尺寸
    float xadvance;        // 水平步进
};

// Font 上下文结构 — Unicode 动态字形缓存
struct FontContext {
    unsigned int    texId;              // 字形图集 GL 纹理 ID
    int             atlasW, atlasH;     // 图集尺寸
    float           fontSize;           // 像素大小
    unsigned char*  ttfBuffer;          // TTF 文件原始数据 (保持有效用于动态烘焙)
    stbtt_fontinfo  fontInfo;           // stb_truetype 字体信息
    unsigned char*  atlasBitmap;        // 图集位图 (用于动态追加字形)
    int             cursorX, cursorY;   // 图集写入光标
    int             rowHeight;          // 当前行最大字形高度
    float           scale;              // 像素缩放因子
    float           ascent;             // 字体上升值

    // 字形缓存: codepoint → GlyphInfo
    // 使用 C 数组实现简单哈希表 (避免 STL 跨 DLL 问题)
    static const int CACHE_SIZE = 4096;  // 最多缓存 4096 个字形
    int       cacheKeys[CACHE_SIZE];
    GlyphInfo cacheVals[CACHE_SIZE];
    int       cacheCount;

    // 查找字形缓存
    GlyphInfo* FindGlyph(int codepoint) {
        for (int i = 0; i < cacheCount; ++i) {
            if (cacheKeys[i] == codepoint) return &cacheVals[i];
        }
        return nullptr;
    }

    // 动态烘焙单个字形到图集 → 返回 GlyphInfo
    GlyphInfo* BakeGlyph(int codepoint) {
        if (cacheCount >= CACHE_SIZE) return nullptr;

        int gw, gh, xoff, yoff;
        unsigned char* bmp = stbtt_GetCodepointBitmap(
            &fontInfo, 0, scale, codepoint, &gw, &gh, &xoff, &yoff);
        if (!bmp || gw == 0 || gh == 0) {
            if (bmp) stbtt_FreeBitmap(bmp, nullptr);
            return nullptr;
        }

        // 检查图集空间, 换行或放弃
        if (cursorX + gw + 1 > atlasW) {
            cursorX = 0;
            cursorY += rowHeight + 1;
            rowHeight = 0;
        }
        if (cursorY + gh + 1 > atlasH) {
            stbtt_FreeBitmap(bmp, nullptr);
            return nullptr;  // 图集满了
        }

        // 复制字形位图到图集
        for (int y = 0; y < gh; ++y) {
            memcpy(&atlasBitmap[(cursorY + y) * atlasW + cursorX],
                   &bmp[y * gw], gw);
        }

        // 计算 xadvance
        int advance, lsb;
        stbtt_GetCodepointHMetrics(&fontInfo, codepoint, &advance, &lsb);

        // 存储字形信息
        int idx = cacheCount++;
        cacheKeys[idx] = codepoint;
        GlyphInfo* gi = &cacheVals[idx];
        gi->u0 = (float)cursorX / atlasW;
        gi->v0 = (float)cursorY / atlasH;
        gi->u1 = (float)(cursorX + gw) / atlasW;
        gi->v1 = (float)(cursorY + gh) / atlasH;
        gi->xoff = (float)xoff;
        gi->yoff = (float)yoff;
        gi->width = (float)gw;
        gi->height = (float)gh;
        gi->xadvance = advance * scale;

        cursorX += gw + 1;
        if (gh > rowHeight) rowHeight = gh;

        stbtt_FreeBitmap(bmp, nullptr);

        // 更新图集纹理 (通过渲染后端)
        g_render->ReplaceTexture(texId, atlasW, atlasH, 1, atlasBitmap);

        return gi;
    }

    // 获取或烘焙字形
    GlyphInfo* GetGlyph(int codepoint) {
        GlyphInfo* gi = FindGlyph(codepoint);
        return gi ? gi : BakeGlyph(codepoint);
    }
};

/// Font.__gc — 释放 TTF 缓冲区和图集
static int l_Font_GC(lua_State* L) {
    FontContext* fc = (FontContext*)lua_touserdata(L, 1);
    if (fc) {
        if (fc->ttfBuffer) { free(fc->ttfBuffer); fc->ttfBuffer = nullptr; }
        if (fc->atlasBitmap) { free(fc->atlasBitmap); fc->atlasBitmap = nullptr; }
        if (fc->texId && g_render) { g_render->DeleteTexture(fc->texId); fc->texId = 0; }
    }
    return 0;
}

/// @lua_api Light.Graphics.Font.__call
/// @brief 构造函数, 加载 TTF 字体
/// @param path string 字体文件路径 (.ttf)
/// @param size number? 字号 (默认 16)
/// @return void
/// @example
/// local font = Light(Light.Graphics.Font):New("font.ttf", 24)
static int l_Font_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    const char* path = luaL_checkstring(L, 2);
    float size = (float)luaL_optnumber(L, 3, 16.0);

    // 读取 TTF 文件
    FILE* f = fopen(path, "rb");
    if (!f) {
        CC::Log(CC::LOG_ERROR, "Font: cannot open '%s'", path);
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char* ttfBuf = (unsigned char*)malloc(fileSize);
    fread(ttfBuf, 1, fileSize, f);
    fclose(f);

    // 创建 FontContext userdata
    FontContext* fc = (FontContext*)lua_newuserdata(L, sizeof(FontContext));
    memset(fc, 0, sizeof(FontContext));
    fc->fontSize = size;
    fc->atlasW = 1024;
    fc->atlasH = 1024;
    fc->ttfBuffer = ttfBuf;

    // 初始化 stb_truetype 字体信息
    if (!stbtt_InitFont(&fc->fontInfo, ttfBuf, 0)) {
        CC::Log(CC::LOG_ERROR, "Font: stbtt_InitFont failed for '%s'", path);
        free(ttfBuf); fc->ttfBuffer = nullptr;
        lua_setfield(L, 1, "__instance");
        return 0;
    }
    fc->scale = stbtt_ScaleForPixelHeight(&fc->fontInfo, size);
    int iascent, idescent, ilineGap;
    stbtt_GetFontVMetrics(&fc->fontInfo, &iascent, &idescent, &ilineGap);
    fc->ascent = iascent * fc->scale;

    // 分配空图集位图
    fc->atlasBitmap = (unsigned char*)calloc(fc->atlasW * fc->atlasH, 1);

    // 创建图集纹理 (单通道, 通过渲染后端)
    fc->texId = g_render->CreateTexture(fc->atlasW, fc->atlasH, 1, fc->atlasBitmap);

    // 设置 __gc 元表
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Font_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    // 预烘焙 ASCII 常用字符 (32-127)
    int preLoaded = 0;
    for (int cp = 32; cp < 128; ++cp) {
        if (fc->BakeGlyph(cp)) preLoaded++;
    }

    CC::Log(CC::LOG_INFO, "Font loaded: %s (size=%.0f, texId=%u, preloaded=%d, atlas=%dx%d)",
            path, size, fc->texId, preLoaded, fc->atlasW, fc->atlasH);

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Font.__tostring — 还原自 sub_1800AE5E0
static int l_Font_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.Font");
    return 1;
}

/// FontGetGlyph — 跨编译单元字形查询桥接函数
/// 从 FontContext userdata 查找或烘焙字形, 结果写入 FontGlyphResult
void FontGetGlyph(void* fontCtx, int codepoint, FontGlyphResult* out) {
    if (!fontCtx || !out) return;
    out->found = 0;

    FontContext* fc = (FontContext*)fontCtx;
    GlyphInfo* gi = fc->GetGlyph(codepoint);
    if (gi) {
        out->u0 = gi->u0; out->v0 = gi->v0;
        out->u1 = gi->u1; out->v1 = gi->v1;
        out->xoff = gi->xoff; out->yoff = gi->yoff;
        out->width = gi->width; out->height = gi->height;
        out->xadvance = gi->xadvance;
        out->found = 1;
    }
}

// ==================== 辅助: 确保 Graphics 父模块 (sub_1800AD380) ====================

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
}

// ==================== luaopen 注册 ====================

// Image — 6 函数 + Drawable 继承
int luaopen_Light_Graphics_Image(lua_State* L) {
    EnsureGraphicsTable(L);

    lua_pushstring(L, "Image");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Image");

        // 继承 Drawable
        LT::EnsureLightTable(L);
        lua_pushstring(L, "New");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_pushstring(L, "Drawable");
        lua_rawget(L, -4);
        lua_call(L, 1, 1);

        const luaL_Reg img_funcs[] = {
            {"GetWidth",      l_Image_GetWidth},
            {"GetHeight",     l_Image_GetHeight},
            {"GetDepth",      l_Image_GetDepth},
            {"GetDimensions", l_Image_GetDimensions},
            // Phase AS.1 — 与 Canvas:GetTextureId 对称, 供 shader:SetTexture 使用
            {"GetTextureId",  l_Image_GetTextureId},
            {"__call",        l_Image_Call},
            {"__tostring",    l_Image_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, img_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Image");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}

// ImageData — 7 函数 (独立, 不继承 Drawable)
int luaopen_Light_Graphics_ImageData(lua_State* L) {
    EnsureGraphicsTable(L);

    lua_pushstring(L, "ImageData");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "ImageData");
        lua_createtable(L, 0, 0);

        const luaL_Reg id_funcs[] = {
            {"GetWidth",   l_ImageData_GetWidth},
            {"GetHeight",  l_ImageData_GetHeight},
            {"GetDepth",   l_ImageData_GetDepth},
            {"GetPointer", l_ImageData_GetPointer},
            {"Count",      l_ImageData_Count},
            {"__call",     l_ImageData_Call},
            {"__tostring", l_ImageData_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, id_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "ImageData");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}

// Font — 2 函数
int luaopen_Light_Graphics_Font(lua_State* L) {
    EnsureGraphicsTable(L);

    lua_pushstring(L, "Font");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Font");
        lua_createtable(L, 0, 0);

        const luaL_Reg font_funcs[] = {
            {"__call",     l_Font_Call},
            {"__tostring", l_Font_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, font_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Font");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}

// PixelFormat — 56 常量枚举 (精确还原自 sub_1800A85B0)
int luaopen_Light_Graphics_PixelFormat(lua_State* L) {
    EnsureGraphicsTable(L);

    lua_pushstring(L, "PixelFormat");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "PixelFormat");
        lua_createtable(L, 0, 0);

        // 精确还原 sub_1800A85B0 的 56 个常量 (值 1~56)
        struct { const char* name; int value; } formats[] = {
            {"R8UNorm",      1},  {"R8Int",        2},  {"R8UInt",       3},
            {"R16UNorm",     4},  {"R16Float",     5},  {"R16Int",       6},
            {"R16UInt",      7},  {"R32Float",     8},  {"R32Int",       9},
            {"R32UInt",     10},  {"RG8UNorm",    11},  {"RG8Int",      12},
            {"RG8UInt",     13},  {"LA8UNorm",    14},  {"RG16UNorm",   15},
            {"RG16Float",   16},  {"RG16Int",     17},  {"RG16UInt",    18},
            {"RG32Float",   19},  {"RG32Int",     20},  {"RG32UInt",    21},
            {"RGB8UNorm",   22},  {"RGB8Int",     23},  {"RGB16UNorm",  24},
            {"RGB16Int",    25},  {"RGB32Float",  26},  {"RGB32Int",    27},
            {"RGBA8UNorm",  28},  {"RGBA8sRGB",   29},  {"BGRA8UNorm",  30},
            {"BGRA8sRGB",   31},  {"RGBA8Int",    32},  {"RGBA8UInt",   33},
            {"RGBA16UNorm", 34},  {"RGBA16Float", 35},  {"RGBA16Int",   36},
            {"RGBA16UInt",  37},  {"RGBA32Float", 38},  {"RGBA32Int",   39},
            {"RGBA32UInt",  40},  {"RGBA4",       41},  {"RGB5A1",      42},
            {"RGB565",      43},  {"RGB10A2",     44},  {"RG11B10Float",45},
            // DXT/压缩格式 (续)
            {"DXT1UNorm",   46},  {"DXT1sRGB",    47},
            {"DXT3UNorm",   48},  {"DXT3sRGB",    49},
            {"DXT5UNorm",   50},  {"DXT5sRGB",    51},
            // 深度/模板格式
            {"D16UNorm",    52},  {"D24UNorm",    53},
            {"D32Float",    54},  {"D24UNormS8",  55},
            {"D32FloatS8",  56},
        };

        for (auto& f : formats) {
            lua_pushinteger(L, f.value);
            lua_setfield(L, -2, f.name);
        }

        lua_rawset(L, -3);
        lua_pushstring(L, "PixelFormat");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}
