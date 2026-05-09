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
#include "render_backend.h"
#include <cstring>

// ==================== Canvas 上下文 ====================

struct CanvasContext {
    unsigned int fbo;       // OpenGL Framebuffer Object
    unsigned int texture;   // 颜色纹理附着
    unsigned int depthRB;   // 深度 Renderbuffer
    int          width;
    int          height;
};

/// Canvas.__gc — 释放 FBO 资源 (通过渲染后端)
static int l_Canvas_GC(lua_State* L) {
    CanvasContext* ctx = (CanvasContext*)lua_touserdata(L, 1);
    if (ctx && g_render) {
        g_render->DeleteFBO(ctx->fbo, ctx->texture, ctx->depthRB);
        ctx->fbo = 0;
        ctx->texture = 0;
        ctx->depthRB = 0;
    }
    return 0;
}

/// @lua_api Light.Graphics.Canvas.__call
/// @brief 构造函数, 创建离屏画布 (FBO)
/// @param w number 画布宽度
/// @param h number 画布高度
/// @return void
/// @example
/// local canvas = Light(Light.Graphics.Canvas):New(800, 600)
static int l_Canvas_Call(lua_State* L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int w = (int)luaL_checkinteger(L, 2);
    int h = (int)luaL_checkinteger(L, 3);

    CanvasContext* ctx = (CanvasContext*)lua_newuserdata(L, sizeof(CanvasContext));
    memset(ctx, 0, sizeof(CanvasContext));
    ctx->width = w;
    ctx->height = h;

    if (g_render) {
        ctx->fbo = g_render->CreateFBO(w, h, &ctx->texture, &ctx->depthRB);
        if (ctx->fbo) {
            CC::Log(CC::LOG_INFO, "Canvas created: %dx%d (fbo=%u, tex=%u)",
                    w, h, ctx->fbo, ctx->texture);
        } else {
            CC::Log(CC::LOG_ERROR, "Canvas: FBO creation failed");
        }
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

// ==================== Phase AS.1 — Canvas 增强方法 ====================

/// 从 instance table 取出底层 CanvasContext (失败返回 nullptr)
static CanvasContext* GetCanvasCtx(lua_State* L, int idx) {
    if (!lua_istable(L, idx)) return nullptr;
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return nullptr; }
    CanvasContext* ctx = (CanvasContext*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return ctx;
}

/// canvas:GetTextureId() -> int  (返回原生 GL texture id, 0 表示无效)
/// 用法: shader:SetTexture("uTex", canvas:GetTextureId(), 1)
static int l_Canvas_GetTextureId(lua_State* L) {
    CanvasContext* ctx = GetCanvasCtx(L, 1);
    lua_pushinteger(L, ctx ? (lua_Integer)ctx->texture : 0);
    return 1;
}

/// canvas:GetWidth() -> int
static int l_Canvas_GetWidth(lua_State* L) {
    CanvasContext* ctx = GetCanvasCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->width : 0);
    return 1;
}

/// canvas:GetHeight() -> int
static int l_Canvas_GetHeight(lua_State* L) {
    CanvasContext* ctx = GetCanvasCtx(L, 1);
    lua_pushinteger(L, ctx ? ctx->height : 0);
    return 1;
}

/// canvas:Clear([r], [g], [b], [a]) -> ()
/// 注意: 仅在 canvas 当前已 SetCanvas/PushCanvas 为渲染目标时清空生效
/// (调用时绑定 canvas FBO, 清后恢复之前绑定状态)
static int l_Canvas_Clear(lua_State* L) {
    CanvasContext* ctx = GetCanvasCtx(L, 1);
    if (!ctx || !ctx->fbo || !g_render) return 0;

    float r = (float)luaL_optnumber(L, 2, 0.0);
    float g = (float)luaL_optnumber(L, 3, 0.0);
    float b = (float)luaL_optnumber(L, 4, 0.0);
    float a = (float)luaL_optnumber(L, 5, 0.0);

    // 临时绑定 canvas FBO -> 清空 -> 解绑 (恢复默认 framebuffer)
    // 注: 这里不保存当前 viewport, 因为 ClearCurrent 不依赖 viewport
    g_render->BindFBO(ctx->fbo);
    g_render->ClearCurrent(r, g, b, a);
    g_render->UnbindFBO();
    return 0;
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
            {"__call",       l_Canvas_Call},
            {"__tostring",   l_Canvas_Tostring},
            // Phase AS.1 — Canvas 增强 (4 个方法)
            {"GetTextureId", l_Canvas_GetTextureId},
            {"GetWidth",     l_Canvas_GetWidth},
            {"GetHeight",    l_Canvas_GetHeight},
            {"Clear",        l_Canvas_Clear},
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
