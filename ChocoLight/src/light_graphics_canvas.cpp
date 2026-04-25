/**
 * @file light_graphics_canvas.cpp
 * @brief Light.Graphics.Canvas 模块 — FBO 离屏渲染画布
 * @note 深度还原自 Light.dll IDA luaopen_Light_Graphics_Canvas (sub_1800A9940)
 *
 * Canvas API (2 函数 + Drawable 继承):
 *   __call(w,h)    — 构造函数, 创建指定尺寸的 FBO
 *   __tostring()   — "Light.Graphics.Canvas"
 *   继承自 Drawable (通过 Light:New 调用 Drawable 基类)
 *
 * Canvas 在 GPU 上创建离屏渲染目标, 可被 Graphics.SetCanvas 设置为
 * 当前渲染目标, Graphics.Draw 可将其作为纹理绘制到屏幕上。
 */

#include "light.h"
#include <cstring>

#ifdef _WIN32
#include <GL/gl.h>
#else
#include <OpenGL/gl.h>
#endif

// ==================== GL FBO 扩展常量 ====================

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER                    0x8D40
#define GL_RENDERBUFFER                   0x8D41
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_DEPTH_COMPONENT16              0x81A5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE                  0x812F
#endif

// ==================== FBO 扩展函数指针 ====================

typedef void (APIENTRY *PFN_glGenFramebuffers)(int, unsigned int*);
typedef void (APIENTRY *PFN_glDeleteFramebuffers)(int, const unsigned int*);
typedef void (APIENTRY *PFN_glBindFramebuffer)(unsigned int, unsigned int);
typedef void (APIENTRY *PFN_glFramebufferTexture2D)(unsigned int, unsigned int, unsigned int, unsigned int, int);
typedef void (APIENTRY *PFN_glGenRenderbuffers)(int, unsigned int*);
typedef void (APIENTRY *PFN_glDeleteRenderbuffers)(int, const unsigned int*);
typedef void (APIENTRY *PFN_glBindRenderbuffer)(unsigned int, unsigned int);
typedef void (APIENTRY *PFN_glRenderbufferStorage)(unsigned int, unsigned int, int, int);
typedef void (APIENTRY *PFN_glFramebufferRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
typedef unsigned int (APIENTRY *PFN_glCheckFramebufferStatus)(unsigned int);

static PFN_glGenFramebuffers          glGenFramebuffers_  = nullptr;
static PFN_glDeleteFramebuffers       glDeleteFramebuffers_ = nullptr;
static PFN_glBindFramebuffer          glBindFramebuffer_  = nullptr;
static PFN_glFramebufferTexture2D     glFramebufferTexture2D_ = nullptr;
static PFN_glGenRenderbuffers         glGenRenderbuffers_ = nullptr;
static PFN_glDeleteRenderbuffers      glDeleteRenderbuffers_ = nullptr;
static PFN_glBindRenderbuffer         glBindRenderbuffer_ = nullptr;
static PFN_glRenderbufferStorage      glRenderbufferStorage_ = nullptr;
static PFN_glFramebufferRenderbuffer  glFramebufferRenderbuffer_ = nullptr;
static PFN_glCheckFramebufferStatus   glCheckFramebufferStatus_ = nullptr;
static bool fboLoaded = false;

/// 延迟加载 FBO 扩展函数 (通过 wglGetProcAddress)
static bool LoadFBOFunctions() {
    if (fboLoaded) return glGenFramebuffers_ != nullptr;
    fboLoaded = true;
#ifdef _WIN32
    glGenFramebuffers_         = (PFN_glGenFramebuffers)wglGetProcAddress("glGenFramebuffers");
    glDeleteFramebuffers_      = (PFN_glDeleteFramebuffers)wglGetProcAddress("glDeleteFramebuffers");
    glBindFramebuffer_         = (PFN_glBindFramebuffer)wglGetProcAddress("glBindFramebuffer");
    glFramebufferTexture2D_    = (PFN_glFramebufferTexture2D)wglGetProcAddress("glFramebufferTexture2D");
    glGenRenderbuffers_        = (PFN_glGenRenderbuffers)wglGetProcAddress("glGenRenderbuffers");
    glDeleteRenderbuffers_     = (PFN_glDeleteRenderbuffers)wglGetProcAddress("glDeleteRenderbuffers");
    glBindRenderbuffer_        = (PFN_glBindRenderbuffer)wglGetProcAddress("glBindRenderbuffer");
    glRenderbufferStorage_     = (PFN_glRenderbufferStorage)wglGetProcAddress("glRenderbufferStorage");
    glFramebufferRenderbuffer_ = (PFN_glFramebufferRenderbuffer)wglGetProcAddress("glFramebufferRenderbuffer");
    glCheckFramebufferStatus_  = (PFN_glCheckFramebufferStatus)wglGetProcAddress("glCheckFramebufferStatus");
#endif
    bool ok = glGenFramebuffers_ && glDeleteFramebuffers_ && glBindFramebuffer_ &&
              glFramebufferTexture2D_ && glCheckFramebufferStatus_;
    if (!ok) CC::Log(CC::LOG_ERROR, "Canvas: FBO extension not available");
    return ok;
}

// ==================== Canvas 上下文 ====================

struct CanvasContext {
    unsigned int fbo;       // OpenGL Framebuffer Object
    unsigned int texture;   // 颜色纹理附着
    unsigned int depthRB;   // 深度 Renderbuffer
    int          width;
    int          height;
};

/// Canvas.__gc — 释放 FBO 资源
static int l_Canvas_GC(lua_State* L) {
    CanvasContext* ctx = (CanvasContext*)lua_touserdata(L, 1);
    if (ctx) {
        if (ctx->fbo && glDeleteFramebuffers_) {
            glDeleteFramebuffers_(1, &ctx->fbo);
            ctx->fbo = 0;
        }
        if (ctx->texture) {
            glDeleteTextures(1, &ctx->texture);
            ctx->texture = 0;
        }
        if (ctx->depthRB && glDeleteRenderbuffers_) {
            glDeleteRenderbuffers_(1, &ctx->depthRB);
            ctx->depthRB = 0;
        }
    }
    return 0;
}

/// Canvas.__call — 构造函数, 创建 FBO
/// 还原自 sub_1800A9610
static int l_Canvas_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);

    CanvasContext* ctx = (CanvasContext*)lua_newuserdata(L, sizeof(CanvasContext));
    memset(ctx, 0, sizeof(CanvasContext));
    ctx->width = w;
    ctx->height = h;

    if (LoadFBOFunctions()) {
        // 创建颜色纹理
        glGenTextures(1, &ctx->texture);
        glBindTexture(GL_TEXTURE_2D, ctx->texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);

        // 创建 FBO 并附着纹理
        glGenFramebuffers_(1, &ctx->fbo);
        glBindFramebuffer_(GL_FRAMEBUFFER, ctx->fbo);
        glFramebufferTexture2D_(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                GL_TEXTURE_2D, ctx->texture, 0);

        // 创建深度 Renderbuffer (用于 3D 场景深度测试)
        if (glGenRenderbuffers_ && glBindRenderbuffer_ &&
            glRenderbufferStorage_ && glFramebufferRenderbuffer_) {
            glGenRenderbuffers_(1, &ctx->depthRB);
            glBindRenderbuffer_(GL_RENDERBUFFER, ctx->depthRB);
            glRenderbufferStorage_(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
            glFramebufferRenderbuffer_(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                       GL_RENDERBUFFER, ctx->depthRB);
            glBindRenderbuffer_(GL_RENDERBUFFER, 0);
        }

        // 检查完整性
        unsigned int status = glCheckFramebufferStatus_(GL_FRAMEBUFFER);
        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "Canvas: FBO incomplete (status=0x%X)", status);
        } else {
            CC::Log(CC::LOG_INFO, "Canvas created: %dx%d (fbo=%u, tex=%u)",
                    w, h, ctx->fbo, ctx->texture);
        }

        // 恢复默认 framebuffer
        glBindFramebuffer_(GL_FRAMEBUFFER, 0);
    }

    // 设置 __gc 元表 (释放 GPU 资源)
    lua_createtable(L, 0, 1);
    lua_pushcfunction(L, l_Canvas_GC);
    lua_setfield(L, -2, "__gc");
    lua_setmetatable(L, -2);

    lua_setfield(L, 1, "__instance");
    return 0;
}

/// Canvas.__tostring
/// 还原自 sub_1800A98F0
static int l_Canvas_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics.Canvas");
    return 1;
}

// Canvas 继承 Drawable — 匹配 sub_1800A9940 注册模式
int luaopen_Light_Graphics_Canvas(lua_State* L) {
    // 确保 Graphics 父模块 (sub_1800AD380)
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

    lua_pushstring(L, "Canvas");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Canvas");

        // 构建继承链: Canvas = Light(Graphics):New("Drawable")
        LT::EnsureLightTable(L);
        lua_pushstring(L, "New");
        lua_rawget(L, -2);
        lua_remove(L, -2);
        lua_pushstring(L, "Drawable");
        lua_rawget(L, -4);
        lua_call(L, 1, 1);

        const luaL_Reg canvas_funcs[] = {
            {"__call",     l_Canvas_Call},
            {"__tostring", l_Canvas_Tostring},
            {NULL, NULL}
        };
        luaL_setfuncs(L, canvas_funcs, 0);
        lua_rawset(L, -3);
        lua_pushstring(L, "Canvas");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    lua_remove(L, -2);
    return 1;
}
