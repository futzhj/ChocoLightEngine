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

/// Phase AS.2 — 3D mesh 顶点格式 (12 floats = 48 bytes)
/// 与 RenderVertex 平行, 不破坏现有 2D 渲染管线
struct RenderVertex3D {
    float x, y, z;       // pos
    float nx, ny, nz;    // normal (单位向量)
    float u, v;          // uv
    float r, g, b, a;    // color (顶点颜色, 与材质相乘)
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
    /// Phase AS.2 — 透视投影 (fovY 角度制, near/far > 0)
    static Mat4 Perspective(float fovYDeg, float aspect, float n, float f);
    /// Phase AS.2 — LookAt 视图矩阵 (eye/target/up 世界空间)
    static Mat4 LookAt(float ex, float ey, float ez,
                       float tx, float ty, float tz,
                       float ux, float uy, float uz);
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

    /// Phase A3 新增: 索引绘制 (Triangles 拓扑专用)
    /// @param verts 顶点数组
    /// @param vertexCount 顶点数 (uint16 索引上限 65536, 即 16384 quad)
    /// @param indices 索引数组 (uint16, 必须是 3 的倍数)
    /// @param indexCount 索引数
    /// @param textureId 当前批次绑定的纹理 (0 = 纯色)
    /// @note 默认实现退化为展开顶点 + DrawArrays(Triangles), 后端可重载为 glDrawElements
    virtual void DrawIndexed(const RenderVertex* verts, int vertexCount,
                             const uint16_t* indices, int indexCount,
                             uint32_t textureId) {
        if (!verts || !indices || indexCount <= 0) return;
        // 默认实现: 按索引展开三角形顶点, 走 DrawArrays
        std::vector<RenderVertex> expanded;
        expanded.reserve(indexCount);
        for (int i = 0; i < indexCount; ++i) {
            uint16_t idx = indices[i];
            if (idx >= (uint16_t)vertexCount) continue; // 安全跳过越界
            expanded.push_back(verts[idx]);
        }
        if (textureId) BindTexture(textureId);
        DrawArrays(DrawMode::Triangles, expanded.data(), (int)expanded.size());
        if (textureId) UnbindTexture();
    }

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

    // ---- 用户 Shader (GL33 支持, Legacy 默认不支持) ----
    virtual bool SupportsShaders() const { return false; }
    /// 编译链接顶点+片段着色器, 返回 shader program ID (0 = 失败)
    virtual uint32_t CreateShader(const char* vertexSrc, const char* fragmentSrc,
                                  char* errLog, int errLogSize) { return 0; }
    virtual void DeleteShader(uint32_t shaderId) {}
    /// 激活用户 shader, false = 切回默认 shader
    virtual bool UseShader(uint32_t shaderId) { return false; }
    virtual void UseDefaultShader() {}
    virtual int  GetUniformLocation(uint32_t shaderId, const char* name) { return -1; }
    // Uniform 设置 (作用于当前激活的 shader)
    virtual void SetUniform1f(int loc, float v) {}
    virtual void SetUniform2f(int loc, float x, float y) {}
    virtual void SetUniform3f(int loc, float x, float y, float z) {}
    virtual void SetUniform4f(int loc, float x, float y, float z, float w) {}
    virtual void SetUniform1i(int loc, int v) {}
    virtual void SetUniformMat4(int loc, const float* m) {}

    // ---- Phase AS.1 新增 uniform setter ----
    /// 3x3 矩阵 (法线变换等)
    virtual void SetUniformMat3(int loc, const float* m) {}
    /// int 向量 (2/3/4 分量, IVec2/3/4)
    virtual void SetUniform2i(int loc, int x, int y) {}
    virtual void SetUniform3i(int loc, int x, int y, int z) {}
    virtual void SetUniform4i(int loc, int x, int y, int z, int w) {}
    /// 数组 (count 个元素)
    virtual void SetUniform1fv(int loc, int count, const float* v) {}
    virtual void SetUniform2fv(int loc, int count, const float* v) {}
    /// 绑定 sampler2D: 把 texId 绑到 slot (GL_TEXTURE0+slot), 设 uniform location 为 slot
    virtual void SetUniformSampler(int loc, int slot, uint32_t texId) {}
    /// 生成纹理的 mipmap 链 (Canvas:GetTextureId 后可选用于优化采样)
    virtual void GenerateMipmap(uint32_t texId) {}
    /// 清空当前绑定的 FBO/默认目标 (Canvas:Clear 用)
    virtual void ClearCurrent(float r, float g, float b, float a) {}

    // ---- Phase AS.2 新增 3D mesh 接口 (GL33 真实现, Legacy 默认 no-op) ----
    /// 是否支持 3D mesh + 深度测试 + perspective 投影
    virtual bool Supports3D() const { return false; }
    /// 创建 mesh (上传顶点+索引到 GPU), 返回 mesh id (0=失败)
    virtual uint32_t CreateMesh(const RenderVertex3D* verts, int vCount,
                                const uint32_t* indices, int iCount) { return 0; }
    /// 释放 mesh GPU 资源
    virtual void DeleteMesh(uint32_t meshId) {}
    /// 绘制 mesh (使用当前 shader/projection/view, textureId=0 时无纹理)
    virtual void DrawMesh(uint32_t meshId, uint32_t textureId) {}
    /// 启用/禁用深度测试 (默认禁用以兼容 2D)
    virtual void SetDepthTest(bool enable) {}
    /// 设置深度比较函数 (0=Less, 1=LEqual, 2=Greater, 3=GEqual, 4=Equal, 5=NotEqual, 6=Always, 7=Never)
    virtual void SetDepthFunc(int func) {}
    /// 加载视图矩阵 (与 modelview 不同, 由 LookAt 生成)
    virtual void LoadView(const float* viewMat4) {}
    /// 加载投影矩阵 (替代 LoadOrtho 的通用版, 用于 Perspective)
    virtual void LoadProjection(const float* projMat4) {}
};

// ==================== 工厂函数 ====================

/// 创建渲染后端: 优先 GL 3.3, 回退 GL 1.x
/// 必须在 PlatformWindow 创建窗口和 GL context 之后调用
RenderBackend* CreateRenderBackend();

/// 全局渲染后端实例 (在 light_ui.cpp Window.Open 中初始化)
extern RenderBackend* g_render;
