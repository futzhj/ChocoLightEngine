/**
 * @file light_graphics.cpp
 * @brief Light.Graphics 模块 — 真实 OpenGL 2D/3D 图形绘制
 * @note 深度还原自 Light.dll IDA 反编译 + OpenGL 实现
 *
 * 完整 API (21 函数 + 2 元方法):
 *   绘图: Draw, DrawQuad, Print, Line, Triangle, Rectangle,
 *          RoundedRectangle, Quad, Polygon, Arc, Circle
 *   变换: Translate, Rotate, Scale, Push, Pop
 *   颜色: SetColor, GetColor
 *   画布: SetCanvas, GetCanvas
 *   裁剪: SetScissor, GetScissor
 *   元:   __call, __tostring
 *   常量: LineMode=1, FillMode=2, Drawable={} 空表
 *
 * 调用约定 (由 main.lua 确认):
 *   - 几何基元: Graphics.Func(args...) — dot 语法, 无 self
 *   - Line: (x1,y1,z1, x2,y2,z2, rx,ry,rz, sx,sy,sz, ox,oy,oz)
 *   - Triangle/Rect/Quad/Arc/Circle: (mode, coords..., rx,ry,rz, sx,sy,sz, ox,oy,oz)
 *   - Polygon: (mode, x1,y1,z1, x2,y2,z2, ...)
 *   - Draw: (drawable, x,y,z, ...) — 第一个参数是 userdata
 *   - Print: (text, font, x,y,z, ...)
 */

#include "light.h"
#include <cmath>
#include <cstring>

// OpenGL 1.x/2.x 头文件 (由 GLFW 间接包含或直接引���)
#include <GLFW/glfw3.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ==================== 图形渲染上下文 ====================

struct GraphicsContext {
    float clearColor[4];    // 清屏颜色
    float drawColor[4];     // 当前绘制颜色 RGBA
    int   drawMode;         // 1=Line, 2=Fill
    void* currentCanvas;    // 当前 FBO Canvas
    int   scissor[4];       // 裁剪区域 x,y,w,h
    bool  scissorEnabled;
};

static GraphicsContext g_ctx = {
    {0.0f, 0.0f, 0.0f, 1.0f},   // clearColor: 黑色
    {1.0f, 1.0f, 1.0f, 1.0f},   // drawColor: 白色
    2,                            // FillMode
    nullptr,
    {0, 0, 0, 0},
    false
};

// ==================== 辅助: 变换组应用 ====================
// main.lua 中几何基元的后 9 个参数: rx,ry,rz, sx,sy,sz, ox,oy,oz
// origin → translate → rotate → scale

static void ApplyTransform(float rx, float ry, float rz,
                           float sx, float sy, float sz,
                           float ox, float oy, float oz) {
    if (ox != 0 || oy != 0 || oz != 0)
        glTranslatef(-ox, -oy, -oz);
    if (rx != 0) glRotatef(rx, 1, 0, 0);
    if (ry != 0) glRotatef(ry, 0, 1, 0);
    if (rz != 0) glRotatef(rz, 0, 0, 1);
    if (sx != 1 || sy != 1 || sz != 1)
        glScalef(sx, sy, sz);
}

// ��� Lua 栈的 base 偏移读取 9 个变换参数 (rx,ry,rz,sx,sy,sz,ox,oy,oz)
static void ReadTransform(lua_State* L, int base,
                          float* rx, float* ry, float* rz,
                          float* sx, float* sy, float* sz,
                          float* ox, float* oy, float* oz) {
    *rx = (float)luaL_optnumber(L, base,     0.0);
    *ry = (float)luaL_optnumber(L, base + 1, 0.0);
    *rz = (float)luaL_optnumber(L, base + 2, 0.0);
    *sx = (float)luaL_optnumber(L, base + 3, 1.0);
    *sy = (float)luaL_optnumber(L, base + 4, 1.0);
    *sz = (float)luaL_optnumber(L, base + 5, 1.0);
    *ox = (float)luaL_optnumber(L, base + 6, 0.0);
    *oy = (float)luaL_optnumber(L, base + 7, 0.0);
    *oz = (float)luaL_optnumber(L, base + 8, 0.0);
}

// 设置 GL 颜色为当前 drawColor
static void ApplyDrawColor() {
    glColor4f(g_ctx.drawColor[0], g_ctx.drawColor[1],
              g_ctx.drawColor[2], g_ctx.drawColor[3]);
}

// ==================== 变换矩阵操作 ====================

/// Graphics.Push() — glPushMatrix
static int l_Push(lua_State* L) {
    glPushMatrix();
    return 0;
}

/// Graphics.Pop() — glPopMatrix
static int l_Pop(lua_State* L) {
    glPopMatrix();
    return 0;
}

/// Graphics.Translate(x, y, z)
static int l_Translate(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float z = (float)luaL_optnumber(L, 3, 0.0);
    glTranslatef(x, y, z);
    return 0;
}

/// Graphics.Rotate(angle, x, y, z)
static int l_Rotate(lua_State* L) {
    float angle = (float)luaL_checknumber(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 1.0);
    glRotatef(angle, x, y, z);
    return 0;
}

/// Graphics.Scale(sx, sy, sz)
static int l_Scale(lua_State* L) {
    float sx = (float)luaL_checknumber(L, 1);
    float sy = (float)luaL_optnumber(L, 2, sx);
    float sz = (float)luaL_optnumber(L, 3, 1.0);
    glScalef(sx, sy, sz);
    return 0;
}

// ==================== 颜色 / 画布 / 裁剪 ====================

/// Graphics.SetColor(r, g, b, a) — 还原自 sub_1800ACF10
static int l_SetColor(lua_State* L) {
    g_ctx.drawColor[0] = (float)luaL_checknumber(L, 1);
    g_ctx.drawColor[1] = (float)luaL_checknumber(L, 2);
    g_ctx.drawColor[2] = (float)luaL_checknumber(L, 3);
    g_ctx.drawColor[3] = (float)luaL_optnumber(L, 4, 1.0);
    glColor4f(g_ctx.drawColor[0], g_ctx.drawColor[1],
              g_ctx.drawColor[2], g_ctx.drawColor[3]);
    return 0;
}

/// Graphics.GetColor() — 返回 r, g, b, a
static int l_GetColor(lua_State* L) {
    lua_pushnumber(L, g_ctx.drawColor[0]);
    lua_pushnumber(L, g_ctx.drawColor[1]);
    lua_pushnumber(L, g_ctx.drawColor[2]);
    lua_pushnumber(L, g_ctx.drawColor[3]);
    return 4;
}

/// Graphics.SetCanvas(canvas) — 设置当前渲染目标为 Canvas FBO
/// canvas 为 nil: 恢复默认 framebuffer + 恢复窗口 viewport
/// canvas 为 Canvas 表: 提取 CanvasContext, 绑定 FBO + 设置 Canvas viewport
static int l_SetCanvas(lua_State* L) {
    // 延迟加载 glBindFramebuffer
    typedef void (APIENTRY *PFN_glBindFramebuffer)(unsigned int, unsigned int);
    static PFN_glBindFramebuffer glBindFB = nullptr;
    static bool loaded = false;
    static int savedViewport[4] = {0, 0, 800, 600};  // 保存窗口 viewport
    if (!loaded) {
        loaded = true;
#ifdef _WIN32
        glBindFB = (PFN_glBindFramebuffer)wglGetProcAddress("glBindFramebuffer");
#endif
    }

    if (lua_isnoneornil(L, 1)) {
        // 恢复默认 framebuffer + 窗口 viewport
        if (glBindFB) glBindFB(0x8D40 /*GL_FRAMEBUFFER*/, 0);
        glViewport(savedViewport[0], savedViewport[1],
                   savedViewport[2], savedViewport[3]);
        g_ctx.currentCanvas = nullptr;
    } else if (lua_istable(L, 1)) {
        // 保存当前 viewport (用于恢复)
        glGetIntegerv(GL_VIEWPORT, savedViewport);

        // 提取 Canvas.__instance → CanvasContext { fbo, texture, depthRB, w, h }
        lua_getfield(L, 1, "__instance");
        if (lua_isuserdata(L, -1)) {
            struct CanvasCtx { unsigned int fbo, texture, depthRB; int w, h; };
            CanvasCtx* cc = (CanvasCtx*)lua_touserdata(L, -1);
            if (cc && cc->fbo && glBindFB) {
                glBindFB(0x8D40 /*GL_FRAMEBUFFER*/, cc->fbo);
                glViewport(0, 0, cc->w, cc->h);
                g_ctx.currentCanvas = cc;
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

/// Graphics.GetCanvas() — 返回当前 Canvas 或 nil
static int l_GetCanvas(lua_State* L) {
    if (g_ctx.currentCanvas) {
        lua_pushlightuserdata(L, g_ctx.currentCanvas);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/// Graphics.SetScissor(x, y, w, h)
static int l_SetScissor(lua_State* L) {
    if (lua_gettop(L) >= 4) {
        g_ctx.scissor[0] = (int)luaL_checkinteger(L, 1);
        g_ctx.scissor[1] = (int)luaL_checkinteger(L, 2);
        g_ctx.scissor[2] = (int)luaL_checkinteger(L, 3);
        g_ctx.scissor[3] = (int)luaL_checkinteger(L, 4);
        g_ctx.scissorEnabled = true;
        glEnable(GL_SCISSOR_TEST);
        glScissor(g_ctx.scissor[0], g_ctx.scissor[1],
                  g_ctx.scissor[2], g_ctx.scissor[3]);
    } else {
        g_ctx.scissorEnabled = false;
        glDisable(GL_SCISSOR_TEST);
    }
    return 0;
}

/// Graphics.GetScissor() — 返回 x, y, w, h
static int l_GetScissor(lua_State* L) {
    lua_pushinteger(L, g_ctx.scissor[0]);
    lua_pushinteger(L, g_ctx.scissor[1]);
    lua_pushinteger(L, g_ctx.scissor[2]);
    lua_pushinteger(L, g_ctx.scissor[3]);
    return 4;
}

// ==================== 绘图基元 ====================

// 辅助: 从 Lua drawable 表中获取 GL 纹理信息
// 支持两种布局:
//   ImageContext:  { texId, width, height, ... }
//   CanvasContext: { fbo, texture, depthRB, width, height }
static bool GetDrawableTexture(lua_State* L, int idx,
                               unsigned int* texId, int* w, int* h) {
    if (!lua_istable(L, idx)) return false;
    lua_getfield(L, idx, "__instance");
    if (!lua_isuserdata(L, -1)) { lua_pop(L, 1); return false; }

    unsigned int* raw = (unsigned int*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (!raw) return false;

    // 检测是否是 Canvas: Canvas 有 fbo 字段 (非零), 纹理在第二个 uint
    // 通过检查 __instance 的 metatable 中是否有 __gc 来区分没有意义,
    // 改用启发式: 检查 Lua 表是否有 Canvas 标记字段
    lua_getfield(L, idx, "__tostring");
    bool isCanvas = false;
    if (lua_isfunction(L, -1)) {
        lua_pushvalue(L, idx);
        lua_call(L, 1, 1);
        const char* name = lua_tostring(L, -1);
        if (name && strstr(name, "Canvas")) isCanvas = true;
        lua_pop(L, 1);
    } else {
        lua_pop(L, 1);
    }

    if (isCanvas) {
        // CanvasContext 布局: { fbo[0], texture[1], depthRB[2], w(int)[3], h(int)[4] }
        if (raw[1] == 0) return false;  // 无纹理
        *texId = raw[1];
        *w = (int)raw[3];
        *h = (int)raw[4];
    } else {
        // ImageContext 布局: { texId[0], width[1], height[2] }
        if (raw[0] == 0) return false;
        *texId = raw[0];
        *w = (int)raw[1];
        *h = (int)raw[2];
    }
    return true;
}

/// Graphics.Draw(drawable, x, y, z, [rx,ry,rz, sx,sy,sz, ox,oy,oz])
/// 绘制纹理/图像 — 还原自 sub_1800A9AB0
static int l_Draw(lua_State* L) {
    unsigned int texId = 0;
    int imgW = 64, imgH = 64;
    bool hasTex = GetDrawableTexture(L, 1, &texId, &imgW, &imgH);

    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 0.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 5, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    glTranslatef(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    float fw = (float)imgW, fh = (float)imgH;

    if (hasTex) {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texId);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 0); glVertex3f(0,  0,  0);
        glTexCoord2f(1, 0); glVertex3f(fw, 0,  0);
        glTexCoord2f(1, 1); glVertex3f(fw, fh, 0);
        glTexCoord2f(0, 1); glVertex3f(0,  fh, 0);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    } else {
        // 无纹理时绘制占位矩形
        glBegin(GL_QUADS);
        glVertex3f(0, 0, 0); glVertex3f(fw, 0, 0);
        glVertex3f(fw, fh, 0); glVertex3f(0, fh, 0);
        glEnd();
    }
    glPopMatrix();
    return 0;
}

/// Graphics.DrawQuad(drawable, x, y, z, qx, qy, qw, qh, [rx...])
/// 绘制纹理子区域 — 还原自 sub_1800A9DD0
static int l_DrawQuad(lua_State* L) {
    unsigned int texId = 0;
    int imgW = 64, imgH = 64;
    bool hasTex = GetDrawableTexture(L, 1, &texId, &imgW, &imgH);

    float x  = (float)luaL_optnumber(L, 2, 0.0);
    float y  = (float)luaL_optnumber(L, 3, 0.0);
    float z  = (float)luaL_optnumber(L, 4, 0.0);
    float qx = (float)luaL_optnumber(L, 5, 0.0);
    float qy = (float)luaL_optnumber(L, 6, 0.0);
    float qw = (float)luaL_optnumber(L, 7, 64.0);
    float qh = (float)luaL_optnumber(L, 8, 64.0);

    glPushMatrix();
    glTranslatef(x, y, z);
    ApplyDrawColor();

    if (hasTex) {
        // 子区域 UV 坐标
        float u0 = qx / imgW, v0 = qy / imgH;
        float u1 = (qx + qw) / imgW, v1 = (qy + qh) / imgH;
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texId);
        glBegin(GL_QUADS);
        glTexCoord2f(u0, v0); glVertex3f(0,  0,  0);
        glTexCoord2f(u1, v0); glVertex3f(qw, 0,  0);
        glTexCoord2f(u1, v1); glVertex3f(qw, qh, 0);
        glTexCoord2f(u0, v1); glVertex3f(0,  qh, 0);
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    } else {
        glBegin(GL_QUADS);
        glVertex3f(0, 0, 0); glVertex3f(qw, 0, 0);
        glVertex3f(qw, qh, 0); glVertex3f(0, qh, 0);
        glEnd();
    }
    glPopMatrix();
    return 0;
}

/// UTF-8 解码: 从字节流中读取一个 Unicode 码点
/// 返回码点, 并更新指针 p 到下一个字符位置
static int DecodeUTF8(const char** p) {
    const unsigned char* s = (const unsigned char*)*p;
    int cp = 0;
    if (s[0] < 0x80) {
        cp = s[0]; *p += 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F);
        *p += 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
        *p += 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
             ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
        *p += 4;
    } else {
        *p += 1;  // 无效字节, 跳过
    }
    return cp;
}

/// Graphics.Print(text, font, x, y, z, [rx,ry,rz, sx,sy,sz, ox,oy,oz])
/// 文字渲染 — 还原自 sub_1800AA170
/// 支持 Unicode/CJK: UTF-8 解码 + FontGetGlyph 动态字形查询
/// 两遍策略: 第一遍预烘焙所有字形 (glTexImage2D 必须在 glBegin 外),
///           第二遍一次性渲染所有四边形
static int l_Print(lua_State* L) {
    const char* text = luaL_checkstring(L, 1);
    float x = (float)luaL_optnumber(L, 3, 0.0);
    float y = (float)luaL_optnumber(L, 4, 0.0);
    float z = (float)luaL_optnumber(L, 5, 0.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 6, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    // 获取 FontContext: texId 在 offset 0, fontSize 在 offset 12
    struct FontCtxHeader { unsigned int texId; int atlasW, atlasH; float fontSize; };
    FontCtxHeader* fch = nullptr;
    void* rawFontCtx = nullptr;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "__instance");
        if (lua_isuserdata(L, -1)) {
            rawFontCtx = lua_touserdata(L, -1);
            fch = (FontCtxHeader*)rawFontCtx;
        }
        lua_pop(L, 1);
    }

    glPushMatrix();
    glTranslatef(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    if (fch && fch->texId && rawFontCtx) {
        float baseLine = fch->fontSize;

        // ====== 第一遍: 预烘焙所有字形 (必须在 glBegin 外调用) ======
        {
            const char* p = text;
            while (*p) {
                int cp = DecodeUTF8(&p);
                if (cp <= 0) continue;
                FontGlyphResult gr;
                FontGetGlyph(rawFontCtx, cp, &gr);
                // 触发 BakeGlyph → glTexImage2D, 不关心返回值
            }
        }

        // ====== 第二遍: 渲染所有字形四边形 ======
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, fch->texId);

        float cx = 0;
        const char* p = text;
        glBegin(GL_QUADS);
        while (*p) {
            int cp = DecodeUTF8(&p);
            if (cp <= 0) continue;

            // 此时所有字形已在第一遍中烘焙, 不会触发 glTexImage2D
            FontGlyphResult gr;
            FontGetGlyph(rawFontCtx, cp, &gr);

            if (!gr.found) {
                cx += fch->fontSize * 0.6f;
                continue;
            }

            float gx0 = cx + gr.xoff;
            float gy0 = baseLine + gr.yoff;
            float gx1 = gx0 + gr.width;
            float gy1 = gy0 + gr.height;

            glTexCoord2f(gr.u0, gr.v0); glVertex3f(gx0, gy0, 0);
            glTexCoord2f(gr.u1, gr.v0); glVertex3f(gx1, gy0, 0);
            glTexCoord2f(gr.u1, gr.v1); glVertex3f(gx1, gy1, 0);
            glTexCoord2f(gr.u0, gr.v1); glVertex3f(gx0, gy1, 0);
            cx += gr.xadvance;
        }
        glEnd();
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    } else {
        // 无字体时绘制占位矩形
        float tw = (float)(strlen(text) * 10);
        glBegin(GL_QUADS);
        glVertex3f(0, 0, 0); glVertex3f(tw, 0, 0);
        glVertex3f(tw, 18, 0); glVertex3f(0, 18, 0);
        glEnd();
    }
    glPopMatrix();
    return 0;
}

/// Graphics.Line(x1,y1,z1, x2,y2,z2, [rx,ry,rz, sx,sy,sz, ox,oy,oz])
/// 直线 — 15个参数, 无 mode, 无 self
static int l_Line(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float z1 = (float)luaL_checknumber(L, 3);
    float x2 = (float)luaL_checknumber(L, 4);
    float y2 = (float)luaL_checknumber(L, 5);
    float z2 = (float)luaL_checknumber(L, 6);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 7, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    glBegin(GL_LINES);
    glVertex3f(x1, y1, z1);
    glVertex3f(x2, y2, z2);
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Triangle(mode, x1,y1,z1, x2,y2,z2, x3,y3,z3, [rx...])
/// 三角形 — mode 是 FillMode(2) 或 LineMode(1)
static int l_Triangle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float v[9];
    for (int i = 0; i < 9; ++i)
        v[i] = (float)luaL_checknumber(L, 2 + i);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 11, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    glBegin(mode == 1 ? GL_LINE_LOOP : GL_TRIANGLES);
    glVertex3f(v[0], v[1], v[2]);
    glVertex3f(v[3], v[4], v[5]);
    glVertex3f(v[6], v[7], v[8]);
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Rectangle(mode, x,y,z, w,h,d, [rx...])
static int l_Rectangle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    float w = (float)luaL_checknumber(L, 5);
    float h = (float)luaL_checknumber(L, 6);
    float d = (float)luaL_optnumber(L, 7, 0.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 8, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    glTranslatef(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    glBegin(mode == 1 ? GL_LINE_LOOP : GL_QUADS);
    glVertex3f(0, 0, 0);
    glVertex3f(w, 0, 0);
    glVertex3f(w, h, d);
    glVertex3f(0, h, d);
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.RoundedRectangle(mode, x,y,z, w,h, r, segments, [rx...])
static int l_RoundedRectangle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float x = (float)luaL_checknumber(L, 2);
    float y = (float)luaL_checknumber(L, 3);
    float z = (float)luaL_checknumber(L, 4);
    float w = (float)luaL_checknumber(L, 5);
    float h = (float)luaL_checknumber(L, 6);
    float r = (float)luaL_optnumber(L, 7, 5.0);
    int segs = (int)luaL_optinteger(L, 8, 8);

    float rx2, ry2, rz2, sx2, sy2, sz2, ox2, oy2, oz2;
    ReadTransform(L, 9, &rx2, &ry2, &rz2, &sx2, &sy2, &sz2, &ox2, &oy2, &oz2);

    glPushMatrix();
    glTranslatef(x, y, z);
    ApplyTransform(rx2, ry2, rz2, sx2, sy2, sz2, ox2, oy2, oz2);
    ApplyDrawColor();

    // 圆角矩形由 4 段弧 + 4 条直线组成
    GLenum glMode = (mode == 1) ? GL_LINE_LOOP : GL_TRIANGLE_FAN;
    glBegin(glMode);
    if (mode == 2) glVertex3f(w / 2, h / 2, 0);  // 中心点 (扇形)

    auto emitArc = [&](float cx, float cy, float startDeg, float endDeg) {
        for (int i = 0; i <= segs; ++i) {
            float t = startDeg + (endDeg - startDeg) * i / segs;
            float rad = (float)(t * M_PI / 180.0);
            glVertex3f(cx + r * cosf(rad), cy + r * sinf(rad), 0);
        }
    };
    emitArc(w - r, r,     -90.0f, 0.0f);    // 右上
    emitArc(w - r, h - r,  0.0f,  90.0f);   // 右下
    emitArc(r,     h - r,  90.0f, 180.0f);  // 左下
    emitArc(r,     r,      180.0f, 270.0f); // 左上
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Quad(mode, x1..z1, x2..z2, x3..z3, x4..z4, [rx...])
static int l_Quad(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float v[12];
    for (int i = 0; i < 12; ++i)
        v[i] = (float)luaL_checknumber(L, 2 + i);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 14, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    glBegin(mode == 1 ? GL_LINE_LOOP : GL_QUADS);
    glVertex3f(v[0], v[1],  v[2]);
    glVertex3f(v[3], v[4],  v[5]);
    glVertex3f(v[6], v[7],  v[8]);
    glVertex3f(v[9], v[10], v[11]);
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Polygon(mode, x1,y1,z1, x2,y2,z2, ...) — 可变顶点
static int l_Polygon(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    int argc = lua_gettop(L);
    int numCoords = argc - 1;
    int numVerts = numCoords / 3;

    glPushMatrix();
    ApplyDrawColor();
    glBegin(mode == 1 ? GL_LINE_LOOP : GL_POLYGON);
    for (int i = 0; i < numVerts; ++i) {
        float vx = (float)luaL_checknumber(L, 2 + i * 3);
        float vy = (float)luaL_checknumber(L, 3 + i * 3);
        float vz = (float)luaL_checknumber(L, 4 + i * 3);
        glVertex3f(vx, vy, vz);
    }
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Arc(mode, x,y,z, radius, startAngle, endAngle, segments, [rx...])
static int l_Arc(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float cx     = (float)luaL_checknumber(L, 2);
    float cy     = (float)luaL_checknumber(L, 3);
    float cz     = (float)luaL_checknumber(L, 4);
    float radius = (float)luaL_checknumber(L, 5);
    float start  = (float)luaL_checknumber(L, 6);
    float end    = (float)luaL_checknumber(L, 7);
    int segs     = (int)luaL_checkinteger(L, 8);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 9, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    glTranslatef(cx, cy, cz);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    GLenum glMode = (mode == 1) ? GL_LINE_STRIP : GL_TRIANGLE_FAN;
    glBegin(glMode);
    if (mode == 2) glVertex3f(0, 0, 0);  // 中心点
    for (int i = 0; i <= segs; ++i) {
        float t = start + (end - start) * i / segs;
        float rad = (float)(t * M_PI / 180.0);
        glVertex3f(radius * cosf(rad), radius * sinf(rad), 0);
    }
    glEnd();
    glPopMatrix();
    return 0;
}

/// Graphics.Circle(mode, x,y,z, radius, segments, [rx...])
static int l_Circle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float cx     = (float)luaL_checknumber(L, 2);
    float cy     = (float)luaL_checknumber(L, 3);
    float cz     = (float)luaL_checknumber(L, 4);
    float radius = (float)luaL_checknumber(L, 5);
    int segs     = (int)luaL_checkinteger(L, 6);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 7, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    glPushMatrix();
    glTranslatef(cx, cy, cz);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    GLenum glMode = (mode == 1) ? GL_LINE_LOOP : GL_TRIANGLE_FAN;
    glBegin(glMode);
    if (mode == 2) glVertex3f(0, 0, 0);
    for (int i = 0; i <= segs; ++i) {
        float t = (float)(2.0 * M_PI * i / segs);
        glVertex3f(radius * cosf(t), radius * sinf(t), 0);
    }
    glEnd();
    glPopMatrix();
    return 0;
}

// ==================== 元方法 ====================

/// __call — Graphics 作为函数调用时的处理
static int l_Graphics_Call(lua_State* L) {
    return 0;
}

/// __tostring — "Light.Graphics"
static int l_Graphics_Tostring(lua_State* L) {
    lua_pushstring(L, "Light.Graphics");
    return 1;
}

// ==================== 函数注册表 ====================
// 精确匹配 IDA off_18025AD60 的顺序 (21个函数 + 2元方法)

/// Graphics.DrawSprite(spriteData, frameIdx, x, y, [rx,ry,rz, sx,sy,sz, ox,oy,oz])
/// 绘制 WAS 精灵帧 — 从 GetSpriteImagesData 返回的表中取帧
/// spriteData.frames[frameIdx] = { x, y, w, h, pixels(userdata) }
/// 懒加载: 首次绘制时将 pixels 上传为 GL 纹理, 缓存在 frame.__texId
static int l_DrawSprite(lua_State* L) {
    // 参数: spriteData(table), frameIdx(int), x, y, z, [transforms...]
    luaL_checktype(L, 1, LUA_TTABLE);
    int frameIdx = (int)luaL_checkinteger(L, 2);
    float x = (float)luaL_optnumber(L, 3, 0.0);
    float y = (float)luaL_optnumber(L, 4, 0.0);
    float z = (float)luaL_optnumber(L, 5, 0.0);

    // 获取 frames 数组
    lua_getfield(L, 1, "frames");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 0; }
    int framesIdx = lua_gettop(L);

    // 获取指定帧
    lua_rawgeti(L, framesIdx, frameIdx);
    if (!lua_istable(L, -1)) { lua_pop(L, 2); return 0; }
    int frameTableIdx = lua_gettop(L);

    // 读取帧属性
    lua_getfield(L, frameTableIdx, "w");
    lua_getfield(L, frameTableIdx, "h");
    int fw = (int)lua_tointeger(L, -2);
    int fh = (int)lua_tointeger(L, -1);
    lua_pop(L, 2);

    if (fw <= 0 || fh <= 0) { lua_pop(L, 2); return 0; }

    // 检查是否已缓存 GL 纹理
    lua_getfield(L, frameTableIdx, "__texId");
    unsigned int texId = 0;
    if (lua_isnumber(L, -1)) {
        texId = (unsigned int)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);

    if (texId == 0) {
        // 懒创建: 从 pixels userdata 上传 GL 纹理
        lua_getfield(L, frameTableIdx, "pixels");
        if (lua_isuserdata(L, -1)) {
            const void* pixels = lua_touserdata(L, -1);
            glGenTextures(1, &texId);
            glBindTexture(GL_TEXTURE_2D, texId);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fw, fh, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            glBindTexture(GL_TEXTURE_2D, 0);

            // 缓存 texId 到帧表
            lua_pushnumber(L, (lua_Number)texId);
            lua_setfield(L, frameTableIdx, "__texId");
        }
        lua_pop(L, 1);  // pop pixels
    }

    if (texId == 0) { lua_pop(L, 2); return 0; }

    // 读取帧偏移 (hotspot)
    lua_getfield(L, frameTableIdx, "x");
    lua_getfield(L, frameTableIdx, "y");
    float ox = (float)lua_tointeger(L, -2);
    float oy = (float)lua_tointeger(L, -1);
    lua_pop(L, 2);

    // 变换参数
    float rx, ry, rz, tsx, tsy, tsz, tox, toy, toz;
    ReadTransform(L, 6, &rx, &ry, &rz, &tsx, &tsy, &tsz, &tox, &toy, &toz);

    // 渲染
    glPushMatrix();
    glTranslatef(x + ox, y + oy, z);
    ApplyTransform(rx, ry, rz, tsx, tsy, tsz, tox, toy, toz);
    ApplyDrawColor();

    float ffW = (float)fw, ffH = (float)fh;
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texId);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex3f(0,   0,   0);
    glTexCoord2f(1, 0); glVertex3f(ffW, 0,   0);
    glTexCoord2f(1, 1); glVertex3f(ffW, ffH, 0);
    glTexCoord2f(0, 1); glVertex3f(0,   ffH, 0);
    glEnd();
    glBindTexture(GL_TEXTURE_2D, 0);
    glDisable(GL_TEXTURE_2D);
    glPopMatrix();

    lua_pop(L, 2);  // pop frame + frames
    return 0;
}

static const luaL_Reg graphics_funcs[] = {
    // --- 绘图基元 ---
    {"Draw",              l_Draw},
    {"DrawQuad",          l_DrawQuad},
    {"DrawSprite",        l_DrawSprite},
    {"Print",             l_Print},
    {"Line",              l_Line},
    {"Triangle",          l_Triangle},
    {"Rectangle",         l_Rectangle},
    {"RoundedRectangle",  l_RoundedRectangle},
    {"Quad",              l_Quad},
    {"Polygon",           l_Polygon},
    {"Arc",               l_Arc},
    {"Circle",            l_Circle},
    // --- 变换 ---
    {"Translate",         l_Translate},
    {"Rotate",            l_Rotate},
    {"Scale",             l_Scale},
    {"Push",              l_Push},
    {"Pop",               l_Pop},
    // --- 颜色/画布/裁剪 ---
    {"GetColor",          l_GetColor},
    {"GetCanvas",         l_GetCanvas},
    {"GetScissor",        l_GetScissor},
    {"SetColor",          l_SetColor},
    {"SetCanvas",         l_SetCanvas},
    {"SetScissor",        l_SetScissor},
    // --- 元方法 ---
    {"__call",            l_Graphics_Call},
    {"__tostring",        l_Graphics_Tostring},
    {NULL, NULL}
};

// ==================== luaopen_Light_Graphics ====================
// 精确还原自 sub_1800AD380

int luaopen_Light_Graphics(lua_State* L) {
    LT::EnsureLightTable(L);

    lua_pushstring(L, "Graphics");
    lua_rawget(L, -2);
    if (!lua_type(L, -1)) {
        lua_settop(L, -2);
        lua_pushstring(L, "Graphics");
        lua_createtable(L, 0, 0);

        luaL_setfuncs(L, graphics_funcs, 0);

        // 设置常量: LineMode=1, FillMode=2
        lua_pushinteger(L, 1);
        lua_setfield(L, -2, "LineMode");
        lua_pushinteger(L, 2);
        lua_setfield(L, -2, "FillMode");

        // Drawable 空表 (供 OOP 使用)
        lua_createtable(L, 0, 0);
        lua_setfield(L, -2, "Drawable");

        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
