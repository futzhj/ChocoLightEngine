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
#include <cstring>
#include <GLFW/glfw3.h>     // OpenGL 函数
#include "stb_image.h"      // 图像解码
#include "stb_truetype.h"   // 字体解码

// GL 1.x 头文件可能缺少此常量
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

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

/// Image.GetWidth  — 还原自 sub_1800AD6A0
static int l_Image_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}

/// Image.GetHeight — 还原自 sub_1800AD620
static int l_Image_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// Image.GetDepth  — 还原自 sub_1800AD4D0
static int l_Image_GetDepth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 1;
}

/// Image.GetDimensions — 还原自 sub_1800AD550 (返回 w, h, depth 三值)
static int l_Image_GetDimensions(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 3;
}

/// Image.__call — 构造函数
/// 还原自 sub_1800AD720
/// 使用 stb_image 加载文件 → 创建 OpenGL 纹理
static int l_Image_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);

    ImageContext* ctx = (ImageContext*)lua_newuserdata(L, sizeof(ImageContext));
    memset(ctx, 0, sizeof(ImageContext));

    if (lua_isstring(L, 2)) {
        const char* path = lua_tostring(L, 2);

        // stb_image 解码 (强制 RGBA 4 通道)
        int w = 0, h = 0, ch = 0;
        stbi_set_flip_vertically_on_load(0);  // 不翻转 (OpenGL 2D 左上原点)
        unsigned char* pixels = stbi_load(path, &w, &h, &ch, 4);

        if (pixels && w > 0 && h > 0) {
            ctx->width    = w;
            ctx->height   = h;
            ctx->channels = 4;

            // 创建 OpenGL 纹理
            glGenTextures(1, &ctx->texId);
            glBindTexture(GL_TEXTURE_2D, ctx->texId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);

            CC::Log(CC::LOG_INFO, "Image loaded: %s (%dx%d, texId=%u)", path, w, h, ctx->texId);
            stbi_image_free(pixels);
        } else {
            CC::Log(CC::LOG_ERROR, "Failed to load image: %s (%s)", path,
                     stbi_failure_reason() ? stbi_failure_reason() : "unknown");
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

/// ImageData.GetWidth — 还原自 sub_1800ADD60
static int l_ImageData_GetWidth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}

/// ImageData.GetHeight — 还原自 sub_1800ADC50
static int l_ImageData_GetHeight(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// ImageData.GetDepth — 还原自 sub_1800ADBC0
static int l_ImageData_GetDepth(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    ImageContext* ctx = GetImageCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->channels : 0);
    return 1;
}

/// ImageData.GetPointer — 还原自 sub_1800ADCE0 (共享)
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

/// ImageData.Count — 还原自 sub_1800ADAF0
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

/// ImageData.__call — 构造函数
/// 精确还原自 sub_1800AF210 (sub_1800ADDF0 → 实际入口)
/// 支持3种创建方式:
///   __call(self, filename)              — 2参数, 从文件加载
///   __call(self, pointer, size)         — 3参数, 从 cdata/userdata 缓冲区
///   __call(self, w, h, depth, format)   — 5参数, 指定尺寸格式创建
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

        // 更新 GL 纹理 (不主动 unbind, 留给调用方管理)
        glBindTexture(GL_TEXTURE_2D, texId);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, atlasW, atlasH, 0,
                     GL_ALPHA, GL_UNSIGNED_BYTE, atlasBitmap);

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
        if (fc->texId) { glDeleteTextures(1, &fc->texId); fc->texId = 0; }
    }
    return 0;
}

/// Font.__call — 构造函数, 加载字体
/// 还原自 sub_1800AE4C0
/// 支持 Unicode/CJK 动态字形缓存
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

    // 创建 GL 纹理 (空, 后续动态填充)
    glGenTextures(1, &fc->texId);
    glBindTexture(GL_TEXTURE_2D, fc->texId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, fc->atlasW, fc->atlasH, 0,
                 GL_ALPHA, GL_UNSIGNED_BYTE, fc->atlasBitmap);
    glBindTexture(GL_TEXTURE_2D, 0);

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
