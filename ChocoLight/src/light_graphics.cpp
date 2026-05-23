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
#include "light_lua_helpers.h"  // Phase G.1.7 — 类型安全 helpers + magic
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
#include "ssr_renderer.h"             // Phase E.9 — SSR (屏幕空间反射)
#include "motion_blur_renderer.h"    // Phase E.15 — Velocity-driven Motion Blur
#include "taa_renderer.h"             // Phase F.0 — TAA 主管线
#include "asset_loader.h"
#include <cmath>
#include <cstring>
#include <vector>     // Phase F.0.10.8 — CreateLUT3D Lua table → byte buffer
#include <string>     // Phase F.0.11   — RecordPNGSequence dir prefix
#include <cstdio>     // Phase F.0.11   — snprintf path
#include <utility>

extern "C" {
#include "stb_image_write.h"   // Phase F.0.11 — PNG 编码 (实现在 third_party/stb_impl.c)
}

// Phase F.0.11.5 — EXR HDR screenshot (tinyexr v1.0.7, 实现在 third_party/tinyexr_impl.cpp).
//   编译选项必须与 tinyexr_impl.cpp 一致, 否则 inline 函数与 impl 分歧导致 ODR 违规.
#define TINYEXR_USE_MINIZ     0
#define TINYEXR_USE_STB_ZLIB  1
#define TINYEXR_USE_THREAD    0
#define TINYEXR_USE_OPENMP    0
#include "tinyexr.h"

// Phase F.0.11.6 — MP4 H.264 录屏 (FFmpeg encoder/muxer 动态加载, 实现在 src/record_mp4.cpp).
#include "record_mp4.h"

// Phase F.0.11.6 — 窗口尺寸 extern (impl 在 light_ui.cpp)
extern "C" int Light_GetWindowWidth();
extern "C" int Light_GetWindowHeight();
extern "C" int Light_GetFramebufferWidth();
extern "C" int Light_GetFramebufferHeight();

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
            int fbW = Light_GetFramebufferWidth();
            int fbH = Light_GetFramebufferHeight();
            if (fbW <= 0) fbW = Light_GetWindowWidth();
            if (fbH <= 0) fbH = Light_GetWindowHeight();
            g_render->UnbindFBO();
            g_render->SetViewport(0, 0, fbW, fbH);
        }
        g_ctx.currentCanvas = nullptr;
    } else if (lua_istable(L, 1)) {
        // 保存当前 viewport
        // TODO: 当前简化处理, 后续可扩展 RenderBackend::GetViewport
        lua_getfield(L, 1, "__instance");
        if (lua_isuserdata(L, -1)) {
            // Phase G.1.7: 镜像 CanvasContext 首字段布局 (与 light_graphics_canvas.cpp 严格同步)
            struct CanvasCtx { uint32_t magic; unsigned int fbo, texture, depthRB; int w, h; };
            CanvasCtx* cc = (CanvasCtx*)lua_touserdata(L, -1);
            if (cc && cc->magic == LT::LT_MAGIC_CANVAS && cc->fbo) {
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
// Phase G.1.7: 首字段 magic 与 light_graphics_canvas.cpp::CanvasContext 严格同步
struct CanvasCtxMirror {
    uint32_t     magic;     // 镜像 LT_MAGIC_CANVAS 首字段
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
        // Phase G.1.7: magic 校验拒绝 type confusion
        if (cc && cc->magic == LT::LT_MAGIC_CANVAS && cc->fbo && g_render) {
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
            int fbW = Light_GetFramebufferWidth();
            int fbH = Light_GetFramebufferHeight();
            if (fbW <= 0) fbW = Light_GetWindowWidth();
            if (fbH <= 0) fbH = Light_GetWindowHeight();
            g_render->UnbindFBO();
            g_render->SetViewport(0, 0, fbW, fbH);
        }
        g_ctx.currentCanvas = nullptr;
    } else {
        CanvasCtxMirror* cc = (CanvasCtxMirror*)slot.canvasCtx;
        // Phase G.1.7: magic 校验 (slot.canvasCtx 是当初 push 入栈的 raw ptr, 同样需验证)
        if (cc && cc->magic == LT::LT_MAGIC_CANVAS && cc->fbo && g_render) {
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

// Phase F.0.10.2 — 视口控制 Lua API (split-screen / 双视口分屏基础)
/// @lua_api Light.Graphics.SetViewport
/// @brief 设置 OpenGL viewport (用于 split-screen 或自定义视口区域)
/// @param x number 视口左下角 X (像素, OpenGL 原点在左下)
/// @param y number 视口左下角 Y
/// @param w number 视口宽
/// @param h number 视口高
/// @return void
/// @note 不会自动恢复; 调用者负责后续 reset (典型用法: 渲染完一个 region 后切下一个)
static int l_SetViewport(lua_State* L) {
    // 注意: 类型检查 + 范围检查必须在 g_render=null 之前 (headless smoke 也需要这些验证)
    int x = (int)luaL_checkinteger(L, 1);
    int y = (int)luaL_checkinteger(L, 2);
    int w = (int)luaL_checkinteger(L, 3);
    int h = (int)luaL_checkinteger(L, 4);
    // 防御性: w/h 必须 > 0 (OpenGL 允许负 viewport 但语义混乱)
    if (w <= 0 || h <= 0) {
        return luaL_error(L, "SetViewport: w/h must be > 0 (got w=%d, h=%d)", w, h);
    }
    // backend 调用仅在 g_render 存在时 (headless 无 backend 时 silent skip, 但参数已校验)
    if (g_render) g_render->SetViewport(x, y, w, h);
    return 0;
}

/// @lua_api Light.Graphics.GetViewport
/// @brief 获取当前 OpenGL viewport
/// @return number,number,number,number x,y,w,h
static int l_GetViewport(lua_State* L) {
    int x = 0, y = 0, w = 0, h = 0;
    if (g_render) g_render->GetViewport(&x, &y, &w, &h);
    lua_pushinteger(L, x);
    lua_pushinteger(L, y);
    lua_pushinteger(L, w);
    lua_pushinteger(L, h);
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
        if (raw[0] == LT::LT_MAGIC_CANVAS) {
            if (raw[2] == 0) return false;  // 无纹理
            *texId = raw[2];
            *w = (int)raw[4];
            *h = (int)raw[5];
        } else {
            if (raw[1] == 0) return false;  // 无纹理
            *texId = raw[1];
            *w = (int)raw[3];
            *h = (int)raw[4];
        }
    } else {
        // ImageContext 布局: { texId[0], width[1], height[2] }
        if (raw[0] == LT::LT_MAGIC_IMAGE) {
            if (raw[1] == 0) return false;
            *texId = raw[1];
            *w = (int)raw[2];
            *h = (int)raw[3];
        } else {
            if (raw[0] == 0) return false;
            *texId = raw[0];
            *w = (int)raw[1];
            *h = (int)raw[2];
        }
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

    // 获取 FontContext: 首字段 magic, 后接 texId / atlasW / atlasH / fontSize
    // Phase G.1.7: 镜像 light_graphics_image.cpp::FontContext 首字段布局
    struct FontCtxHeader { uint32_t magic; unsigned int texId; int atlasW, atlasH; float fontSize; };
    FontCtxHeader* fch = nullptr;
    void* rawFontCtx = nullptr;
    if (lua_istable(L, 2)) {
        lua_getfield(L, 2, "__instance");
        if (lua_isuserdata(L, -1)) {
            rawFontCtx = lua_touserdata(L, -1);
            fch = (FontCtxHeader*)rawFontCtx;
            // Phase G.1.7: magic 校验拒绝 type confusion (如 Image userdata 伪装为 Font)
            if (fch && fch->magic != LT::LT_MAGIC_FONT) {
                fch = nullptr;
                rawFontCtx = nullptr;
            }
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
            // Phase G.1.2 — VRAM Tracking (Sprite 帧懒创建; Untrack 留 Sprite GC, 现 GC 路径分散 v1.3)
            if (texId) LT::GpuMem::Track("Sprite frame", "RGBA8", fw, fh);

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

/// @lua_api Light.Graphics.HDR.BeginScene
/// @brief Phase F.0.10.9.2 — 手动开始 HDR scene (bind active instance FBO + clear)
/// @return void
/// @note Window:__call 内部已自动调一次 (active=0 时), 手动场景:
///       multi-instance 同帧切换 (主屏 instance 0 + PIP instance 1) 必须显式 BeginScene/EndScene
///       配对调用. HDR 未启用 / paused / 资源失效时静默 no-op.
static int l_HDR_BeginScene(lua_State* L) {
    (void)L;
    HDRRenderer::BeginScene();
    return 0;
}

/// @lua_api Light.Graphics.HDR.EndScene
/// @brief Phase F.0.10.9.2 — 手动结束 HDR scene (unbind FBO + SSAO/AE/LensFx + auto tonemap 若开)
/// @return void
/// @note Window:__call 内部已自动调一次 (active 当前 instance), 手动场景:
///       multi-instance 同帧切换 (主屏 instance 0 + PIP instance 1) 必须显式 BeginScene/EndScene
///       配对调用. autoTonemap=false 时 EndScene 不做 fullscreen tonemap, 用户调 HDR.Tonemap(rgn).
static int l_HDR_EndScene(lua_State* L) {
    (void)L;
    HDRRenderer::EndScene();
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

// ============================================================================
// Phase F.0.10.9.x.4 — SetState helpers (5 renderer 共用)
// 用于解析 lua table 的 K-V 字段, 按类型校验后分发到 SetterFn.
// 类型不符 / 字段缺失: silent skip (使 partial table 友好).
// 调用方需保证 lua_State* L 入参 1 是 table, 且 int applied 局部变量已声明.
// ============================================================================
#define APPLY_NUM(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isnumber(L, -1)) { SETTER((float)lua_tonumber(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_INT(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isnumber(L, -1)) { SETTER((int)lua_tointeger(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_BOOL(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isboolean(L, -1)) { SETTER(lua_toboolean(L, -1) != 0); ++applied; } \
         lua_pop(L, 1); } while(0)
#define APPLY_STR(K, SETTER) \
    do { lua_getfield(L, 1, K); \
         if (lua_isstring(L, -1)) { SETTER(lua_tostring(L, -1)); ++applied; } \
         lua_pop(L, 1); } while(0)

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

// ==================== Phase E.14 — Velocity dilation / format ====================

/// @lua_api Light.Graphics.HDR.SetVelocityDilation
/// @brief 控制 SSRTemporal 是否对 velocity buffer 做 3x3 max-length 邻域采样 (抗几何边缘伪影)
/// @param on boolean 默认 true; 设 false 退化为单点采样 (省 8 次 texture fetch / pixel)
/// @return boolean true = 设置成功; nil, string = 入参非 boolean
static int l_HDR_SetVelocityDilation(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetVelocityDilation: expect boolean");
        return 2;
    }
    bool on = lua_toboolean(L, 1) != 0;
    HDRRenderer::SetVelocityDilation(on);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetVelocityDilation
/// @return boolean 当前 dilation 开关状态 (默认 true)
static int l_HDR_GetVelocityDilation(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetVelocityDilation() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetVelocityDilationHalfRes
/// @brief Phase E.18.1 — dilation pass 半分辨率开关 (默认 OFF / full-res)
/// @param on boolean true=半分辨率 (VRAM -75% / dilation perf +4×) / false=全分辨率
/// @return boolean true = 设置成功; nil, string = 入参非 boolean
/// @note 仅在 dilation pass 启用 (SetVelocityDilation(true) + backend 支持) 时有意义；
///       切换时若已 Enable 立即重建 dilated RT (双 RT 同步)
static int l_HDR_SetVelocityDilationHalfRes(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetVelocityDilationHalfRes: expect boolean");
        return 2;
    }
    bool on = lua_toboolean(L, 1) != 0;
    HDRRenderer::SetVelocityDilationHalfRes(on);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetVelocityDilationHalfRes
/// @return boolean 当前 dilation 半分辨率状态 (默认 false)
static int l_HDR_GetVelocityDilationHalfRes(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetVelocityDilationHalfRes() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetVelocityDilationAutoSkip
/// @brief Phase E.18.2 — dilation pass 自动跳过单消费者场景 (默认 OFF)
/// @param on boolean true=自动检测仅 SSR Temporal 单消费者时跳过 / false=始终运行 dilation pass
/// @return boolean true = 设置成功; nil, string = 入参非 boolean
/// @note 仅 SSR Temporal 单消费者场景受益 (省 1 fetch/px);
///       MB only / SSR+MB / 都不启 时不会跳过 (avoid net loss)
static int l_HDR_SetVelocityDilationAutoSkip(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetVelocityDilationAutoSkip: expect boolean");
        return 2;
    }
    bool on = lua_toboolean(L, 1) != 0;
    HDRRenderer::SetVelocityDilationAutoSkip(on);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetVelocityDilationAutoSkip
/// @return boolean 当前 dilation 自动跳过状态 (默认 false)
static int l_HDR_GetVelocityDilationAutoSkip(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetVelocityDilationAutoSkip() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetAutoTAA
/// @brief Phase F.0.10.2 — 是否在 EndScene 内自动调 TAA Process (默认 true = 零回归)
/// @param on boolean true=自动调用 (老行为) / false=用户手动 TAA.Process / TAA.ProcessRegion 控时序
/// @return boolean true = 设置成功; nil, string = 入参非 boolean
/// @note split-screen 多 instance 场景必须设 false, 让用户手动用 TAA.Process(rgnX, rgnY, rgnW, rgnH) 按区域更新
static int l_HDR_SetAutoTAA(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetAutoTAA: expect boolean");
        return 2;
    }
    bool on = lua_toboolean(L, 1) != 0;
    HDRRenderer::SetAutoTAA(on);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetAutoTAA
/// @return boolean 当前 auto-TAA 状态 (默认 true)
static int l_HDR_GetAutoTAA(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetAutoTAA() ? 1 : 0);
    return 1;
}

// ==================== Phase F.0.10.3 — Auto-Bloom/SSR/MotionBlur (split-screen 多 player 必备) ====================
//
// 三对开关同模式 (与 SetAutoTAA 一致):
//   - 默认 true → EndScene 内自动调对应 Renderer::Process() 全屏处理
//   - 设 false → 用户手动调 Bloom.Process / SSR.Process / MotionBlur.Process 控时序 (含 region 重载)
//   - 入参非 boolean 返 nil + err string

/// @lua_api Light.Graphics.HDR.SetAutoBloom
static int l_HDR_SetAutoBloom(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetAutoBloom: expect boolean");
        return 2;
    }
    HDRRenderer::SetAutoBloom(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetAutoBloom
static int l_HDR_GetAutoBloom(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetAutoBloom() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetAutoSSR
static int l_HDR_SetAutoSSR(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetAutoSSR: expect boolean");
        return 2;
    }
    HDRRenderer::SetAutoSSR(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetAutoSSR
static int l_HDR_GetAutoSSR(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetAutoSSR() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetAutoMotionBlur
static int l_HDR_SetAutoMotionBlur(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetAutoMotionBlur: expect boolean");
        return 2;
    }
    HDRRenderer::SetAutoMotionBlur(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetAutoMotionBlur
static int l_HDR_GetAutoMotionBlur(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetAutoMotionBlur() ? 1 : 0);
    return 1;
}

// ==================== Phase F.0.10.6 — Auto-Tonemap + per-region Tonemap ====================

/// @lua_api Light.Graphics.HDR.SetAutoTonemap
/// @brief 是否在 EndScene 内自动 fullscreen tonemap (默认 true 零回归)
/// @param on boolean true=自动 (老行为) / false=用户手动 HDR.Tonemap(rgn) 控时序与每 region 不同 params
/// @return boolean true = 设置成功; nil, string = 入参非 boolean
/// @note split-screen 多 instance (P1 黄昏 vs P2 冷夜) 场景必须设 false, 用 HDR.Tonemap(rgn, params) 为每 region 独立 tonemap
static int l_HDR_SetAutoTonemap(lua_State* L) {
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetAutoTonemap: expect boolean");
        return 2;
    }
    HDRRenderer::SetAutoTonemap(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetAutoTonemap
/// @return boolean
static int l_HDR_GetAutoTonemap(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetAutoTonemap() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.Tonemap
/// @brief 对指定 region 执行 tonemap pass (split-screen multi-instance 必备)
/// @param rgnX integer (像素, 左下原点 GL convention)
/// @param rgnY integer
/// @param rgnW integer  region 宽度 (<=0 时退化为全屏 tonemap, 与 fullscreen 等价)
/// @param rgnH integer  region 高度 (<=0 时退化为全屏)
/// @param params table? 可选 { exposure=number, gamma=number, tonemap=string|integer }
///                       不传 params 时用全局 (g.exposure 含 AE 叠加 / g.gamma / g.tonemap)
///                       params.tonemap: "aces" | "reinhard" | "uncharted2" | "linear" 或 0..3 整数
/// @return boolean true / nil, string  HDR 未启用 / sceneTex=0 时 nil + 错误描述
static int l_HDR_Tonemap(lua_State* L) {
    const int rgnX = (int)luaL_checkinteger(L, 1);
    const int rgnY = (int)luaL_checkinteger(L, 2);
    const int rgnW = (int)luaL_checkinteger(L, 3);
    const int rgnH = (int)luaL_checkinteger(L, 4);

    // HDR 未启用 silent fail (与 Bloom.Process(rgn) 同模式)
    if (!HDRRenderer::IsEnabled() || HDRRenderer::GetSceneTexture() == 0) {
        lua_pushnil(L);
        lua_pushstring(L, "HDR.Tonemap: HDR not enabled (sceneTex = 0)");
        return 2;
    }

    // params_table 可选 (第 5 参数); 不传 / 非 table 走全局 path
    if (lua_istable(L, 5)) {
        // 解析 5 字段, 缺省回填全局值
        // 1) exposure
        lua_getfield(L, 5, "exposure");
        float exposure = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1)
                                             : HDRRenderer::GetExposure();
        lua_pop(L, 1);
        // 2) gamma
        lua_getfield(L, 5, "gamma");
        float gamma = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1)
                                          : HDRRenderer::GetGamma();
        lua_pop(L, 1);
        // 3) tonemap (string 或 int 双兼容)
        lua_getfield(L, 5, "tonemap");
        int mode = HDRRenderer::GetTonemapper();   // 缺省回填全局
        if (lua_isstring(L, -1) && !lua_isnumber(L, -1)) {
            mode = hdr_tonemap_name_to_mode(lua_tostring(L, -1));
        } else if (lua_isnumber(L, -1)) {
            int v = (int)lua_tointeger(L, -1);
            if (v >= HDRRenderer::TONEMAP_ACES && v <= HDRRenderer::TONEMAP_LINEAR) {
                mode = v;
            }
        }
        lua_pop(L, 1);
        // Phase F.0.10.8 — 4) lut, 5) lutStrength (缺省回填全局)
        lua_getfield(L, 5, "lut");
        uint32_t lutTex = lua_isnumber(L, -1) ? (uint32_t)lua_tointeger(L, -1)
                                              : HDRRenderer::GetGradingLUTId();
        lua_pop(L, 1);
        lua_getfield(L, 5, "lutStrength");
        float lutStrength = lua_isnumber(L, -1) ? (float)lua_tonumber(L, -1)
                                                : HDRRenderer::GetGradingLUTStrength();
        lua_pop(L, 1);
        // 显式走 6 参重载 (params 5 字段完全覆盖, 不混叠 AE / 不沿用全局 LUT)
        HDRRenderer::Tonemap(rgnX, rgnY, rgnW, rgnH, exposure, gamma, mode,
                             lutTex, lutStrength);
    } else {
        // 全局 path: 用 g.exposure (含 AE) / g.gamma / g.tonemap / g.lutTexId / g.lutStrength
        HDRRenderer::Tonemap(rgnX, rgnY, rgnW, rgnH);
    }

    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase F.0.10.8 — 3D LUT (Color Grading) ====================

/// @lua_api Light.Graphics.HDR.CreateLUT3D
/// @param size int LUT 边长 [4, 64]
/// @param data string (binary RGB bytes) 或 table (int array, 0..255)
/// @return integer tex_id (> 0) / nil, string
/// @note data 长度必须 = size^3 * 3 bytes RGB (R 变化最快)
static int l_HDR_CreateLUT3D(lua_State* L) {
    const int size = (int)luaL_checkinteger(L, 1);
    if (size < 4 || size > 64) {
        lua_pushnil(L);
        lua_pushfstring(L, "CreateLUT3D: size %d out of range [4,64]", size);
        return 2;
    }
    const size_t expected = (size_t)size * (size_t)size * (size_t)size * 3u;

    // data 支持 string (binary) 或 table (int array)
    const int t = lua_type(L, 2);
    std::vector<uint8_t> buf;
    const uint8_t* dataPtr = nullptr;
    size_t dataLen = 0;

    if (t == LUA_TSTRING) {
        size_t len = 0;
        const char* s = lua_tolstring(L, 2, &len);
        dataPtr = reinterpret_cast<const uint8_t*>(s);
        dataLen = len;
    } else if (t == LUA_TTABLE) {
        const size_t n = (size_t)lua_objlen(L, 2);   // Lua 5.1 (lumen) API
        buf.resize(n);
        for (size_t i = 0; i < n; ++i) {
            lua_rawgeti(L, 2, (int)(i + 1));
            int v = (int)lua_tointeger(L, -1);
            lua_pop(L, 1);
            if (v < 0) v = 0;
            else if (v > 255) v = 255;
            buf[i] = (uint8_t)v;
        }
        dataPtr = buf.data();
        dataLen = buf.size();
    } else {
        lua_pushnil(L);
        lua_pushfstring(L, "CreateLUT3D: data must be string or table (got %s)",
                        lua_typename(L, t));
        return 2;
    }

    if (dataLen != expected) {
        lua_pushnil(L);
        lua_pushfstring(L, "CreateLUT3D: data length %zu != expected %zu (size^3 * 3)",
                        dataLen, expected);
        return 2;
    }
    uint32_t id = HDRRenderer::CreateLUT3D(size, dataPtr, dataLen);
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, "CreateLUT3D: backend create failed (HDR backend not supported / OOM)");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/// @lua_api Light.Graphics.HDR.DeleteLUT3D
/// @param tex_id integer 来自 CreateLUT3D 的 id
/// @return boolean
static int l_HDR_DeleteLUT3D(lua_State* L) {
    const uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    bool ok = HDRRenderer::DeleteLUT3D(id);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetGradingLUT
/// @param tex_id integer LUT id (0 = 关 LUT)
/// @param strength number 混合强度 [0, 1]
/// @return boolean
static int l_HDR_SetGradingLUT(lua_State* L) {
    const uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    const float    s  = (float)luaL_checknumber(L, 2);
    bool ok = HDRRenderer::SetGradingLUT(id, s);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetGradingLUTId
static int l_HDR_GetGradingLUTId(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)HDRRenderer::GetGradingLUTId());
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetGradingLUTStrength
static int l_HDR_GetGradingLUTStrength(lua_State* L) {
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetGradingLUTStrength());
    return 1;
}

/// @lua_api Light.Graphics.HDR.LoadCubeLUT
/// @brief Phase F.0.10.8.1 — 从 .cube 文件加载 3D LUT (Adobe Cube LUT 1.0)
/// @param path string .cube 文件路径 (相对 CWD 或绝对)
/// @return integer | nil tex_id (>0), string? err
/// @note 不支持 LUT_1D_SIZE; size 必须 ∈ [4, 64]; 注释 + 空行自动 skip
/// @usage local id = HDR.LoadCubeLUT("assets/luts/sunset.cube")
///        if id then HDR.SetGradingLUT(id, 0.8) end
static int l_HDR_LoadCubeLUT(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char errBuf[256] = {0};
    uint32_t id = HDRRenderer::LoadCubeLUTFile(path, errBuf, sizeof(errBuf));
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, errBuf[0] ? errBuf : "LoadCubeLUT: unknown error");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/// @lua_api Light.Graphics.HDR.LoadHaldLUT
/// @brief Phase F.0.10.8.2 — 从 HALD CLUT 图像 (PNG/JPG/BMP/TGA) 加载 3D LUT
/// @param path string 图像文件路径
/// @return integer | nil tex_id (>0), string? err
/// @note HALD level N ∈ [2, 8] → LUT size ∈ [4, 64]; 图像必须方阵 N³×N³
/// @usage local id = HDR.LoadHaldLUT("assets/luts/sepia_hald8.png")
///        if id then HDR.SetGradingLUT(id, 1.0) end
static int l_HDR_LoadHaldLUT(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char errBuf[256] = {0};
    uint32_t id = HDRRenderer::LoadHaldLUTFile(path, errBuf, sizeof(errBuf));
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, errBuf[0] ? errBuf : "LoadHaldLUT: unknown error");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

// Phase G.1.5 — 改为 int 返 push 数量 (LUT: 默认 1)
static int LUTPushResult_(void* L_, AssetLoader::FutureState* state) {
    lua_State* L = (lua_State*)L_;
    if (!L) return 0;
    if (!state || state->status.load() != (int)AssetLoader::FutureStatus::Ready || !state->resLUTId) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)state->resLUTId);
    return 1;
}

static void LUTAsyncDispatcher_(void* L_, AssetLoader::FutureState* state, int cbLuaRef) {
    lua_State* L = (lua_State*)L_;
    if (!L || !state || cbLuaRef < 0) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, cbLuaRef);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return;
    }
    if (state->status.load() == (int)AssetLoader::FutureStatus::Ready) {
        LUTPushResult_(L, state);
        lua_pushnil(L);
    } else {
        lua_pushnil(L);
        lua_pushstring(L, state->errorMsg.empty() ? "unknown error" : state->errorMsg.c_str());
    }
    if (lua_pcall(L, 2, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "LUT LoadAsync cb error: %s", err ? err : "(none)");
        lua_pop(L, 1);
    }
}

static int PushLUTAsyncFuture_(lua_State* L, std::shared_ptr<AssetLoader::FutureState> state, int cbRef) {
    AssetLoader::RegisterResultPusher(state, LUTPushResult_);
    if (cbRef >= 0) {
        AssetLoader::RegisterCallback(state, LUTAsyncDispatcher_, L, cbRef);
        if (state->status.load() != (int)AssetLoader::FutureStatus::Pending) {
            LUTAsyncDispatcher_(L, state.get(), cbRef);
            luaL_unref(L, LUA_REGISTRYINDEX, cbRef);
            state->dispatcher = nullptr;
            state->cbLuaRef = -1;
            state->cbLuaState = nullptr;
        }
    }
    return AssetLoader::PushAsyncFuture(L, std::move(state));
}

static int l_HDR_LoadCubeLUTAsync(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int cbRef = -1;
    if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return PushLUTAsyncFuture_(L, AssetLoader::LoadCubeLUTAsync(path), cbRef);
}

static int l_HDR_LoadHaldLUTAsync(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int cbRef = -1;
    if (lua_gettop(L) >= 2 && lua_isfunction(L, 2)) {
        lua_pushvalue(L, 2);
        cbRef = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    return PushLUTAsyncFuture_(L, AssetLoader::LoadHaldLUTAsync(path), cbRef);
}

// ==================== Phase F.0.10.8.3 — LUT 热重载 ====================

/// @lua_api Light.Graphics.HDR.WatchLUT
/// @brief 注册 LUT 文件到 watch list (内部加载 + 跟踪 mtime)
/// @param path string LUT 文件路径 (.cube / .png/.jpg/.jpeg/.bmp/.tga)
/// @return integer | nil tex_id (>0), string? err
/// @note 后续调 PollLUTReloads() 触发 mtime 检查 + 自动 reload
static int l_HDR_WatchLUT(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    char errBuf[256] = {0};
    uint32_t id = HDRRenderer::WatchLUT(path, errBuf, sizeof(errBuf));
    if (!id) {
        lua_pushnil(L);
        lua_pushstring(L, errBuf[0] ? errBuf : "WatchLUT: unknown error");
        return 2;
    }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/// @lua_api Light.Graphics.HDR.UnwatchLUT
/// @brief 从 watch list 移除 + 删除 GL texture
/// @param tex_id integer GL tex id (WatchLUT 返回值)
/// @return boolean true = 成功; false = id 不在 list (silent)
static int l_HDR_UnwatchLUT(lua_State* L) {
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    lua_pushboolean(L, HDRRenderer::UnwatchLUT(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetWatchedLUTId
/// @brief 查 path 当前的 LUT id (reload 后 path 不变但 id 已变, 用此查最新)
/// @param path string watched LUT 路径
/// @return integer | nil 当前 GL tex id (>0); nil = path 不在 watchList
static int l_HDR_GetWatchedLUTId(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    uint32_t id = HDRRenderer::GetWatchedLUTId(path);
    if (!id) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, (lua_Integer)id);
    return 1;
}

/// @lua_api Light.Graphics.HDR.PollLUTReloads
/// @brief 遍历 watch list, 检 mtime 变化 → 自动 reload + 替换 id
/// @return integer 本次 reload 成功数 (>= 0)
/// @note 用户控制 poll 频率 (典型每秒 1 次), 关闭 SetLUTHotReload(false) 后直接返 0
/// @usage local n = HDR.PollLUTReloads()
///        if n > 0 then print('reloaded', n, 'LUT(s)') end
static int l_HDR_PollLUTReloads(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)HDRRenderer::PollLUTReloads());
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetLUTHotReload
/// @brief 全局热重载开关 (默认 true)
/// @param enabled boolean true = 开 (默认); false = PollLUTReloads 立即返 0
static int l_HDR_SetLUTHotReload(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    HDRRenderer::SetLUTHotReload(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.HDR.GetLUTHotReload
/// @brief 查全局热重载开关
/// @return boolean (默认 true)
static int l_HDR_GetLUTHotReload(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::GetLUTHotReload() ? 1 : 0);
    return 1;
}

// ==================== Phase F.0.10.8.4 — LUT reload 回调 (Lua trampoline) ====================
//
// 设计:
//   - HDRRenderer 提供 C-style fn ptr 接口 (不依赖 lua_State).
//   - light_graphics 在 Lua 侧用 Registry ref 持 Lua function, 注册一个
//     C-style trampoline 给 HDRRenderer; trampoline 收到 reload 通知后
//     把参数推到 Lua 栈, lua_pcall 调真 callback, 错误用 CC::Log 报.
//   - 用 userData 传 lua_State*, 避免单例 lua_State 假设.
//   - SetLUTReloadCallback(nil) 清: luaL_unref ref + HDRRenderer Set(nullptr, nullptr).
//
// 状态:
//   s_lutReloadCbRef = LUA_NOREF 时表示无 Lua callback (但 trampoline 仍可能注册到 HDRRenderer);
//   每次 Set 时先 unref 旧 ref + ref 新 callback, 一一对应.
static int s_lutReloadCbRef = LUA_NOREF;   // Lua function ref in registry (LUA_NOREF = none)

// C-style trampoline: 由 HDRRenderer::PollLUTReloads 触发
static void LUTReloadTrampoline_(const char* path, uint32_t oldId, uint32_t newId, void* userData) {
    lua_State* L = static_cast<lua_State*>(userData);
    if (!L || s_lutReloadCbRef == LUA_NOREF) return;
    // 推 Lua function
    lua_rawgeti(L, LUA_REGISTRYINDEX, s_lutReloadCbRef);
    if (!lua_isfunction(L, -1)) {   // 防御: ref 失效
        lua_pop(L, 1);
        return;
    }
    // 推 3 参: path, oldId, newId
    lua_pushstring(L, path ? path : "");
    lua_pushinteger(L, (lua_Integer)oldId);
    lua_pushinteger(L, (lua_Integer)newId);
    if (lua_pcall(L, 3, 0, 0) != 0) {
        const char* err = lua_tostring(L, -1);
        CC::Log(CC::LOG_WARN, "HDR.LUTReloadCallback error: %s", err ? err : "unknown");
        lua_pop(L, 1);
    }
}

/// @lua_api Light.Graphics.HDR.SetLUTReloadCallback
/// @brief Phase F.0.10.8.4 — 注册 LUT reload 回调
/// @param cb function | nil 回调 fn(path, oldId, newId), 或 nil 清除
/// @return nil
/// @note 单一全局回调, 后注册者覆盖前者
/// @note 回调内**禁止**调 PollLUTReloads / UnwatchLUT / SetLUTReloadCallback (递归)
/// @usage HDR.SetLUTReloadCallback(function(path, oldId, newId)
///            print('LUT reloaded:', path, oldId, '→', newId)
///        end)
static int l_HDR_SetLUTReloadCallback(lua_State* L) {
    // 接受 function 或 nil
    const int t = lua_type(L, 1);
    if (t != LUA_TFUNCTION && t != LUA_TNIL && t != LUA_TNONE) {
        return luaL_error(L, "SetLUTReloadCallback: expected function or nil, got %s",
                           lua_typename(L, t));
    }

    // 清旧 ref (无论新值是 fn 还是 nil)
    if (s_lutReloadCbRef != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, s_lutReloadCbRef);
        s_lutReloadCbRef = LUA_NOREF;
    }

    if (t == LUA_TFUNCTION) {
        lua_pushvalue(L, 1);
        s_lutReloadCbRef = luaL_ref(L, LUA_REGISTRYINDEX);
        HDRRenderer::SetLUTReloadCallback(&LUTReloadTrampoline_, L);
    } else {
        // nil / none → 清 backend 注册
        HDRRenderer::SetLUTReloadCallback(nullptr, nullptr);
    }
    return 0;
}

/// @lua_api Light.Graphics.HDR.GetLUTReloadCallback
/// @brief 查询当前是否注册了 reload 回调
/// @return boolean true = 已注册, false = 未注册
static int l_HDR_GetLUTReloadCallback(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::HasLUTReloadCallback() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SupportsHDRLUT
/// @brief Phase F.0.10.8.6 — 探测 backend 是否支持 HDR LUT (RGB16F)
///
/// 用途: Lua UI 显示 "HDR LUT supported / fallback to RGB8"; 美术决策 HDR vs LDR .cube
/// 注: 即使返 false, LoadCubeLUT/LoadHaldLUT 对 HDR 输入仍可用 (自动 fallback + warn log)
///
/// @return boolean true = 支持 (gl33 / GLES3); false = legacy backend / 未初始化
static int l_HDR_SupportsHDRLUT(lua_State* L) {
    lua_pushboolean(L, HDRRenderer::SupportsHDRLUT() ? 1 : 0);
    return 1;
}

// ==================== Phase F.0.10.9 — Multi-Instance HDR Lua API ====================

/// @lua_api Light.Graphics.HDR.CreateInstance
/// @brief 创建新 HDR instance (split-screen / 多窗口 / PIP 必备)
/// @return integer instance id ∈ [1, 3]; 失败返 0 (槽满)
/// @note 老 API 默认作用于 default instance (id=0). 切换 SetActiveInstance(id) 后透明作用于新 instance.
static int l_HDR_CreateInstance(lua_State* L) {
    lua_pushinteger(L, HDRRenderer::CreateInstance());
    return 1;
}

/// @lua_api Light.Graphics.HDR.DestroyInstance
/// @brief 销毁 instance, 释放该槽 RT + 标空闲. id=0 (default) 拒绝.
/// @param id integer 要销毁的 instance id (1..3)
/// @return boolean true = 成功; false = 非法 id 或 未分配
static int l_HDR_DestroyInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, HDRRenderer::DestroyInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetActiveInstance
/// @brief 切换 active instance, 后续老 API 全部作用于该 instance
/// @param id integer instance id (0 = default, 1..3 = 用户创建)
/// @return boolean true = 切换成功; false = 非法 id 或 未分配
static int l_HDR_SetActiveInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, HDRRenderer::SetActiveInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetActiveInstance
/// @return integer 当前 active instance id
static int l_HDR_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, HDRRenderer::GetActiveInstance());
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetInstanceCount
/// @return integer 已分配 instance 数 (>=1, default 永远占用)
static int l_HDR_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, HDRRenderer::GetInstanceCount());
    return 1;
}

/// @lua_api Light.Graphics.HDR.CloneInstance — Phase F.0.10.9.x.3 (1-line setup)
static int l_HDR_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, HDRRenderer::CloneInstance(srcId));
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetState — Phase F.0.10.9.x.3 (snapshot)
/// 返回当前 active instance 全部调参字段 (exposure / tonemap_mode / gamma / dilation / auto-* / lut / ...)
static int l_HDR_GetState(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetExposure());                lua_setfield(L, -2, "exposure");
    lua_pushinteger(L, (lua_Integer)HDRRenderer::GetTonemapper());            lua_setfield(L, -2, "tonemap_mode");
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetGamma());                   lua_setfield(L, -2, "gamma");
    lua_pushboolean(L, HDRRenderer::GetVelocityDilation() ? 1 : 0);           lua_setfield(L, -2, "velocity_dilation");
    lua_pushboolean(L, HDRRenderer::GetVelocityDilationHalfRes() ? 1 : 0);    lua_setfield(L, -2, "velocity_dilation_half_res");
    lua_pushboolean(L, HDRRenderer::GetVelocityDilationAutoSkip() ? 1 : 0);   lua_setfield(L, -2, "velocity_dilation_auto_skip");
    lua_pushboolean(L, HDRRenderer::GetAutoTonemap() ? 1 : 0);                lua_setfield(L, -2, "auto_tonemap");
    lua_pushboolean(L, HDRRenderer::GetAutoTAA() ? 1 : 0);                    lua_setfield(L, -2, "auto_taa");
    lua_pushboolean(L, HDRRenderer::GetAutoBloom() ? 1 : 0);                  lua_setfield(L, -2, "auto_bloom");
    lua_pushboolean(L, HDRRenderer::GetAutoSSR() ? 1 : 0);                    lua_setfield(L, -2, "auto_ssr");
    lua_pushboolean(L, HDRRenderer::GetAutoMotionBlur() ? 1 : 0);             lua_setfield(L, -2, "auto_motion_blur");
    lua_pushinteger(L, (lua_Integer)HDRRenderer::GetGradingLUTId());          lua_setfield(L, -2, "lut_id");
    lua_pushnumber(L, (lua_Number)HDRRenderer::GetGradingLUTStrength());      lua_setfield(L, -2, "lut_strength");
    lua_pushboolean(L, HDRRenderer::IsEnabled() ? 1 : 0);                     lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, HDRRenderer::IsSupported() ? 1 : 0);                   lua_setfield(L, -2, "supported");
    return 1;
}

/// @lua_api Light.Graphics.HDR.SetState — Phase F.0.10.9.x.4 (反向 GetState)
/// @param state table  字段名匹配 GetState (任 partial 子集); enabled/supported 静默跳过
/// @return boolean, integer  ok=至少应用 1 字段 / applied=应用字段数
/// @return nil, string       入参非 table (类型错误)
/// @note tonemap_mode 接 int (0..3), 与 GetState 输出对称; 想用 string 请走 HDR.SetTonemapper
///       lut_id 不校验是否已 LoadCubeLUT; 用户责任传合法 id
static int l_HDR_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("exposure",                     HDRRenderer::SetExposure);
    APPLY_INT("tonemap_mode",                 HDRRenderer::SetTonemapper);
    APPLY_NUM("gamma",                        HDRRenderer::SetGamma);
    APPLY_BOOL("velocity_dilation",           HDRRenderer::SetVelocityDilation);
    APPLY_BOOL("velocity_dilation_half_res",  HDRRenderer::SetVelocityDilationHalfRes);
    APPLY_BOOL("velocity_dilation_auto_skip", HDRRenderer::SetVelocityDilationAutoSkip);
    APPLY_BOOL("auto_tonemap",                HDRRenderer::SetAutoTonemap);
    APPLY_BOOL("auto_taa",                    HDRRenderer::SetAutoTAA);
    APPLY_BOOL("auto_bloom",                  HDRRenderer::SetAutoBloom);
    APPLY_BOOL("auto_ssr",                    HDRRenderer::SetAutoSSR);
    APPLY_BOOL("auto_motion_blur",            HDRRenderer::SetAutoMotionBlur);
    // lut_id + lut_strength 合并调 SetGradingLUT(id, strength) 单函数
    // (GetState 拆 2 字段输出, SetState 重组; 任一字段提供即触发, 缺省 fallback 当前值)
    {
        bool hasId = false, hasStrength = false;
        uint32_t newId       = HDRRenderer::GetGradingLUTId();
        float    newStrength = HDRRenderer::GetGradingLUTStrength();
        lua_getfield(L, 1, "lut_id");
        if (lua_isnumber(L, -1)) { newId = (uint32_t)lua_tointeger(L, -1); hasId = true; }
        lua_pop(L, 1);
        lua_getfield(L, 1, "lut_strength");
        if (lua_isnumber(L, -1)) { newStrength = (float)lua_tonumber(L, -1); hasStrength = true; }
        lua_pop(L, 1);
        if (hasId || hasStrength) {
            HDRRenderer::SetGradingLUT(newId, newStrength);
            applied += (hasId ? 1 : 0) + (hasStrength ? 1 : 0);
        }
    }
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}

/// @lua_api Light.Graphics.HDR.SetVelocityFormat
/// @brief 切换 velocity buffer 存储格式 (RG16F 默认 / RG8 节省 4x VRAM)
/// @param fmt string "rg16f" | "rg8" (大小写敏感)
/// @return boolean true = 切换成功 (含 RT 重建); false = 重建失败; nil, string = 入参非法
/// @note HDR 未 Enable 时仅更新 state，下次 Enable 生效；切换会隐含重置 velocity history
static int l_HDR_SetVelocityFormat(lua_State* L) {
    const char* s = luaL_checkstring(L, 1);
    VelocityFormat fmt;
    if (strcmp(s, "rg16f") == 0) {
        fmt = VelocityFormat::RG16F;
    } else if (strcmp(s, "rg8") == 0) {
        fmt = VelocityFormat::RG8;
    } else {
        lua_pushnil(L);
        lua_pushfstring(L, "SetVelocityFormat: expect 'rg16f' or 'rg8', got '%s'", s);
        return 2;
    }
    bool ok = HDRRenderer::SetVelocityFormat(fmt);
    lua_pushboolean(L, ok ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.HDR.GetVelocityFormat
/// @return string 当前格式名 (规范小写: "rg16f" | "rg8")
static int l_HDR_GetVelocityFormat(lua_State* L) {
    VelocityFormat fmt = HDRRenderer::GetVelocityFormat();
    lua_pushstring(L, (fmt == VelocityFormat::RG8) ? "rg8" : "rg16f");
    return 1;
}

static const luaL_Reg hdr_funcs[] = {
    {"Enable",          l_HDR_Enable},
    {"Disable",         l_HDR_Disable},
    // Phase F.0.10.9.2 — 手动 BeginScene/EndScene (multi-instance 同帧切换必备)
    {"BeginScene",      l_HDR_BeginScene},
    {"EndScene",        l_HDR_EndScene},
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
    // Phase E.14 — velocity dilation + 双格式存储
    {"SetVelocityDilation", l_HDR_SetVelocityDilation},
    {"GetVelocityDilation", l_HDR_GetVelocityDilation},
    {"SetVelocityFormat",   l_HDR_SetVelocityFormat},
    {"GetVelocityFormat",   l_HDR_GetVelocityFormat},
    // Phase E.18.1 — dilation pass 半分辨率开关
    {"SetVelocityDilationHalfRes", l_HDR_SetVelocityDilationHalfRes},
    {"GetVelocityDilationHalfRes", l_HDR_GetVelocityDilationHalfRes},
    // Phase E.18.2 — dilation pass 自动跳过单消费者场景
    {"SetVelocityDilationAutoSkip", l_HDR_SetVelocityDilationAutoSkip},
    {"GetVelocityDilationAutoSkip", l_HDR_GetVelocityDilationAutoSkip},
    // Phase F.0.10.2 — Auto-TAA 开关 (split-screen 多 instance 必备)
    {"SetAutoTAA",                  l_HDR_SetAutoTAA},
    {"GetAutoTAA",                  l_HDR_GetAutoTAA},
    // Phase F.0.10.3 — Auto-Bloom/SSR/MotionBlur 开关 (split-screen 多 player 必备)
    {"SetAutoBloom",                l_HDR_SetAutoBloom},
    {"GetAutoBloom",                l_HDR_GetAutoBloom},
    {"SetAutoSSR",                  l_HDR_SetAutoSSR},
    {"GetAutoSSR",                  l_HDR_GetAutoSSR},
    {"SetAutoMotionBlur",           l_HDR_SetAutoMotionBlur},
    {"GetAutoMotionBlur",           l_HDR_GetAutoMotionBlur},
    // Phase F.0.10.6 — Auto-Tonemap + per-region Tonemap (split-screen multi-instance 必备)
    {"SetAutoTonemap",              l_HDR_SetAutoTonemap},
    {"GetAutoTonemap",              l_HDR_GetAutoTonemap},
    {"Tonemap",                     l_HDR_Tonemap},
    // Phase F.0.10.8 — 3D LUT (Color Grading)
    {"CreateLUT3D",                 l_HDR_CreateLUT3D},
    {"DeleteLUT3D",                 l_HDR_DeleteLUT3D},
    {"SetGradingLUT",               l_HDR_SetGradingLUT},
    {"GetGradingLUTId",             l_HDR_GetGradingLUTId},
    {"GetGradingLUTStrength",       l_HDR_GetGradingLUTStrength},
    // Phase F.0.10.8.1 — .cube LUT 文件解析 (Adobe Cube LUT 1.0)
    {"LoadCubeLUT",                 l_HDR_LoadCubeLUT},
    {"LoadCubeLUTAsync",            l_HDR_LoadCubeLUTAsync},
    // Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 (PNG/JPG/BMP/TGA)
    {"LoadHaldLUT",                 l_HDR_LoadHaldLUT},
    {"LoadHaldLUTAsync",            l_HDR_LoadHaldLUTAsync},
    // Phase F.0.10.8.3 — LUT 热重载 (mtime polling)
    {"WatchLUT",                    l_HDR_WatchLUT},
    {"UnwatchLUT",                  l_HDR_UnwatchLUT},
    {"GetWatchedLUTId",             l_HDR_GetWatchedLUTId},
    {"PollLUTReloads",              l_HDR_PollLUTReloads},
    {"SetLUTHotReload",             l_HDR_SetLUTHotReload},
    {"GetLUTHotReload",             l_HDR_GetLUTHotReload},
    // Phase F.0.10.8.4 — LUT reload 回调
    {"SetLUTReloadCallback",        l_HDR_SetLUTReloadCallback},
    {"GetLUTReloadCallback",        l_HDR_GetLUTReloadCallback},
    // Phase F.0.10.8.6 — HDR LUT 能力探测
    {"SupportsHDRLUT",              l_HDR_SupportsHDRLUT},
    // Phase F.0.10.9 — Multi-Instance HDR (split-screen / 多窗口 / PIP)
    {"CreateInstance",              l_HDR_CreateInstance},
    {"DestroyInstance",             l_HDR_DestroyInstance},
    {"SetActiveInstance",           l_HDR_SetActiveInstance},
    {"GetActiveInstance",           l_HDR_GetActiveInstance},
    {"GetInstanceCount",            l_HDR_GetInstanceCount},
    // Phase F.0.10.9.x.3 — Clone + Snapshot
    {"CloneInstance",               l_HDR_CloneInstance},
    {"GetState",                    l_HDR_GetState},
    // Phase F.0.10.9.x.4 — SetState (反向 GetState, 一键还原 profile)
    {"SetState",                    l_HDR_SetState},
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

/// @lua_api Light.Graphics.Bloom.Process
/// @param x integer? region 左下角 X (默认 0)
/// @param y integer? region 左下角 Y (默认 0)
/// @param w integer? region 宽度 (默认 0 = 全屏老路径)
/// @param h integer? region 高度 (默认 0 = 全屏老路径)
/// @return boolean true 成功; nil, string = 参数非法 / HDR 未启用
/// @note Phase F.0.10.3 — region 化 Bloom (split-screen 必备 overload)
///       内部按 mip 链 region 缩半 (downsample) / 翻倍 (upsample)
///       典型用法 (split-screen):
///         HDR.SetAutoBloom(false)
///         Bloom.Process(0,    0, W/2, H)  -- 左半屏 player 1
///         Bloom.Process(W/2,  0, W/2, H)  -- 右半屏 player 2
static int l_Bloom_Process(lua_State* L) {
    // 参数检查 (4 个 optional integer, 缺省 = 0): 全部 0 = 全屏老接口
    const int nargs = lua_gettop(L);
    int rgnX = 0, rgnY = 0, rgnW = 0, rgnH = 0;
    if (nargs >= 1) rgnX = (int)luaL_checkinteger(L, 1);
    if (nargs >= 2) rgnY = (int)luaL_checkinteger(L, 2);
    if (nargs >= 3) rgnW = (int)luaL_checkinteger(L, 3);
    if (nargs >= 4) rgnH = (int)luaL_checkinteger(L, 4);

    // 防御性: 部分 region 参数 (只传 2 个 / 3 个) 视为非法
    if (nargs != 0 && nargs != 4) {
        lua_pushnil(L);
        lua_pushfstring(L, "Bloom.Process: expected 0 or 4 args (got %d); region=(x,y,w,h) all-or-none", nargs);
        return 2;
    }
    // 防御性: 区域 w/h 必须 >= 0
    if (rgnW < 0 || rgnH < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "Bloom.Process: w/h must be >= 0 (got w=%d, h=%d)", rgnW, rgnH);
        return 2;
    }

    // 取 HDR fbo + sceneTex (HDR 未启用时返 0)
    const uint32_t fbo = HDRRenderer::GetFBO();
    const uint32_t tex = HDRRenderer::GetSceneTexture();
    if (!fbo || !tex) {
        lua_pushnil(L);
        lua_pushliteral(L, "Bloom.Process: HDR not enabled (fbo / sceneTex = 0)");
        return 2;
    }

    // 转发到 BloomRenderer (rgnW/rgnH=0 时 backend 内部跳过 scissor, 与无参 Process 等价)
    BloomRenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH);
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase F.0.10.9.x.2 — Bloom Multi-Instance API ====================

/// @lua_api Light.Graphics.Bloom.CreateInstance
/// @brief 创建新 Bloom instance (split-screen 4 player 各自不同 profile)
/// @return integer instance id ∈ [1, 3]; 失败返 0 (槽满)
static int l_Bloom_CreateInstance(lua_State* L) {
    lua_pushinteger(L, BloomRenderer::CreateInstance());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.DestroyInstance
/// @param id integer 要销毁的 instance id (1..3)
/// @return boolean true = 成功; false = 非法 id 或 未分配
static int l_Bloom_DestroyInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, BloomRenderer::DestroyInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetActiveInstance
/// @param id integer instance id (0 = default, 1..3 = 用户创建)
/// @return boolean true = 切换成功; false = 非法 id 或 未分配
static int l_Bloom_SetActiveInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, BloomRenderer::SetActiveInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.Bloom.GetActiveInstance
/// @return integer 当前 active instance id
static int l_Bloom_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, BloomRenderer::GetActiveInstance());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.GetInstanceCount
/// @return integer 已分配 instance 数 (>=1, default 永远占用)
static int l_Bloom_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, BloomRenderer::GetInstanceCount());
    return 1;
}

/// @lua_api Light.Graphics.Bloom.CloneInstance — Phase F.0.10.9.x.3 (1-line setup)
static int l_Bloom_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, BloomRenderer::CloneInstance(srcId));
    return 1;
}

/// @lua_api Light.Graphics.Bloom.SetState — Phase F.0.10.9.x.4 (反向 GetState)
/// @param state table  字段名匹配 GetState; enabled/supported 静默跳过
/// @return boolean, integer  ok / applied
/// @return nil, string       入参非 table
static int l_Bloom_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("threshold",   BloomRenderer::SetThreshold);
    APPLY_NUM("intensity",   BloomRenderer::SetIntensity);
    APPLY_NUM("radius",      BloomRenderer::SetRadius);
    APPLY_INT("levels",      BloomRenderer::SetLevels);
    APPLY_BOOL("auto_enable", BloomRenderer::SetAutoEnable);
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}

/// @lua_api Light.Graphics.Bloom.GetState — Phase F.0.10.9.x.3 (snapshot)
static int l_Bloom_GetState(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetThreshold()); lua_setfield(L, -2, "threshold");
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetIntensity()); lua_setfield(L, -2, "intensity");
    lua_pushnumber(L, (lua_Number)BloomRenderer::GetRadius());    lua_setfield(L, -2, "radius");
    lua_pushinteger(L, (lua_Integer)BloomRenderer::GetLevels());  lua_setfield(L, -2, "levels");
    lua_pushboolean(L, BloomRenderer::GetAutoEnable() ? 1 : 0);   lua_setfield(L, -2, "auto_enable");
    lua_pushboolean(L, BloomRenderer::IsEnabled() ? 1 : 0);       lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, BloomRenderer::IsSupported() ? 1 : 0);     lua_setfield(L, -2, "supported");
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
    // Phase F.0.10.3 — 手动 region Bloom (配合 HDR.SetAutoBloom(false) 做真物理 split-screen)
    {"Process",         l_Bloom_Process},
    // Phase F.0.10.9.x.2 — Multi-Instance Bloom (4 instance, default + 3 user)
    {"CreateInstance",     l_Bloom_CreateInstance},
    {"DestroyInstance",    l_Bloom_DestroyInstance},
    {"SetActiveInstance",  l_Bloom_SetActiveInstance},
    {"GetActiveInstance",  l_Bloom_GetActiveInstance},
    {"GetInstanceCount",   l_Bloom_GetInstanceCount},
    // Phase F.0.10.9.x.3 — Clone + Snapshot (1-line setup + state introspection)
    {"CloneInstance",      l_Bloom_CloneInstance},
    {"GetState",           l_Bloom_GetState},
    // Phase F.0.10.9.x.4 — SetState (反向 GetState)
    {"SetState",           l_Bloom_SetState},
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

// ==================== Phase F.2.5 — AutoExposure Multi-Instance ====================
static int l_AE_CreateInstance(lua_State* L) {
    lua_pushinteger(L, AutoExposureRenderer::CreateInstance());
    return 1;
}
static int l_AE_DestroyInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, AutoExposureRenderer::DestroyInstance(id) ? 1 : 0);
    return 1;
}
static int l_AE_SetActiveInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, AutoExposureRenderer::SetActiveInstance(id) ? 1 : 0);
    return 1;
}
static int l_AE_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, AutoExposureRenderer::GetActiveInstance());
    return 1;
}
static int l_AE_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, AutoExposureRenderer::GetInstanceCount());
    return 1;
}
static int l_AE_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, AutoExposureRenderer::CloneInstance(srcId));
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
    // Phase F.2.5 — Multi-Instance (4 instance, default + 3 user)
    {"CreateInstance",        l_AE_CreateInstance},
    {"DestroyInstance",       l_AE_DestroyInstance},
    {"SetActiveInstance",     l_AE_SetActiveInstance},
    {"GetActiveInstance",     l_AE_GetActiveInstance},
    {"GetInstanceCount",      l_AE_GetInstanceCount},
    {"CloneInstance",         l_AE_CloneInstance},
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

// ==================== Phase F.2.3 — LensDirt Multi-Instance ====================
static int l_LD_CreateInstance(lua_State* L) {
    lua_pushinteger(L, LensDirtRenderer::CreateInstance()); return 1;
}
static int l_LD_DestroyInstance(lua_State* L) {
    lua_pushboolean(L, LensDirtRenderer::DestroyInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_LD_SetActiveInstance(lua_State* L) {
    lua_pushboolean(L, LensDirtRenderer::SetActiveInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_LD_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, LensDirtRenderer::GetActiveInstance()); return 1;
}
static int l_LD_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, LensDirtRenderer::GetInstanceCount()); return 1;
}
static int l_LD_CloneInstance(lua_State* L) {
    lua_pushinteger(L, LensDirtRenderer::CloneInstance((int)luaL_checkinteger(L, 1))); return 1;
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
    // Phase F.2.3 — Multi-Instance
    {"CreateInstance",    l_LD_CreateInstance},
    {"DestroyInstance",   l_LD_DestroyInstance},
    {"SetActiveInstance", l_LD_SetActiveInstance},
    {"GetActiveInstance", l_LD_GetActiveInstance},
    {"GetInstanceCount",  l_LD_GetInstanceCount},
    {"CloneInstance",     l_LD_CloneInstance},
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

// ==================== Phase F.2.4 — Streak Multi-Instance ====================
static int l_ST_CreateInstance(lua_State* L) {
    lua_pushinteger(L, StreakRenderer::CreateInstance()); return 1;
}
static int l_ST_DestroyInstance(lua_State* L) {
    lua_pushboolean(L, StreakRenderer::DestroyInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_ST_SetActiveInstance(lua_State* L) {
    lua_pushboolean(L, StreakRenderer::SetActiveInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_ST_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, StreakRenderer::GetActiveInstance()); return 1;
}
static int l_ST_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, StreakRenderer::GetInstanceCount()); return 1;
}
static int l_ST_CloneInstance(lua_State* L) {
    lua_pushinteger(L, StreakRenderer::CloneInstance((int)luaL_checkinteger(L, 1))); return 1;
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
    // Phase F.2.4 — Multi-Instance
    {"CreateInstance",    l_ST_CreateInstance},
    {"DestroyInstance",   l_ST_DestroyInstance},
    {"SetActiveInstance", l_ST_SetActiveInstance},
    {"GetActiveInstance", l_ST_GetActiveInstance},
    {"GetInstanceCount",  l_ST_GetInstanceCount},
    {"CloneInstance",     l_ST_CloneInstance},
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

// ==================== Phase F.2.2 — LensFlare Multi-Instance ====================
static int l_LF_CreateInstance(lua_State* L) {
    lua_pushinteger(L, LensFlareRenderer::CreateInstance()); return 1;
}
static int l_LF_DestroyInstance(lua_State* L) {
    lua_pushboolean(L, LensFlareRenderer::DestroyInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_LF_SetActiveInstance(lua_State* L) {
    lua_pushboolean(L, LensFlareRenderer::SetActiveInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_LF_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, LensFlareRenderer::GetActiveInstance()); return 1;
}
static int l_LF_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, LensFlareRenderer::GetInstanceCount()); return 1;
}
static int l_LF_CloneInstance(lua_State* L) {
    lua_pushinteger(L, LensFlareRenderer::CloneInstance((int)luaL_checkinteger(L, 1))); return 1;
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
    // Phase F.2.2 — Multi-Instance
    {"CreateInstance",          l_LF_CreateInstance},
    {"DestroyInstance",         l_LF_DestroyInstance},
    {"SetActiveInstance",       l_LF_SetActiveInstance},
    {"GetActiveInstance",       l_LF_GetActiveInstance},
    {"GetInstanceCount",        l_LF_GetInstanceCount},
    {"CloneInstance",           l_LF_CloneInstance},
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

/// @lua_api Light.Graphics.SSAO.GetNormalTexId — Phase E.8.x 调试接口
/// @return integer G-buffer view-space normal RG16F 纹理 GL id
///                  (0 = HDR 未启用 / 后端不支持 MRT)
/// 用途: smoke / sample 验证 G-buffer normal 通路是否打通, 不应在生产代码使用.
static int l_SSAO_GetNormalTexId(lua_State* L) {
    uint32_t fbo = HDRRenderer::GetFBO();
    if (!g_render || !fbo) {
        lua_pushinteger(L, 0);
        return 1;
    }
    lua_pushinteger(L, (lua_Integer)g_render->GetHDRNormalTex(fbo));
    return 1;
}

// ==================== Phase F.2.1 — SSAO Multi-Instance ====================
static int l_SSAO_CreateInstance(lua_State* L) {
    lua_pushinteger(L, SSAORenderer::CreateInstance()); return 1;
}
static int l_SSAO_DestroyInstance(lua_State* L) {
    lua_pushboolean(L, SSAORenderer::DestroyInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_SSAO_SetActiveInstance(lua_State* L) {
    lua_pushboolean(L, SSAORenderer::SetActiveInstance((int)luaL_checkinteger(L, 1)) ? 1 : 0); return 1;
}
static int l_SSAO_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, SSAORenderer::GetActiveInstance()); return 1;
}
static int l_SSAO_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, SSAORenderer::GetInstanceCount()); return 1;
}
static int l_SSAO_CloneInstance(lua_State* L) {
    lua_pushinteger(L, SSAORenderer::CloneInstance((int)luaL_checkinteger(L, 1))); return 1;
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
    {"GetNormalTexId",  l_SSAO_GetNormalTexId},   // Phase E.8.x 调试
    // Phase F.2.1 — Multi-Instance
    {"CreateInstance",    l_SSAO_CreateInstance},
    {"DestroyInstance",   l_SSAO_DestroyInstance},
    {"SetActiveInstance", l_SSAO_SetActiveInstance},
    {"GetActiveInstance", l_SSAO_GetActiveInstance},
    {"GetInstanceCount",  l_SSAO_GetInstanceCount},
    {"CloneInstance",     l_SSAO_CloneInstance},
    {NULL, NULL}
};

// ==================== Phase E.9+E.10+E.11 — Light.Graphics.SSR Lua API ====================
//
// API (28 fn): lifecycle 5 + autoEnable 2 + params 20 (10 Set+Get pairs) + debug 1
//   Lifecycle:  Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)
//   AutoEnable: SetAutoEnable / GetAutoEnable
//   Params:     SetMaxSteps / GetMaxSteps              (int [8, 128], 默认 64)
//               SetStepSize / GetStepSize              (float [0.01, 1.0], 默认 0.1)
//               SetThickness / GetThickness            (float [0.01, 5.0], 默认 0.5)
//               SetMaxDistance / GetMaxDistance        (float [1.0, 1000.0], 默认 50.0)
//               SetIntensity / GetIntensity            (float [0.0, 2.0], 默认 0.7)
//               SetEdgeFade / GetEdgeFade              (float [0.0, 0.5], 默认 0.1)
//               SetBlurEnabled / GetBlurEnabled        (bool, 默认 false; Phase E.10 已激活)
//               SetBlurRadius / GetBlurRadius          (float [0.5, 4.0], 默认 1.5; Phase E.10)
//               SetBilateralEnabled / GetBilateralEnabled (bool, 默认 true; Phase E.11)
//               SetBlurDepthSigma / GetBlurDepthSigma  (float [50, 500], 默认 200; Phase E.11)
//   Debug:      GetReflectionTexId                     (uint32_t reflection RT GL id, 0 = 未启用)

static int l_SSR_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SSRRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_SSR_Disable(lua_State* L)     { (void)L; SSRRenderer::Disable(); return 0; }
static int l_SSR_IsEnabled(lua_State* L)   { lua_pushboolean(L, SSRRenderer::IsEnabled()   ? 1 : 0); return 1; }
static int l_SSR_IsSupported(lua_State* L) { lua_pushboolean(L, SSRRenderer::IsSupported() ? 1 : 0); return 1; }

static int l_SSR_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, SSRRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_SSR_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    SSRRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_SSR_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, SSRRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

static int l_SSR_SetMaxSteps(lua_State* L) {
    SSRRenderer::SetMaxSteps((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_SSR_GetMaxSteps(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SSRRenderer::GetMaxSteps());
    return 1;
}

static int l_SSR_SetStepSize(lua_State* L) {
    SSRRenderer::SetStepSize((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSR_GetStepSize(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetStepSize());
    return 1;
}

static int l_SSR_SetThickness(lua_State* L) {
    SSRRenderer::SetThickness((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSR_GetThickness(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetThickness());
    return 1;
}

static int l_SSR_SetMaxDistance(lua_State* L) {
    SSRRenderer::SetMaxDistance((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSR_GetMaxDistance(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetMaxDistance());
    return 1;
}

static int l_SSR_SetIntensity(lua_State* L) {
    SSRRenderer::SetIntensity((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSR_GetIntensity(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetIntensity());
    return 1;
}

static int l_SSR_SetEdgeFade(lua_State* L) {
    SSRRenderer::SetEdgeFade((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_SSR_GetEdgeFade(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetEdgeFade());
    return 1;
}

static int l_SSR_SetBlurEnabled(lua_State* L) {
    luaL_checkany(L, 1);
    SSRRenderer::SetBlurEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_SSR_GetBlurEnabled(lua_State* L) {
    lua_pushboolean(L, SSRRenderer::GetBlurEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetBlurRadius — Phase E.10 反射模糊半径
/// @param v float, clamp [0.5, 4.0], 默认 1.5. 仅 BlurEnabled=true 时生效.
static int l_SSR_SetBlurRadius(lua_State* L) {
    SSRRenderer::SetBlurRadius((float)luaL_checknumber(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetBlurRadius — Phase E.10 反射模糊半径读取
/// @return number 当前 clamp 后的 blur radius
static int l_SSR_GetBlurRadius(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetBlurRadius());
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetBilateralEnabled — Phase E.11 切换 bilateral 模式
/// @param flag boolean true=depth-aware bilateral (默认), false=纯 Gaussian (Phase E.10 行为)
/// @note 仅在 BlurEnabled=true 时影响视觉; 不影响资源分配.
static int l_SSR_SetBilateralEnabled(lua_State* L) {
    SSRRenderer::SetBilateralEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetBilateralEnabled — Phase E.11 bilateral 模式读取
/// @return boolean 当前 bilateral 开关
static int l_SSR_GetBilateralEnabled(lua_State* L) {
    lua_pushboolean(L, SSRRenderer::GetBilateralEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetBlurDepthSigma — Phase E.11 bilateral 深度权重 σ
/// @param v number, clamp [50.0, 500.0], 默认 200.0. σ 越大跨边模糊衰减越快.
/// @note 仅 BilateralEnabled=true && BlurEnabled=true 时影响视觉.
static int l_SSR_SetBlurDepthSigma(lua_State* L) {
    SSRRenderer::SetBlurDepthSigma((float)luaL_checknumber(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetBlurDepthSigma — Phase E.11 bilateral σ 读取
/// @return number 当前 clamp 后的 sigma
static int l_SSR_GetBlurDepthSigma(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetBlurDepthSigma());
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetTemporalEnabled — Phase E.12 Temporal SSR 开关
/// @param flag boolean true=启用时序累积降噪 (默认, TAA-style 业界标准),
///                    false=等同 Phase E.11 行为 (raw → blur → composite, 无 history)
/// @note 状态切换时内部重置 首帧标志, 避免失效的 prev 矩阵让 reproject 出错.
static int l_SSR_SetTemporalEnabled(lua_State* L) {
    SSRRenderer::SetTemporalEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetTemporalEnabled — Phase E.12 Temporal 开关读取
/// @return boolean 当前 temporal 开关
static int l_SSR_GetTemporalEnabled(lua_State* L) {
    lua_pushboolean(L, SSRRenderer::GetTemporalEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetTemporalAlpha — Phase E.12 history blend 权重
/// @param v number, clamp [0.5, 0.99], 默认 0.9. 越高 history 权重越大、去噪越强、响应越慢.
/// @note 仅 TemporalEnabled=true 时影响视觉.
static int l_SSR_SetTemporalAlpha(lua_State* L) {
    SSRRenderer::SetTemporalAlpha((float)luaL_checknumber(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetTemporalAlpha — Phase E.12 history 权重读取
/// @return number 当前 clamp 后的 alpha
static int l_SSR_GetTemporalAlpha(lua_State* L) {
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetTemporalAlpha());
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetRejectionMode — Phase E.12 history rejection 模式
/// @param mode integer, clamp {0, 1}, 默认 1.
///                       0 = current-depth threshold rejection
///                       1 = neighborhood AABB clip (9-tap min/max, 抗 ghost)
/// @note 仅 TemporalEnabled=true 时影响视觉.
static int l_SSR_SetRejectionMode(lua_State* L) {
    SSRRenderer::SetRejectionMode((int)luaL_checkinteger(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.SSR.GetRejectionMode — Phase E.12 rejection 模式读取
/// @return integer 0 或 1
static int l_SSR_GetRejectionMode(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SSRRenderer::GetRejectionMode());
    return 1;
}

/// @lua_api Light.Graphics.SSR.GetReflectionTexId — Phase E.9 调试接口
/// @return integer 当前反射 RT (RGBA16F full-res) GL id, 0 = SSR 未启用.
/// 用途: smoke / sample 可视化反射通路, 不应在生产代码使用.
static int l_SSR_GetReflectionTexId(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)SSRRenderer::GetReflectionTexId());
    return 1;
}

/// @lua_api Light.Graphics.SSR.Process
/// @param x integer? region 左下角 X (默认 0)
/// @param y integer? region 左下角 Y (默认 0)
/// @param w integer? region 宽度 (默认 0 = 全屏老路径)
/// @param h integer? region 高度 (默认 0 = 全屏老路径)
/// @return boolean true 成功; nil, string = 参数非法 / HDR 未启用
/// @note Phase F.0.10.3 — region 化 SSR (split-screen 必备 overload)
///       内部 5 pass 全部 region 化: depth blit / raw / temporal / blur×2 / composite
///       blur 自动缩半 region (full-res → half-res)
///       典型用法 (split-screen):
///         HDR.SetAutoSSR(false)
///         SSR.Process(0,    0, W/2, H)  -- 左半屏 player 1
///         SSR.Process(W/2,  0, W/2, H)  -- 右半屏 player 2
static int l_SSR_Process(lua_State* L) {
    // 参数检查 (4 个 optional integer, 缺省 = 0): 全部 0 = 全屏老接口
    const int nargs = lua_gettop(L);
    int rgnX = 0, rgnY = 0, rgnW = 0, rgnH = 0;
    if (nargs >= 1) rgnX = (int)luaL_checkinteger(L, 1);
    if (nargs >= 2) rgnY = (int)luaL_checkinteger(L, 2);
    if (nargs >= 3) rgnW = (int)luaL_checkinteger(L, 3);
    if (nargs >= 4) rgnH = (int)luaL_checkinteger(L, 4);

    // 防御性: 部分 region 参数 (只传 2 个 / 3 个) 视为非法
    if (nargs != 0 && nargs != 4) {
        lua_pushnil(L);
        lua_pushfstring(L, "SSR.Process: expected 0 or 4 args (got %d); region=(x,y,w,h) all-or-none", nargs);
        return 2;
    }
    // 防御性: 区域 w/h 必须 >= 0
    if (rgnW < 0 || rgnH < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "SSR.Process: w/h must be >= 0 (got w=%d, h=%d)", rgnW, rgnH);
        return 2;
    }

    // 取 HDR fbo + sceneTex
    const uint32_t fbo = HDRRenderer::GetFBO();
    const uint32_t tex = HDRRenderer::GetSceneTexture();
    if (!fbo || !tex) {
        lua_pushnil(L);
        lua_pushliteral(L, "SSR.Process: HDR not enabled (fbo / sceneTex = 0)");
        return 2;
    }

    SSRRenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH);
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase F.0.10.9.x.2 — SSR Multi-Instance API ====================

/// @lua_api Light.Graphics.SSR.CreateInstance
/// @brief 创建新 SSR instance, 各自独立 depth/reflect/blur/history RT + temporal state
/// @return integer instance id ∈ [1, 3]; 失败返 0 (槽满)
static int l_SSR_CreateInstance(lua_State* L) {
    lua_pushinteger(L, SSRRenderer::CreateInstance());
    return 1;
}

/// @lua_api Light.Graphics.SSR.DestroyInstance
static int l_SSR_DestroyInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, SSRRenderer::DestroyInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetActiveInstance
static int l_SSR_SetActiveInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, SSRRenderer::SetActiveInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SSR.GetActiveInstance
static int l_SSR_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, SSRRenderer::GetActiveInstance());
    return 1;
}

/// @lua_api Light.Graphics.SSR.GetInstanceCount
static int l_SSR_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, SSRRenderer::GetInstanceCount());
    return 1;
}

/// @lua_api Light.Graphics.SSR.CloneInstance — Phase F.0.10.9.x.3
static int l_SSR_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, SSRRenderer::CloneInstance(srcId));
    return 1;
}

/// @lua_api Light.Graphics.SSR.SetState — Phase F.0.10.9.x.4 (反向 GetState)
/// @param state table  字段名匹配 GetState; enabled/supported 静默跳过
/// @return boolean, integer  ok / applied
/// @return nil, string       入参非 table
static int l_SSR_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_INT("max_steps",          SSRRenderer::SetMaxSteps);
    APPLY_NUM("step_size",          SSRRenderer::SetStepSize);
    APPLY_NUM("thickness",          SSRRenderer::SetThickness);
    APPLY_NUM("max_distance",       SSRRenderer::SetMaxDistance);
    APPLY_NUM("intensity",          SSRRenderer::SetIntensity);
    APPLY_NUM("edge_fade",          SSRRenderer::SetEdgeFade);
    APPLY_BOOL("blur_enabled",      SSRRenderer::SetBlurEnabled);
    APPLY_NUM("blur_radius",        SSRRenderer::SetBlurRadius);
    APPLY_BOOL("bilateral_enabled", SSRRenderer::SetBilateralEnabled);
    APPLY_NUM("blur_depth_sigma",   SSRRenderer::SetBlurDepthSigma);
    APPLY_BOOL("temporal_enabled",  SSRRenderer::SetTemporalEnabled);
    APPLY_NUM("temporal_alpha",     SSRRenderer::SetTemporalAlpha);
    APPLY_INT("rejection_mode",     SSRRenderer::SetRejectionMode);
    APPLY_BOOL("auto_enable",       SSRRenderer::SetAutoEnable);
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}

/// @lua_api Light.Graphics.SSR.GetState — Phase F.0.10.9.x.3 (snapshot)
static int l_SSR_GetState(lua_State* L) {
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)SSRRenderer::GetMaxSteps());          lua_setfield(L, -2, "max_steps");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetStepSize());            lua_setfield(L, -2, "step_size");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetThickness());           lua_setfield(L, -2, "thickness");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetMaxDistance());         lua_setfield(L, -2, "max_distance");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetIntensity());           lua_setfield(L, -2, "intensity");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetEdgeFade());            lua_setfield(L, -2, "edge_fade");
    lua_pushboolean(L, SSRRenderer::GetBlurEnabled() ? 1 : 0);            lua_setfield(L, -2, "blur_enabled");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetBlurRadius());          lua_setfield(L, -2, "blur_radius");
    lua_pushboolean(L, SSRRenderer::GetBilateralEnabled() ? 1 : 0);       lua_setfield(L, -2, "bilateral_enabled");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetBlurDepthSigma());      lua_setfield(L, -2, "blur_depth_sigma");
    lua_pushboolean(L, SSRRenderer::GetTemporalEnabled() ? 1 : 0);        lua_setfield(L, -2, "temporal_enabled");
    lua_pushnumber(L, (lua_Number)SSRRenderer::GetTemporalAlpha());       lua_setfield(L, -2, "temporal_alpha");
    lua_pushinteger(L, (lua_Integer)SSRRenderer::GetRejectionMode());     lua_setfield(L, -2, "rejection_mode");
    lua_pushboolean(L, SSRRenderer::GetAutoEnable() ? 1 : 0);             lua_setfield(L, -2, "auto_enable");
    lua_pushboolean(L, SSRRenderer::IsEnabled() ? 1 : 0);                 lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, SSRRenderer::IsSupported() ? 1 : 0);               lua_setfield(L, -2, "supported");
    return 1;
}

static const luaL_Reg ssr_funcs[] = {
    // lifecycle (5)
    {"Enable",              l_SSR_Enable},
    {"Disable",             l_SSR_Disable},
    {"IsEnabled",           l_SSR_IsEnabled},
    {"IsSupported",         l_SSR_IsSupported},
    {"Resize",              l_SSR_Resize},
    // autoEnable (2)
    {"SetAutoEnable",       l_SSR_SetAutoEnable},
    {"GetAutoEnable",       l_SSR_GetAutoEnable},
    // params (14 = 7 对)
    {"SetMaxSteps",         l_SSR_SetMaxSteps},
    {"GetMaxSteps",         l_SSR_GetMaxSteps},
    {"SetStepSize",         l_SSR_SetStepSize},
    {"GetStepSize",         l_SSR_GetStepSize},
    {"SetThickness",        l_SSR_SetThickness},
    {"GetThickness",        l_SSR_GetThickness},
    {"SetMaxDistance",      l_SSR_SetMaxDistance},
    {"GetMaxDistance",      l_SSR_GetMaxDistance},
    {"SetIntensity",        l_SSR_SetIntensity},
    {"GetIntensity",        l_SSR_GetIntensity},
    {"SetEdgeFade",         l_SSR_SetEdgeFade},
    {"GetEdgeFade",         l_SSR_GetEdgeFade},
    {"SetBlurEnabled",      l_SSR_SetBlurEnabled},
    {"GetBlurEnabled",      l_SSR_GetBlurEnabled},
    // Phase E.10 — 反射模糊半径 (1 对新增)
    {"SetBlurRadius",       l_SSR_SetBlurRadius},
    {"GetBlurRadius",       l_SSR_GetBlurRadius},
    // Phase E.11 — depth-aware bilateral 双对 (2 对 +4)
    {"SetBilateralEnabled", l_SSR_SetBilateralEnabled},
    {"GetBilateralEnabled", l_SSR_GetBilateralEnabled},
    {"SetBlurDepthSigma",   l_SSR_SetBlurDepthSigma},
    {"GetBlurDepthSigma",   l_SSR_GetBlurDepthSigma},
    // Phase E.12 — Temporal SSR (3 对 +6)
    {"SetTemporalEnabled",  l_SSR_SetTemporalEnabled},
    {"GetTemporalEnabled",  l_SSR_GetTemporalEnabled},
    {"SetTemporalAlpha",    l_SSR_SetTemporalAlpha},
    {"GetTemporalAlpha",    l_SSR_GetTemporalAlpha},
    {"SetRejectionMode",    l_SSR_SetRejectionMode},
    {"GetRejectionMode",    l_SSR_GetRejectionMode},
    // debug (1)
    {"GetReflectionTexId",  l_SSR_GetReflectionTexId},
    // Phase F.0.10.3 — 手动 region SSR (配合 HDR.SetAutoSSR(false) 做真物理 split-screen)
    {"Process",             l_SSR_Process},
    // Phase F.0.10.9.x.2 — Multi-Instance SSR (4 instance, default + 3 user)
    {"CreateInstance",      l_SSR_CreateInstance},
    {"DestroyInstance",     l_SSR_DestroyInstance},
    {"SetActiveInstance",   l_SSR_SetActiveInstance},
    {"GetActiveInstance",   l_SSR_GetActiveInstance},
    {"GetInstanceCount",    l_SSR_GetInstanceCount},
    // Phase F.0.10.9.x.3 — Clone + Snapshot
    {"CloneInstance",       l_SSR_CloneInstance},
    {"GetState",            l_SSR_GetState},
    // Phase F.0.10.9.x.4 — SetState (反向 GetState)
    {"SetState",            l_SSR_SetState},
    {NULL, NULL}
};

// ==================== Phase E.15 — Light.Graphics.MotionBlur Lua API ====================
//
// 子表 Light.Graphics.MotionBlur 挂在 luaopen_Light_Graphics 注册时附加 (SSR 子表之后).
//
// API 设计 (11 函数):
//   生命周期 5: Enable / Disable / IsEnabled / IsSupported / Resize
//   联动     2: SetAutoEnable / GetAutoEnable                  (默认 false)
//   参数     4: SetStrength / GetStrength (clamp [0, 4])
//                SetSampleCount / GetSampleCount (clamp [1, 32])

static int l_MB_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, MotionBlurRenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

static int l_MB_Disable(lua_State* L) {
    (void)L;
    MotionBlurRenderer::Disable();
    return 0;
}

static int l_MB_IsEnabled(lua_State* L) {
    lua_pushboolean(L, MotionBlurRenderer::IsEnabled() ? 1 : 0);
    return 1;
}

static int l_MB_IsSupported(lua_State* L) {
    lua_pushboolean(L, MotionBlurRenderer::IsSupported() ? 1 : 0);
    return 1;
}

static int l_MB_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, MotionBlurRenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

static int l_MB_SetAutoEnable(lua_State* L) {
    luaL_checkany(L, 1);
    MotionBlurRenderer::SetAutoEnable(lua_toboolean(L, 1) != 0);
    return 0;
}

static int l_MB_GetAutoEnable(lua_State* L) {
    lua_pushboolean(L, MotionBlurRenderer::GetAutoEnable() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetStrength
/// @param v number 强度 (clamp [0, 4]; 1.0 = velocity 位移直接做 blur)
static int l_MB_SetStrength(lua_State* L) {
    MotionBlurRenderer::SetStrength((float)luaL_checknumber(L, 1));
    return 0;
}

static int l_MB_GetStrength(lua_State* L) {
    lua_pushnumber(L, (lua_Number)MotionBlurRenderer::GetStrength());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetSampleCount
/// @param n integer 采样数 (clamp [1, 32]; 8 默认平衡，高质量 16~32)
static int l_MB_SetSampleCount(lua_State* L) {
    MotionBlurRenderer::SetSampleCount((int)luaL_checkinteger(L, 1));
    return 0;
}

static int l_MB_GetSampleCount(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)MotionBlurRenderer::GetSampleCount());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetMode
/// @param m integer Phase E.16 motion blur 模式 (clamp [0, 2])
///                  0=combined (与 Phase E.15 一致) / 1=camera_only / 2=object_only
static int l_MB_SetMode(lua_State* L) {
    MotionBlurRenderer::SetMode((int)luaL_checkinteger(L, 1));
    return 0;
}

/// @lua_api Light.Graphics.MotionBlur.GetMode
/// @return integer 当前 motion blur 模式 (0/1/2)
static int l_MB_GetMode(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)MotionBlurRenderer::GetMode());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetHalfRes
/// @param flag boolean Phase E.17 half-res motion blur 开关
///                     true = motionBlurTex 改 ((w+1)/2, (h+1)/2)，VRAM -75%、Pass1 性能 ~4×
///                     false = full-res (与 Phase E.15/E.16 一致，默认)
///                     已 Enable 时切换 → 立即 Resize 重建 RT
static int l_MB_SetHalfRes(lua_State* L) {
    MotionBlurRenderer::SetHalfRes(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.MotionBlur.GetHalfRes
/// @return boolean 当前 half-res 开关
static int l_MB_GetHalfRes(lua_State* L) {
    lua_pushboolean(L, MotionBlurRenderer::GetHalfRes() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.Process
/// @param x integer? region 左下角 X (默认 0)
/// @param y integer? region 左下角 Y (默认 0)
/// @param w integer? region 宽度 (默认 0 = 全屏老路径)
/// @param h integer? region 高度 (默认 0 = 全屏老路径)
/// @return boolean true 成功; nil, string = 参数非法 / HDR 未启用
/// @note Phase F.0.10.3 — region 化 MotionBlur (split-screen 必备 overload)
///       内部用 HDRRenderer::GetFBO() + GetSceneTexture() 作为目标
///       headless / HDR 未启 时静默 no-op (返 nil + err string)
///       典型用法 (split-screen):
///         HDR.SetAutoMotionBlur(false)
///         MotionBlur.Process(0,    0, W/2, H)  -- 左半屏 player 1
///         MotionBlur.Process(W/2,  0, W/2, H)  -- 右半屏 player 2
static int l_MB_Process(lua_State* L) {
    // 参数检查 (4 个 optional integer, 缺省 = 0): 全部 0 = 全屏老接口
    const int nargs = lua_gettop(L);
    int rgnX = 0, rgnY = 0, rgnW = 0, rgnH = 0;
    if (nargs >= 1) rgnX = (int)luaL_checkinteger(L, 1);
    if (nargs >= 2) rgnY = (int)luaL_checkinteger(L, 2);
    if (nargs >= 3) rgnW = (int)luaL_checkinteger(L, 3);
    if (nargs >= 4) rgnH = (int)luaL_checkinteger(L, 4);

    // 防御性: 部分 region 参数 (只传 2 个 / 3 个) 视为非法
    if (nargs != 0 && nargs != 4) {
        lua_pushnil(L);
        lua_pushfstring(L, "MotionBlur.Process: expected 0 or 4 args (got %d); region=(x,y,w,h) all-or-none", nargs);
        return 2;
    }
    // 防御性: 区域 w/h 必须 >= 0 (0/0/0/0 = 全屏 path; w<0 或 h<0 拒绝)
    if (rgnW < 0 || rgnH < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "MotionBlur.Process: w/h must be >= 0 (got w=%d, h=%d)", rgnW, rgnH);
        return 2;
    }

    // 取 HDR fbo + sceneTex (HDR 未启用时返 0, MotionBlurRenderer::Process 内部静默 no-op)
    const uint32_t fbo = HDRRenderer::GetFBO();
    const uint32_t tex = HDRRenderer::GetSceneTexture();
    if (!fbo || !tex) {
        lua_pushnil(L);
        lua_pushliteral(L, "MotionBlur.Process: HDR not enabled (fbo / sceneTex = 0)");
        return 2;
    }

    // 转发到 MotionBlurRenderer (rgnW/rgnH=0 时 backend 内部跳过 scissor + sub-rect blit, 与无参 Process 等价)
    MotionBlurRenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH);
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase F.0.10.9.x.2 — MotionBlur Multi-Instance API ====================

/// @lua_api Light.Graphics.MotionBlur.CreateInstance
static int l_MB_CreateInstance(lua_State* L) {
    lua_pushinteger(L, MotionBlurRenderer::CreateInstance());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.DestroyInstance
static int l_MB_DestroyInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, MotionBlurRenderer::DestroyInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetActiveInstance
static int l_MB_SetActiveInstance(lua_State* L) {
    int id = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, MotionBlurRenderer::SetActiveInstance(id) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.GetActiveInstance
static int l_MB_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, MotionBlurRenderer::GetActiveInstance());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.GetInstanceCount
static int l_MB_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, MotionBlurRenderer::GetInstanceCount());
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.CloneInstance — Phase F.0.10.9.x.3
static int l_MB_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, MotionBlurRenderer::CloneInstance(srcId));
    return 1;
}

/// @lua_api Light.Graphics.MotionBlur.SetState — Phase F.0.10.9.x.4 (反向 GetState)
/// @param state table  字段名匹配 GetState; enabled/supported 静默跳过
/// @return boolean, integer  ok / applied
/// @return nil, string       入参非 table
/// @note mode 接 int (0=combined / 1=camera_only / 2=object_only), 与 GetState 输出对称
static int l_MB_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("strength",     MotionBlurRenderer::SetStrength);
    APPLY_INT("sample_count", MotionBlurRenderer::SetSampleCount);
    APPLY_INT("mode",         MotionBlurRenderer::SetMode);
    APPLY_BOOL("half_res",    MotionBlurRenderer::SetHalfRes);
    APPLY_BOOL("auto_enable", MotionBlurRenderer::SetAutoEnable);
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}

/// @lua_api Light.Graphics.MotionBlur.GetState — Phase F.0.10.9.x.3 (snapshot)
static int l_MB_GetState(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)MotionBlurRenderer::GetStrength());        lua_setfield(L, -2, "strength");
    lua_pushinteger(L, (lua_Integer)MotionBlurRenderer::GetSampleCount());   lua_setfield(L, -2, "sample_count");
    lua_pushinteger(L, (lua_Integer)MotionBlurRenderer::GetMode());          lua_setfield(L, -2, "mode");
    lua_pushboolean(L, MotionBlurRenderer::GetHalfRes() ? 1 : 0);            lua_setfield(L, -2, "half_res");
    lua_pushboolean(L, MotionBlurRenderer::GetAutoEnable() ? 1 : 0);         lua_setfield(L, -2, "auto_enable");
    lua_pushboolean(L, MotionBlurRenderer::IsEnabled() ? 1 : 0);             lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, MotionBlurRenderer::IsSupported() ? 1 : 0);           lua_setfield(L, -2, "supported");
    return 1;
}

static const luaL_Reg mb_funcs[] = {
    // lifecycle (5)
    {"Enable",         l_MB_Enable},
    {"Disable",        l_MB_Disable},
    {"IsEnabled",      l_MB_IsEnabled},
    {"IsSupported",    l_MB_IsSupported},
    {"Resize",         l_MB_Resize},
    // autoEnable (2)
    {"SetAutoEnable",  l_MB_SetAutoEnable},
    {"GetAutoEnable",  l_MB_GetAutoEnable},
    // params (4 = 2 对)
    {"SetStrength",    l_MB_SetStrength},
    {"GetStrength",    l_MB_GetStrength},
    {"SetSampleCount", l_MB_SetSampleCount},
    {"GetSampleCount", l_MB_GetSampleCount},
    // Phase E.16 — mode (2 = 1 对): 0=combined / 1=camera_only / 2=object_only
    {"SetMode",        l_MB_SetMode},
    {"GetMode",        l_MB_GetMode},
    // Phase E.17 — half-res 开关 (2 = 1 对): VRAM -75% 性能 ~4×
    {"SetHalfRes",     l_MB_SetHalfRes},
    {"GetHalfRes",     l_MB_GetHalfRes},
    // Phase F.0.10.3 — 手动 region MotionBlur (配合 HDR.SetAutoMotionBlur(false) 做真物理 split-screen)
    {"Process",        l_MB_Process},
    // Phase F.0.10.9.x.2 — Multi-Instance MotionBlur (4 instance, default + 3 user)
    {"CreateInstance",     l_MB_CreateInstance},
    {"DestroyInstance",    l_MB_DestroyInstance},
    {"SetActiveInstance",  l_MB_SetActiveInstance},
    {"GetActiveInstance",  l_MB_GetActiveInstance},
    {"GetInstanceCount",   l_MB_GetInstanceCount},
    // Phase F.0.10.9.x.3 — Clone + Snapshot
    {"CloneInstance",      l_MB_CloneInstance},
    {"GetState",           l_MB_GetState},
    // Phase F.0.10.9.x.4 — SetState (反向 GetState)
    {"SetState",           l_MB_SetState},
    {NULL, NULL}
};

// ==================== Phase F.0 — TAA Master Pipeline (Light.Graphics.TAA.*) ====================
//   13 函数: 5 lifecycle + 2 对参数 (alpha/clip/jitter) + 2 状态查询 (frameCounter/jitter)
//   推荐启用 TAA 时手动 Light.Graphics.SSR.SetTemporalEnabled(false) 避免双 temporal 模糊反射

/// @lua_api Light.Graphics.TAA.Enable
/// @param w integer, h integer
/// @return boolean true=成功创建 history RT (RGBA16F × 2), false=backend 不支持/参数非法
static int l_TAA_Enable(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, TAARenderer::Enable(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.Disable
static int l_TAA_Disable(lua_State* L) {
    (void)L;
    TAARenderer::Disable();
    return 0;
}

/// @lua_api Light.Graphics.TAA.IsEnabled
/// @return boolean
static int l_TAA_IsEnabled(lua_State* L) {
    lua_pushboolean(L, TAARenderer::IsEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.IsSupported
/// @return boolean backend 是否支持 TAA (shader 编译成功 + RGBA16F 可用)
static int l_TAA_IsSupported(lua_State* L) {
    lua_pushboolean(L, TAARenderer::IsSupported() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.Resize
/// @param w integer, h integer
/// @return boolean
static int l_TAA_Resize(lua_State* L) {
    int w = (int)luaL_checkinteger(L, 1);
    int h = (int)luaL_checkinteger(L, 2);
    lua_pushboolean(L, TAARenderer::Resize(w, h) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetBlendAlpha
/// @param a number history 权重 (clamp [0, 1]; 默认 0.92, 高=累积稳/响应慢, 低=响应快/抖动)
static int l_TAA_SetBlendAlpha(lua_State* L) {
    float a = (float)luaL_checknumber(L, 1);
    TAARenderer::SetBlendAlpha(a);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetBlendAlpha
/// @return number
static int l_TAA_GetBlendAlpha(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetBlendAlpha());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetNeighborhoodClip
/// @param on boolean true=启用 9-tap AABB clip (默认), false=纯 reproject+blend (累积更软但易 ghost)
static int l_TAA_SetNeighborhoodClip(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetNeighborhoodClip: 期望 boolean 参数");
        return 2;
    }
    TAARenderer::SetNeighborhoodClip(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetNeighborhoodClip
/// @return boolean
static int l_TAA_GetNeighborhoodClip(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetNeighborhoodClip() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetJitterEnabled
/// @param on boolean true=启用 sub-pixel projection jitter (默认, 含 super-sampling 效果),
///                     false=纯时序 stability filter (无 super-sampling)
static int l_TAA_SetJitterEnabled(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetJitterEnabled: 期望 boolean 参数");
        return 2;
    }
    TAARenderer::SetJitterEnabled(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetJitterEnabled
/// @return boolean
static int l_TAA_GetJitterEnabled(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetJitterEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetSharpness
/// @param s number 4-tap unsharp mask 强度 (clamp [0, 2], 默认 0.5)
///                 0 = 关闭锐化 (纯 blit, 零 ALU); > 0 启用 sharpen pass
///                 推荐 0.3~0.8; > 1.5 易产生 ringing
static int l_TAA_SetSharpness(lua_State* L) {
    float s = (float)luaL_checknumber(L, 1);
    TAARenderer::SetSharpness(s);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetSharpness
/// @return number
static int l_TAA_GetSharpness(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetSharpness());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetAntiFlicker
/// @param on boolean Karis luma weighting blend 开关 (默认 true)
///                   true  = Karis 加权 blend (压制 firefly 闪烁, 与 sharpening 配合)
///                   false = 纯 alpha blend (Phase F.0 原始行为)
/// 错误处理: 非 boolean 返回 nil + 错误信息 (与 SetNeighborhoodClip / SetJitterEnabled 同模式)
static int l_TAA_SetAntiFlicker(lua_State* L) {
    luaL_checkany(L, 1);
    if (!lua_isboolean(L, 1)) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetAntiFlicker: 期望 boolean 参数");
        return 2;
    }
    TAARenderer::SetAntiFlicker(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetAntiFlicker
/// @return boolean
static int l_TAA_GetAntiFlicker(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetAntiFlicker() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetClipMode
/// @param mode string  "rgb" / "ycocg" / "variance" (大小写不敏感; 默认 "ycocg")
///                     "rgb"      = F.0 原生三通道 AABB clip (零 ALU 增量)
///                     "ycocg"    = YCoCg AABB clip (F.0.2, 色彩边缘更鲁棒, +0.05ms @ 1080p)
///                     "variance" = YCoCg variance clip (F.0.3, Salvi 2016 / UE5 default, +0.07ms @ 1080p)
///                                  算法: clip = [mean - γσ, mean + γσ]; 对 single-outlier 更鲁棒
/// 错误处理: 非 string / 未识别值 返 nil + err (与 SetVelocityFormat 同模式)
static int l_TAA_SetClipMode(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetClipMode: 期望 string 参数 ('rgb' / 'ycocg' / 'variance')");
        return 2;
    }
    const char* mode = lua_tostring(L, 1);
    // C++ 层: 未识别字符串静默保持 state; 此处先在 Lua 层做白名单校验返 nil+err
    // 大小写不敏感: 转 lower 比对
    char lower[16] = {0};
    for (int i = 0; i < 15 && mode[i]; ++i) {
        char c = mode[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (strcmp(lower, "rgb") != 0 && strcmp(lower, "ycocg") != 0 && strcmp(lower, "variance") != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.SetClipMode: 未识别的 mode '%s' (期望 'rgb' / 'ycocg' / 'variance')", mode);
        return 2;
    }
    TAARenderer::SetClipMode(mode);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetClipMode
/// @return string  "rgb" / "ycocg" / "variance"
static int l_TAA_GetClipMode(lua_State* L) {
    lua_pushstring(L, TAARenderer::GetClipMode());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetVarianceGamma
/// @param gamma number variance clip 收紧系数 γ (clamp [0, 4]; 默认 1.0)
///                     仅 ClipMode=="variance" 时生效
///                     γ 越小 → clip 越严 → ghost 抑制越强, 但可能出现 trail/over-smoothing
///                     γ 越大 → clip 越宽松 → 接近无 clip
///                     γ = 0 → 极端激进 (mn=mx=mean)
///                     Salvi 2016 / UE5 默认推荐 1.0
static int l_TAA_SetVarianceGamma(lua_State* L) {
    float gamma = (float)luaL_checknumber(L, 1);
    TAARenderer::SetVarianceGamma(gamma);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetVarianceGamma
/// @return number
static int l_TAA_GetVarianceGamma(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetVarianceGamma());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetHalfResHistory
/// @param flag boolean Phase F.0.5 — TAA history RT 半分辨率开关
///                     true = history RT 改 (w/2, h/2)，VRAM -75%、TAA pass 像素 -75%
///                     false = full-res (与 Phase F.0/F.0.1/F.0.2/F.0.3/F.0.4 一致，默认)
///                     已 Enable 时切换 → 立即重建 RT + invalidate hasHistory
/// @return true on success（错误统一通过 luaL_check 抛错）
static int l_TAA_SetHalfResHistory(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    TAARenderer::SetHalfResHistory(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetHalfResHistory
/// @return boolean 当前 history RT 是否半分辨率
static int l_TAA_GetHalfResHistory(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetHalfResHistory() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetSharpenMode
/// @param mode string  Phase F.0.6/F.0.12 — TAA sharpen 算法三选一 (大小写不敏感)
///                     "unsharp" = F.0.1 4-tap unsharp mask (默认, [0, 2] sharpness)
///                     "cas"     = AMD FidelityFX FSR1 5-tap contrast-adaptive sharpening
///                                 ([0, 1] sharpness, 内部 clamp 到 1; +0.02 ms vs unsharp)
///                                 contrast-adaptive: 平滑区域不锁牰 + HDR safe + perceptual gamma
///                     "rcas"    = AMD FidelityFX FSR2 5-tap Robust CAS (Phase F.0.12)
///                                 ([0, 2] sharpness; +0.03 ms vs unsharp)
///                                 noise detection + edge protection: smooth 区不放大 noise / edges 不 ringing
/// 错误处理: 非 string / 未识别值 返 nil + err (与 SetClipMode 同模式)
static int l_TAA_SetSharpenMode(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetSharpenMode: 期望 string 参数 ('unsharp' / 'cas' / 'rcas')");
        return 2;
    }
    const char* mode = lua_tostring(L, 1);
    // 小写化, 与 SetClipMode 同模式
    char lower[16] = {0};
    int  i = 0;
    while (mode[i] && i < 15) {
        char c = mode[i];
        lower[i++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    if (strcmp(lower, "unsharp") != 0 && strcmp(lower, "cas") != 0 && strcmp(lower, "rcas") != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.SetSharpenMode: 未识别的 mode '%s' (期望 'unsharp' / 'cas' / 'rcas')", mode);
        return 2;
    }
    TAARenderer::SetSharpenMode(mode);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetSharpenMode
/// @return string  "unsharp" / "cas" / "rcas"
static int l_TAA_GetSharpenMode(lua_State* L) {
    lua_pushstring(L, TAARenderer::GetSharpenMode());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetMotionGamma
/// @param gamma number Phase F.0.8 — motion-adaptive 高速区域 γ (UE5 高级形式)
///                     仅 ClipMode=="variance" && MotionAdaptive==true 生效
///                     clamp [0, 4]; 默认 1.5 (UE5 推荐)
///                     静止 (|vel|≈0) → 用 varianceGamma; 高速 (>4px) → lerp 到 motionGamma
static int l_TAA_SetMotionGamma(lua_State* L) {
    float g_motion = (float)luaL_checknumber(L, 1);
    TAARenderer::SetMotionGamma(g_motion);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetMotionGamma
/// @return number motion-adaptive 高速区域 γ (默认 1.5)
static int l_TAA_GetMotionGamma(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetMotionGamma());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetMotionAdaptive
/// @param flag boolean Phase F.0.8 — motion-adaptive γ 开关
///                     true = 按 |velocity| 长度 lerp varianceGamma 与 motionGamma
///                     false = 仅用 varianceGamma (F.0.3 行为, 默认零回归)
///                     仅 ClipMode=="variance" 时实际有效果 (其他 clipMode 时 uniform 上传但不读)
static int l_TAA_SetMotionAdaptive(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    TAARenderer::SetMotionAdaptive(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetMotionAdaptive
/// @return boolean motion-adaptive γ 开关 (默认 false)
static int l_TAA_GetMotionAdaptive(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetMotionAdaptive() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetMotionAdaptiveSharpness
/// @param on boolean Phase F.0.13 — motion-adaptive sharpness 开关
///                     true  = 高速相机运动时 effSharpness lerp 到 motionSharpness 减 trail
///                     false = 全屏静态 sharpness (零回归, 与 F.0.1/F.0.6/F.0.12 行为一致)
/// 错误处理: 非 boolean → 返 nil + err (与 SetAntiFlicker 同模式)
static int l_TAA_SetMotionAdaptiveSharpness(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TBOOLEAN) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetMotionAdaptiveSharpness: 期望 boolean 参数");
        return 2;
    }
    TAARenderer::SetMotionAdaptiveSharpness(lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetMotionAdaptiveSharpness
/// @return boolean motion-adaptive sharpness 开关 (默认 false)
static int l_TAA_GetMotionAdaptiveSharpness(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetMotionAdaptiveSharpness() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetMotionSharpness
/// @param s number Phase F.0.13 — 高速运动时目标 sharpness
///                  clamp [0, 2] (与 sharpness 同范围); 默认 0.1 (高速时几乎不锐化, 减 trail 最大)
///                  仅 motionAdaptiveSharpness=true 生效
/// 错误处理: 非 number → 返 nil + err
static int l_TAA_SetMotionSharpness(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetMotionSharpness: 期望 number 参数 (clamp [0, 2])");
        return 2;
    }
    TAARenderer::SetMotionSharpness((float)lua_tonumber(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetMotionSharpness
/// @return number 高速目标 sharpness (clamp [0, 2], 默认 0.1)
static int l_TAA_GetMotionSharpness(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetMotionSharpness());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetUpscaleMode
/// @param mode string Phase F.0.9 — TAA history → sceneTex 上采样算法 (大小写不敏感)
///                     "bilinear" = F.0.5 老路径 (GL_LINEAR stretch, 默认)
///                     "bicubic"  = Catmull-Rom 9-tap bicubic (Sigggraph 2018 Filmic SMAA)
///                                  -50% blur vs bilinear, +0.025 ms @ 1080p
///                                  仅 sharpness=0 && halfResHistory=true 时实际生效
/// 错误处理: 非 string / 未识别值 返 nil + err (与 SetClipMode/SetSharpenMode 同模式)
static int l_TAA_SetUpscaleMode(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetUpscaleMode: 期望 string 参数 ('bilinear' / 'bicubic' / 'lanczos')");
        return 2;
    }
    const char* mode = lua_tostring(L, 1);
    char lower[16] = {0};
    int  i = 0;
    while (mode[i] && i < 15) {
        char c = mode[i];
        lower[i++] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
    }
    // Phase F.0.14: 白名单加 "lanczos" (Lanczos-2 25-tap 5x5 上采样)
    if (strcmp(lower, "bilinear") != 0 && strcmp(lower, "bicubic") != 0 && strcmp(lower, "lanczos") != 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.SetUpscaleMode: 未识别的 mode '%s' (期望 'bilinear' / 'bicubic' / 'lanczos')", mode);
        return 2;
    }
    TAARenderer::SetUpscaleMode(mode);
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetUpscaleMode
/// @return string  "bilinear" / "bicubic"
static int l_TAA_GetUpscaleMode(lua_State* L) {
    lua_pushstring(L, TAARenderer::GetUpscaleMode());
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetFrameCounter
/// @return integer 当前帧 Halton 索引 (0-7, 用于 debug HUD)
static int l_TAA_GetFrameCounter(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)TAARenderer::GetFrameCounter());
    return 1;
}

// ==================== Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 (8 fn) ====================

/// @lua_api Light.Graphics.TAA.SetTAAUEnabled
/// @param flag boolean TAAU 总开关 (默认 false)
/// @return boolean 切换成功 (失败 = HDR 未启用 / 非 default instance / OOM)
static int l_TAA_SetTAAUEnabled(lua_State* L) {
    luaL_checkany(L, 1);
    const bool flag = (lua_toboolean(L, 1) != 0);
    lua_pushboolean(L, TAARenderer::SetTAAUEnabled(flag) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetTAAUEnabled
/// @return boolean
static int l_TAA_GetTAAUEnabled(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetTAAUEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetRenderScale
/// @param scale number [0.5, 1.0] 渲染分辨率比例 (默认 1.0)
///                     0.5  = performance  /  0.667 = balanced
///                     0.75 = quality      /  1.0   = native
static int l_TAA_SetRenderScale(lua_State* L) {
    const lua_Number s = luaL_checknumber(L, 1);
    TAARenderer::SetRenderScale((float)s);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetRenderScale
/// @return number 当前 renderScale (0.5..1.0)
static int l_TAA_GetRenderScale(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetRenderScale());
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetUpscalePreset
/// @param preset string  "performance" / "balanced" / "quality" / "native"
/// @return boolean  接受 = true, 未知字符串 = false (+ warning)
/// 与 SetRenderScale 双向同步 (preset 改 scale, scale 改近似 preset)
static int l_TAA_SetUpscalePreset(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TSTRING) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetUpscalePreset: 期望 string 参数 ('performance' / 'balanced' / 'quality' / 'native')");
        return 2;
    }
    TAARenderer::SetUpscalePreset(lua_tostring(L, 1));
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetUpscalePreset
/// @return string  当前预设名 (找最近档, 容差 0.01)
static int l_TAA_GetUpscalePreset(lua_State* L) {
    lua_pushstring(L, TAARenderer::GetUpscalePreset());
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetRenderResolution
/// @return integer, integer  渲染分辨率 (taauEnabled=false 时 == output-res)
static int l_TAA_GetRenderResolution(lua_State* L) {
    int w = 0, h = 0;
    TAARenderer::GetRenderResolution(&w, &h);
    lua_pushinteger(L, (lua_Integer)w);
    lua_pushinteger(L, (lua_Integer)h);
    return 2;
}

/// @lua_api Light.Graphics.TAA.GetOutputResolution
/// @return integer, integer  输出分辨率 (== 用户 Enable 入参 / 当前 sceneTex output 尺寸)
static int l_TAA_GetOutputResolution(lua_State* L) {
    int w = 0, h = 0;
    TAARenderer::GetOutputResolution(&w, &h);
    lua_pushinteger(L, (lua_Integer)w);
    lua_pushinteger(L, (lua_Integer)h);
    return 2;
}

// ==================== Phase F.1.1 — Mipmap LOD Bias (4 fn) ====================

/// @lua_api Light.Graphics.TAA.SetAutoMipBias
/// @param flag boolean  默认 true (TAAU 启用时自动调 bias)
/// 关闭后 backend 永远收到 bias=0, 用户可用 SetMipBias 手动覆盖.
static int l_TAA_SetAutoMipBias(lua_State* L) {
    luaL_checkany(L, 1);
    TAARenderer::SetAutoMipBias(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetAutoMipBias
/// @return boolean
static int l_TAA_GetAutoMipBias(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetAutoMipBias() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetMipBias
/// @param bias number  clamp [-4.0, +4.0]; 推荐 [-2, 0] 范围
/// 仅在 autoMipBias=false 时持久生效 (auto 模式下被自动 hook 覆盖)
static int l_TAA_SetMipBias(lua_State* L) {
    const lua_Number b = luaL_checknumber(L, 1);
    TAARenderer::SetMipBias((float)b);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetMipBias
/// @return number  当前 backend 应用的 bias (autoMipBias=true 时反映自动计算结果)
static int l_TAA_GetMipBias(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetMipBias());
    return 1;
}

// ==================== Phase F.1.4 — Dynamic Resolution Scaling (5 fn) ====================
//
// 帧率自适应 — 用户每帧调 UpdateDRS, 内部按滑动窗口决策 4 档预设跳转
// 详见 docs/Phase F.1.4 .../DESIGN_PhaseF_1_4.md
//
// 暴露 5 个 Lua API: SetDynamicEnabled / SetDynamicTarget / UpdateDRS / GetDynamicStats / SetDynamicConfig
// (其余 setter pair 通过 SetDynamicConfig table 字段统一访问)

/// @lua_api Light.Graphics.TAA.SetDynamicEnabled
/// @param flag boolean  DRS 总开关; 默认 false (零回归)
/// 关闭时清滑动窗口/cooldown, 但保留 adjustments 累计计数
static int l_TAA_SetDynamicEnabled(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    TAARenderer::SetDynamicEnabled(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetDynamicEnabled
/// @return boolean
static int l_TAA_GetDynamicEnabled(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetDynamicEnabled() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetDynamicTarget
/// @param fps number 目标 FPS, clamp [30, 240]; <=0 自动关 DRS
static int l_TAA_SetDynamicTarget(lua_State* L) {
    const lua_Number fps = luaL_checknumber(L, 1);
    TAARenderer::SetDynamicTarget((float)fps);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetDynamicTarget
/// @return number
static int l_TAA_GetDynamicTarget(lua_State* L) {
    lua_pushnumber(L, (lua_Number)TAARenderer::GetDynamicTarget());
    return 1;
}

/// @lua_api Light.Graphics.TAA.UpdateDRS
/// @param dt number  秒, 当前帧的 deltaTime
/// 用户每帧调用. drsEnabled=false / dt<=0 时立即返回 (no-op)
static int l_TAA_UpdateDRS(lua_State* L) {
    const lua_Number dt = luaL_checknumber(L, 1);
    TAARenderer::UpdateDRS((float)dt);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetDynamicStats
/// @return table {
///   enabled, targetFps, avgFrameTimeMs, avgFps, currentScale, currentPreset,
///   adjustments, framesSinceLastAdjust, warmingUp, windowProgress,
///   gpuFrameTimeMs (F.1.5), source (F.1.5: "none"/"cpu"/"gpu")
/// }
static int l_TAA_GetDynamicStats(lua_State* L) {
    lua_createtable(L, 0, 12);   // F.1.5: 10 → 12 字段
    lua_pushboolean(L, TAARenderer::GetDynamicEnabled() ? 1 : 0);
    lua_setfield(L, -2, "enabled");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetDynamicTarget());
    lua_setfield(L, -2, "targetFps");
    lua_pushnumber(L, (lua_Number)TAARenderer::DynamicAvgFrameTimeMs());
    lua_setfield(L, -2, "avgFrameTimeMs");
    lua_pushnumber(L, (lua_Number)TAARenderer::DynamicAvgFps());
    lua_setfield(L, -2, "avgFps");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetRenderScale());
    lua_setfield(L, -2, "currentScale");
    lua_pushstring(L, TAARenderer::GetUpscalePreset());
    lua_setfield(L, -2, "currentPreset");
    lua_pushinteger(L, (lua_Integer)TAARenderer::DynamicAdjustments());
    lua_setfield(L, -2, "adjustments");
    lua_pushinteger(L, (lua_Integer)TAARenderer::DynamicFramesSinceLastAdjust());
    lua_setfield(L, -2, "framesSinceLastAdjust");
    lua_pushboolean(L, TAARenderer::DynamicWarmingUp() ? 1 : 0);
    lua_setfield(L, -2, "warmingUp");
    lua_pushnumber(L, (lua_Number)TAARenderer::DynamicWindowProgress());
    lua_setfield(L, -2, "windowProgress");
    // Phase F.1.5 — GPU timer 数据
    lua_pushnumber(L, (lua_Number)TAARenderer::DynamicGpuFrameTimeMs());
    lua_setfield(L, -2, "gpuFrameTimeMs");
    lua_pushstring(L, TAARenderer::DynamicLastSource());
    lua_setfield(L, -2, "source");
    return 1;
}

// ==================== Phase F.1.5 — GPU Timer for DRS (2 fn) ====================
//
// 用户开关让 DRS 决策强制 CPU/GPU 时间源.
// 默认 prefer GPU (backend->SupportsGpuTimer()=true 时启用).

/// @lua_api Light.Graphics.TAA.SetPreferGpuSource
/// @param flag boolean 默认 true; 关后 DRS 走 CPU 路径 (即使 backend 支持 GPU timer)
static int l_TAA_SetPreferGpuSource(lua_State* L) {
    luaL_checktype(L, 1, LUA_TBOOLEAN);
    TAARenderer::SetPreferGpuSource(lua_toboolean(L, 1) != 0);
    return 0;
}

/// @lua_api Light.Graphics.TAA.GetPreferGpuSource
/// @return boolean
static int l_TAA_GetPreferGpuSource(lua_State* L) {
    lua_pushboolean(L, TAARenderer::GetPreferGpuSource() ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetDynamicConfig
/// @param cfg table  可选字段:
///   windowSize     integer [5, 120]   滑动窗口大小 (默认 30)
///   cooldownFrames integer [10, 600]  调整冷却帧数 (默认 60)
///   downThreshold  number  [1.01, 2.0] 降级阈值 (默认 1.10)
///   upThreshold    number  [0.5, 0.99] 升级阈值 (默认 0.85)
/// 缺失字段保持原值; 越界值 clamp + log warning (不 raise, 友好降级)
static int l_TAA_SetDynamicConfig(lua_State* L) {
    if (lua_type(L, 1) != LUA_TTABLE) {
        // 友好降级: 非 table 时静默忽略 (与 SetClipMode/SetSharpenMode 容错风格一致)
        return 0;
    }
    // windowSize
    lua_getfield(L, 1, "windowSize");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        TAARenderer::SetDynamicWindowSize((int)lua_tointeger(L, -1));
    }
    lua_pop(L, 1);
    // cooldownFrames
    lua_getfield(L, 1, "cooldownFrames");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        TAARenderer::SetDynamicCooldownFrames((int)lua_tointeger(L, -1));
    }
    lua_pop(L, 1);
    // downThreshold
    lua_getfield(L, 1, "downThreshold");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        TAARenderer::SetDynamicDownThreshold((float)lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    // upThreshold
    lua_getfield(L, 1, "upThreshold");
    if (lua_type(L, -1) == LUA_TNUMBER) {
        TAARenderer::SetDynamicUpThreshold((float)lua_tonumber(L, -1));
    }
    lua_pop(L, 1);
    return 0;
}

// ==================== Phase F.0.10 — Multi-Instance API (5 fn) ====================

/// @lua_api Light.Graphics.TAA.CreateInstance
/// @return integer instance ID [1, 3] 或 0 (槽满 / 未 Init)
/// 新 instance 默认 disabled, 需 SetActiveInstance + Enable 启用; 继承 default 的 backend ptr
static int l_TAA_CreateInstance(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)TAARenderer::CreateInstance());
    return 1;
}

/// @lua_api Light.Graphics.TAA.DestroyInstance
/// @param id integer instance ID [1, 3]; id=0 (default) 拒绝
/// @return boolean true=成功; 非法 id / 未分配 id 返 nil + err
/// 销毁后, 若 active 是该 id, 自动切回 default (0)
static int l_TAA_DestroyInstance(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.DestroyInstance: 期望 integer 参数 (instance id [1, 3])");
        return 2;
    }
    const int id = (int)lua_tointeger(L, 1);
    if (!TAARenderer::DestroyInstance(id)) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.DestroyInstance: id=%d 非法或未分配 (id=0 是 default 不可销毁)", id);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetActiveInstance
/// @param id integer instance ID [0, 3]; 0=default; 1..3=用户 instance
/// @return boolean true=成功; 非法 id / 未分配 id 返 nil + err
/// 切换后, 后续 35 fn 全部作用于新 active instance
static int l_TAA_SetActiveInstance(lua_State* L) {
    luaL_checkany(L, 1);
    if (lua_type(L, 1) != LUA_TNUMBER) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.SetActiveInstance: 期望 integer 参数 (instance id [0, 3])");
        return 2;
    }
    const int id = (int)lua_tointeger(L, 1);
    if (!TAARenderer::SetActiveInstance(id)) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.SetActiveInstance: id=%d 非法或未分配", id);
        return 2;
    }
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetActiveInstance
/// @return integer 当前 active instance ID (default = 0)
static int l_TAA_GetActiveInstance(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)TAARenderer::GetActiveInstance());
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetInstanceCount
/// @return integer 已分配 instance 数 [1, 4] (含 default)
static int l_TAA_GetInstanceCount(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)TAARenderer::GetInstanceCount());
    return 1;
}

/// @lua_api Light.Graphics.TAA.CloneInstance — Phase F.0.10.9.x.3 (1-line setup)
static int l_TAA_CloneInstance(lua_State* L) {
    int srcId = (int)luaL_checkinteger(L, 1);
    lua_pushinteger(L, TAARenderer::CloneInstance(srcId));
    return 1;
}

/// @lua_api Light.Graphics.TAA.SetState — Phase F.0.10.9.x.4 (反向 GetState)
/// @param state table  字段名匹配 GetState (任 partial 子集); enabled/supported 静默跳过
/// @return boolean, integer  ok=至少应用 1 字段 / applied=应用字段数
/// @return nil, string       入参非 table
/// @note clip_mode/sharpen_mode/upscale_mode 接 string (与 GetState 输出对称)
static int l_TAA_SetState(lua_State* L) {
    if (!lua_istable(L, 1)) {
        lua_pushnil(L);
        lua_pushstring(L, "SetState: expect table arg");
        return 2;
    }
    int applied = 0;
    APPLY_NUM("blend_alpha",                  TAARenderer::SetBlendAlpha);
    APPLY_BOOL("neighborhood_clip",           TAARenderer::SetNeighborhoodClip);
    APPLY_STR("clip_mode",                    TAARenderer::SetClipMode);
    APPLY_BOOL("jitter_enabled",              TAARenderer::SetJitterEnabled);
    APPLY_NUM("sharpness",                    TAARenderer::SetSharpness);
    APPLY_NUM("variance_gamma",               TAARenderer::SetVarianceGamma);
    APPLY_BOOL("half_res_history",            TAARenderer::SetHalfResHistory);
    APPLY_STR("sharpen_mode",                 TAARenderer::SetSharpenMode);
    APPLY_STR("upscale_mode",                 TAARenderer::SetUpscaleMode);
    APPLY_BOOL("anti_flicker",                TAARenderer::SetAntiFlicker);
    APPLY_NUM("motion_gamma",                 TAARenderer::SetMotionGamma);
    APPLY_BOOL("motion_adaptive_gamma",       TAARenderer::SetMotionAdaptive);
    APPLY_NUM("motion_sharpness",             TAARenderer::SetMotionSharpness);
    APPLY_BOOL("motion_adaptive_sharpness",   TAARenderer::SetMotionAdaptiveSharpness);
    lua_pushboolean(L, applied > 0 ? 1 : 0);
    lua_pushinteger(L, applied);
    return 2;
}

/// @lua_api Light.Graphics.TAA.GetState — Phase F.0.10.9.x.3 (snapshot)
/// 补全 F.0.10.10.1: 增加 clip_mode / anti_flicker / motion_gamma 等字段方便 setup 审查
static int l_TAA_GetState(lua_State* L) {
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)TAARenderer::GetBlendAlpha());        lua_setfield(L, -2, "blend_alpha");
    lua_pushboolean(L, TAARenderer::GetNeighborhoodClip() ? 1 : 0);     lua_setfield(L, -2, "neighborhood_clip");
    lua_pushstring(L, TAARenderer::GetClipMode());                      lua_setfield(L, -2, "clip_mode");
    lua_pushboolean(L, TAARenderer::GetJitterEnabled() ? 1 : 0);        lua_setfield(L, -2, "jitter_enabled");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetSharpness());         lua_setfield(L, -2, "sharpness");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetVarianceGamma());     lua_setfield(L, -2, "variance_gamma");
    lua_pushboolean(L, TAARenderer::GetHalfResHistory() ? 1 : 0);       lua_setfield(L, -2, "half_res_history");
    lua_pushstring(L, TAARenderer::GetSharpenMode());                   lua_setfield(L, -2, "sharpen_mode");
    lua_pushstring(L, TAARenderer::GetUpscaleMode());                   lua_setfield(L, -2, "upscale_mode");
    lua_pushboolean(L, TAARenderer::GetAntiFlicker() ? 1 : 0);          lua_setfield(L, -2, "anti_flicker");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetMotionGamma());       lua_setfield(L, -2, "motion_gamma");
    lua_pushboolean(L, TAARenderer::GetMotionAdaptive() ? 1 : 0);       lua_setfield(L, -2, "motion_adaptive_gamma");
    lua_pushnumber(L, (lua_Number)TAARenderer::GetMotionSharpness());   lua_setfield(L, -2, "motion_sharpness");
    lua_pushboolean(L, TAARenderer::GetMotionAdaptiveSharpness() ? 1 : 0); lua_setfield(L, -2, "motion_adaptive_sharpness");
    lua_pushboolean(L, TAARenderer::IsEnabled() ? 1 : 0);               lua_setfield(L, -2, "enabled");
    lua_pushboolean(L, TAARenderer::IsSupported() ? 1 : 0);             lua_setfield(L, -2, "supported");
    return 1;
}

/// @lua_api Light.Graphics.TAA.GetCurrentJitter
/// @return number, number 本帧 sub-pixel jitter offset (±0.5 pixel, 仅 enabled+jitter 时非零)
static int l_TAA_GetCurrentJitter(lua_State* L) {
    float x = 0.0f, y = 0.0f;
    TAARenderer::GetCurrentJitter(&x, &y);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 2;
}

/// @lua_api Light.Graphics.TAA.Process
/// @brief Phase F.0.10.2 — 手动 TAA process (用户控制 TAA 时序, 配合 HDR.SetAutoTAA(false))
/// @param rgnX integer 可选: 区域左下 X (默认 0)
/// @param rgnY integer 可选: 区域左下 Y (默认 0)
/// @param rgnW integer 可选: 区域宽 (默认 0 = 全屏, 零回归)
/// @param rgnH integer 可选: 区域高 (默认 0 = 全屏)
/// @return boolean true 成功 (或静默 no-op); nil, string = 参数非法 / HDR 未启用
/// @note 内部用 HDRRenderer::GetFBO() + GetSceneTexture() 作为 hdr 目标, 无需用户传句柄
///       老的 EndScene 自动 TAA 仍 default 启用, 用户通常需先 HDR.SetAutoTAA(false) 避免双 TAA
///       headless / HDR 未启 时静默 no-op (返 false + err string)
///       典型用法:
///         HDR.SetAutoTAA(false)
///         TAA.SetActiveInstance(1); TAA.ApplyJitter(); -- ...draw player 1...
///         TAA.Process(0, 0, W/2, H)   -- 处理左半 region (instance 1 history)
///         TAA.SetActiveInstance(2); TAA.ApplyJitter(); -- ...draw player 2...
///         TAA.Process(W/2, 0, W/2, H) -- 处理右半 region (instance 2 history)
static int l_TAA_Process(lua_State* L) {
    // 参数检查 (4 个 optional integer, 缺省 = 0): 全部 0 = 全屏老接口
    const int nargs = lua_gettop(L);
    int rgnX = 0, rgnY = 0, rgnW = 0, rgnH = 0;
    if (nargs >= 1) rgnX = (int)luaL_checkinteger(L, 1);
    if (nargs >= 2) rgnY = (int)luaL_checkinteger(L, 2);
    if (nargs >= 3) rgnW = (int)luaL_checkinteger(L, 3);
    if (nargs >= 4) rgnH = (int)luaL_checkinteger(L, 4);

    // 防御性: 部分 region 参数 (只传 2 个 / 3 个) 视为非法
    if (nargs != 0 && nargs != 4) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.Process: expected 0 or 4 args (got %d); region=(x,y,w,h) all-or-none", nargs);
        return 2;
    }
    // 防御性: 区域 w/h 必须 >= 0 (0/0/0/0 = 全屏 path; w<0 或 h<0 拒绝)
    if (rgnW < 0 || rgnH < 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "TAA.Process: w/h must be >= 0 (got w=%d, h=%d)", rgnW, rgnH);
        return 2;
    }

    // 取 HDR fbo + sceneTex (HDR 未启用时返 0, TAARenderer::Process 内部静默 no-op)
    const uint32_t fbo = HDRRenderer::GetFBO();
    const uint32_t tex = HDRRenderer::GetSceneTexture();
    if (!fbo || !tex) {
        lua_pushnil(L);
        lua_pushliteral(L, "TAA.Process: HDR not enabled (fbo / sceneTex = 0)");
        return 2;
    }

    // 转发到 TAARenderer (rgnW/rgnH=0 时 backend 内部跳过 scissor, 与无参 Process 等价)
    TAARenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH);
    lua_pushboolean(L, 1);
    return 1;
}

static const luaL_Reg taa_funcs[] = {
    // lifecycle (5)
    {"Enable",                l_TAA_Enable},
    {"Disable",               l_TAA_Disable},
    {"IsEnabled",             l_TAA_IsEnabled},
    {"IsSupported",           l_TAA_IsSupported},
    {"Resize",                l_TAA_Resize},
    // params (6 = 3 对): blendAlpha + neighborhoodClip + jitterEnabled
    {"SetBlendAlpha",         l_TAA_SetBlendAlpha},
    {"GetBlendAlpha",         l_TAA_GetBlendAlpha},
    {"SetNeighborhoodClip",   l_TAA_SetNeighborhoodClip},
    {"GetNeighborhoodClip",   l_TAA_GetNeighborhoodClip},
    {"SetJitterEnabled",      l_TAA_SetJitterEnabled},
    {"GetJitterEnabled",      l_TAA_GetJitterEnabled},
    // Phase F.0.1 — sharpness (2 = 1 对): 4-tap unsharp mask, clamp [0, 2], 默认 0.5
    {"SetSharpness",          l_TAA_SetSharpness},
    {"GetSharpness",          l_TAA_GetSharpness},
    // Phase F.0.4 — anti-flicker (2 = 1 对): Karis luma-weighted blend, 默认 true
    {"SetAntiFlicker",        l_TAA_SetAntiFlicker},
    {"GetAntiFlicker",        l_TAA_GetAntiFlicker},
    // Phase F.0.2/F.0.3 — clip color space (2 = 1 对): "rgb" / "ycocg" / "variance", 默认 "ycocg"
    {"SetClipMode",           l_TAA_SetClipMode},
    {"GetClipMode",           l_TAA_GetClipMode},
    // Phase F.0.3 — variance gamma (2 = 1 对): 仅 ClipMode=="variance" 生效, clamp [0, 4], 默认 1.0
    {"SetVarianceGamma",      l_TAA_SetVarianceGamma},
    {"GetVarianceGamma",      l_TAA_GetVarianceGamma},
    // Phase F.0.5 — half-res history (2 = 1 对): VRAM -75%, 默认 false (零回归)
    {"SetHalfResHistory",     l_TAA_SetHalfResHistory},
    {"GetHalfResHistory",     l_TAA_GetHalfResHistory},
    // Phase F.0.6/F.0.12 — sharpen mode (2 = 1 对): "unsharp" (默认) / "cas" (FSR1) / "rcas" (FSR2)
    {"SetSharpenMode",        l_TAA_SetSharpenMode},
    {"GetSharpenMode",        l_TAA_GetSharpenMode},
    // Phase F.0.8 — motion-adaptive γ (4 = 2 对): UE5 高级形式, motion γ + 开关, 默认 false (零回归)
    {"SetMotionGamma",        l_TAA_SetMotionGamma},
    {"GetMotionGamma",        l_TAA_GetMotionGamma},
    {"SetMotionAdaptive",     l_TAA_SetMotionAdaptive},
    {"GetMotionAdaptive",     l_TAA_GetMotionAdaptive},
    // Phase F.0.13 — motion-adaptive sharpness (4 = 2 对): 高速时 sharpness lerp 到 motionSharpness, 默认 OFF
    {"SetMotionAdaptiveSharpness", l_TAA_SetMotionAdaptiveSharpness},
    {"GetMotionAdaptiveSharpness", l_TAA_GetMotionAdaptiveSharpness},
    {"SetMotionSharpness",         l_TAA_SetMotionSharpness},
    {"GetMotionSharpness",         l_TAA_GetMotionSharpness},
    // Phase F.0.9 — custom upsampler (2 = 1 对): "bilinear" (F.0.5 默认) / "bicubic" (Catmull-Rom)
    {"SetUpscaleMode",        l_TAA_SetUpscaleMode},
    {"GetUpscaleMode",        l_TAA_GetUpscaleMode},
    // Phase F.0.10 — multi-instance API (5 fn): default + 3 user instance, split-screen 双人/四人
    {"CreateInstance",        l_TAA_CreateInstance},
    {"DestroyInstance",       l_TAA_DestroyInstance},
    {"SetActiveInstance",     l_TAA_SetActiveInstance},
    {"GetActiveInstance",     l_TAA_GetActiveInstance},
    {"GetInstanceCount",      l_TAA_GetInstanceCount},
    // Phase F.0.10.9.x.3 — Clone + Snapshot
    {"CloneInstance",         l_TAA_CloneInstance},
    {"GetState",              l_TAA_GetState},
    // Phase F.0.10.9.x.4 — SetState (反向 GetState)
    {"SetState",              l_TAA_SetState},
    // Phase F.0.10.2 — 手动 TAA Process (region 可选, 配合 HDR.SetAutoTAA(false) 做真物理 split-screen)
    {"Process",               l_TAA_Process},
    // status (2): debug HUD 用
    {"GetFrameCounter",       l_TAA_GetFrameCounter},
    {"GetCurrentJitter",      l_TAA_GetCurrentJitter},
    // Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 (8 fn: 3 setter + 3 getter + 2 resolution query)
    {"SetTAAUEnabled",        l_TAA_SetTAAUEnabled},
    {"GetTAAUEnabled",        l_TAA_GetTAAUEnabled},
    {"SetRenderScale",        l_TAA_SetRenderScale},
    {"GetRenderScale",        l_TAA_GetRenderScale},
    {"SetUpscalePreset",      l_TAA_SetUpscalePreset},
    {"GetUpscalePreset",      l_TAA_GetUpscalePreset},
    {"GetRenderResolution",   l_TAA_GetRenderResolution},
    {"GetOutputResolution",   l_TAA_GetOutputResolution},
    // Phase F.1.1 TAAU — Mipmap LOD bias (4 fn: 2 setter + 2 getter)
    {"SetAutoMipBias",        l_TAA_SetAutoMipBias},
    {"GetAutoMipBias",        l_TAA_GetAutoMipBias},
    {"SetMipBias",            l_TAA_SetMipBias},
    {"GetMipBias",            l_TAA_GetMipBias},
    // Phase F.1.4 TAAU — Dynamic Resolution Scaling (7 fn: 5 主 + 2 query)
    {"SetDynamicEnabled",     l_TAA_SetDynamicEnabled},
    {"GetDynamicEnabled",     l_TAA_GetDynamicEnabled},
    {"SetDynamicTarget",      l_TAA_SetDynamicTarget},
    {"GetDynamicTarget",      l_TAA_GetDynamicTarget},
    {"UpdateDRS",             l_TAA_UpdateDRS},
    {"GetDynamicStats",       l_TAA_GetDynamicStats},
    {"SetDynamicConfig",      l_TAA_SetDynamicConfig},
    // Phase F.1.5 — GPU Timer for DRS (2 fn)
    {"SetPreferGpuSource",    l_TAA_SetPreferGpuSource},
    {"GetPreferGpuSource",    l_TAA_GetPreferGpuSource},
    {NULL, NULL}
};

// ============================================================================
// Phase F.0.11 — Screenshot / PNG Sequence Record
//
// 设计:
//   1) 单帧截图: glReadPixels(default fb) → stbi_write_png
//   2) 录屏: 全局 g_record 状态; 在 l_Window_Call EndFrame 之后 SwapBuffers 之前
//      由 light_ui.cpp 调 RecordTickHook() 完成每帧 readback + 写盘
//   3) Y 翻转: OpenGL 左下原点 vs PNG 左上原点 → stbi_flip_vertically_on_write(1)
//
// 跨 phase 协作:
//   - light_ui.cpp 通过 extern 调用 Light_Graphics_RecordTickHook()
//   - PNG 编码全平台 stb_image_write (公共领域, 与 stb_image 同源)
// ============================================================================

struct RecordState {
    bool        active        = false;
    std::string dir_prefix;            // e.g. "frames/"
    int         frame_count   = 0;     // 已写 PNG 数 (递增, 用作文件名 frame_NNNN)
    int         max_frames    = 0;     // 0=unlimited; 否则达此值自动 stop (按已写 PNG 数算)
    // Phase F.0.11.1: 帧跳过 (降低录屏 IO 开销)
    //   frame_skip=1 → 每渲染帧写 1 张 (默认, 与 F.0.11 行为一致)
    //   frame_skip=2 → 每 2 帧写 1 张 (60fps 渲染 → 30fps 录屏)
    //   frame_skip=3 → 每 3 帧写 1 张 (60fps 渲染 → 20fps 录屏, 显著降卡顿)
    int         frame_skip    = 1;
    int         tick_count    = 0;     // 渲染帧累计 (内部, 用于 % frame_skip 判断)
    // Phase F.0.11.2: 异步 readback (PBO ping-pong, GPU 无 stall)
    //   true  = 用 backend->ReadbackDefaultFBAsync (PNG 写盘与下一帧渲染并行)
    //   false = 用同步 ReadbackDefaultFB (默认, 与 F.0.11 行为一致)
    bool        use_async     = false;
    // Phase F.0.11.3: 缓存上次 readback 的尺寸, 用于 StopRecord 时 flush 最后 1 帧
    //   StopRecord 时 last_w/last_h > 0 且 use_async 且 backend 支持 → 调 FlushLast 取数据写 PNG
    int         last_w        = 0;
    int         last_h        = 0;
    // Phase F.0.11.6 — 录屏模式: 0=PNG sequence (F.0.11 默认) / 1=MP4 H.264 (FFmpeg encoder)
    //   mode=1 时 dir_prefix 当作 mp4 path 用 (用户传 "out.mp4" 而非目录前缀)
    int         mode          = 0;
    // Phase F.0.11.6.1.A7 — 录屏期间屏幕角落显示 ● REC 闪烁标记 (默认开)
    //   绘制时机: l_Window_Call 内 RecordTickHook 之后, SwapBuffers 之前
    //   → readback 看不到 OSD, mp4 内不会出现红点 (只在屏幕显示)
    bool        show_osd      = true;
    // Phase F.0.11.6.3.A9 — ROI 录屏 (仅 mp4 模式): 只录屏幕指定矩形区域
    //   坐标系: 屏幕左上原点 (Lua 友好); readback 时转 GL 左下原点 (win_h - y - h)
    //   roi_w/h <= 0 表示未启用 → 录全屏 (与 F.0.11.6.2 之前行为一致)
    //   尺寸必须偶数 (libx264 要求), Lua 解析时 round down 到偶数
    int         roi_x         = 0;
    int         roi_y         = 0;
    int         roi_w         = 0;   // > 0 = 启用 ROI
    int         roi_h         = 0;
};
static RecordState g_record;

/// 共用 helper: 读 default fb (x,y,w,h) → 写 PNG
/// @return 1=成功; 0=失败 (readback 或 PNG 写盘)
static int do_screenshot_internal(const char* path, int x, int y, int w, int h) {
    if (!g_render || w <= 0 || h <= 0 || !path) return 0;
    // 分配一次性 buffer (栈太大, 1280x720x4 ≈ 3.5MB, 用 heap)
    std::vector<unsigned char> buf((size_t)w * h * 4);
    if (!g_render->ReadbackDefaultFB(x, y, w, h, buf.data())) return 0;
    stbi_flip_vertically_on_write(1);   // GL 左下 → PNG 左上
    return stbi_write_png(path, w, h, 4, buf.data(), w * 4);
}

/// Phase F.0.11.6.3.A9 — 计算本帧实际 readback 区域 (ROI 或全屏)
///   屏幕左上原点 (roi_x, roi_y) → GL 左下原点 (gl_x, gl_y)
///   roi_w/h <= 0 时退回全屏 (0,0,win_w,win_h, 全屏 → gl_y=0)
///   返 true = 区域合法可读; false = ROI 越界 / 0 尺寸 / 窗口过小
static bool calc_readback_region_(int win_w, int win_h,
                                  int* out_x, int* out_y, int* out_w, int* out_h) {
    if (g_record.roi_w <= 0 || g_record.roi_h <= 0) {
        // 未启用 ROI → 全屏
        *out_x = 0;  *out_y = 0;
        *out_w = win_w;  *out_h = win_h;
        return true;
    }
    // ROI 模式: 屏幕坐标 → GL 坐标 (Y 翻转)
    const int x = g_record.roi_x;
    const int y = g_record.roi_y;
    const int w = g_record.roi_w;
    const int h = g_record.roi_h;
    // 越界检查: ROI 必须完全在当前窗口内
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > win_w || y + h > win_h) {
        return false;
    }
    *out_x = x;
    *out_y = win_h - y - h;   // GL 左下原点
    *out_w = w;
    *out_h = h;
    return true;
}

/// 录屏 tick hook — light_ui.cpp::l_Window_Call 在 SwapBuffers 之前调用
/// 不抛 lua 异常 (因为 light_ui 不在 lua_State* 上下文里); 录屏失败 silent
/// 注: 录屏读 default fb 当前窗口尺寸 (用户切窗口大小后, 下一帧自动用新尺寸)
extern "C" void Light_Graphics_RecordTickHook(int win_w, int win_h) {
    if (!g_record.active || win_w <= 0 || win_h <= 0 || !g_render) return;
    // Phase F.0.11.6.2.A10: 暂停中跳过整个 tick (不递增 tick_count, 不写 PNG/mp4)
    //   mp4 模式: RecordMP4::IsPaused() 同步返 true → Acquire/Commit 跳过, pts 不前进
    //   PNG 模式: 文件名 frame_NNNN 不递增, 录屏暂停期间不产生空隙
    if (g_record.mode == 1 && RecordMP4::IsPaused()) return;
    // Phase F.0.11.1: frame_skip 检查 — 每 N 渲染帧才实际写 1 张 PNG
    // tick_count 从 1 起递增, 仅当 (tick_count % frame_skip == 0) 写盘
    ++g_record.tick_count;
    if (g_record.frame_skip > 1 && (g_record.tick_count % g_record.frame_skip) != 0) return;

    // Phase F.0.11.6: MP4 模式分支 (mode=1) — 走 RecordMP4 encoder, 不写 PNG
    // Phase F.0.11.6.1.A1: 用 zero-copy ring buffer 路径 (Acquire → readback 直写 slot → Commit)
    // Phase F.0.11.6.1.A2: 当 use_async=true 时走 PBO ping-pong 异步 readback (主线程不 stall GPU)
    // Phase F.0.11.6.3.A9: 当 roi_w/h > 0 时只录 ROI 区域 (mp4 输出尺寸 = ROI 尺寸)
    if (g_record.mode == 1) {
        if (!RecordMP4::IsActive()) return;   // Open 失败时被动 deactivate

        // A9 — 计算 readback 区域 (ROI 或全屏); 越界时跳过本帧
        int rb_x, rb_y, rb_w, rb_h;
        if (!calc_readback_region_(win_w, win_h, &rb_x, &rb_y, &rb_w, &rb_h)) {
            CC::Log(CC::LOG_WARN, "RecordMP4: ROI [%d,%d %dx%d] out of window %dx%d (skip frame %d)",
                    g_record.roi_x, g_record.roi_y, g_record.roi_w, g_record.roi_h,
                    win_w, win_h, g_record.frame_count);
            return;
        }

        // A2 异步路径: ReadbackDefaultFBAsync 启动新 PBO + 取上一帧数据 (首帧无数据延后 1 帧)
        if (g_record.use_async) {
            g_record.last_w = rb_w;   // 缓存 readback 尺寸 (供 StopRecord flush)
            g_record.last_h = rb_h;
            uint8_t* slot = RecordMP4::AcquireWriteSlot(g_record.frame_count);
            if (!slot) return;
            if (g_render->ReadbackDefaultFBAsync(rb_x, rb_y, rb_w, rb_h, slot)) {
                // 取到上一帧 PBO 数据 → Commit
                RecordMP4::CommitWriteSlot();
                ++g_record.frame_count;
                if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
                    // max 达成: flush 最后 1 帧 pending PBO 后再 Close (避免漏最后一帧)
                    uint8_t* last_slot = RecordMP4::AcquireWriteSlot(g_record.frame_count);
                    if (last_slot && g_render->ReadbackAsyncFlushLast(last_slot)) {
                        RecordMP4::CommitWriteSlot();
                        ++g_record.frame_count;
                    } else if (last_slot) {
                        RecordMP4::CancelWriteSlot();
                    }
                    RecordMP4::Close();
                    g_record.active = false;
                }
            } else {
                // 首次调用 PBO 已启动但没数据 → 取消 slot, 不算一帧
                RecordMP4::CancelWriteSlot();
            }
            return;
        }

        // 同步路径 (use_async=false, 默认): glReadPixels 同步 stall
        uint8_t* slot = RecordMP4::AcquireWriteSlot(g_record.frame_count);
        if (!slot) return;
        if (!g_render->ReadbackDefaultFB(rb_x, rb_y, rb_w, rb_h, slot)) {
            // Phase F.0.11.6.1.A4: 失败精确取消, 不写坏帧到 worker
            RecordMP4::CancelWriteSlot();
            CC::Log(CC::LOG_WARN, "RecordMP4: ReadbackDefaultFB failed at frame %d (skipped)", g_record.frame_count);
            return;
        }
        RecordMP4::CommitWriteSlot();
        ++g_record.frame_count;
        if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
            RecordMP4::Close();
            g_record.active = false;
        }
        return;
    }

    // Phase F.0.11.2: 异步路径 — PBO ping-pong, glReadPixels 不 stall GPU
    //   首次调用启动 PBO[0], 取不到数据 → 不写 PNG (frame_count 不递增, 用户感知"延 1 帧")
    //   第 2+ 次启动新 readback + 取上一帧数据 → 写 PNG
    //   用户场景: 录 60 帧 (max_frames=60), 实际渲染 ~61 帧, 输出 60 PNG, ffmpeg 不漏号
    if (g_record.use_async) {
        // F.0.11.3: 缓存尺寸供 StopRecord flush 用
        g_record.last_w = win_w;
        g_record.last_h = win_h;
        std::vector<unsigned char> buf((size_t)win_w * win_h * 4);
        if (g_render->ReadbackDefaultFBAsync(0, 0, win_w, win_h, buf.data())) {
            // 取到上一帧数据 → 写盘
            char path[512];
            std::snprintf(path, sizeof(path), "%sframe_%04d.png",
                          g_record.dir_prefix.c_str(), g_record.frame_count);
            stbi_flip_vertically_on_write(1);
            stbi_write_png(path, win_w, win_h, 4, buf.data(), win_w * 4);
            ++g_record.frame_count;
            if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
                g_record.active = false;
            }
        }
        // else: 首次调用 (PBO 已启动, 下一帧取数据), 或 backend 不支持 async (默认 fallback)
        return;
    }

    // 同步路径 (默认, 与 F.0.11 行为一致)
    char path[512];
    std::snprintf(path, sizeof(path), "%sframe_%04d.png",
                  g_record.dir_prefix.c_str(), g_record.frame_count);
    do_screenshot_internal(path, 0, 0, win_w, win_h);
    ++g_record.frame_count;
    if (g_record.max_frames > 0 && g_record.frame_count >= g_record.max_frames) {
        g_record.active = false;   // auto-stop (按已写 PNG 数算, 不算 skip 跳过的)
    }
}

/// Phase F.0.11.6.1.A7 — 录屏指示器 OSD (左上角红点闪烁)
/// 调用时机: light_ui::l_Window_Call 在 Light_Graphics_RecordTickHook 之后, SwapBuffers 之前.
/// → readback 已完成, OSD 不进 mp4; 仅显示在屏幕给用户视觉反馈.
/// 实现: 直接走 g_render->DrawArrays immediate 路径 (不走 BatchRenderer, 因 EndFrame 已调).
extern "C" void Light_Graphics_DrawRecordOSD(int win_w, int win_h) {
    if (!g_record.active || !g_record.show_osd) return;
    if (!g_render || win_w <= 0 || win_h <= 0) return;

    // OSD 几何: 左上角距边 12px, 16x16 红色方块 (OpenGL 像素坐标系, 左下原点)
    const float pad  = 12.0f;
    const float size = 16.0f;
    const float x0   = pad;
    const float x1   = pad + size;
    const float y0   = (float)win_h - pad - size;
    const float y1   = (float)win_h - pad;

    // 闪烁: 1 秒周期 (60 ticks @ 60fps), 前 30 ticks 满红, 后 30 ticks 暗红
    //   tick_count 来自 RecordTickHook 累计帧数 (与 frame_skip 无关, 始终递增)
    const int   phase = g_record.tick_count % 60;
    const float alpha = (phase < 30) ? 1.0f : 0.35f;

    RenderVertex verts[4];
    for (int i = 0; i < 4; ++i) {
        verts[i].r = 1.0f; verts[i].g = 0.0f; verts[i].b = 0.0f; verts[i].a = alpha;
        verts[i].u = 0.0f; verts[i].v = 0.0f;
        verts[i].z = 0.0f;
    }
    // Quad: BL, BR, TR, TL (OpenGL DrawMode::Quads 顺序与 GL_QUADS 一致)
    verts[0].x = x0; verts[0].y = y0;
    verts[1].x = x1; verts[1].y = y0;
    verts[2].x = x1; verts[2].y = y1;
    verts[3].x = x0; verts[3].y = y1;

    // immediate 提交 (跳过 BatchRenderer, EndFrame 已调过)
    g_render->DrawArrays(DrawMode::Quads, verts, 4);
}

/// @lua_api Light.Graphics.Screenshot
/// @brief Phase F.0.11 — 同步截屏当前 default fb 全屏 → PNG
/// @param path string  输出文件路径 (调用方需保证目录存在; PNG 编码 RGBA8)
/// @return boolean true=成功; nil, string=g_render 未就绪 / readback 失败 / 写盘失败
/// @note Y 自动翻转 (GL 左下 → PNG 左上); HDR 场景下读 default fb (tonemap 之后 LDR)
static int l_Graphics_Screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int x = 0, y = 0, w = 0, h = 0;
    if (g_render) g_render->GetViewport(&x, &y, &w, &h);
    // viewport 通常 = window size; 若 0 则失败
    if (w <= 0 || h <= 0) {
        lua_pushnil(L);
        lua_pushstring(L, "Screenshot: invalid viewport (w<=0 or h<=0)");
        return 2;
    }
    if (do_screenshot_internal(path, 0, 0, w, h)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "Screenshot: failed to write '%s' (check dir exists + write permission)", path);
    return 2;
}

/// @lua_api Light.Graphics.ScreenshotRegion
/// @brief Phase F.0.11 — region 截图 (单 quad / 自定义子区域)
/// @param path string
/// @param x integer  region 左下角 X (OpenGL 习惯, 左下原点)
/// @param y integer  region 左下角 Y
/// @param w integer  region 宽
/// @param h integer  region 高
/// @return boolean true / nil, err
static int l_Graphics_ScreenshotRegion(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    int x = (int)luaL_checkinteger(L, 2);
    int y = (int)luaL_checkinteger(L, 3);
    int w = (int)luaL_checkinteger(L, 4);
    int h = (int)luaL_checkinteger(L, 5);
    if (w <= 0 || h <= 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "ScreenshotRegion: w/h must be > 0 (got %d x %d)", w, h);
        return 2;
    }
    if (do_screenshot_internal(path, x, y, w, h)) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "ScreenshotRegion: failed to write '%s'", path);
    return 2;
}

/// @lua_api Light.Graphics.RecordPNGSequence
/// @brief Phase F.0.11 — 开始录屏, PNG 序列输出到 dir; F.0.11.1 加 frame_skip 性能优化
/// @param dir_prefix string  目录前缀 (调用方需保证目录已存在); 输出名 dir/frame_NNNN.png
/// @param max_frames integer 最大帧数 (>0 时达此值自动 stop, 0=无限直到 StopRecord)
/// @param frame_skip integer? 每 N 渲染帧写 1 张 (默认 1=每帧都写; 2=隔帧, 3=每 3 帧, ...)
/// @return boolean true / nil, err
/// @note 同 RecordPNGSequence 多次调用: 后调用复位计数; 不允许同时录多份.
/// @note frame_skip 与 max_frames 独立: max_frames 是已写 PNG 数, 不计 skip 跳过的渲染帧.
///       例: frame_skip=3, max_frames=60 → 录到 60 张 PNG, 实际跨 ~180 渲染帧.
static int l_Graphics_RecordPNGSequence(lua_State* L) {
    const char* dir = luaL_checkstring(L, 1);
    int max_frames  = (int)luaL_optinteger(L, 2, 0);
    int frame_skip  = (int)luaL_optinteger(L, 3, 1);
    if (max_frames < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "RecordPNGSequence: max_frames must be >= 0 (0=unlimited)");
        return 2;
    }
    if (frame_skip < 1) {
        lua_pushnil(L);
        lua_pushfstring(L, "RecordPNGSequence: frame_skip must be >= 1 (got %d)", frame_skip);
        return 2;
    }
    g_record.dir_prefix  = dir;
    g_record.frame_count = 0;
    g_record.max_frames  = max_frames;
    g_record.frame_skip  = frame_skip;
    g_record.tick_count  = 0;     // 复位渲染帧计数
    g_record.active      = true;
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.RecordMP4
/// @brief Phase F.0.11.6 — 开始录制 H.264 MP4 (FFmpeg libavcodec/libavformat 动态加载)
/// @param path string  输出 mp4 文件路径
/// @param opts table?  { fps=30, bitrate=5000000, max_frames=0, frame_skip=1 }
///                     fps: 输出帧率 (默认 30)
///                     bitrate: 目标码率 bit/s (0 = CRF 模式, libx264 默认 crf=23)
///                     max_frames: 自动 stop 帧数 (0 = 无限直到 StopRecord)
///                     frame_skip: 渲染帧采样间隔 (1=每帧, 2=隔帧, 默认 1)
/// @return boolean true=成功; nil, string=FFmpeg DLL 缺失 / encoder 不可用 / 文件创建失败
/// @note 不支持与 RecordPNGSequence 并发, 调用前需 StopRecord (若有正在录制).
///       Windows: 需要 lib/ 目录内有 avcodec-59.dll / avformat-59.dll / avutil-57.dll / swscale-9.dll
///       w/h 必须偶数 (libx264 要求); 当前帧来自当前窗口 (Light.UI.Window 大小)
static int l_Graphics_RecordMP4(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    int fps         = 30;
    int64_t bitrate = 5000000;   // 5 Mbps default
    int max_frames  = 0;
    int frame_skip  = 1;
    // Phase F.0.11.6.3.A9: 每次新录屏前复位 ROI (避免上次值残留)
    g_record.roi_x = 0;
    g_record.roi_y = 0;
    g_record.roi_w = 0;
    g_record.roi_h = 0;
    // Phase F.0.11.6.1.A5: 显式 encoder 偏好 (auto / libx264 / h264_nvenc / h264_amf)
    //   prefer_hwenc=false 等价于 encoder="libx264"; 两者并存时 encoder 优先级高.
    const char* encoder_pref = nullptr;
    bool prefer_hwenc = true;   // 默认开 (现有行为)
    // Phase F.0.11.6.2.A8: GOP 大小 (0=默认 fps×2)
    int gop_size = 0;
    // Phase F.0.11.6.5.A13: 启用音频录制 (Windows WASAPI loopback + AAC); 默认 false (兼容)
    bool enable_audio = false;
    if (lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TTABLE) {
        lua_getfield(L, 2, "fps");
        if (lua_isnumber(L, -1)) fps = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "bitrate");
        if (lua_isnumber(L, -1)) bitrate = (int64_t)lua_tonumber(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "max_frames");
        if (lua_isnumber(L, -1)) max_frames = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "frame_skip");
        if (lua_isnumber(L, -1)) frame_skip = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        // A5: encoder + prefer_hwenc
        lua_getfield(L, 2, "encoder");
        if (lua_isstring(L, -1)) encoder_pref = lua_tostring(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, 2, "prefer_hwenc");
        if (lua_isboolean(L, -1)) prefer_hwenc = lua_toboolean(L, -1) != 0;
        lua_pop(L, 1);
        // A8: gop_size (1=全 I 帧, 0/缺省=fps×2)
        lua_getfield(L, 2, "gop_size");
        if (lua_isnumber(L, -1)) gop_size = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
        // Phase F.0.11.6.5.A13: audio 录制 (Windows WASAPI loopback + AAC)
        //   opts.audio = true / false (boolean) — 默认 false
        //   opts.audio = "system" → true 别名 (语义更清晰)
        //   非 Windows / FFmpeg AAC 不可用 → Open 内部静默 disable audio, 仅 video
        lua_getfield(L, 2, "audio");
        if (lua_isboolean(L, -1)) {
            enable_audio = lua_toboolean(L, -1) != 0;
        } else if (lua_isstring(L, -1)) {
            const char* s = lua_tostring(L, -1);
            enable_audio = (s && (strcmp(s, "system") == 0 || strcmp(s, "on") == 0));
        }
        lua_pop(L, 1);
        // Phase F.0.11.6.3.A9: ROI 录屏 — opts.roi = { x, y, w, h } (屏幕左上原点)
        //   缺省 / nil = 不启用 ROI, 录全屏 (兼容旧行为)
        lua_getfield(L, 2, "roi");
        if (lua_type(L, -1) == LUA_TTABLE) {
            lua_getfield(L, -1, "x"); if (lua_isnumber(L,-1)) g_record.roi_x = (int)lua_tointeger(L,-1); lua_pop(L,1);
            lua_getfield(L, -1, "y"); if (lua_isnumber(L,-1)) g_record.roi_y = (int)lua_tointeger(L,-1); lua_pop(L,1);
            lua_getfield(L, -1, "w"); if (lua_isnumber(L,-1)) g_record.roi_w = (int)lua_tointeger(L,-1); lua_pop(L,1);
            lua_getfield(L, -1, "h"); if (lua_isnumber(L,-1)) g_record.roi_h = (int)lua_tointeger(L,-1); lua_pop(L,1);
            // libx264 偶数对齐: w/h 向下取整到偶数
            g_record.roi_w &= ~1;
            g_record.roi_h &= ~1;
        }
        lua_pop(L, 1);   // pop 'roi' table (or non-table value)
    }
    // A5: prefer_hwenc=false 时强制软件编码 (除非用户已显式指定 encoder)
    if (!encoder_pref && !prefer_hwenc) encoder_pref = "libx264";

    if (fps <= 0 || fps > 240) {
        lua_pushnil(L);
        lua_pushfstring(L, "RecordMP4: fps must be in [1, 240], got %d", fps);
        return 2;
    }
    if (max_frames < 0) {
        lua_pushnil(L);
        lua_pushstring(L, "RecordMP4: max_frames must be >= 0 (0=unlimited)");
        return 2;
    }
    if (frame_skip < 1) {
        lua_pushnil(L);
        lua_pushfstring(L, "RecordMP4: frame_skip must be >= 1 (got %d)", frame_skip);
        return 2;
    }
    if (g_record.active) {
        lua_pushnil(L);
        lua_pushstring(L, "RecordMP4: another recording is active, call StopRecord first");
        return 2;
    }

    // 获取窗口尺寸 (与 PNG 录屏一致 — 录 default fb 当前窗口大小)
    if (!g_render) {
        lua_pushnil(L); lua_pushstring(L, "RecordMP4: render backend not ready");
        return 2;
    }
    // 用 backend 当前 viewport 推断 (light_ui RecordTickHook 每帧透传 win_w/win_h, 但 Open 时
    // 还没第一次 tick. 从 PlatformWindow 取尺寸)
    const int w = Light_GetWindowWidth();
    const int h = Light_GetWindowHeight();
    if (w <= 0 || h <= 0) {
        lua_pushnil(L);
        lua_pushfstring(L, "RecordMP4: invalid window size (%dx%d), open window first", w, h);
        return 2;
    }

    // Phase F.0.11.6.3.A9: mp4 尺寸 = ROI 尺寸 (如启用) 或 窗口尺寸 (默认)
    //   ROI 启用时必须越界检查 (Open 时已知窗口尺寸, 早失败比录屏中 silent skip 友好)
    int mp4_w = w, mp4_h = h;
    if (g_record.roi_w > 0 && g_record.roi_h > 0) {
        if (g_record.roi_x < 0 || g_record.roi_y < 0 ||
            g_record.roi_x + g_record.roi_w > w ||
            g_record.roi_y + g_record.roi_h > h) {
            lua_pushnil(L);
            lua_pushfstring(L, "RecordMP4: ROI [%d,%d %dx%d] out of window %dx%d",
                            g_record.roi_x, g_record.roi_y,
                            g_record.roi_w, g_record.roi_h, w, h);
            return 2;
        }
        mp4_w = g_record.roi_w;
        mp4_h = g_record.roi_h;
    }

    if (!RecordMP4::Open(path, mp4_w, mp4_h, fps, bitrate, encoder_pref, gop_size, enable_audio)) {
        lua_pushnil(L);
        lua_pushfstring(L, "RecordMP4: encoder open failed (encoder='%s'; check FFmpeg DLL / GPU driver / file path)",
                        encoder_pref ? encoder_pref : "auto");
        return 2;
    }

    g_record.active      = true;
    g_record.mode        = 1;
    g_record.dir_prefix  = path;
    g_record.frame_count = 0;
    g_record.max_frames  = max_frames;
    g_record.frame_skip  = frame_skip;
    g_record.tick_count  = 0;
    g_record.last_w      = 0;
    g_record.last_h      = 0;

    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.GetRecordMode
/// @return integer 0=PNG sequence (默认), 1=MP4 (Phase F.0.11.6)
static int l_Graphics_GetRecordMode(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)(g_record.active ? g_record.mode : 0));
    return 1;
}

/// @lua_api Light.Graphics.StopRecord
/// @brief Phase F.0.11 — 主动停止录屏; F.0.11.3 async 模式下 flush 最后 1 帧 PBO 防丢
/// @return integer 实际写入帧数 (含 flush 的最后一帧)
/// @note async 模式下, 上次 ReadbackDefaultFBAsync 启动的 readback 数据未取就 stop 会丢 1 帧.
///       F.0.11.3 加 backend->ReadbackAsyncFlushLast() 同步取出 (会 sync block 1 次, ~3-8ms).
static int l_Graphics_StopRecord(lua_State* L) {
    // Phase F.0.11.6: MP4 模式 — flush encoder + 写 trailer + 关闭文件
    // Phase F.0.11.6.1.A2: PBO async 模式下 flush 最后 1 帧 pending PBO 数据 (避免漏最后一帧)
    if (g_record.mode == 1) {
        if (g_record.use_async && g_render &&
            g_record.last_w > 0 && g_record.last_h > 0 &&
            (g_record.max_frames <= 0 || g_record.frame_count < g_record.max_frames)) {
            uint8_t* slot = RecordMP4::AcquireWriteSlot(g_record.frame_count);
            if (slot && g_render->ReadbackAsyncFlushLast(slot)) {
                RecordMP4::CommitWriteSlot();
                ++g_record.frame_count;
            } else if (slot) {
                RecordMP4::CancelWriteSlot();
            }
        }
        RecordMP4::Close();
        int n = g_record.frame_count;
        g_record.active = false;
        g_record.mode = 0;
        g_record.last_w = 0;
        g_record.last_h = 0;
        lua_pushinteger(L, n);
        return 1;
    }

    // F.0.11.3: async 模式下 flush 最后 1 帧 pending PBO
    //   触发条件: 有 g_render, 用 async, 缓存尺寸有效, max_frames 未达上限 (避免写超额 PNG)
    if (g_record.use_async && g_render &&
        g_record.last_w > 0 && g_record.last_h > 0 &&
        (g_record.max_frames <= 0 || g_record.frame_count < g_record.max_frames)) {
        const int w = g_record.last_w;
        const int h = g_record.last_h;
        std::vector<unsigned char> buf((size_t)w * h * 4);
        if (g_render->ReadbackAsyncFlushLast(buf.data())) {
            char path[512];
            std::snprintf(path, sizeof(path), "%sframe_%04d.png",
                          g_record.dir_prefix.c_str(), g_record.frame_count);
            stbi_flip_vertically_on_write(1);
            if (stbi_write_png(path, w, h, 4, buf.data(), w * 4)) {
                ++g_record.frame_count;
            }
        }
    }
    int n = g_record.frame_count;
    g_record.active = false;
    g_record.last_w = 0;
    g_record.last_h = 0;
    lua_pushinteger(L, n);
    return 1;
}

/// @lua_api Light.Graphics.IsRecording
/// @brief Phase F.0.11 — 查询录屏状态
/// @return boolean active, integer frame_count
static int l_Graphics_IsRecording(lua_State* L) {
    lua_pushboolean(L, g_record.active ? 1 : 0);
    lua_pushinteger(L, g_record.frame_count);
    return 2;
}

/// @lua_api Light.Graphics.SetRecordAsync
/// @brief Phase F.0.11.2 — 切换录屏 readback 模式 (sync vs PBO async)
/// @param enabled boolean true=异步 (PBO ping-pong, GPU 不 stall, 录屏延 1 帧);
///                        false=同步 (默认, 与 F.0.11 行为一致)
/// @return boolean true=成功; nil, err=录屏中切换被拒 (需先 StopRecord)
/// @note 性能对比 (1280x720):
///         sync  : glReadPixels 阻塞 ~3-8ms/帧 (GPU stall) + PNG 写盘 ~5-15ms (sync block)
///         async : glReadPixels 异步 ~0ms 阻塞 + PNG 写盘与下一帧渲染并行
/// @note 需 backend 支持 (GL3.3+); 不支持时 RecordTickHook 永远取不到数据 silent fail
static int l_Graphics_SetRecordAsync(lua_State* L) {
    bool enabled = lua_toboolean(L, 1) != 0;
    // 录屏中切换会让 PBO 状态错乱 (sync→async 时 PBO 未启动, async→sync 时丢 1 帧 pending)
    if (g_record.active) {
        lua_pushnil(L);
        lua_pushstring(L, "SetRecordAsync: cannot switch while recording (call StopRecord first)");
        return 2;
    }
    // 切回 sync 时主动释放 PBO (节省 ~7MB)
    if (!enabled && g_record.use_async && g_render) {
        g_render->ReadbackAsyncShutdown();
    }
    g_record.use_async = enabled;
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.IsRecordAsync
/// @brief Phase F.0.11.2 — 查询录屏 readback 是否 async (PBO 模式)
/// @return boolean true=async, false=sync
static int l_Graphics_IsRecordAsync(lua_State* L) {
    lua_pushboolean(L, g_record.use_async ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SetRecordOSD
/// @brief Phase F.0.11.6.1.A7 — 切换录屏指示器 OSD 显示开关
/// @param enabled boolean true=显示左上角红点闪烁 (默认); false=不显示
/// @return boolean true (始终成功)
/// @note 录屏中可随时切换 (无副作用); OSD 不会进 mp4 (绘制时机晚于 readback)
static int l_Graphics_SetRecordOSD(lua_State* L) {
    g_record.show_osd = lua_toboolean(L, 1) != 0;
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.GetRecordOSD
/// @brief Phase F.0.11.6.1.A7 — 查询录屏 OSD 是否启用
/// @return boolean true=显示, false=不显示
static int l_Graphics_GetRecordOSD(lua_State* L) {
    lua_pushboolean(L, g_record.show_osd ? 1 : 0);
    return 1;
}

// ============================================================
// Phase F.0.11.6.2 — A10 Pause / A11 SetRecordMaxSize / A12 GetRecordStats
// ============================================================

/// @lua_api Light.Graphics.PauseRecord
/// @brief Phase F.0.11.6.2.A10 — 暂停录制 (mp4 pts 不前进, PNG 序号不递增)
/// @return boolean true; nil, string=未在录屏
/// @note 暂停期间渲染照常, 但 RecordTickHook 跳过 readback/encode
static int l_Graphics_PauseRecord(lua_State* L) {
    if (!g_record.active) {
        lua_pushnil(L);
        lua_pushstring(L, "PauseRecord: not currently recording");
        return 2;
    }
    if (g_record.mode == 1) RecordMP4::PauseRecord();
    // PNG 模式: 暂停由 RecordTickHook 顶部检查 g_record.paused (复用 RecordMP4::IsPaused 不适用 → 单独 flag)
    // 简化: 仅 mp4 模式生效 (PNG 录屏快速 dump, 用户可直接 StopRecord 替代)
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.ResumeRecord
/// @brief Phase F.0.11.6.2.A10 — 恢复录制 (Pause 配对)
/// @return boolean true; nil, string=未在录屏
static int l_Graphics_ResumeRecord(lua_State* L) {
    if (!g_record.active) {
        lua_pushnil(L);
        lua_pushstring(L, "ResumeRecord: not currently recording");
        return 2;
    }
    if (g_record.mode == 1) RecordMP4::ResumeRecord();
    lua_pushboolean(L, 1);
    return 1;
}

/// @lua_api Light.Graphics.IsRecordPaused
/// @brief Phase F.0.11.6.2.A10 — 查询录屏是否暂停
/// @return boolean true=暂停中, false=正常录制 / 未录屏
static int l_Graphics_IsRecordPaused(lua_State* L) {
    lua_pushboolean(L, (g_record.active && g_record.mode == 1 && RecordMP4::IsPaused()) ? 1 : 0);
    return 1;
}

/// @lua_api Light.Graphics.SetRecordMaxSize
/// @brief Phase F.0.11.6.2.A11 — 设置 mp4 文件大小上限 (字节; 0=无限)
/// @param max_bytes integer 字节数, 0 解除限制
/// @return boolean true (始终成功; 即使未录屏也接受设置, Open 后生效)
/// @note 仅 mp4 模式生效; 超限时不自动停, 主线程脚本需查 GetRecordStats 自行决定 StopRecord
static int l_Graphics_SetRecordMaxSize(lua_State* L) {
    lua_Integer max_bytes = luaL_checkinteger(L, 1);
    if (max_bytes < 0) max_bytes = 0;
    RecordMP4::SetMaxSizeBytes((int64_t)max_bytes);
    lua_pushboolean(L, 1);
    return 1;
}

// ==================== Phase G.1 — VRAM Tracking Lua API ====================
//
// Light.Graphics.GetMemoryStats() -> table
//   返回结构:
//     {
//       total_bytes    = N,
//       render_targets = {count=N, bytes=N},
//       ubos           = {count=N, bytes=N},
//       items          = { {name, format, count, bytes, w, h}, ... }
//     }
//
// 跟踪范围 (v1): 仅高层 wrapper (HDR/TAA/SSR/Dilate/UBO Skin)
//   不含: 用户 Image/ImageData/Mesh/Font 等动态资源
//   未来 v2 扩展时 items 数组自然增加, API 形状不变.
static int l_Graphics_GetMemoryStats(lua_State* L) {
    return LT::GpuMem::PushStats(L);
}

// Light.Graphics.ResetMemoryStats() — 清空跟踪 (smoke / 调试用)
//   注意: 仅清 tracker 数据, 不动实际 GPU 资源
static int l_Graphics_ResetMemoryStats(lua_State* L) {
    (void)L;
    LT::GpuMem::Reset();
    return 0;
}

/// @lua_api Light.Graphics.GetRecordStats
/// @brief Phase F.0.11.6.2.A12 — 查询录屏统计 (帧数 / 字节数 / encoder / paused)
/// @return table { frames=N, bytes=N, max_bytes=N, encoder='...', paused=bool, mode=0|1, active=bool }
///         或 nil, string=未在录屏 / 非 mp4 模式
/// @note PNG 模式仅 mode/active 字段有意义 (encoder/bytes/frames=0)
static int l_Graphics_GetRecordStats(lua_State* L) {
    lua_newtable(L);
    // 通用字段 (PNG + mp4 共用)
    lua_pushinteger(L, g_record.active ? g_record.frame_count : 0);
    lua_setfield(L, -2, "tick_frame_count");   // 主线程已写盘帧数 (PNG 文件序号)
    lua_pushinteger(L, g_record.mode);
    lua_setfield(L, -2, "mode");                // 0=PNG, 1=mp4
    lua_pushboolean(L, g_record.active ? 1 : 0);
    lua_setfield(L, -2, "active");

    // MP4 专属字段 (RecordMP4::GetStats)
    int64_t frames = 0, bytes = 0, max_bytes = 0;
    char    encoder_name[32] = {};
    bool    paused = false;
    if (RecordMP4::GetStats(&frames, &bytes, &max_bytes, encoder_name, (int)sizeof(encoder_name), &paused)) {
        lua_pushinteger(L, (lua_Integer)frames);
        lua_setfield(L, -2, "frames");
        lua_pushinteger(L, (lua_Integer)bytes);
        lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, (lua_Integer)max_bytes);
        lua_setfield(L, -2, "max_bytes");
        lua_pushstring(L, encoder_name);
        lua_setfield(L, -2, "encoder");
        lua_pushboolean(L, paused ? 1 : 0);
        lua_setfield(L, -2, "paused");
    } else {
        // 未在录 mp4: 填默认值 (避免脚本访问 nil 字段)
        lua_pushinteger(L, 0); lua_setfield(L, -2, "frames");
        lua_pushinteger(L, 0); lua_setfield(L, -2, "bytes");
        lua_pushinteger(L, 0); lua_setfield(L, -2, "max_bytes");
        lua_pushstring(L, "");  lua_setfield(L, -2, "encoder");
        lua_pushboolean(L, 0);  lua_setfield(L, -2, "paused");
    }
    // Phase F.0.11.6.5.A13 — audio 字段 (audio_enabled / audio_frames / sample_rate / channels / dropped)
    bool a_enabled = false;
    int64_t a_frames = 0, a_dropped = 0;
    int a_rate = 0, a_channels = 0;
    RecordMP4::GetAudioStats(&a_enabled, &a_frames, &a_rate, &a_channels, &a_dropped);
    lua_pushboolean(L, a_enabled ? 1 : 0);  lua_setfield(L, -2, "audio_enabled");
    lua_pushinteger(L, (lua_Integer)a_frames);   lua_setfield(L, -2, "audio_frames");
    lua_pushinteger(L, (lua_Integer)a_rate);     lua_setfield(L, -2, "audio_sample_rate");
    lua_pushinteger(L, (lua_Integer)a_channels); lua_setfield(L, -2, "audio_channels");
    lua_pushinteger(L, (lua_Integer)a_dropped);  lua_setfield(L, -2, "audio_dropped");
    return 1;
}

/// @lua_api Light.Graphics.ScreenshotHDR
/// @brief Phase F.0.11.4 — 截取当前 active HDR instance 的 sceneTex (RGBA16F) → 写 .hdr
/// @param path string  输出文件路径 (推荐扩展名 .hdr); 编码为 Radiance RGBE 32-bit
/// @param instance_id integer? Phase F.0.11.7 — 可选, 临时切到指定 HDR instance (PIP/split-screen).
///                              缺省 / nil / -1 = 用当前 active instance (F.0 行为, 零回归).
/// @return boolean true=成功; nil, string=HDR 未启用 / readback 失败 / 写盘失败
/// @note 用于美术 HDR 工作流 / 调试 HDR 数据 (Photoshop / Krita 可读 .hdr)
/// @note 读 HDR sceneTex (tonemap 之前的浮点数据), 与 Screenshot 不同 (后者读 LDR default fb)
/// @note 写 3-channel RGB (丢 alpha); stbi_write_hdr 不支持 4-channel
static int l_Graphics_ScreenshotHDR(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    // Phase F.0.11.7 — 可选 instance_id
    const int instanceId = (lua_gettop(L) >= 2 && !lua_isnil(L, 2))
        ? (int)luaL_checkinteger(L, 2) : -1;

    if (!g_render) {
        lua_pushnil(L); lua_pushstring(L, "ScreenshotHDR: render backend not ready");
        return 2;
    }
    if (!HDRRenderer::IsEnabled()) {
        lua_pushnil(L);
        lua_pushstring(L, "ScreenshotHDR: HDR not enabled (call HDR.Enable first)");
        return 2;
    }

    // Phase F.0.11.7 — 临时切 active instance (若指定); 函数末尾复位
    const int savedActive = HDRRenderer::GetActiveInstance();
    if (instanceId >= 0 && instanceId != savedActive) {
        if (!HDRRenderer::SetActiveInstance(instanceId)) {
            lua_pushnil(L);
            lua_pushfstring(L, "ScreenshotHDR: invalid instance_id=%d (not allocated or out of range)",
                            instanceId);
            return 2;
        }
    }

    const uint32_t tex = HDRRenderer::GetSceneTexture();
    const int w = HDRRenderer::GetWidth();
    const int h = HDRRenderer::GetHeight();
    if (!tex || w <= 0 || h <= 0) {
        if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
        lua_pushnil(L);
        lua_pushfstring(L, "ScreenshotHDR: invalid HDR scene (tex=%u, %dx%d, instance=%d)",
                        tex, w, h, HDRRenderer::GetActiveInstance());
        return 2;
    }
    // 1) 读 RGBA float 数据 (4 channels)
    std::vector<float> rgba((size_t)w * h * 4);
    if (!g_render->ReadbackTextureRGBAFloat(tex, w, h, rgba.data())) {
        if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
        lua_pushnil(L);
        lua_pushstring(L, "ScreenshotHDR: ReadbackTextureRGBAFloat failed (backend support?)");
        return 2;
    }
    // 2) 转 RGBA → RGB 紧凑 (stbi_write_hdr 仅支持 3-channel)
    std::vector<float> rgb((size_t)w * h * 3);
    for (int i = 0; i < w * h; ++i) {
        rgb[i * 3 + 0] = rgba[i * 4 + 0];
        rgb[i * 3 + 1] = rgba[i * 4 + 1];
        rgb[i * 3 + 2] = rgba[i * 4 + 2];
    }
    // 3) Y 翻转 + 写 Radiance .hdr
    stbi_flip_vertically_on_write(1);
    const bool ok = stbi_write_hdr(path, w, h, 3, rgb.data()) != 0;

    // Phase F.0.11.7 — 复位 active instance
    if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);

    if (ok) {
        lua_pushboolean(L, 1);
        return 1;
    }
    lua_pushnil(L);
    lua_pushfstring(L, "ScreenshotHDR: stbi_write_hdr failed for '%s'", path);
    return 2;
}

// Phase F.0.11.5 — compression mode 字符串 → tinyexr 枚举
//   "none" / "rle" / "zips" (line-by-line zip) / "zip" (default, block zip) / "piz" (wavelet, default in EXR spec)
static int parse_exr_compression_(const char* s) {
    if (!s) return TINYEXR_COMPRESSIONTYPE_ZIP;
    if (strcmp(s, "none") == 0) return TINYEXR_COMPRESSIONTYPE_NONE;
    if (strcmp(s, "rle")  == 0) return TINYEXR_COMPRESSIONTYPE_RLE;
    if (strcmp(s, "zips") == 0) return TINYEXR_COMPRESSIONTYPE_ZIPS;
    if (strcmp(s, "zip")  == 0) return TINYEXR_COMPRESSIONTYPE_ZIP;
    if (strcmp(s, "piz")  == 0) return TINYEXR_COMPRESSIONTYPE_PIZ;
    return -1;   // 非法
}

/// @lua_api Light.Graphics.ScreenshotEXR
/// @brief Phase F.0.11.5 — 截取当前 active HDR instance 的 sceneTex (RGBA16F) → 写 OpenEXR 文件
/// @param path string  输出路径 (推荐扩展名 .exr)
/// @param opts table?  可选 { bit_depth=16|32 (default 16),
///                            compression="zip"|"none"|"rle"|"zips"|"piz" (default "zip"),
///                            instance_id=integer (Phase F.0.11.7: 临时切到指定 HDR instance) }
/// @return boolean true=成功; nil, string=错误信息
/// @note 与 ScreenshotHDR 区别: EXR 保留 4 channels (含 alpha), 16-bit half 比 RGBE 精度更高;
///        Nuke / DaVinci Resolve / After Effects 等影视后期工具的标准 HDR 输入格式.
static int l_Graphics_ScreenshotEXR(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);

    // 解析可选 options
    int bit_depth = 16;
    int compression = TINYEXR_COMPRESSIONTYPE_ZIP;
    int instanceId = -1;   // Phase F.0.11.7: -1 = 不切, 用 active
    if (lua_gettop(L) >= 2 && lua_type(L, 2) == LUA_TTABLE) {
        lua_getfield(L, 2, "bit_depth");
        if (lua_isnumber(L, -1)) bit_depth = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);

        lua_getfield(L, 2, "compression");
        if (lua_isstring(L, -1)) {
            const int parsed = parse_exr_compression_(lua_tostring(L, -1));
            if (parsed < 0) {
                const char* badStr = lua_tostring(L, -1);
                lua_pop(L, 1);
                lua_pushnil(L);
                lua_pushfstring(L, "ScreenshotEXR: invalid compression mode '%s' (none/rle/zips/zip/piz)",
                                badStr);
                return 2;
            }
            compression = parsed;
        }
        lua_pop(L, 1);

        // Phase F.0.11.7 — instance_id 字段
        lua_getfield(L, 2, "instance_id");
        if (lua_isnumber(L, -1)) instanceId = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }

    if (bit_depth != 16 && bit_depth != 32) {
        lua_pushnil(L);
        lua_pushfstring(L, "ScreenshotEXR: bit_depth must be 16 (half) or 32 (float), got %d", bit_depth);
        return 2;
    }

    if (!g_render) {
        lua_pushnil(L); lua_pushstring(L, "ScreenshotEXR: render backend not ready");
        return 2;
    }
    if (!HDRRenderer::IsEnabled()) {
        lua_pushnil(L);
        lua_pushstring(L, "ScreenshotEXR: HDR not enabled (call HDR.Enable first)");
        return 2;
    }

    // Phase F.0.11.7 — 临时切 active instance (若指定); 函数末尾复位
    const int savedActive = HDRRenderer::GetActiveInstance();
    if (instanceId >= 0 && instanceId != savedActive) {
        if (!HDRRenderer::SetActiveInstance(instanceId)) {
            lua_pushnil(L);
            lua_pushfstring(L, "ScreenshotEXR: invalid instance_id=%d (not allocated or out of range)",
                            instanceId);
            return 2;
        }
    }
    // RAII-风格复位 (任何路径返回前必须复位)
    auto restoreActive = [&]() {
        if (instanceId >= 0 && instanceId != savedActive) HDRRenderer::SetActiveInstance(savedActive);
    };

    const uint32_t tex = HDRRenderer::GetSceneTexture();
    const int w = HDRRenderer::GetWidth();
    const int h = HDRRenderer::GetHeight();
    if (!tex || w <= 0 || h <= 0) {
        restoreActive();
        lua_pushnil(L);
        lua_pushfstring(L, "ScreenshotEXR: invalid HDR scene (tex=%u, %dx%d, instance=%d)",
                        tex, w, h, HDRRenderer::GetActiveInstance());
        return 2;
    }

    // 1) 读 RGBA float (4 channels, OpenGL bottom-left 原点)
    std::vector<float> rgba((size_t)w * h * 4);
    if (!g_render->ReadbackTextureRGBAFloat(tex, w, h, rgba.data())) {
        restoreActive();
        lua_pushnil(L);
        lua_pushstring(L, "ScreenshotEXR: ReadbackTextureRGBAFloat failed (backend support?)");
        return 2;
    }

    // 2) 拆 channel + Y 翻转 (OpenGL bottom-left -> EXR top-left)
    //    EXR channels 内部按字母序: A B G R
    std::vector<float> chR((size_t)w * h);
    std::vector<float> chG((size_t)w * h);
    std::vector<float> chB((size_t)w * h);
    std::vector<float> chA((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const int srcY = h - 1 - y;
        for (int x = 0; x < w; ++x) {
            const size_t dst = (size_t)y * w + x;
            const size_t src = ((size_t)srcY * w + x) * 4;
            chR[dst] = rgba[src + 0];
            chG[dst] = rgba[src + 1];
            chB[dst] = rgba[src + 2];
            chA[dst] = rgba[src + 3];
        }
    }

    // 3) 构造 EXRHeader + EXRImage
    EXRHeader header; InitEXRHeader(&header);
    EXRImage  image;  InitEXRImage(&image);

    image.num_channels = 4;
    image.width  = w;
    image.height = h;
    float* image_ptr[4] = { chA.data(), chB.data(), chG.data(), chR.data() };   // A B G R 字母序
    image.images = (unsigned char**)image_ptr;

    EXRChannelInfo chans[4];
    memset(chans, 0, sizeof(chans));
    // strncpy 防止 channel name 溢出 (tinyexr 内部 buf 256 字节)
    strncpy(chans[0].name, "A", 255);
    strncpy(chans[1].name, "B", 255);
    strncpy(chans[2].name, "G", 255);
    strncpy(chans[3].name, "R", 255);
    header.num_channels = 4;
    header.channels = chans;

    int pixel_types[4];
    int requested_pixel_types[4];
    const int pt_value = (bit_depth == 16) ? TINYEXR_PIXELTYPE_HALF : TINYEXR_PIXELTYPE_FLOAT;
    for (int i = 0; i < 4; ++i) {
        pixel_types[i]           = TINYEXR_PIXELTYPE_FLOAT;   // 源数据是 32-bit float
        requested_pixel_types[i] = pt_value;                  // 目标存储 (half = 16-bit)
    }
    header.pixel_types           = pixel_types;
    header.requested_pixel_types = requested_pixel_types;
    header.compression_type      = compression;

    // 4) 写盘
    const char* err = nullptr;
    const int ret = SaveEXRImageToFile(&image, &header, path, &err);
    if (ret != TINYEXR_SUCCESS) {
        const char* msg = err ? err : "(unknown)";
        restoreActive();   // Phase F.0.11.7: 失败路径也要复位
        lua_pushnil(L);
        lua_pushfstring(L, "ScreenshotEXR: SaveEXRImageToFile failed for '%s': %s (code=%d)",
                        path, msg, ret);
        if (err) FreeEXRErrorMessage(err);
        return 2;
    }

    restoreActive();   // Phase F.0.11.7: 成功路径复位
    lua_pushboolean(L, 1);
    return 1;
}

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
    // Phase F.0.10.2 — viewport 控制 (split-screen 基础)
    {"SetViewport",       l_SetViewport},
    {"GetViewport",       l_GetViewport},
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
    // Phase F.0.11 — Screenshot / PNG Sequence Record (default fb readback)
    {"Screenshot",                 l_Graphics_Screenshot},
    {"ScreenshotRegion",           l_Graphics_ScreenshotRegion},
    {"RecordPNGSequence",          l_Graphics_RecordPNGSequence},
    // Phase F.0.11.6 — H.264 MP4 录屏 (FFmpeg encoder 动态加载)
    {"RecordMP4",                  l_Graphics_RecordMP4},
    // Phase F.0.11.6.4.A14 — GIF 录屏 (复用 RecordMP4 入口, 后缀 .gif 自动切到 gif encoder + BGR8 pix_fmt)
    {"RecordGIF",                  l_Graphics_RecordMP4},
    {"GetRecordMode",              l_Graphics_GetRecordMode},
    {"StopRecord",                 l_Graphics_StopRecord},
    {"IsRecording",                l_Graphics_IsRecording},
    // Phase F.0.11.2 — 异步 readback (PBO ping-pong)
    {"SetRecordAsync",             l_Graphics_SetRecordAsync},
    {"IsRecordAsync",              l_Graphics_IsRecordAsync},
    // Phase F.0.11.6.1.A7 — 录屏指示器 OSD (左上角红点闪烁, 不进 mp4)
    {"SetRecordOSD",               l_Graphics_SetRecordOSD},
    {"GetRecordOSD",               l_Graphics_GetRecordOSD},
    // Phase F.0.11.6.2 — A10/A11/A12 录屏控制扩展
    {"PauseRecord",                l_Graphics_PauseRecord},
    {"ResumeRecord",               l_Graphics_ResumeRecord},
    {"IsRecordPaused",             l_Graphics_IsRecordPaused},
    {"SetRecordMaxSize",           l_Graphics_SetRecordMaxSize},
    {"GetRecordStats",             l_Graphics_GetRecordStats},
    // Phase G.1 — VRAM Tracking
    {"GetMemoryStats",             l_Graphics_GetMemoryStats},
    {"ResetMemoryStats",           l_Graphics_ResetMemoryStats},
    // Phase F.0.11.4 — HDR 截图 (RGBA16F → .hdr Radiance RGBE)
    {"ScreenshotHDR",              l_Graphics_ScreenshotHDR},
    // Phase F.0.11.5 — EXR 截图 (RGBA16F → .exr OpenEXR half/float, 影视后期工业标准)
    {"ScreenshotEXR",              l_Graphics_ScreenshotEXR},
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

        // Phase E.9 — SSR 子表 (Light.Graphics.SSR.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, ssr_funcs, 0);
        lua_setfield(L, -2, "SSR");

        // Phase E.15 — MotionBlur 子表 (Light.Graphics.MotionBlur.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, mb_funcs, 0);
        lua_setfield(L, -2, "MotionBlur");

        // Phase F.0 — TAA 子表 (Light.Graphics.TAA.*)
        lua_createtable(L, 0, 0);
        luaL_setfuncs(L, taa_funcs, 0);
        lua_setfield(L, -2, "TAA");

        lua_rawset(L, -3);
        lua_pushstring(L, "Graphics");
        lua_rawget(L, -2);
    }
    lua_remove(L, -2);
    return 1;
}
