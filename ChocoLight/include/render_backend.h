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

// Phase E.1.5 — 前向声明 Lighting2D::State, 避免把 light_lighting2d.h 拉进所有使用 backend 的翻译单元
namespace Lighting2D { struct State; }

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

/// Phase AW — GPU Skinning 顶点格式 (17 floats + 1 uint32 = 68 bytes)
/// joints_packed: 4 个 uint8 关节索引按小端打包成 uint32
///   byte0=joints[0], byte1=joints[1], byte2=joints[2], byte3=joints[3]
/// 通过 glVertexAttribIPointer(loc=4, 4, GL_UNSIGNED_BYTE, ...) 暴露给 shader 为 uvec4
struct RenderVertex3DSkin {
    float    x, y, z;        // POSITION (location=0)
    float    nx, ny, nz;     // NORMAL (location=1)
    float    u, v;           // UV (location=2)
    float    r, g, b, a;     // COLOR (location=3)
    uint32_t joints_packed;  // JOINTS_0 (location=4, uvec4 via IPointer)
    float    weights[4];     // WEIGHTS_0 (location=5, vec4)
};
static_assert(sizeof(RenderVertex3DSkin) == 68,
              "RenderVertex3DSkin must be 68 bytes: pos(12)+normal(12)+uv(8)+color(16)+joints(4)+weights(16)");

/// Phase E.1 — 2D Lit 顶点格式 (16 floats = 64 bytes)
/// 用于 BatchRenderer 之外的 forward multi-light 路径 (含可选 normal map)
/// 与 RenderVertex 平行: 默认 sprite 仍走 RenderVertex (9 floats), 仅启用 Light.Graphics.DrawLit
/// 或 ECS LitSprite component 的 sprite 走本格式
///
/// 顶点属性 layout (与 VS_LIT2D 一致):
///   location 0: aPos     vec3 (x,y,z)        -- 世界坐标 (z 通常为 0 / depth)
///   location 1: aUV      vec2 (u,v)
///   location 2: aColor   vec4 (r,g,b,a)
///   location 3: aNormal  vec3 (nx,ny,nz)     -- 默认 (0,0,1) 平面 sprite
///   location 4: aTangent vec4 (tx,ty,tz,tw)  -- xyz=tangent, w=bitangent sign (默认 1,0,0,1)
struct RenderVertex2DLit {
    float x, y, z;            // 位置 (12)
    float u, v;               // UV  (8)
    float r, g, b, a;         // 顶点色 (16)
    float nx, ny, nz;         // 法线 (12) — normal map 启用时 shader 内会用 TBN 重算
    float tx, ty, tz, tw;     // 切线 + bitangent sign (16)
};
static_assert(sizeof(RenderVertex2DLit) == 64,
              "RenderVertex2DLit must be 64 bytes: pos(12)+uv(8)+color(16)+normal(12)+tangent(16)");

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

// ==================== Phase AS.4 — 材质系统数据结构 ====================

/// Material 描述符 (POD; light_graphics_material.cpp 维护, RenderBackend 使用)
struct MaterialDesc {
    int   mode;                    // 0=Unlit, 1=PBR
    float color[4];                // baseColor (default 1,1,1,1)
    float emissive[3];             // default 0,0,0
    float metallic;                // default 0
    float roughness;               // default 1
    float normalScale;             // normal map 强度, default 1
    float occlusionStrength;       // AO 贴图权重, default 1
    uint32_t texBaseColor;         // 0 = 无
    uint32_t texMetallicRoughness; // 0 = 无 (G=roughness, B=metallic per glTF spec)
    uint32_t texNormal;            // 0 = 无
    uint32_t texEmissive;          // 0 = 无
    uint32_t texOcclusion;         // 0 = 无
    int   alphaMode;               // 0=opaque, 1=blend, 2=mask
    float alphaCutoff;             // 仅 mask 时有效, default 0.5
    int   doubleSided;             // 0/1
};

/// 单个点光描述
struct PointLight {
    float pos[3];
    float color[3];
    float range;       // 衰减半径 (世界坐标距离)
    float intensity;   // 强度 multiplier
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

    // ---- Phase AS.4 新增材质系统 (GL33 真实现, Legacy 默认 no-op) ----
    /// 用 material 描述符绘制 mesh, 替代 DrawMesh(meshId, textureId)
    virtual void DrawMeshMaterial(uint32_t meshId, const MaterialDesc* desc) {}
    /// 设置主方向光 (世界坐标方向, 已归一化指向光源)
    virtual void SetDirectionalLight(const float* dir, const float* color, float intensity, bool enabled) {}
    /// 设置环境光 (单色 ambient)
    virtual void SetAmbientLight(const float* rgb) {}
    /// 设置摄像机世界坐标位置 (PBR view direction 计算需要)
    virtual void SetCameraPos(const float* pos) {}
    /// 添加点光, 返回 id (1..MaxPoint), 0=已满
    virtual int  AddPointLight(const PointLight* light) { return 0; }
    virtual void RemovePointLight(int id) {}
    virtual void ClearPointLights() {}
    virtual int  GetPointLightCount() const { return 0; }
    virtual int  GetMaxPointLights() const { return 0; }

    // ---- Phase AW 新增 GPU Skinning 接口 (GL33 实现, Legacy 默认 no-op) ----
    /**
     * @brief 当前后端是否支持 GPU skinning
     *
     * 判定标准 (GL33Backend Init 期间检测并缓存):
     *   GL_MAX_UNIFORM_BLOCK_SIZE   >= 4096 bytes (= 64 mat4)
     *   GL_MAX_VERTEX_UNIFORM_BLOCKS >= 1
     *   programUnlitSkin / programPBRSkin 至少一个 link 成功
     *
     * LegacyBackend 默认返回 false (不重写)。
     */
    virtual bool SupportsGPUSkinning() const { return false; }

    /**
     * @brief 创建 GPU skinned mesh (一次上传, 永不重传)
     *
     * @param verts   含 pos/normal/uv/color/joints/weights 的顶点数组
     * @param vCount  顶点数 (> 0)
     * @param indices uint32 三角形索引
     * @param iCount  索引数 (必须为 3 的倍数)
     *
     * @return meshId (高位 0x80000000 区分普通 mesh ID); 失败返回 0
     *
     * 失败条件: !verts / vCount<=0 / !indices / iCount<=0 / !SupportsGPUSkinning()
     */
    virtual uint32_t CreateSkinnedMesh(const RenderVertex3DSkin* verts, int vCount,
                                        const uint32_t* indices, int iCount) { return 0; }

    /**
     * @brief 用 jointMatrices 调色板渲染 GPU skinned mesh
     *
     * @param meshId        必须是 CreateSkinnedMesh 返回的 ID
     * @param desc          MaterialDesc (mode = 0 unlit / 1 PBR)
     * @param jointMatrices 16 floats × jointCount, 列主序
     * @param jointCount    实际关节数 (≤ 64; 超过将被截断到 64)
     *
     * 内部行为:
     *   1. glUseProgram(programUnlitSkin or programPBRSkin)
     *   2. glBindBuffer + glBufferSubData 上传 jointMatrices 到 UBO
     *   3. 上传 MVP/Model/Material/Lighting uniforms (复用 helper)
     *   4. glBindVertexArray + glDrawElements
     *   5. 切回默认 2D shader
     */
    virtual void DrawSkinnedMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                          const float* jointMatrices, int jointCount) {}

    // ---- Phase AX 新增 GPU Morph Target 接口 (GL33 实现, Legacy 默认 no-op) ----
    /**
     * @brief 是否支持 GPU morph target 渲染
     *
     * 仅在以下条件全部满足时返回 true:
     *   - GL 3.3+ context (texture 2D + glUniform1fv 数组支持)
     *   - SupportsGPUSkinning() == true (morph 总是与 skin 共存)
     *   - VS3D_SKIN_MORPH program 编译/链接成功
     *
     * LegacyBackend 默认返回 false (不重写).
     * 调用方应在 false 时回退到 CPU 路径 (DrawSkinnedMorphMeshCPU).
     */
    virtual bool SupportsMorphTargets() const { return false; }

    /**
     * @brief 创建 GPU skinned + morph mesh (一次上传, 永不重传)
     *
     * @param verts             含 pos/normal/uv/color/joints/weights 的顶点数组 (与 CreateSkinnedMesh 相同)
     * @param vCount            顶点数 (> 0)
     * @param indices           uint32 三角形索引
     * @param iCount            索引数 (3 的倍数)
     * @param posDeltas         vCount × 3 × morphTargetCount floats (POSITION delta)
     * @param nrmDeltas         vCount × 3 × morphTargetCount floats (NORMAL delta), 可 nullptr
     * @param morphTargetCount  morph target 数量 (1..MORPH_TARGET_MAX=8)
     *
     * @return meshId; 失败返回 0
     *
     * 内部: 创建 VAO+VBO+EBO (与 skin 一致) + 创建 RGB32F 2D texture
     *       (width=vCount, height=morphTargetCount) 上传 morph delta.
     */
    virtual uint32_t CreateSkinnedMorphMesh(const RenderVertex3DSkin* verts, int vCount,
                                              const uint32_t* indices, int iCount,
                                              const float* posDeltas,
                                              const float* nrmDeltas,
                                              int morphTargetCount) { return 0; }

    /**
     * @brief 用 jointMatrices + morphWeights 调色板渲染 GPU skinned+morph mesh
     *
     * @param meshId            CreateSkinnedMorphMesh 返回的 ID
     * @param desc              MaterialDesc (mode = 0 unlit / 1 PBR)
     * @param jointMatrices     16 floats × jointCount, 列主序
     * @param jointCount        实际关节数 (≤ 64)
     * @param morphWeights      morphTargetCount 个 float (与 CreateSkinnedMorphMesh 一致)
     * @param morphTargetCount  morph target 数量 (≤ MORPH_TARGET_MAX = 8)
     */
    virtual void DrawSkinnedMorphMeshMaterial(uint32_t meshId, const MaterialDesc* desc,
                                                 const float* jointMatrices, int jointCount,
                                                 const float* morphWeights, int morphTargetCount) {}

    // ---- Phase E.1 — 2D Lit (forward 多光 + Normal Map) ----
    /**
     * @brief 是否支持 2D Lit 渲染路径
     *
     * 判定标准 (GL33Backend Init 期间检测并缓存):
     *   - VAO/VBO/EBO 创建成功 (E.1.1)
     *   - programLit2D link 成功 (E.1.2 接入)
     *
     * Legacy / GL ES 2.0 等不支持 forward multi-light 的后端默认返回 false,
     * 调用方应在 false 时回退到非 Lit 路径 (普通 Draw / DrawQuad).
     */
    virtual bool SupportsLit2D() const { return false; }

    /**
     * @brief 创建 2D Lit mesh (一次上传, 永不重传; 与 CreateMesh 平行)
     *
     * @param verts        含 pos/uv/color/normal/tangent 的顶点数组
     * @param vCount       顶点数 (> 0)
     * @param indices      uint32 三角形索引
     * @param iCount       索引数 (必须为 3 的倍数)
     *
     * @return meshId; 失败返回 0
     *
     * 默认实现返回 0 (Legacy 后端无 Lit2D 支持)。
     * GL33Backend 在 E.1.5 后实现真实上传逻辑。
     */
    virtual uint32_t CreateLit2DMesh(const RenderVertex2DLit* verts, int vCount,
                                       const uint32_t* indices, int iCount) { return 0; }

    /// 释放 Lit2D mesh GPU 资源
    virtual void DeleteLit2DMesh(uint32_t meshId) {}

    /**
     * @brief 用单个 quad 顶点 + 可选 normal map 绘制 Lit2D sprite
     *
     * @param verts          4 个顶点 (顺序: 左下, 右下, 右上, 左上)
     * @param baseColorTex   baseColor 纹理 (0 = 纯色 sprite, 仅顶点色)
     * @param normalMapTex   法线贴图 (0 = 用默认 N=(0,0,1) 平面光照)
     *
     * 内部行为 (GL33, E.1.5 实现):
     *   1. glUseProgram(programLit2D)
     *   2. 绑定 texture (slot 0=base, slot 1=normal)
     *   3. 上传 MVP/Model/HasNormalMap uniforms
     *   4. 上传 Lighting2D state uniforms (调用 Lighting2D::UploadToShader)
     *   5. glBindVertexArray(vaoLit2D) + glBufferSubData(4 verts) + glDrawElements
     *   6. 切回默认 2D shader (与 DrawMeshMaterial 一致)
     */
    virtual void DrawLit2DQuad(const RenderVertex2DLit verts[4],
                                uint32_t baseColorTex,
                                uint32_t normalMapTex) {}

    /**
     * @brief 用任意三角形顶点流 + 可选 normal map 绘制 Lit2D 几何
     *
     * @param verts          顶点数组 (count 必须为 3 的倍数)
     * @param count          顶点数
     * @param baseColorTex   baseColor 纹理 (0 = 纯色)
     * @param normalMapTex   法线贴图 (0 = 平面法线)
     */
    virtual void DrawLit2DTriangles(const RenderVertex2DLit* verts, int count,
                                      uint32_t baseColorTex,
                                      uint32_t normalMapTex) {}

    /**
     * @brief 上传 Lighting2D 状态到 programLit2D 的 uniform 数组
     *
     * 调用点: 由 Lighting2D::UploadToShader 转发; 也可被 DrawLit2DQuad 内部复用.
     *
     * 为什么不拆 SOA: state 中 Light 是 POD且包成 16-slot 密集数组，后端可在
     * 本调用内一次性 build 起 SOA 临时数组 (高逻辑性能趋同接口可读性).
     *
     * 默认实现: no-op (Legacy / 不支持 Lit2D 的后端会被 Lit2DSupported() 干掉调用路径).
     * GL33Backend 在 E.1.5 里拆出 SOA + glUniform*v 一次上传全部 16 位灯.
     */
    virtual void UploadLighting2D(const Lighting2D::State* state) {}
};

// ==================== 工厂函数 ====================

/// 创建渲染后端: 优先 GL 3.3, 回退 GL 1.x
/// 必须在 PlatformWindow 创建窗口和 GL context 之后调用
RenderBackend* CreateRenderBackend();

/// 全局渲染后端实例 (在 light_ui.cpp Window.Open 中初始化)
extern RenderBackend* g_render;
