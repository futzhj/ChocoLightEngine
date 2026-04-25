/**
 * @file render_backend.h
 * @brief ChocoLight 渲染后端抽象接口
 * @note 运行时自动检测 GPU 能力, 选择 GL 3.3 Core 或 GL 1.x Legacy 后端
 *       为 Phase 3 (Vulkan/Metal) 预留扩展点
 *
 * 设计原则:
 *   - 接口覆盖 light_graphics.cpp 中所有 GL 调用模式
 *   - RenderVertex 统一顶点格式, 一次提交完整顶点数组
 *   - 变换栈由后端自管理, GL33 用软件矩阵, Legacy 用 glPushMatrix
 */

#pragma once

#include <cstdint>
#include <vector>

// ==================== 顶点结构 ====================

struct RenderVertex {
    float x, y, z;       // 位置
    float u, v;           // 纹理坐标
    float r, g, b, a;    // 颜色
};

// ==================== 绘制模式 ====================

enum class DrawMode {
    Lines,        // GL_LINES
    LineLoop,     // GL_LINE_LOOP
    LineStrip,    // GL_LINE_STRIP
    Triangles,    // GL_TRIANGLES
    TriangleFan,  // GL_TRIANGLE_FAN
    Quads         // GL_QUADS (Legacy) 或拆分为 2 个三角形 (GL33)
};

// ==================== 简易 4x4 矩阵 (列主序) ====================

struct Mat4 {
    float m[16];

    // 单位矩阵
    static Mat4 Identity();
    // 正交投影
    static Mat4 Ortho(float left, float right, float bottom, float top, float near, float far);
    // 平移
    static Mat4 Translate(float x, float y, float z);
    // 绕任意轴旋转 (角度制)
    static Mat4 Rotate(float angleDeg, float ax, float ay, float az);
    // 缩放
    static Mat4 Scale(float sx, float sy, float sz);
    // 矩阵乘法: this * other
    Mat4 operator*(const Mat4& other) const;
};

// ==================== 渲染后端接口 ====================

class RenderBackend {
public:
    virtual ~RenderBackend() = default;

    // ---- 生命周期 ----
    virtual bool Init() = 0;
    virtual void Shutdown() = 0;
    virtual const char* GetName() const = 0;

    // ---- 帧控制 ----
    virtual void BeginFrame(float clearR, float clearG, float clearB, float clearA) = 0;
    virtual void EndFrame() = 0;

    // ---- 状态 ----
    virtual void SetColor(float r, float g, float b, float a) = 0;
    virtual void GetColor(float* r, float* g, float* b, float* a) = 0;
    virtual void SetViewport(int x, int y, int w, int h) = 0;

    // ---- 变换栈 ----
    virtual void PushMatrix() = 0;
    virtual void PopMatrix() = 0;
    virtual void Translate(float x, float y, float z) = 0;
    virtual void Rotate(float angleDeg, float ax, float ay, float az) = 0;
    virtual void Scale(float sx, float sy, float sz) = 0;
    virtual void LoadOrtho(float left, float right, float bottom, float top, float near, float far) = 0;

    // ---- 绘制 ----
    // 提交顶点数组, 由后端决定 GPU 提交方式
    virtual void DrawArrays(DrawMode mode, const RenderVertex* verts, int count) = 0;

    // ---- 纹理 ----
    virtual uint32_t CreateTexture(int w, int h, int channels, const void* pixels) = 0;
    virtual void DeleteTexture(uint32_t texId) = 0;
    virtual void BindTexture(uint32_t texId) = 0;
    virtual void UnbindTexture() = 0;
    virtual void UpdateTexture(uint32_t texId, int x, int y, int w, int h,
                               int channels, const void* pixels) = 0;
    // 整体更新纹理内容 (用于字体图集扩展)
    virtual void ReplaceTexture(uint32_t texId, int w, int h, int channels, const void* pixels) = 0;

    // ---- FBO (Canvas) ----
    virtual uint32_t CreateFBO(int w, int h, uint32_t* outTexture, uint32_t* outDepthRB) = 0;
    virtual void DeleteFBO(uint32_t fbo, uint32_t texture, uint32_t depthRB) = 0;
    virtual void BindFBO(uint32_t fbo) = 0;
    virtual void UnbindFBO() = 0;

    // ---- 裁剪 ----
    virtual void SetScissor(bool enable, int x, int y, int w, int h) = 0;
};

// ==================== 工厂函数 ====================

/// 创建渲染后端: 优先 GL 3.3, 回退 GL 1.x
/// 必须在 PlatformWindow 创建窗口和 GL context 之后调用
RenderBackend* CreateRenderBackend();

/// 全局渲染后端实例 (在 light_ui.cpp Window.Open 中初始化)
extern RenderBackend* g_render;
