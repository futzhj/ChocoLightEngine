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
#include "render_backend.h"
#include "batch_renderer.h"
#include "lit_batch_renderer.h"   // Phase E.2.3 — Lit2D 批渲染
#include "hdr_renderer.h"          // Phase E.3.2 — HDR 离屏管线
#include "bloom_renderer.h"          // Phase E.4.2 — Bloom 后处理
#include "auto_exposure_renderer.h"  // Phase E.5.2 — Auto Exposure (Eye Adaptation)
#include "lens_dirt_renderer.h"      // Phase E.6.2 — Lens Dirt
#include "streak_renderer.h"         // Phase E.6.2 — Streak (Anamorphic Flare)
#include "lens_flare_renderer.h"     // Phase E.7.2 — Lens Flare (Ghost + Halo + Chromatic)
#include "ssao_renderer.h"            // Phase E.8.2 — SSAO (屏幕空间环境光遮蔽)
#include <cmath>
#include <cstring>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ==================== Phase A7: BatchRenderer helper ====================
//
// 统一替代 g_render->BindTexture + DrawArrays + UnbindTexture 三步组合:
//   - BatchRenderer 已就绪: 走批渲染路径 (Quad/Triangles 累积, Lines/不规则立即提交)
//   - 未就绪: fallback 到 g_render->DrawArrays (保持原有行为)
//
// textureId = 0 表示纯色(无纹理), 非 0 表示带纹理 quad/triangle
static inline void SubmitOrDraw(DrawMode mode, const RenderVertex* verts, int count, uint32_t texId) {
    // Phase E.2.3: 切到普通 sprite/几何前, 先把当前累积的 Lit 批刷出, 保证画家顺序
    // (普通批和 Lit 批用不同 shader/VAO; 不互相感知, 必须显式 Flush 维持视觉顺序).
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();

    if (BatchRenderer::IsInited()) {
        if (mode == DrawMode::Quads && count == 4) {
            BatchRenderer::SubmitQuad(verts, texId);
        } else if (mode == DrawMode::Triangles) {
            BatchRenderer::SubmitTriangles(verts, count, texId);
        } else if (mode == DrawMode::Lines) {
            BatchRenderer::SubmitLines(verts, count);
        } else {
            // LineLoop / LineStrip / TriangleFan / 多 quad
            BatchRenderer::SubmitImmediate(mode, verts, count, texId);
        }
        return;
    }
    // fallback
    if (texId) g_render->BindTexture(texId);
    g_render->DrawArrays(mode, verts, count);
    if (texId) g_render->UnbindTexture();
}

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
        g_render->Translate(-ox, -oy, -oz);
    if (rx != 0) g_render->Rotate(rx, 1, 0, 0);
    if (ry != 0) g_render->Rotate(ry, 0, 1, 0);
    if (rz != 0) g_render->Rotate(rz, 0, 0, 1);
    if (sx != 1 || sy != 1 || sz != 1)
        g_render->Scale(sx, sy, sz);
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

// 设置当前绘制颜色 (通过渲染后端)
static void ApplyDrawColor() {
    g_render->SetColor(g_ctx.drawColor[0], g_ctx.drawColor[1],
                       g_ctx.drawColor[2], g_ctx.drawColor[3]);
}

// ==================== 变换矩阵操作 ====================

/// @lua_api Light.Graphics.Push
/// @brief 保存当前变换矩阵到栈
/// @return void
static int l_Push(lua_State* L) {
    // Phase E.2.3: matrix stack 变动前必 Flush 当前 Lit 批
    // (batch 内 quad 共享一个 modelview, push 后 modelview 变了 batch 会错位)
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
    g_render->PushMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Pop
/// @brief 从栈恢复上一个变换矩阵
/// @return void
static int l_Pop(lua_State* L) {
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Translate
/// @brief 平移变换
/// @param x number 水平偏移
/// @param y number 垂直偏移
/// @param z number? 深度偏移 (默认 0)
/// @return void
static int l_Translate(lua_State* L) {
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    float z = (float)luaL_optnumber(L, 3, 0.0);
    g_render->Translate(x, y, z);
    return 0;
}

/// @lua_api Light.Graphics.Rotate
/// @brief 旋转变换
/// @param angle number 旋转角度 (度)
/// @param x number? 旋转轴 X (默认 0)
/// @param y number? 旋转轴 Y (默认 0)
/// @param z number? 旋转轴 Z (默认 1)
/// @return void
static int l_Rotate(lua_State* L) {
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
    float angle = (float)luaL_checknumber(L, 1);
    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 1.0);
    g_render->Rotate(angle, x, y, z);
    return 0;
}

/// @lua_api Light.Graphics.Scale
/// @brief 缩放变换
/// @param sx number X 缩放比
/// @param sy number? Y 缩放比 (默认同 sx)
/// @param sz number? Z 缩放比 (默认 1)
/// @return void
static int l_Scale(lua_State* L) {
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
    float sx = (float)luaL_checknumber(L, 1);
    float sy = (float)luaL_optnumber(L, 2, sx);
    float sz = (float)luaL_optnumber(L, 3, 1.0);
    g_render->Scale(sx, sy, sz);
    return 0;
}

// ==================== 颜色 / 画布 / 裁剪 ====================

/// @lua_api Light.Graphics.SetColor
/// @brief 设置当前绘制颜色
/// @param r number 红色 (0~1)
/// @param g number 绿色 (0~1)
/// @param b number 蓝色 (0~1)
/// @param a number? 透明度 (默认 1)
/// @return void
static int l_SetColor(lua_State* L) {
    g_ctx.drawColor[0] = (float)luaL_checknumber(L, 1);
    g_ctx.drawColor[1] = (float)luaL_checknumber(L, 2);
    g_ctx.drawColor[2] = (float)luaL_checknumber(L, 3);
    g_ctx.drawColor[3] = (float)luaL_optnumber(L, 4, 1.0);
    g_render->SetColor(g_ctx.drawColor[0], g_ctx.drawColor[1],
                       g_ctx.drawColor[2], g_ctx.drawColor[3]);
    return 0;
}

/// @lua_api Light.Graphics.GetColor
/// @brief 获取当前绘制颜色
/// @return number,number,number,number r,g,b,a
static int l_GetColor(lua_State* L) {
    lua_pushnumber(L, g_ctx.drawColor[0]);
    lua_pushnumber(L, g_ctx.drawColor[1]);
    lua_pushnumber(L, g_ctx.drawColor[2]);
    lua_pushnumber(L, g_ctx.drawColor[3]);
    return 4;
}

/// @lua_api Light.Graphics.SetCanvas
/// @brief 设置当前渲染目标
/// @param canvas Canvas|nil Canvas 离屏画布, nil 恢复默认
/// @return void
static int l_SetCanvas(lua_State* L) {
    static int savedViewport[4] = {0, 0, 800, 600};

    // Phase E.3.2: 切换渲染目标前必须 Flush 所有 batch
    // (否则当前 batch 会绘到新 FBO 上, 破坏画家顺序)
    if (BatchRenderer::IsInited())    BatchRenderer::Flush();
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();

    if (lua_isnoneornil(L, 1)) {
        // Phase E.3.2: SetCanvas(nil) 分两种情形
        //   (1) HDR 启用 + 之前被 Pause → Resume (重新绑 HDR RT, 场景累积继续)
        //   (2) HDR 未启用 → 组照原逻辑 (恢复 default fb)
        if (HDRRenderer::IsEnabled() && HDRRenderer::IsPaused()) {
            HDRRenderer::Resume();  // 内部 BindFBO(HDR_RT) + SetViewport
        } else {
            g_render->UnbindFBO();
            g_render->SetViewport(savedViewport[0], savedViewport[1],
                                  savedViewport[2], savedViewport[3]);
        }
        g_ctx.currentCanvas = nullptr;
    } else if (lua_istable(L, 1)) {
        // 保存当前 viewport
        // TODO: 当前简化处理, 后续可扩展 RenderBackend::GetViewport
        lua_getfield(L, 1, "__instance");
        if (lua_isuserdata(L, -1)) {
            struct CanvasCtx { unsigned int fbo, texture, depthRB; int w, h; };
            CanvasCtx* cc = (CanvasCtx*)lua_touserdata(L, -1);
            if (cc && cc->fbo) {
                // Phase E.3.2: HDR 启用时标记 Pause (切到 user RT, HDR 暂停累积)
                if (HDRRenderer::IsEnabled() && !HDRRenderer::IsPaused()) {
                    HDRRenderer::Pause();
                }
                g_render->BindFBO(cc->fbo);
                g_render->SetViewport(0, 0, cc->w, cc->h);
                g_ctx.currentCanvas = cc;
            }
        }
        lua_pop(L, 1);
    }
    return 0;
}

/// @lua_api Light.Graphics.GetCanvas
/// @brief 获取当前渲染目标
/// @return Canvas|nil
static int l_GetCanvas(lua_State* L) {
    if (g_ctx.currentCanvas) {
        lua_pushlightuserdata(L, g_ctx.currentCanvas);
    } else {
        lua_pushnil(L);
    }
    return 1;
}

// ==================== Phase AS.2 — 3D camera + 深度测试 ====================

/// @lua_api Light.Graphics.SetPerspective
/// @brief 设置透视投影 (用于 3D 渲染)
/// @param fovYDeg number 垂直视野角度 (度)
/// @param aspect number 宽高比 (w/h)
/// @param near number 近裁剪面 (>0)
/// @param far number 远裁剪面 (>near)
/// @return void
static int l_SetPerspective(lua_State* L) {
    float fovY   = (float)luaL_checknumber(L, 1);
    float aspect = (float)luaL_checknumber(L, 2);
    float near_  = (float)luaL_checknumber(L, 3);
    float far_   = (float)luaL_checknumber(L, 4);
    if (g_render) {
        Mat4 proj = Mat4::Perspective(fovY, aspect, near_, far_);
        g_render->LoadProjection(proj.m);
    }
    return 0;
}

/// @lua_api Light.Graphics.SetCamera
/// @brief 设置 3D 摄像机 (LookAt 视图矩阵)
/// @param ex number 摄像机 X
/// @param ey number 摄像机 Y
/// @param ez number 摄像机 Z
/// @param tx number 注视点 X
/// @param ty number 注视点 Y
/// @param tz number 注视点 Z
/// @param ux number? 上方向 X (默认 0)
/// @param uy number? 上方向 Y (默认 1)
/// @param uz number? 上方向 Z (默认 0)
/// @return void
static int l_SetCamera(lua_State* L) {
    float ex = (float)luaL_checknumber(L, 1);
    float ey = (float)luaL_checknumber(L, 2);
    float ez = (float)luaL_checknumber(L, 3);
    float tx = (float)luaL_checknumber(L, 4);
    float ty = (float)luaL_checknumber(L, 5);
    float tz = (float)luaL_checknumber(L, 6);
    float ux = (float)luaL_optnumber(L, 7, 0.0);
    float uy = (float)luaL_optnumber(L, 8, 1.0);
    float uz = (float)luaL_optnumber(L, 9, 0.0);
    if (g_render) {
        Mat4 view = Mat4::LookAt(ex, ey, ez, tx, ty, tz, ux, uy, uz);
        g_render->LoadView(view.m);
    }
    return 0;
}

// 当前深度测试状态 (Lua 端镜像, GetDepthTest 用)
static bool g_depthTestEnabled = false;

/// @lua_api Light.Graphics.SetDepthTest
/// @brief 启用/禁用深度测试 (默认禁用以兼容 2D 渲染)
/// @param enable boolean true=启用, false=禁用
/// @return void
static int l_SetDepthTest(lua_State* L) {
    bool enable = lua_toboolean(L, 1) != 0;
    if (g_render) {
        g_render->SetDepthTest(enable);
    }
    g_depthTestEnabled = enable;
    return 0;
}

/// @lua_api Light.Graphics.GetDepthTest
/// @brief 查询深度测试是否启用
/// @return boolean
static int l_GetDepthTest(lua_State* L) {
    lua_pushboolean(L, g_depthTestEnabled ? 1 : 0);
    return 1;
}

// ==================== Phase AS.4 — 多光源 API ====================

/// @lua_api Light.Graphics.SetDirectionalLight
/// @brief 设置主方向光 (引擎仅支持 1 个 directional)
/// @param dx number 光方向 X (世界坐标, 指向光源)
/// @param dy number 光方向 Y
/// @param dz number 光方向 Z
/// @param r  number 颜色 R (0..1)
/// @param g  number 颜色 G
/// @param b  number 颜色 B
/// @param intensity number? 强度 (默认 1)
static int l_SetDirectionalLight(lua_State* L) {
    float dx = (float)luaL_checknumber(L, 1);
    float dy = (float)luaL_checknumber(L, 2);
    float dz = (float)luaL_checknumber(L, 3);
    float r  = (float)luaL_checknumber(L, 4);
    float g  = (float)luaL_checknumber(L, 5);
    float b  = (float)luaL_checknumber(L, 6);
    float intensity = (float)luaL_optnumber(L, 7, 1.0);
    if (g_render) {
        // 归一化方向 (避免 shader 重复算)
        float len = sqrtf(dx*dx + dy*dy + dz*dz);
        if (len > 1e-6f) { dx /= len; dy /= len; dz /= len; }
        float dir[3]   = { dx, dy, dz };
        float color[3] = { r, g, b };
        g_render->SetDirectionalLight(dir, color, intensity, true);
    }
    return 0;
}

/// @lua_api Light.Graphics.SetDirectionalLightEnabled
/// @brief 启用/禁用主方向光 (不清除颜色, 仅切换是否参与计算)
static int l_SetDirectionalLightEnabled(lua_State* L) {
    bool enabled = lua_toboolean(L, 1) != 0;
    if (g_render) {
        // 用 dummy dir/color 仅切换 enabled (backend 内部保留之前的值)
        // 简化: 不传颜色, 让 backend 内部记忆
        float dir[3]   = { 0, 1, 0 };
        float color[3] = { 1, 1, 1 };
        g_render->SetDirectionalLight(dir, color, 1.0f, enabled);
    }
    return 0;
}

/// @lua_api Light.Graphics.SetAmbientLight
/// @brief 设置全局环境光
/// @param r number
/// @param g number
/// @param b number
static int l_SetAmbientLight(lua_State* L) {
    float r = (float)luaL_checknumber(L, 1);
    float g = (float)luaL_checknumber(L, 2);
    float b = (float)luaL_checknumber(L, 3);
    if (g_render) {
        float rgb[3] = { r, g, b };
        g_render->SetAmbientLight(rgb);
    }
    return 0;
}

/// @lua_api Light.Graphics.AddPointLight
/// @brief 添加点光 (最多 4 个, 满了返回 0)
/// @param x number 位置 X
/// @param y number 位置 Y
/// @param z number 位置 Z
/// @param r number 颜色 R
/// @param g number 颜色 G
/// @param b number 颜色 B
/// @param range number 衰减半径
/// @param intensity number? 强度 (默认 1)
/// @return integer light_id (1..4, 0 = 已满)
static int l_AddPointLight(lua_State* L) {
    PointLight pl;
    pl.pos[0]   = (float)luaL_checknumber(L, 1);
    pl.pos[1]   = (float)luaL_checknumber(L, 2);
    pl.pos[2]   = (float)luaL_checknumber(L, 3);
    pl.color[0] = (float)luaL_checknumber(L, 4);
    pl.color[1] = (float)luaL_checknumber(L, 5);
    pl.color[2] = (float)luaL_checknumber(L, 6);
    pl.range    = (float)luaL_checknumber(L, 7);
    pl.intensity= (float)luaL_optnumber(L, 8, 1.0);
    int id = 0;
    if (g_render) id = g_render->AddPointLight(&pl);
    lua_pushinteger(L, id);
    return 1;
}

/// @lua_api Light.Graphics.RemovePointLight
/// @param id integer
static int l_RemovePointLight(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    if (g_render) g_render->RemovePointLight(id);
    return 0;
}

static int l_ClearPointLights(lua_State* L) {
    (void)L;
    if (g_render) g_render->ClearPointLights();
    return 0;
}

static int l_GetPointLightCount(lua_State* L) {
    int n = 0;
    if (g_render) n = g_render->GetPointLightCount();
    lua_pushinteger(L, n);
    return 1;
}

static int l_GetMaxPointLights(lua_State* L) {
    int n = 0;
    if (g_render) n = g_render->GetMaxPointLights();
    lua_pushinteger(L, n);
    return 1;
}

// ==================== Phase AW.x — Backend 内省 ====================
// 返回当前渲染后端的静态名称字符串, 永不 raise / 永不返回 nil。
// 已知返回值: "GL33Core" / "LegacyGL" / "None" (无后端) / "Unknown" (异常)
// 用于调试 / sample 显示 / smoke 验证。
static int l_Graphics_GetBackendName(lua_State* L) {
    if (!g_render) {
        lua_pushliteral(L, "None");
        return 1;
    }
    const char* name = g_render->GetName();
    lua_pushstring(L, (name && *name) ? name : "Unknown");
    return 1;
}

// ==================== Phase AS.1 — Canvas 渲染目标栈 ====================
// 设计: 软限制 8 层栈深度 (Q2 决策),超出仅警告不中断;
//      Pop 在空栈时也仅警告不中断,确保 Lua 端误用不会崩。

struct CanvasStackEntry {
    void* canvasCtx;       // CanvasContext* (nullptr = 默认渲染目标)
    int   viewport[4];     // 入栈时的 viewport 快照
    int   ref;             // Lua 表 registry ref (用于 PopCanvas 恢复 Lua 端 currentCanvas table 引用)
};

static const int kCanvasStackMax = 8;
static CanvasStackEntry g_canvasStack[kCanvasStackMax];
static int              g_canvasStackTop = 0;

// CanvasContext 内存布局必须与 light_graphics_canvas.cpp 中的定义一致 (内部结构,
// 此处镜像声明仅用于读取 fbo/texture/width/height,避免跨编译单元 include 一份头)
struct CanvasCtxMirror {
    unsigned int fbo;
    unsigned int texture;
    unsigned int depthRB;
    int          width;
    int          height;
};

/// @lua_api Light.Graphics.PushCanvas
/// @brief 入栈当前渲染目标, 切换到给定 canvas (栈式渲染)
/// @param canvas Canvas 实例
/// @return void
static int l_PushCanvas(lua_State* L) {
    if (g_canvasStackTop >= kCanvasStackMax) {
        CC::Log(CC::LOG_WARN, "PushCanvas: stack overflow (max %d), ignored", kCanvasStackMax);
        return 0;
    }
    if (!lua_istable(L, 1)) {
        CC::Log(CC::LOG_WARN, "PushCanvas: argument must be a Canvas table");
        return 0;
    }

    // 1) 保存当前状态到栈顶
    CanvasStackEntry& slot = g_canvasStack[g_canvasStackTop];
    slot.canvasCtx = g_ctx.currentCanvas;
    // 简化: viewport 用一个稳定默认值 (与 SetCanvas 中 savedViewport 一致语义)
    // 真实场景下后端可扩展 GetViewport, 当前保留 placeholder
    slot.viewport[0] = 0;
    slot.viewport[1] = 0;
    slot.viewport[2] = 0;
    slot.viewport[3] = 0;
    slot.ref = LUA_NOREF;
    g_canvasStackTop++;

    // 2) 切换到新 canvas (复用 SetCanvas 逻辑)
    lua_getfield(L, 1, "__instance");
    if (lua_isuserdata(L, -1)) {
        CanvasCtxMirror* cc = (CanvasCtxMirror*)lua_touserdata(L, -1);
        if (cc && cc->fbo && g_render) {
            g_render->BindFBO(cc->fbo);
            g_render->SetViewport(0, 0, cc->width, cc->height);
            g_ctx.currentCanvas = cc;
        }
    }
    lua_pop(L, 1);
    return 0;
}

/// @lua_api Light.Graphics.PopCanvas
/// @brief 出栈, 恢复上一个渲染目标
/// @return void
static int l_PopCanvas(lua_State* L) {
    if (g_canvasStackTop <= 0) {
        CC::Log(CC::LOG_WARN, "PopCanvas: stack underflow, ignored");
        return 0;
    }

    g_canvasStackTop--;
    CanvasStackEntry& slot = g_canvasStack[g_canvasStackTop];

    // 恢复上一个 canvas (nullptr = 默认渲染目标)
    if (slot.canvasCtx == nullptr) {
        if (g_render) {
            g_render->UnbindFBO();
            // viewport 用 800x600 默认 (与 SetCanvas savedViewport 兼容)
            g_render->SetViewport(0, 0, 800, 600);
        }
        g_ctx.currentCanvas = nullptr;
    } else {
        CanvasCtxMirror* cc = (CanvasCtxMirror*)slot.canvasCtx;
        if (cc && cc->fbo && g_render) {
            g_render->BindFBO(cc->fbo);
            g_render->SetViewport(0, 0, cc->width, cc->height);
            g_ctx.currentCanvas = cc;
        }
    }

    return 0;
}

/// @lua_api Light.Graphics.SetScissor
/// @brief 设置裁剪区域 (无参数时禁用裁剪)
/// @param x number? 裁剪区域左上 X
/// @param y number? 裁剪区域左上 Y
/// @param w number? 裁剪区域宽
/// @param h number? 裁剪区域高
/// @return void
static int l_SetScissor(lua_State* L) {
    if (lua_gettop(L) >= 4) {
        g_ctx.scissor[0] = (int)luaL_checkinteger(L, 1);
        g_ctx.scissor[1] = (int)luaL_checkinteger(L, 2);
        g_ctx.scissor[2] = (int)luaL_checkinteger(L, 3);
        g_ctx.scissor[3] = (int)luaL_checkinteger(L, 4);
        g_ctx.scissorEnabled = true;
        g_render->SetScissor(true, g_ctx.scissor[0], g_ctx.scissor[1],
                             g_ctx.scissor[2], g_ctx.scissor[3]);
    } else {
        g_ctx.scissorEnabled = false;
        g_render->SetScissor(false, 0, 0, 0, 0);
    }
    return 0;
}

/// @lua_api Light.Graphics.GetScissor
/// @brief 获取当前裁剪区域
/// @return number,number,number,number x,y,w,h
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

/// @lua_api Light.Graphics.Draw
/// @brief 绘制纹理/图像到屏幕
/// @param drawable Image|Canvas 可绘制对象
/// @param x number? 水平位置 (默认 0)
/// @param y number? 垂直位置 (默认 0)
/// @param z number? 深度 (默认 0)
/// @param rx number? X 旋转 (后续 9 个变换参数可选)
/// @return void
/// @example
/// local img = Light(Light.Graphics.Image):New("hero.png")
/// Light.Graphics.Draw(img, 100, 200)
static int l_Draw(lua_State* L) {
    unsigned int texId = 0;
    int imgW = 64, imgH = 64;
    bool hasTex = GetDrawableTexture(L, 1, &texId, &imgW, &imgH);

    float x = (float)luaL_optnumber(L, 2, 0.0);
    float y = (float)luaL_optnumber(L, 3, 0.0);
    float z = (float)luaL_optnumber(L, 4, 0.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 5, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    g_render->PushMatrix();
    g_render->Translate(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    float fw = (float)imgW, fh = (float)imgH;
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    if (hasTex) {
        RenderVertex verts[4] = {
            {0,  0,  0,  0, 0,  cr, cg, cb, ca},
            {fw, 0,  0,  1, 0,  cr, cg, cb, ca},
            {fw, fh, 0,  1, 1,  cr, cg, cb, ca},
            {0,  fh, 0,  0, 1,  cr, cg, cb, ca},
        };
        SubmitOrDraw(DrawMode::Quads, verts, 4, texId); // Phase A7
    } else {
        RenderVertex verts[4] = {
            {0,  0,  0,  0, 0,  cr, cg, cb, ca},
            {fw, 0,  0,  0, 0,  cr, cg, cb, ca},
            {fw, fh, 0,  0, 0,  cr, cg, cb, ca},
            {0,  fh, 0,  0, 0,  cr, cg, cb, ca},
        };
        SubmitOrDraw(DrawMode::Quads, verts, 4, 0); // Phase A7
    }
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.DrawQuad
/// @brief 绘制纹理子区域 (sprite sheet 裁切)
/// @param drawable Image|Canvas 可绘制对象
/// @param x number? 屏幕位置 X
/// @param y number? 屏幕位置 Y
/// @param z number? 深度
/// @param qx number? 子区域左上 X
/// @param qy number? 子区域左上 Y
/// @param qw number? 子区域宽
/// @param qh number? 子区域高
/// @return void
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

    g_render->PushMatrix();
    g_render->Translate(x, y, z);
    ApplyDrawColor();

    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    if (hasTex) {
        float u0 = qx / imgW, v0 = qy / imgH;
        float u1 = (qx + qw) / imgW, v1 = (qy + qh) / imgH;
        RenderVertex verts[4] = {
            {0,  0,  0,  u0, v0,  cr, cg, cb, ca},
            {qw, 0,  0,  u1, v0,  cr, cg, cb, ca},
            {qw, qh, 0,  u1, v1,  cr, cg, cb, ca},
            {0,  qh, 0,  u0, v1,  cr, cg, cb, ca},
        };
        SubmitOrDraw(DrawMode::Quads, verts, 4, texId); // Phase A7
    } else {
        RenderVertex verts[4] = {
            {0,  0,  0,  0, 0,  cr, cg, cb, ca},
            {qw, 0,  0,  0, 0,  cr, cg, cb, ca},
            {qw, qh, 0,  0, 0,  cr, cg, cb, ca},
            {0,  qh, 0,  0, 0,  cr, cg, cb, ca},
        };
        SubmitOrDraw(DrawMode::Quads, verts, 4, 0); // Phase A7
    }
    g_render->PopMatrix();
    return 0;
}

// ============================================================================
// Phase E.1.5 + E.2.3 — Light.Graphics.DrawLit / DrawLitQuad (2D Lit forward 多灯)
// ============================================================================
//
// Phase E.1.5 (老路径): Push matrix → Translate → ApplyTransform → DrawLit2DQuad → Pop
//                      每 sprite 一次 draw call, lighting state 每次重传 uniform.
//
// Phase E.2.3 (新路径): CPU 端用 rz/sx/sy/ox/oy 把 transform 烘焙到 4 个 vertex.pos,
//                      然后提交到 LitBatchRenderer 累积. 同 (baseTex, normTex) 合批,
//                      EndFrame / 状态切换时 Flush 触发一次 DrawLit2DBatch.
//
// 关键约束 (调用方约定):
//   - 调用 DrawLit/DrawLitQuad 前必须用 BatchRenderer::Flush 排空普通 sprite (本函数内做).
//   - matrix stack (Push/Translate) 仍可被本函数外部使用 (camera transform); batch 内
//     共享同一 modelview, batch 期间调用方不能 push/pop/translate. 若必须改 matrix,
//     先调 LitBatchRenderer::Flush.
//
// CPU 烘焙公式 (与原 ApplyTransform 等价但只考虑 z-axis rotation):
//   local_pos[i]:    4 个 quad 角点 (0..fw, 0..fh)
//   step 1 (origin): pos -= (ox, oy, oz)
//   step 2 (scale):  pos *= (sx, sy, sz)
//   step 3 (rotate): pos = R_z(rz) * pos     (2D 主流; rx/ry 罕用, 简化处理)
//   step 4 (translate): pos += (x, y, z)
//
// tangent 随 rz 旋转 (维持 TBN 矩阵正确); normal=(0,0,1) z 轴旋转不变.

/// CPU 端烘焙 sprite transform 到 4 个 vertex.pos / tangent
/// @param ux0/uy0 - ux1/uy1  UV 范围
static void BakeLit2DQuad(float lx0, float ly0, float lx1, float ly1,
                          float ux0, float uy0, float ux1, float uy1,
                          float x, float y, float z,
                          float rx, float ry, float rz,
                          float sx, float sy, float sz,
                          float ox, float oy, float oz,
                          float cr, float cg, float cb, float ca,
                          RenderVertex2DLit out[4]) {
    (void)rx; (void)ry;  // 2D sprite 主流只用 z-axis rotation
    // (lx0,ly0) = 左上, (lx1,ly0) = 右上, (lx1,ly1) = 右下, (lx0,ly1) = 左下
    const float lx[4] = { lx0, lx1, lx1, lx0 };
    const float ly[4] = { ly0, ly0, ly1, ly1 };
    const float u [4] = { ux0, ux1, ux1, ux0 };
    const float v [4] = { uy0, uy0, uy1, uy1 };

    const float rad = rz * (float)(M_PI / 180.0);
    const float cs = cosf(rad), sn = sinf(rad);

    for (int i = 0; i < 4; ++i) {
        // origin offset (相当于 g_render->Translate(-ox, -oy, -oz) 推入栈)
        float px = lx[i] - ox;
        float py = ly[i] - oy;
        float pz = -oz;
        // scale
        px *= sx; py *= sy; pz *= sz;
        // z-axis rotate
        float rxp = cs * px - sn * py;
        float ryp = sn * px + cs * py;
        // translate
        out[i].x = x + rxp;
        out[i].y = y + ryp;
        out[i].z = z + pz;
        out[i].u = u[i];
        out[i].v = v[i];
        out[i].r = cr; out[i].g = cg; out[i].b = cb; out[i].a = ca;
        // normal 默认 (0,0,1); z 旋转不影响
        out[i].nx = 0; out[i].ny = 0; out[i].nz = 1;
        // tangent (1,0,0,1) z 轴旋转后 = (cs, sn, 0, 1)
        out[i].tx = cs; out[i].ty = sn; out[i].tz = 0; out[i].tw = 1;
    }
}

/// @lua_api Light.Graphics.DrawLit(image, normalMap, x, y, [z, rx, ry, rz, sx, sy, sz, ox, oy, oz])
/// @brief Phase E.2.3 — 绘制受光照 sprite, 走 LitBatchRenderer 批渲染
static int l_DrawLit(lua_State* L) {
    if (!g_render || !g_render->SupportsLit2D()) return 0;

    unsigned int baseTex = 0, normTex = 0;
    int imgW = 64, imgH = 64;
    bool hasBase = GetDrawableTexture(L, 1, &baseTex, &imgW, &imgH);
    int nW = 0, nH = 0;
    GetDrawableTexture(L, 2, &normTex, &nW, &nH);  // nil/非表 → normTex 保持 0
    (void)hasBase;  // hasBase 仅当年 E.1.5 用来切 uv; CPU 烘焙路径 uv 仍是 0..1

    float x = (float)luaL_optnumber(L, 3, 0.0);
    float y = (float)luaL_optnumber(L, 4, 0.0);
    float z = (float)luaL_optnumber(L, 5, 0.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 6, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    // Phase E.2.3: Lit 走自己的批渲染; 进入前先 Flush 普通 BatchRenderer 保证画家顺序
    if (BatchRenderer::IsInited()) BatchRenderer::Flush();

    ApplyDrawColor();
    const float fw = (float)imgW, fh = (float)imgH;
    const float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    const float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    RenderVertex2DLit verts[4];
    BakeLit2DQuad(0, 0, fw, fh,         // local quad: (0,0) ~ (fw,fh)
                  0, 0, 1, 1,           // 全图 UV
                  x, y, z,
                  rx, ry, rz,
                  sx, sy, sz,
                  ox, oy, oz,
                  cr, cg, cb, ca,
                  verts);

    LitBatchRenderer::SubmitQuad(verts, baseTex, normTex);
    return 0;
}

/// @lua_api Light.Graphics.DrawLitQuad(image, normalMap, x, y, z, qx, qy, qw, qh, [rx, ry, rz, sx, sy, sz, ox, oy, oz])
/// @brief Phase E.2.3 — 绘制 sprite 子区域 (sprite sheet), 走 LitBatchRenderer
static int l_DrawLitQuad(lua_State* L) {
    if (!g_render || !g_render->SupportsLit2D()) return 0;

    unsigned int baseTex = 0, normTex = 0;
    int imgW = 64, imgH = 64;
    bool hasBase = GetDrawableTexture(L, 1, &baseTex, &imgW, &imgH);
    int nW = 0, nH = 0;
    GetDrawableTexture(L, 2, &normTex, &nW, &nH);

    float x  = (float)luaL_optnumber(L, 3, 0.0);
    float y  = (float)luaL_optnumber(L, 4, 0.0);
    float z  = (float)luaL_optnumber(L, 5, 0.0);
    float qx = (float)luaL_optnumber(L, 6, 0.0);
    float qy = (float)luaL_optnumber(L, 7, 0.0);
    float qw = (float)luaL_optnumber(L, 8, 64.0);
    float qh = (float)luaL_optnumber(L, 9, 64.0);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 10, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    if (BatchRenderer::IsInited()) BatchRenderer::Flush();

    ApplyDrawColor();
    const float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    const float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;
    if (hasBase && imgW > 0 && imgH > 0) {
        u0 = qx / (float)imgW;          v0 = qy / (float)imgH;
        u1 = (qx + qw) / (float)imgW;   v1 = (qy + qh) / (float)imgH;
    }

    RenderVertex2DLit verts[4];
    BakeLit2DQuad(0, 0, qw, qh,
                  u0, v0, u1, v1,
                  x, y, z,
                  rx, ry, rz,
                  sx, sy, sz,
                  ox, oy, oz,
                  cr, cg, cb, ca,
                  verts);

    LitBatchRenderer::SubmitQuad(verts, baseTex, normTex);
    return 0;
}

/// @lua_api Light.Graphics.FlushLitBatch() -> void
/// @brief Phase E.2.3 — 立即 flush 当前累积的 Lit2D 批 (画家算法 / 状态切换前调用).
///        ECS Render() 2D 阶段末调一次保证 camera pop 前刷干净.
static int l_FlushLitBatch(lua_State* L) {
    (void)L;
    if (LitBatchRenderer::IsInited()) LitBatchRenderer::Flush();
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

/// @lua_api Light.Graphics.Print
/// @brief 文字渲染 (支持 Unicode/CJK)
/// @param text string 要渲染的文本
/// @param font Font 字体对象
/// @param x number? 水平位置
/// @param y number? 垂直位置
/// @param z number? 深度
/// @return void
/// @example
/// local font = Light(Light.Graphics.Font):New("Arial.ttf", 24)
/// Light.Graphics.Print("Hello World", font, 10, 10)
///
/// 还原自 sub_1800AA170
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

    g_render->PushMatrix();
    g_render->Translate(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();

    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    if (fch && fch->texId && rawFontCtx) {
        float baseLine = fch->fontSize;

        // ====== 第一遍: 预烘焙所有字形 ======
        {
            const char* p = text;
            while (*p) {
                int cp = DecodeUTF8(&p);
                if (cp <= 0) continue;
                FontGlyphResult gr;
                FontGetGlyph(rawFontCtx, cp, &gr);
            }
        }

        // ====== 第二遍: 收集所有字形顶点后一次提交 ======
        g_render->BindTexture(fch->texId);
        std::vector<RenderVertex> glyphVerts;
        glyphVerts.reserve(128);

        float cx = 0;
        const char* p = text;
        while (*p) {
            int cp = DecodeUTF8(&p);
            if (cp <= 0) continue;

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

            glyphVerts.push_back({gx0, gy0, 0, gr.u0, gr.v0, cr, cg, cb, ca});
            glyphVerts.push_back({gx1, gy0, 0, gr.u1, gr.v0, cr, cg, cb, ca});
            glyphVerts.push_back({gx1, gy1, 0, gr.u1, gr.v1, cr, cg, cb, ca});
            glyphVerts.push_back({gx0, gy1, 0, gr.u0, gr.v1, cr, cg, cb, ca});
            cx += gr.xadvance;
        }
        if (!glyphVerts.empty()) {
            // Phase A7: 文本字符 quad 走批渲染, 同一字体纹理 1 draw call
            // SubmitOrDraw 内部判断: BatchRenderer 走 SubmitImmediate (多 quad 不能走 SubmitQuad), 未启用走 fallback
            // 但文本场景下多 quad 同纹理, 最佳是在 BatchRenderer 内拆分 quad. 这里使用多-quad SubmitImmediate 保留原陷状态。
            // 性能收益在 1000 字符场景仍为 1 draw call。
            SubmitOrDraw(DrawMode::Quads, glyphVerts.data(), (int)glyphVerts.size(), fch->texId);
            if (!BatchRenderer::IsInited()) g_render->UnbindTexture();
        } else {
            g_render->UnbindTexture();
        }
    } else {
        float tw = (float)(strlen(text) * 10);
        RenderVertex verts[4] = {
            {0,  0,  0,  0, 0, cr, cg, cb, ca},
            {tw, 0,  0,  0, 0, cr, cg, cb, ca},
            {tw, 18, 0,  0, 0, cr, cg, cb, ca},
            {0,  18, 0,  0, 0, cr, cg, cb, ca},
        };
        g_render->DrawArrays(DrawMode::Quads, verts, 4);
    }
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Line
/// @brief 绘制直线
/// @param x1 number 起点 X
/// @param y1 number 起点 Y
/// @param z1 number 起点 Z
/// @param x2 number 终点 X
/// @param y2 number 终点 Y
/// @param z2 number 终点 Z
/// @return void
static int l_Line(lua_State* L) {
    float x1 = (float)luaL_checknumber(L, 1);
    float y1 = (float)luaL_checknumber(L, 2);
    float z1 = (float)luaL_checknumber(L, 3);
    float x2 = (float)luaL_checknumber(L, 4);
    float y2 = (float)luaL_checknumber(L, 5);
    float z2 = (float)luaL_checknumber(L, 6);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 7, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    g_render->PushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    RenderVertex verts[2] = {
        {x1, y1, z1, 0, 0, cr, cg, cb, ca},
        {x2, y2, z2, 0, 0, cr, cg, cb, ca},
    };
    g_render->DrawArrays(DrawMode::Lines, verts, 2);
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Triangle
/// @brief 绘制三角形
/// @param mode number 1=线框(LineMode) 2=填充(FillMode)
/// @param x1 number 顶点 1 X
/// @param y1 number 顶点 1 Y
/// @param z1 number 顶点 1 Z
/// @return void
static int l_Triangle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float v[9];
    for (int i = 0; i < 9; ++i)
        v[i] = (float)luaL_checknumber(L, 2 + i);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 11, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    g_render->PushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    RenderVertex verts[3] = {
        {v[0], v[1], v[2], 0, 0, cr, cg, cb, ca},
        {v[3], v[4], v[5], 0, 0, cr, cg, cb, ca},
        {v[6], v[7], v[8], 0, 0, cr, cg, cb, ca},
    };
    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::Triangles;
    g_render->DrawArrays(dm, verts, 3);
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Rectangle
/// @brief 绘制矩形
/// @param mode number 1=线框 2=填充
/// @param x number 左上 X
/// @param y number 左上 Y
/// @param z number 深度
/// @param w number 宽度
/// @param h number 高度
/// @param d number? 深度尺寸 (默认 0)
/// @return void
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

    g_render->PushMatrix();
    g_render->Translate(x, y, z);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    RenderVertex verts[4] = {
        {0, 0, 0, 0, 0, cr, cg, cb, ca},
        {w, 0, 0, 0, 0, cr, cg, cb, ca},
        {w, h, d, 0, 0, cr, cg, cb, ca},
        {0, h, d, 0, 0, cr, cg, cb, ca},
    };
    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::Quads;
    g_render->DrawArrays(dm, verts, 4);
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.RoundedRectangle
/// @brief 绘制圆角矩形
/// @param mode number 1=线框 2=填充
/// @param x number 左上 X
/// @param y number 左上 Y
/// @param z number 深度
/// @param w number 宽度
/// @param h number 高度
/// @param r number? 圆角半径 (默认 5)
/// @param segments number? 圆弧段数 (默认 8)
/// @return void
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

    g_render->PushMatrix();
    g_render->Translate(x, y, z);
    ApplyTransform(rx2, ry2, rz2, sx2, sy2, sz2, ox2, oy2, oz2);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    std::vector<RenderVertex> verts;
    verts.reserve(4 * segs + 8);
    if (mode == 2) verts.push_back({w / 2, h / 2, 0, 0, 0, cr, cg, cb, ca});

    auto emitArc = [&](float acx, float acy, float startDeg, float endDeg) {
        for (int i = 0; i <= segs; ++i) {
            float t = startDeg + (endDeg - startDeg) * i / segs;
            float rad = (float)(t * M_PI / 180.0);
            verts.push_back({acx + r * cosf(rad), acy + r * sinf(rad), 0, 0, 0, cr, cg, cb, ca});
        }
    };
    emitArc(w - r, r,     -90.0f, 0.0f);
    emitArc(w - r, h - r,  0.0f,  90.0f);
    emitArc(r,     h - r,  90.0f, 180.0f);
    emitArc(r,     r,      180.0f, 270.0f);

    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::TriangleFan;
    g_render->DrawArrays(dm, verts.data(), (int)verts.size());
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Quad
/// @brief 绘制任意四边形
/// @param mode number 1=线框 2=填充
/// @return void
static int l_Quad(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float v[12];
    for (int i = 0; i < 12; ++i)
        v[i] = (float)luaL_checknumber(L, 2 + i);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 14, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    g_render->PushMatrix();
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    RenderVertex verts[4] = {
        {v[0], v[1],  v[2],  0, 0, cr, cg, cb, ca},
        {v[3], v[4],  v[5],  0, 0, cr, cg, cb, ca},
        {v[6], v[7],  v[8],  0, 0, cr, cg, cb, ca},
        {v[9], v[10], v[11], 0, 0, cr, cg, cb, ca},
    };
    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::Quads;
    g_render->DrawArrays(dm, verts, 4);
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Polygon
/// @brief 绘制多边形 (可变顶点数)
/// @param mode number 1=线框 2=填充
/// @return void
static int l_Polygon(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    int argc = lua_gettop(L);
    int numCoords = argc - 1;
    int numVerts = numCoords / 3;

    g_render->PushMatrix();
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    std::vector<RenderVertex> verts(numVerts);
    for (int i = 0; i < numVerts; ++i) {
        verts[i] = {
            (float)luaL_checknumber(L, 2 + i * 3),
            (float)luaL_checknumber(L, 3 + i * 3),
            (float)luaL_checknumber(L, 4 + i * 3),
            0, 0, cr, cg, cb, ca
        };
    }
    // GL_POLYGON → TriangleFan (GL 3.3 Core 无 GL_POLYGON)
    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::TriangleFan;
    g_render->DrawArrays(dm, verts.data(), numVerts);
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Arc
/// @brief 绘制圆弧
/// @param mode number 1=线框 2=填充
/// @param x number 中心 X
/// @param y number 中心 Y
/// @param z number 深度
/// @param radius number 半径
/// @param startAngle number 起始角度 (度)
/// @param endAngle number 结束角度 (度)
/// @param segments number 圆弧段数
/// @return void
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

    g_render->PushMatrix();
    g_render->Translate(cx, cy, cz);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    std::vector<RenderVertex> verts;
    verts.reserve(segs + 2);
    if (mode == 2) verts.push_back({0, 0, 0, 0, 0, cr, cg, cb, ca});
    for (int i = 0; i <= segs; ++i) {
        float t = start + (end - start) * i / segs;
        float rad = (float)(t * M_PI / 180.0);
        verts.push_back({radius * cosf(rad), radius * sinf(rad), 0, 0, 0, cr, cg, cb, ca});
    }
    DrawMode dm = (mode == 1) ? DrawMode::LineStrip : DrawMode::TriangleFan;
    g_render->DrawArrays(dm, verts.data(), (int)verts.size());
    g_render->PopMatrix();
    return 0;
}

/// @lua_api Light.Graphics.Circle
/// @brief 绘制圆形
/// @param mode number 1=线框 2=填充
/// @param x number 中心 X
/// @param y number 中心 Y
/// @param z number 深度
/// @param radius number 半径
/// @param segments number 分段数
/// @return void
static int l_Circle(lua_State* L) {
    int mode = (int)luaL_checkinteger(L, 1);
    float cx     = (float)luaL_checknumber(L, 2);
    float cy     = (float)luaL_checknumber(L, 3);
    float cz     = (float)luaL_checknumber(L, 4);
    float radius = (float)luaL_checknumber(L, 5);
    int segs     = (int)luaL_checkinteger(L, 6);

    float rx, ry, rz, sx, sy, sz, ox, oy, oz;
    ReadTransform(L, 7, &rx, &ry, &rz, &sx, &sy, &sz, &ox, &oy, &oz);

    g_render->PushMatrix();
    g_render->Translate(cx, cy, cz);
    ApplyTransform(rx, ry, rz, sx, sy, sz, ox, oy, oz);
    ApplyDrawColor();
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];

    std::vector<RenderVertex> verts;
    verts.reserve(segs + 2);
    if (mode == 2) verts.push_back({0, 0, 0, 0, 0, cr, cg, cb, ca});
    for (int i = 0; i <= segs; ++i) {
        float t = (float)(2.0 * M_PI * i / segs);
        verts.push_back({radius * cosf(t), radius * sinf(t), 0, 0, 0, cr, cg, cb, ca});
    }
    DrawMode dm = (mode == 1) ? DrawMode::LineLoop : DrawMode::TriangleFan;
    g_render->DrawArrays(dm, verts.data(), (int)verts.size());
    g_render->PopMatrix();
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

/// @lua_api Light.Graphics.DrawSprite
/// @brief 绘制 WAS 精灵帧
/// @param spriteData table GetSpriteImagesData 返回的精灵数据
/// @param frameIdx number 帧索引 (1-based)
/// @param x number? 屏幕位置 X
/// @param y number? 屏幕位置 Y
/// @return void
/// @note 懒加载: 首次绘制时将 pixels 上传为纹理, 缓存在 frame.__texId
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
        // 懒创建: 从 pixels userdata 上传纹理 (通过渲染后端)
        lua_getfield(L, frameTableIdx, "pixels");
        if (lua_isuserdata(L, -1)) {
            const void* pixels = lua_touserdata(L, -1);
            texId = g_render->CreateTexture(fw, fh, 4, pixels);

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

    // 渲染 (通过渲染后端)
    g_render->PushMatrix();
    g_render->Translate(x + ox, y + oy, z);
    ApplyTransform(rx, ry, rz, tsx, tsy, tsz, tox, toy, toz);
    ApplyDrawColor();

    float ffW = (float)fw, ffH = (float)fh;
    float cr = g_ctx.drawColor[0], cg = g_ctx.drawColor[1];
    float cb = g_ctx.drawColor[2], ca = g_ctx.drawColor[3];
    RenderVertex verts[4] = {
        {0,   0,   0,  0, 0,  cr, cg, cb, ca},
        {ffW, 0,   0,  1, 0,  cr, cg, cb, ca},
        {ffW, ffH, 0,  1, 1,  cr, cg, cb, ca},
        {0,   ffH, 0,  0, 1,  cr, cg, cb, ca},
    };
    SubmitOrDraw(DrawMode::Quads, verts, 4, texId); // Phase A7
    g_render->PopMatrix();

    lua_pop(L, 2);  // pop frame + frames
    return 0;
}

// ==================== Phase E.3.3 — Light.Graphics.HDR Lua API ====================
//
// 子表 Light.Graphics.HDR 挂在 luaopen_Light_Graphics 注册时附加.
//
// API 设计:
//   Enable(w, h) -> bool        创建 HDR RT (RGBA16F); 失败 false
//   Disable()                   释放 HDR RT
//   IsEnabled() -> bool         当前是否启用 HDR
//   IsSupported() -> bool       后端是否支持 (GL33 = true, Legacy = false)
//   Resize(w, h) -> bool        变更 HDR RT 尺寸 (内部 Disable + Enable)
//   SetExposure(v)              线性曝光预乘 (默认 1.0)
//   GetExposure() -> number
//   SetGamma(v)                 sRGB encode gamma (默认 2.2)
//   GetGamma() -> number
//   GetSceneTexture() -> int    HDR RT 的 GL texture id (0 = 未启用)

/// @lua_api Light.Graphics.HDR.Enable
/// @brief 启用 HDR 离屏管线 (创建 RGBA16F RT)
/// @param w number RT 宽度 (像素, > 0)
/// @param h number RT 高度 (像素, > 0)
/// @return boolean true 成功; false = 后端不支持 / 参数非法 / 资源创建失败
static int l_HDR_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, HDRRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.Disable
/// @brief 禁用 HDR 离屏管线 (释放 RT, 后续主循环走 LDR 路径)
/// @return void
static int l_HDR_Disable(lua_State* L) {
    HDRRenderer::Disable();
    return 0;
}

/// @lua_api Light.Graphics.HDR.IsEnabled
/// @return boolean HDR 是否启用
static int l_HDR_IsEnabled(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.IsSupported
/// @brief 查询后端是否支持 HDR (Enable 前可查)
/// @return boolean GL33 = true, Legacy / 不支持 float RT 的后端 = false
static int l_HDR_IsSupported(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::IsSupported() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.Resize
/// @brief 变更 HDR RT 尺寸 (窗口 resize 时调用方主动调)
/// @param w number
/// @param h number
/// @return boolean
static int l_HDR_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, HDRRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetExposure
/// @param v number 线性曝光倍率 (默认 1.0; < 1.0 = 暗; > 1.0 = 亮)
static int l_HDR_SetExposure(lua_State* L) {
    HDRRenderer::SetExposure((float)luaL_checknumber(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.HDR.GetExposure
/// @return number
static int l_HDR_GetExposure(lua_State* L) {
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetExposure());
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetGamma
/// @param v number sRGB encode gamma (默认 2.2)
static int l_HDR_SetGamma(lua_State* L) {
    HDRRenderer::SetGamma((float)luaL_checknumber(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.HDR.GetGamma
/// @return number
static int l_HDR_GetGamma(lua_State* L) {
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetGamma());
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetSceneTexture
/// @brief 高级用法: 获取 HDR RT 的 GL texture id (用于自定义 shader 采样)
/// @return integer 0 = 未启用 / 非 GL33 后端
static int l_HDR_GetSceneTexture(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)HDRRenderer::GetSceneTexture());
    return 1;
}

// ==================== Phase E.3.4 — Tonemap operator (按字符串名) ====================

/// 字符串 → tonemap mode int (无效名回退 ACES)
/// 支持大小写无关: "aces" / "ACES" / "Reinhard" 等
static int hdr_tonemap_name_to_mode(const char* s) {
    if (!s) return HDRRenderer::TONEMAP_ACES;
    // 简易大小写无关比较 (4 个常量, 不引外部库)
    auto eq_ci = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == 0 && *b == 0;
    };
    if (eq_ci(s, "aces"))       return HDRRenderer::TONEMAP_ACES;
    if (eq_ci(s, "reinhard"))   return HDRRenderer::TONEMAP_REINHARD;
    if (eq_ci(s, "uncharted2")) return HDRRenderer::TONEMAP_UNCHARTED2;
    if (eq_ci(s, "linear"))     return HDRRenderer::TONEMAP_LINEAR;
    return HDRRenderer::TONEMAP_ACES;   // 未知名 → ACES
}

/// int → 字符串名 (规范小写)
static const char* hdr_tonemap_mode_to_name(int mode) {
    switch (mode) {
        case HDRRenderer::TONEMAP_REINHARD:   return "reinhard";
        case HDRRenderer::TONEMAP_UNCHARTED2: return "uncharted2";
        case HDRRenderer::TONEMAP_LINEAR:     return "linear";
        case HDRRenderer::TONEMAP_ACES:
        default:                              return "aces";
    }
}

/// @lua_api Light.Graphics.HDR.SetTonemapper
/// @brief 选择 tonemap 曲线 (Phase E.3.4)
/// @param name string "aces" | "reinhard" | "uncharted2" | "linear" (大小写无关; 未知名回退 "aces")
static int l_HDR_SetTonemapper(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    HDRRenderer::SetTonemapper(hdr_tonemap_name_to_mode(name));
    return 0;
}

/// @lua_api Light.Graphics.HDR.GetTonemapper
/// @return string 当前 tonemap operator 名 (规范小写: "aces" | "reinhard" | "uncharted2" | "linear")
static int l_HDR_GetTonemapper(lua_State* L) {
    lua_pushstring(L, hdr_tonemap_mode_to_name(HDRRenderer::GetTonemapper()));
    return 1;
}

static const luaL_Reg hdr_funcs[] = {
    {"Enable",          l_HDR_Enable},
    {"Disable",         l_HDR_Disable},
    {"IsEnabled",       l_HDR_IsEnabled},
    {"IsSupported",     l_HDR_IsSupported},
    {"Resize",          l_HDR_Resize},
    {"SetExposure",     l_HDR_SetExposure},
    {"GetExposure",     l_HDR_GetExposure},
    {"SetGamma",        l_HDR_SetGamma},
    {"GetGamma",        l_HDR_GetGamma},
    {"GetSceneTexture", l_HDR_GetSceneTexture},
    // Phase E.3.4
    {"SetTonemapper",   l_HDR_SetTonemapper},
    {"GetTonemapper",   l_HDR_GetTonemapper},
    {NULL, NULL}
};

// ==================== Phase E.4.3 — Light.Graphics.Bloom Lua API ====================
//
// 子表 Light.Graphics.Bloom 挂在 luaopen_Light_Graphics 注册时附加 (在 HDR 子表之后).
//
// API 设计 (13 函数):
//   Enable(w, h) -> bool           显式启用 (通常不用; HDR.Enable 默认自动联动)
//   Disable()                       关闭 Bloom (HDR 可保留)
//   IsEnabled() -> bool
//   IsSupported() -> bool          后端是否支持 (GL33 = true)
//   Resize(w, h) -> bool
//   SetAutoEnable(flag)             HDR.Enable 时是否自动拉起 Bloom (默认 true)
//   GetAutoEnable() -> bool
//   SetThreshold(v)                亮度阈值 (默认 1.0; clamp [0, +inf))
//   GetThreshold() -> number
//   SetIntensity(v)                合成强度 (默认 0.8; clamp [0, +inf))
//   GetIntensity() -> number
//   SetRadius(v)                   扩散半径 (默认 0.7; clamp [0, 1])
//   GetRadius() -> number
//   SetLevels(n)                   pyramid 层数 (默认 5; clamp [2, 8]; 下次 Enable/Resize 生效)
//   GetLevels() -> integer

/// @lua_api Light.Graphics.Bloom.Enable
/// @brief 启用 Bloom 管线 (通常不用; 默认 autoEnable=true, HDR.Enable 会自动拉起)
/// @param w number pyramid[0] 宽
/// @param h number pyramid[0] 高
/// @return boolean true 成功; false = 后端不支持 / 参数非法 / 资源分配失败
static int l_Bloom_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, BloomRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.Disable
static int l_Bloom_Disable(lua_State* L) {
    (void)L;
    BloomRenderer::Disable();
    return 0;
}

/// @lua_api Light.Graphics.Bloom.IsEnabled
static int l_Bloom_IsEnabled(lua_State* L) {
    lua_pushboolean(L, BloomRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.IsSupported
static int l_Bloom_IsSupported(lua_State* L) {
    lua_pushboolean(L, BloomRenderer::IsSupported() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.Resize
static int l_Bloom_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, BloomRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetAutoEnable
/// @brief 设置 HDR.Enable 时是否自动拉起 Bloom (默认 true)
/// @param flag boolean
static int l_Bloom_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    BloomRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.Bloom.GetAutoEnable
/// @return boolean
static int l_Bloom_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, BloomRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetThreshold
/// @param v number 亮度阈值 (clamp [0, +inf))
static int l_Bloom_SetThreshold(lua_State* L) {
    BloomRenderer::SetThreshold((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_Bloom_GetThreshold(lua_State* L) {
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetThreshold());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetIntensity
/// @param v number 合成强度 (clamp [0, +inf))
static int l_Bloom_SetIntensity(lua_State* L) {
    BloomRenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_Bloom_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetIntensity());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetRadius
/// @param v number 扩散半径 (clamp [0, 1])
static int l_Bloom_SetRadius(lua_State* L) {
    BloomRenderer::SetRadius((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_Bloom_GetRadius(lua_State* L) {
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetRadius());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetLevels
/// @param n integer pyramid 层数 (clamp [2, 8]; 下次 Enable/Resize 生效)
static int l_Bloom_SetLevels(lua_State* L) {
    BloomRenderer::SetLevels((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_Bloom_GetLevels(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)BloomRenderer::GetLevels());
    return 1;
}

static const luaL_Reg bloom_funcs[] = {
    {"Enable",          l_Bloom_Enable},
    {"Disable",         l_Bloom_Disable},
    {"IsEnabled",       l_Bloom_IsEnabled},
    {"IsSupported",     l_Bloom_IsSupported},
    {"Resize",          l_Bloom_Resize},
    {"SetAutoEnable",   l_Bloom_SetAutoEnable},
    {"GetAutoEnable",   l_Bloom_GetAutoEnable},
    {"SetThreshold",    l_Bloom_SetThreshold},
    {"GetThreshold",    l_Bloom_GetThreshold},
    {"SetIntensity",    l_Bloom_SetIntensity},
    {"GetIntensity",    l_Bloom_GetIntensity},
    {"SetRadius",       l_Bloom_SetRadius},
    {"GetRadius",       l_Bloom_GetRadius},
    {"SetLevels",       l_Bloom_SetLevels},
    {"GetLevels",       l_Bloom_GetLevels},
    {NULL, NULL}
};

// ==================== Phase E.5.3 — Light.Graphics.AutoExposure Lua API ====================
//
// 子表 Light.Graphics.AutoExposure 挂在 luaopen_Light_Graphics 注册时附加 (在 Bloom 子表之后).
//
// API 设计 (18 函数):
//   生命周期 5: Enable(w,h)/Disable/IsEnabled/IsSupported/Resize(w,h)
//   联动 2:    SetAutoEnable(flag)/GetAutoEnable     (默认 false; 与 Bloom 默认 true 区别)
//   参数 10:   Set+Get TargetEV/SpeedUp/SpeedDown/MinEV/MaxEV
//   debug 3:   GetCurrentEV/GetCurrentExposure/GetMeasuredLuminance

/// @lua_api Light.Graphics.AutoExposure.Enable
/// @param w integer; @param h integer; @return boolean
static int l_AE_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, AutoExposureRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_AE_Disable(lua_State* L) {
    (void)L;
    AutoExposureRenderer::Disable();
    return 0;
}

static int l_AE_IsEnabled(lua_State* L) {
    lua_pushboolean(L, AutoExposureRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

static int l_AE_IsSupported(lua_State* L) {
    lua_pushboolean(L, AutoExposureRenderer::IsSupported() ? 1 : 0);
    return 1;
}

static int l_AE_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, AutoExposureRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_AE_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    AutoExposureRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_AE_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, AutoExposureRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.SetTargetEV
/// @param v number 中灰偏移 EV (默认 0.0)
static int l_AE_SetTargetEV(lua_State* L) {
    AutoExposureRenderer::SetTargetEV((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_AE_GetTargetEV(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetTargetEV());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.SetSpeedUp
/// @param v number 暗→亮 适应速度 EV/sec (clamp [0.1, 20])
static int l_AE_SetSpeedUp(lua_State* L) {
    AutoExposureRenderer::SetSpeedUp((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_AE_GetSpeedUp(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetSpeedUp());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.SetSpeedDown
/// @param v number 亮→暗 适应速度 EV/sec (clamp [0.1, 20])
static int l_AE_SetSpeedDown(lua_State* L) {
    AutoExposureRenderer::SetSpeedDown((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_AE_GetSpeedDown(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetSpeedDown());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.SetMinEV
/// @param v number 当前 EV 下限 (默认 -8.0)
static int l_AE_SetMinEV(lua_State* L) {
    AutoExposureRenderer::SetMinEV((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_AE_GetMinEV(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetMinEV());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.SetMaxEV
/// @param v number 当前 EV 上限 (默认 +8.0)
static int l_AE_SetMaxEV(lua_State* L) {
    AutoExposureRenderer::SetMaxEV((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_AE_GetMaxEV(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetMaxEV());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.GetCurrentEV
/// @return number 平滑后当前 EV
static int l_AE_GetCurrentEV(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetCurrentEV());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.GetCurrentExposure
/// @return number 当前 EV 转 exposure 倍率 (= 2^GetCurrentEV)
static int l_AE_GetCurrentExposure(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetCurrentExposure());
    return 1;
}

/// @lua_api Light.Graphics.AutoExposure.GetMeasuredLuminance
/// @return number 上一帧测得的 log luma (debug)
static int l_AE_GetMeasuredLuminance(lua_State* L) {
    lua_pushnumber(L, (lua_Number)AutoExposureRenderer::GetMeasuredLuminance());
    return 1;
}

static const luaL_Reg ae_funcs[] = {
    {"Enable",                l_AE_Enable},
    {"Disable",               l_AE_Disable},
    {"IsEnabled",             l_AE_IsEnabled},
    {"IsSupported",           l_AE_IsSupported},
    {"Resize",                l_AE_Resize},
    {"SetAutoEnable",         l_AE_SetAutoEnable},
    {"GetAutoEnable",         l_AE_GetAutoEnable},
    {"SetTargetEV",           l_AE_SetTargetEV},
    {"GetTargetEV",           l_AE_GetTargetEV},
    {"SetSpeedUp",            l_AE_SetSpeedUp},
    {"GetSpeedUp",            l_AE_GetSpeedUp},
    {"SetSpeedDown",          l_AE_SetSpeedDown},
    {"GetSpeedDown",          l_AE_GetSpeedDown},
    {"SetMinEV",              l_AE_SetMinEV},
    {"GetMinEV",              l_AE_GetMinEV},
    {"SetMaxEV",              l_AE_SetMaxEV},
    {"GetMaxEV",              l_AE_GetMaxEV},
    {"GetCurrentEV",          l_AE_GetCurrentEV},
    {"GetCurrentExposure",    l_AE_GetCurrentExposure},
    {"GetMeasuredLuminance",  l_AE_GetMeasuredLuminance},
    {NULL, NULL}
};

// ==================== Phase E.6.3 — Light.Graphics.LensDirt Lua API ====================
//
// 子表 Light.Graphics.LensDirt 挂在 luaopen_Light_Graphics 注册时附加 (AE 子表之后).
//
// API 设计 (10 函数):
//   生命周期 4: Enable / Disable / IsEnabled / IsSupported
//   联动 2:    SetAutoEnable / GetAutoEnable            (默认 false)
//   参数 4:    SetDirtTexture / GetDirtTextureId / SetIntensity / GetIntensity

static int l_LD_Enable(lua_State* L) {
    (void)L;
    lua_pushboolean(L, LensDirtRenderer::Enable() ? 1 : 0);
    return 1;
}

static int l_LD_Disable(lua_State* L) {
    (void)L;
    LensDirtRenderer::Disable();
    return 0;
}

static int l_LD_IsEnabled(lua_State* L) {
    lua_pushboolean(L, LensDirtRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

static int l_LD_IsSupported(lua_State* L) {
    lua_pushboolean(L, LensDirtRenderer::IsSupported() ? 1 : 0);
    return 1;
}

static int l_LD_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    LensDirtRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_LD_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, LensDirtRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.LensDirt.SetDirtTexture
/// @param arg Image table / number (tex id) / nil (reset to fallback)
/// @brief 接受 Image userdata (table with :GetTextureId()) / number / nil
static int l_LD_SetDirtTexture(lua_State* L) {
    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        LensDirtRenderer::SetDirtTextureId(0);    // fallback to backend 1x1 white
        return 0;
    }
    int t = lua_type(L, 1);
    if (t == LUA_TNUMBER) {
        // 直接是 GL tex id
        LensDirtRenderer::SetDirtTextureId((uint32_t)lua_tointeger(L, 1));
        return 0;
    }
    if (t == LUA_TTABLE) {
        // Image table: 调用 :GetTextureId() 取 id
        lua_getfield(L, 1, "GetTextureId");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "LensDirt.SetDirtTexture: table missing GetTextureId() method");
        }
        lua_pushvalue(L, 1);   // self
        lua_call(L, 1, 1);
        uint32_t tid = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        LensDirtRenderer::SetDirtTextureId(tid);
        return 0;
    }
    return luaL_error(L, "LensDirt.SetDirtTexture: expected Image table, number, or nil (got %s)",
                     lua_typename(L, t));
}

static int l_LD_GetDirtTextureId(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LensDirtRenderer::GetDirtTextureId());
    return 1;
}

static int l_LD_SetIntensity(lua_State* L) {
    LensDirtRenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LD_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensDirtRenderer::GetIntensity());
    return 1;
}

static const luaL_Reg lens_dirt_funcs[] = {
    {"Enable",            l_LD_Enable},
    {"Disable",           l_LD_Disable},
    {"IsEnabled",         l_LD_IsEnabled},
    {"IsSupported",       l_LD_IsSupported},
    {"SetAutoEnable",     l_LD_SetAutoEnable},
    {"GetAutoEnable",     l_LD_GetAutoEnable},
    {"SetDirtTexture",    l_LD_SetDirtTexture},
    {"GetDirtTextureId",  l_LD_GetDirtTextureId},
    {"SetIntensity",      l_LD_SetIntensity},
    {"GetIntensity",      l_LD_GetIntensity},
    {NULL, NULL}
};

// ==================== Phase E.6.3 — Light.Graphics.Streak Lua API ====================
//
// API 设计 (13 函数):
//   生命周期 5: Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
//   联动 2:    SetAutoEnable / GetAutoEnable           (默认 false)
//   参数 6:    Set+Get Threshold / Intensity / Length / Iterations (+ Direction 2返)

static int l_ST_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, StreakRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_ST_Disable(lua_State* L) {
    (void)L;
    StreakRenderer::Disable();
    return 0;
}

static int l_ST_IsEnabled(lua_State* L) {
    lua_pushboolean(L, StreakRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

static int l_ST_IsSupported(lua_State* L) {
    lua_pushboolean(L, StreakRenderer::IsSupported() ? 1 : 0);
    return 1;
}

static int l_ST_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, StreakRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_ST_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    StreakRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_ST_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, StreakRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

static int l_ST_SetThreshold(lua_State* L) {
    StreakRenderer::SetThreshold((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_ST_GetThreshold(lua_State* L) {
    lua_pushnumber(L, (lua_Number)StreakRenderer::GetThreshold());
    return 1;
}

static int l_ST_SetIntensity(lua_State* L) {
    StreakRenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_ST_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)StreakRenderer::GetIntensity());
    return 1;
}

static int l_ST_SetLength(lua_State* L) {
    StreakRenderer::SetLength((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_ST_GetLength(lua_State* L) {
    lua_pushnumber(L, (lua_Number)StreakRenderer::GetLength());
    return 1;
}

/// @param x number; @param y number
static int l_ST_SetDirection(lua_State* L) {
    float x = (float)luaL_checknumber(L, 1);
    float y = (float)luaL_checknumber(L, 2);
    StreakRenderer::SetDirection(x, y);
    return 0;
}

/// @return x, y (2 returns)
static int l_ST_GetDirection(lua_State* L) {
    float x = 0.0f, y = 0.0f;
    StreakRenderer::GetDirection(x, y);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 2;
}

static int l_ST_SetIterations(lua_State* L) {
    StreakRenderer::SetIterations((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_ST_GetIterations(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)StreakRenderer::GetIterations());
    return 1;
}

static const luaL_Reg streak_funcs[] = {
    {"Enable",          l_ST_Enable},
    {"Disable",         l_ST_Disable},
    {"IsEnabled",       l_ST_IsEnabled},
    {"IsSupported",     l_ST_IsSupported},
    {"Resize",          l_ST_Resize},
    {"SetAutoEnable",   l_ST_SetAutoEnable},
    {"GetAutoEnable",   l_ST_GetAutoEnable},
    {"SetThreshold",    l_ST_SetThreshold},
    {"GetThreshold",    l_ST_GetThreshold},
    {"SetIntensity",    l_ST_SetIntensity},
    {"GetIntensity",    l_ST_GetIntensity},
    {"SetLength",       l_ST_SetLength},
    {"GetLength",       l_ST_GetLength},
    {"SetDirection",    l_ST_SetDirection},
    {"GetDirection",    l_ST_GetDirection},
    {"SetIterations",   l_ST_SetIterations},
    {"GetIterations",   l_ST_GetIterations},
    {NULL, NULL}
};

// ==================== Phase E.7.3 — Light.Graphics.LensFlare Lua API ====================
//
// API (21 fn): lifecycle 5 + autoEnable 2 + params 14 (7 Set+Get pairs)

static int l_LF_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, LensFlareRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_LF_Disable(lua_State* L)     { (void)L; LensFlareRenderer::Disable(); return 0; }
static int l_LF_IsEnabled(lua_State* L)   { lua_pushboolean(L, LensFlareRenderer::IsEnabled()   ? 1 : 0); return 1; }
static int l_LF_IsSupported(lua_State* L) { lua_pushboolean(L, LensFlareRenderer::IsSupported() ? 1 : 0); return 1; }

static int l_LF_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, LensFlareRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_LF_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    LensFlareRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_LF_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, LensFlareRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

static int l_LF_SetThreshold(lua_State* L) {
    LensFlareRenderer::SetThreshold((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LF_GetThreshold(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensFlareRenderer::GetThreshold());
    return 1;
}

static int l_LF_SetIntensity(lua_State* L) {
    LensFlareRenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LF_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensFlareRenderer::GetIntensity());
    return 1;
}

static int l_LF_SetGhostCount(lua_State* L) {
    LensFlareRenderer::SetGhostCount((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_LF_GetGhostCount(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LensFlareRenderer::GetGhostCount());
    return 1;
}

static int l_LF_SetGhostDispersal(lua_State* L) {
    LensFlareRenderer::SetGhostDispersal((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LF_GetGhostDispersal(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensFlareRenderer::GetGhostDispersal());
    return 1;
}

static int l_LF_SetHaloWidth(lua_State* L) {
    LensFlareRenderer::SetHaloWidth((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LF_GetHaloWidth(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensFlareRenderer::GetHaloWidth());
    return 1;
}

static int l_LF_SetChromaticAberration(lua_State* L) {
    LensFlareRenderer::SetChromaticAberration((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_LF_GetChromaticAberration(lua_State* L) {
    lua_pushnumber(L, (lua_Number)LensFlareRenderer::GetChromaticAberration());
    return 1;
}

static int l_LF_SetDistortionEnabled(lua_State* L) {
    luaL_checkany(L, 1);
    LensFlareRenderer::SetDistortionEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_LF_GetDistortionEnabled(lua_State* L) {
    lua_pushboolean(L, LensFlareRenderer::GetDistortionEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.LensFlare.SetFlareTexture
/// @param arg Image table (with :GetTextureId()) / number (tex id) / nil (reset to fallback)
/// @brief Phase E.7.4 — 接受 Image userdata / number / nil; nil 时回到 1x1 白 fallback (纯 procedural)
static int l_LF_SetFlareTexture(lua_State* L) {
    if (lua_gettop(L) < 1 || lua_isnil(L, 1)) {
        LensFlareRenderer::SetFlareTextureId(0);    // fallback to backend 1x1 white
        return 0;
    }
    int t = lua_type(L, 1);
    if (t == LUA_TNUMBER) {
        // 直接是 GL tex id
        LensFlareRenderer::SetFlareTextureId((uint32_t)lua_tointeger(L, 1));
        return 0;
    }
    if (t == LUA_TTABLE) {
        // Image userdata: 调 :GetTextureId()
        lua_getfield(L, 1, "GetTextureId");
        if (!lua_isfunction(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "LensFlare.SetFlareTexture: table missing GetTextureId() method");
        }
        lua_pushvalue(L, 1);   // self
        lua_call(L, 1, 1);
        uint32_t tid = (uint32_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
        LensFlareRenderer::SetFlareTextureId(tid);
        return 0;
    }
    return luaL_error(L, "LensFlare.SetFlareTexture: expected Image table, number, or nil (got %s)",
                     lua_typename(L, t));
}

static int l_LF_GetFlareTextureId(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)LensFlareRenderer::GetFlareTextureId());
    return 1;
}

static const luaL_Reg lens_flare_funcs[] = {
    {"Enable",                  l_LF_Enable},
    {"Disable",                 l_LF_Disable},
    {"IsEnabled",               l_LF_IsEnabled},
    {"IsSupported",             l_LF_IsSupported},
    {"Resize",                  l_LF_Resize},
    {"SetAutoEnable",           l_LF_SetAutoEnable},
    {"GetAutoEnable",           l_LF_GetAutoEnable},
    {"SetThreshold",            l_LF_SetThreshold},
    {"GetThreshold",            l_LF_GetThreshold},
    {"SetIntensity",            l_LF_SetIntensity},
    {"GetIntensity",            l_LF_GetIntensity},
    {"SetGhostCount",           l_LF_SetGhostCount},
    {"GetGhostCount",           l_LF_GetGhostCount},
    {"SetGhostDispersal",       l_LF_SetGhostDispersal},
    {"GetGhostDispersal",       l_LF_GetGhostDispersal},
    {"SetHaloWidth",            l_LF_SetHaloWidth},
    {"GetHaloWidth",            l_LF_GetHaloWidth},
    {"SetChromaticAberration",  l_LF_SetChromaticAberration},
    {"GetChromaticAberration",  l_LF_GetChromaticAberration},
    {"SetDistortionEnabled",    l_LF_SetDistortionEnabled},
    {"GetDistortionEnabled",    l_LF_GetDistortionEnabled},
    {"SetFlareTexture",         l_LF_SetFlareTexture},        // Phase E.7.4
    {"GetFlareTextureId",       l_LF_GetFlareTextureId},      // Phase E.7.4
    {NULL, NULL}
};

// ==================== Phase E.8.3 — Light.Graphics.SSAO Lua API ====================
//
// API (19 fn): lifecycle 5 + autoEnable 2 + params 12 (6 Set+Get pairs)
//   Lifecycle:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
//   AutoEnable: SetAutoEnable / GetAutoEnable
//   Params:     SetRadius / GetRadius                  (float [0.05, 5.0])
//               SetBias / GetBias                      (float [0, 0.2])
//               SetIntensity / GetIntensity            (float [0, 4.0])
//               SetKernelSize / GetKernelSize          (int {8, 16})
//               SetPower / GetPower                    (float [0.5, 8.0])
//               SetBlurEnabled / GetBlurEnabled        (bool)

static int l_SSAO_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SSAORenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_SSAO_Disable(lua_State* L)     { (void)L; SSAORenderer::Disable(); return 0; }
static int l_SSAO_IsEnabled(lua_State* L)   { lua_pushboolean(L, SSAORenderer::IsEnabled()   ? 1 : 0); return 1; }
static int l_SSAO_IsSupported(lua_State* L) { lua_pushboolean(L, SSAORenderer::IsSupported() ? 1 : 0); return 1; }

static int l_SSAO_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SSAORenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_SSAO_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    SSAORenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_SSAO_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, SSAORenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

static int l_SSAO_SetRadius(lua_State* L) {
    SSAORenderer::SetRadius((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSAO_GetRadius(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSAORenderer::GetRadius());
    return 1;
}

static int l_SSAO_SetBias(lua_State* L) {
    SSAORenderer::SetBias((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSAO_GetBias(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSAORenderer::GetBias());
    return 1;
}

static int l_SSAO_SetIntensity(lua_State* L) {
    SSAORenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSAO_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSAORenderer::GetIntensity());
    return 1;
}

static int l_SSAO_SetKernelSize(lua_State* L) {
    SSAORenderer::SetKernelSize((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_SSAO_GetKernelSize(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SSAORenderer::GetKernelSize());
    return 1;
}

static int l_SSAO_SetPower(lua_State* L) {
    SSAORenderer::SetPower((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSAO_GetPower(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSAORenderer::GetPower());
    return 1;
}

static int l_SSAO_SetBlurEnabled(lua_State* L) {
    luaL_checkany(L, 1);
    SSAORenderer::SetBlurEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_SSAO_GetBlurEnabled(lua_State* L) {
    lua_pushboolean(L, SSAORenderer::GetBlurEnabled() ? 1 : 0);
    return 1;
}

static const luaL_Reg ssao_funcs[] = {
    {"Enable",          l_SSAO_Enable},
    {"Disable",         l_SSAO_Disable},
    {"IsEnabled",       l_SSAO_IsEnabled},
    {"IsSupported",     l_SSAO_IsSupported},
    {"Resize",          l_SSAO_Resize},
    {"SetAutoEnable",   l_SSAO_SetAutoEnable},
    {"GetAutoEnable",   l_SSAO_GetAutoEnable},
    {"SetRadius",       l_SSAO_SetRadius},
    {"GetRadius",       l_SSAO_GetRadius},
    {"SetBias",         l_SSAO_SetBias},
    {"GetBias",         l_SSAO_GetBias},
    {"SetIntensity",    l_SSAO_SetIntensity},
    {"GetIntensity",    l_SSAO_GetIntensity},
    {"SetKernelSize",   l_SSAO_SetKernelSize},
    {"GetKernelSize",   l_SSAO_GetKernelSize},
    {"SetPower",        l_SSAO_SetPower},
    {"GetPower",        l_SSAO_GetPower},
    {"SetBlurEnabled",  l_SSAO_SetBlurEnabled},
    {"GetBlurEnabled",  l_SSAO_GetBlurEnabled},
    {NULL, NULL}
};

static const luaL_Reg graphics_funcs[] = {
    // --- 绘图基元 ---
    {"Draw",              l_Draw},
    {"DrawQuad",          l_DrawQuad},
    {"DrawSprite",        l_DrawSprite},
    // Phase E.1.5 — 2D Lit forward (配合 Light.Lighting2D + 可选 normal map)
    {"DrawLit",           l_DrawLit},
    {"DrawLitQuad",       l_DrawLitQuad},
    // Phase E.2.3 — 立即 flush 当前 Lit 批 (画家算法 / 状态切换前调用)
    {"FlushLitBatch",     l_FlushLitBatch},
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
    // Phase AS.1 — Canvas 栈
    {"PushCanvas",        l_PushCanvas},
    {"PopCanvas",         l_PopCanvas},
    // Phase AS.2 — 3D camera + 深度测试
    {"SetPerspective",    l_SetPerspective},
    {"SetCamera",         l_SetCamera},
    {"SetDepthTest",      l_SetDepthTest},
    {"GetDepthTest",      l_GetDepthTest},
    // Phase AS.4 — 多光源 API
    {"SetDirectionalLight",        l_SetDirectionalLight},
    {"SetDirectionalLightEnabled", l_SetDirectionalLightEnabled},
    {"SetAmbientLight",            l_SetAmbientLight},
    {"AddPointLight",              l_AddPointLight},
    {"RemovePointLight",           l_RemovePointLight},
    {"ClearPointLights",           l_ClearPointLights},
    {"GetPointLightCount",         l_GetPointLightCount},
    {"GetMaxPointLights",          l_GetMaxPointLights},
    // Phase AW.x — Backend 内省
    {"GetBackendName",             l_Graphics_GetBackendName},
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

        // Phase E.3.3 — HDR 子表 (Light.Graphics.HDR.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, hdr_funcs, 0);
        lua_setfield(L, -2, "HDR");

        // Phase E.4.3 — Bloom 子表 (Light.Graphics.Bloom.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, bloom_funcs, 0);
        lua_setfield(L, -2, "Bloom");

        // Phase E.5.3 — AutoExposure 子表 (Light.Graphics.AutoExposure.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, ae_funcs, 0);
        lua_setfield(L, -2, "AutoExposure");

        // Phase E.6.3 — LensDirt 子表 (Light.Graphics.LensDirt.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, lens_dirt_funcs, 0);
        lua_setfield(L, -2, "LensDirt");

        // Phase E.6.3 — Streak 子表 (Light.Graphics.Streak.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, streak_funcs, 0);
        lua_setfield(L, -2, "Streak");

        // Phase E.7.3 — LensFlare 子表 (Light.Graphics.LensFlare.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, lens_flare_funcs, 0);
        lua_setfield(L, -2, "LensFlare");

        // Phase E.8.3 — SSAO 子表 (Light.Graphics.SSAO.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, ssao_funcs, 0);
        lua_setfield(L, -2, "SSAO");

        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
