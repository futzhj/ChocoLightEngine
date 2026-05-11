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
     * @brief Phase E.2.3 — 批量 Lit2D 绘制 (支持索引 EBO, 为 LitBatchRenderer 服务)
     * @param verts          RenderVertex2DLit 数组 (CPU 端已烘焙 transform)
     * @param vertCount      顶点数 (必须 > 0, 每 4 顶点对应 1 quad)
     * @param indices        uint32 索引数组 (必须 6 的倍数, 描述 N 个 quad 的三角形索引)
     * @param idxCount       索引数 (= quadCount * 6)
     * @param baseColorTex   baseColor 纹理 (0 = 纯色)
     * @param normalMapTex   法线贴图 (0 = 平面法线)
     *
     * 实现要求:
     *   1. glUseProgram(programLit2D) + glBindVertexArray(vaoLit2D)
     *   2. glBufferData(VBO, verts * sizeof(RenderVertex2DLit))
     *   3. glBufferData(EBO, indices * sizeof(uint32_t))  (单 batch 内动态索引)
     *   4. MVP/Model uniform + texture bind
     *   5. UploadLighting2D(Lighting2D::GetState())  (E.2.1 dirty bit 护航, 未变则 no-op)
     *   6. glDrawElements(GL_TRIANGLES, idxCount, GL_UNSIGNED_INT, 0)
     *   7. 切回默认 2D shader
     *
     * 默认实现: no-op (与其它 Lit2D 虚接口一致, 非 Lit2D 后端静默跳过).
     */
    virtual void DrawLit2DBatch(const RenderVertex2DLit* verts, int vertCount,
                                 const uint32_t* indices, int idxCount,
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

    // ==================== Phase E.3: HDR + Tonemapping ====================

    /**
     * @brief HDR 渲染能力检测
     *
     * GL33Core (RGBA16F + Depth24 + fragment shader) → true
     * Legacy GL / 不支持浮点 RT 的后端 → false
     *
     * 调用方 (HDRRenderer::Init) 应在 false 时返回 false 让 LDR 路径继续使用.
     */
    virtual bool SupportsHDR() const { return false; }

    /**
     * @brief 创建 HDR FBO (RGBA16F 颜色附件 + Depth24 RBO)
     *
     * 与现有 CreateFBO (RGBA8 + Depth16) 不同:
     *  - 颜色: GL_RGBA16F 内部格式, GL_FLOAT 元素 (可存 HDR 值 > 1.0)
     *  - 深度: GL_DEPTH_COMPONENT24 (替代 16, 给 3D 留精度)
     *  - filter: GL_LINEAR (HDR RT 必须可线性采样, tonemap pass 需要)
     *  - wrap: GL_CLAMP_TO_EDGE
     *
     * @param[in]  w        RT 宽度 (像素)
     * @param[in]  h        RT 高度 (像素)
     * @param[out] outTex   返回创建的 RGBA16F 颜色纹理 id
     * @return     fbo id (0 = 失败, 通常是后端不支持或 OOM)
     *
     * Depth RBO 内部由后端管理 (与 DeleteHDRFBO 配对释放).
     * 默认实现 (Legacy): return 0.
     */
    virtual uint32_t CreateHDRFBO(int /*w*/, int /*h*/, uint32_t* /*outTex*/) { return 0; }

    /**
     * @brief 释放 HDR FBO 资源
     *
     * 释放 fbo + 颜色纹理 + 内部管理的 depth RBO.
     * 与 CreateHDRFBO 配对调用.
     *
     * 默认实现 (Legacy): no-op.
     */
    virtual void DeleteHDRFBO(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /**
     * @brief 用 ACES tonemap shader 把 HDR 纹理全屏 blit 到当前绑定的 framebuffer
     *
     * 典型用法 (主循环 EndScene):
     *   1. backend->UnbindFBO()            (切到 default framebuffer)
     *   2. backend->DrawTonemapFullscreen(hdrTex, exposure, gamma)
     *   3. 接着 SwapBuffers
     *
     * shader 内部完成:
     *   1. hdr = texture(uHDRTex, vUV).rgb * uExposure
     *   2. ldr = ACESFilm(hdr)   (Narkowicz 2-pass fitted)
     *   3. srgb = pow(ldr, 1/uGamma)
     *   4. fragColor = vec4(srgb, 1.0)
     *
     * 实现细节:
     *   - glDisable(GL_DEPTH_TEST) / GL_BLEND  (tonemap 是 destructive write)
     *   - 6 顶点全屏 quad (无 EBO, GL_TRIANGLES)
     *   - 调用结束前不恢复 depth/blend state (下次 BeginFrame 重置)
     *
     * 默认实现 (Legacy): no-op.
     *
     * @param hdrTex     HDR 颜色纹理 (来自 CreateHDRFBO 返回的 *outTex)
     * @param exposure   HDR 输入预乘的曝光值 (默认 1.0)
     * @param gamma      sRGB encode 的 gamma 值 (默认 2.2)
     * @param tonemapMode Phase E.3.4 — tonemap operator 选择
     *                   0=ACES (Narkowicz fitted, 默认)
     *                   1=Reinhard (x/(1+x), 简单基线)
     *                   2=Uncharted2 (Hable filmic, 含 white scale)
     *                   3=Linear (clamp 0..1, 无 tonemap, 调试用)
     */
    virtual void DrawTonemapFullscreen(uint32_t /*hdrTex*/,
                                        float /*exposure*/,
                                        float /*gamma*/,
                                        int   /*tonemapMode*/ = 0) {}

    // ==================== Phase E.4 — Bloom 后处理 ====================

    /**
     * @brief 是否支持 Bloom 后处理 (要求: HDR RGBA16F + 多 FBO pyramid)
     *
     * 默认实现 (Legacy): 返回 false, 所有 Bloom API no-op.
     */
    virtual bool SupportsBloom() const { return false; }

    /**
     * @brief 创建 Bloom RT pyramid (各级 RGBA16F, 无 depth)
     *
     * pyramid[0] 与 HDR RT 同大小; 每级 /2 直到 levels-1 或 1x1.
     * 失败时 outFbos/outTexs 未写入的位置保持为 0.
     *
     * @param w        顶级 (level 0) 宽, 通常 = HDR RT 宽
     * @param h        顶级 (level 0) 高
     * @param levels   期望层数 (2..8)
     * @param outFbos  [out] FBO id 数组, 容量 >= levels
     * @param outTexs  [out] texture id 数组, 容量 >= levels
     * @return         实际成功创建的层数; 0 = 失败 (outFbos/outTexs 未动)
     */
    virtual int CreateBloomPyramid(int /*w*/, int /*h*/, int /*levels*/,
                                    uint32_t* /*outFbos*/, uint32_t* /*outTexs*/) { return 0; }

    /**
     * @brief 释放 Bloom RT pyramid 资源
     */
    virtual void DeleteBloomPyramid(uint32_t* /*fbos*/, uint32_t* /*texs*/, int /*levels*/) {}

    /**
     * @brief Bright Pass: HDR RT → Bloom pyramid[0]
     *
     * shader 内: luminance = dot(c, rec709), soft knee 过渡, 输出 c * contribution.
     *
     * @param sceneTex  HDR RT 纹理 id (输入)
     * @param outFbo    Bloom pyramid[0] 的 FBO id (目标)
     * @param w, h      目标 RT 大小
     * @param threshold 亮度阈值 (L > threshold 时保留)
     */
    virtual void DrawBloomBrightPass(uint32_t /*sceneTex*/, uint32_t /*outFbo*/,
                                      int /*w*/, int /*h*/, float /*threshold*/) {}

    /**
     * @brief Downsample: srcTex → dstFbo (13-tap COD AW filter)
     *
     * 典型: pyramid[i-1].tex → pyramid[i].fbo, dst 大小 /2.
     */
    virtual void DrawBloomDownsample(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                      int /*dstW*/, int /*dstH*/) {}

    /**
     * @brief Upsample + additive blend: srcTex → dstFbo (tent 3x3 filter)
     *
     * 调用方应事先启用 GL blend (ONE, ONE) 让结果累加到 dstFbo 已有内容.
     * radius 控制 UV 偏移 (越大 glow 越宽, 过大会失真).
     */
    virtual void DrawBloomUpsample(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                    int /*dstW*/, int /*dstH*/, float /*radius*/) {}

    /**
     * @brief Final composite: bloomTex additive blend → hdrFbo (intensity scaled)
     *
     * 复用 DrawBloomUpsample 的 shader, 以 radius=0 退化为 0-offset 采样,
     * intensity 通过 uniform 传入. 调用方应启用 GL blend (ONE, ONE).
     */
    virtual void DrawBloomComposite(uint32_t /*bloomTex*/, uint32_t /*hdrFbo*/,
                                     int /*w*/, int /*h*/, float /*intensity*/) {}

    // ==================== Phase E.5 — Auto Exposure (Eye Adaptation) ====================

    /**
     * @brief 是否支持 Auto Exposure (要求: R16F mipmap-able RT + glReadPixels)
     *
     * 默认实现 (Legacy): 返回 false, 所有 AE API no-op.
     */
    virtual bool SupportsAutoExposure() const { return false; }

    /**
     * @brief 创建 luminance RT (单色 R16F, mipmap-able)
     *
     * 内部按 srcW/4, srcH/4 创建 (减少 fragment 工作量); 如果太小则向上 clamp 到 8x8.
     * texture min filter 设为 LINEAR_MIPMAP_LINEAR, 以让 GenerateLuminanceMipmap 自动 reduce.
     *
     * @param srcW, srcH    源 HDR RT 尺寸 (如 1920x1080)
     * @param outFbo, outTex  [out] FBO 与单色 R16F tex id
     * @param outW, outH    [out] 实际创建尺寸
     * @return true = 成功; false = 不支持 / 资源失败
     */
    virtual bool CreateLuminanceTarget(int /*srcW*/, int /*srcH*/,
                                        uint32_t* /*outFbo*/, uint32_t* /*outTex*/,
                                        int* /*outW*/, int* /*outH*/) { return false; }

    /**
     * @brief 释放 luminance RT 资源
     */
    virtual void DeleteLuminanceTarget(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /**
     * @brief Pass 1: hdrTex 全屏 quad → lumFbo (log luminance, R16F)
     *
     * shader 内:
     *   luma     = dot(rgb, vec3(0.2126, 0.7152, 0.0722))   // Rec.709
     *   logLuma  = log(max(luma, 0.0001))                    // 防 underflow
     *   fragColor.r = clamp(logLuma, -12.0, 12.0)            // R16F precision guard
     *
     * @param hdrTex  源 HDR RT 颜色 tex
     * @param lumFbo  目标 luminance RT FBO
     * @param w, h    lumFbo 尺寸 (设 viewport 用)
     */
    virtual void DrawLuminanceExtract(uint32_t /*hdrTex*/,
                                       uint32_t /*lumFbo*/,
                                       int /*w*/, int /*h*/) {}

    /**
     * @brief 调用 glGenerateMipmap 让 GPU 自动算 luminance tex 的 mipmap 链
     *
     * 最后一层 (1x1) 即为全图平均 log luminance.
     * 调用前 lumTex 必须 R16F 且 GL_TEXTURE_MIN_FILTER 设为 LINEAR_MIPMAP_LINEAR.
     */
    virtual void GenerateLuminanceMipmap(uint32_t /*lumTex*/) {}

    /**
     * @brief 同步读 luminance RT 最后一层 mip (1x1 R16F) 到 CPU
     *
     * v1 实现: glReadPixels (吃 1 frame stall, ~10us, 2 bytes/frame).
     * v2 优化: PBO double-buffer 异步 readback (TODO).
     *
     * @param lumFbo        luminance RT FBO
     * @param lastMipLevel  最后一层 mip 等级 (log2(maxDim))
     * @return float        log luminance 值; 失败 / 不支持时返 0.0
     */
    virtual float ReadbackLuminance1x1(uint32_t /*lumFbo*/, int /*lastMipLevel*/) { return 0.0f; }

    // ==================== Phase E.6 — Lens Dirt ====================

    /**
     * @brief 是否支持 Lens Dirt (shader 编译成功)
     * 默认实现 (Legacy): 返回 false, 所有 LensDirt API no-op.
     */
    virtual bool SupportsLensDirt() const { return false; }

    /**
     * @brief Lens dirt 合成: hdrFbo += bloomTex x dirtTex x intensity
     *
     * shader:
     *   bloom = texture(uBloomTex, vUV).rgb
     *   dirt  = texture(uDirtTex,  vUV).rgb
     *   out   = bloom * dirt * intensity
     * 调用方内部会 enable GL_BLEND (ONE, ONE), 结束后 disable.
     *
     * @param bloomTex   Bloom pyramid[0] 颜色 tex (输入)
     * @param dirtTex    用户 dirt 纹理; 0 = 后端内部 1x1 白纹理 fallback
     * @param hdrFbo     HDR RT FBO id (目标, additive 写入)
     * @param w, h       hdrFbo 尺寸 (viewport)
     * @param intensity  合成强度 (>= 0)
     */
    virtual void DrawLensDirtComposite(uint32_t /*bloomTex*/, uint32_t /*dirtTex*/,
                                        uint32_t /*hdrFbo*/,
                                        int /*w*/, int /*h*/, float /*intensity*/) {}

    // ==================== Phase E.6 — Streak (Anamorphic Flare) ====================

    /**
     * @brief 是否支持 Streak (shader 编译成功 + RGBA16F FBO 可用)
     */
    virtual bool SupportsStreak() const { return false; }

    /**
     * @brief 创建 streak ping-pong RT 对 (2 x RGBA16F + FBO, 同尺寸)
     *
     * 内部按 srcW/2, srcH/2 创建 (节省 fragment); 下限 32x32.
     *
     * @param srcW, srcH   源 HDR RT 尺寸
     * @param outFbos[2]   [out] 两个 FBO id
     * @param outTexs[2]   [out] 两个 tex id
     * @param outW, outH   [out] 实际创建尺寸
     * @return             true 成功; false 失败 (会清理已分配资源)
     */
    virtual bool CreateStreakTargets(int /*srcW*/, int /*srcH*/,
                                      uint32_t* /*outFbos*/,   // uint32_t[2]
                                      uint32_t* /*outTexs*/,   // uint32_t[2]
                                      int* /*outW*/, int* /*outH*/) { return false; }

    virtual void DeleteStreakTargets(uint32_t* /*fbos*/, uint32_t* /*texs*/) {}

    /**
     * @brief Streak bright pass: hdrTex -> outFbo (亮度阈值提取)
     *
     * v1 实现复用 Bloom programBloomBright (相同算法 + soft knee).
     */
    virtual void DrawStreakBright(uint32_t /*hdrTex*/, uint32_t /*outFbo*/,
                                   int /*w*/, int /*h*/, float /*threshold*/) {}

    /**
     * @brief 1D 方向模糊: srcTex -> dstFbo (7-tap 方向高斯)
     *
     * shader 内 normalize(direction). 步长 = direction * length.
     * 不启用 blend (直接覆盖写 dst).
     *
     * @param srcTex      输入 streak RT 的一侧
     * @param dstFbo      输出 streak RT 的另一侧 (ping-pong)
     * @param w, h        dst 尺寸
     * @param length      单步 UV 距离
     * @param dirX, dirY  方向向量 (shader 内 normalize)
     */
    virtual void DrawStreakBlur(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                 int /*w*/, int /*h*/,
                                 float /*length*/,
                                 float /*dirX*/, float /*dirY*/) {}

    /**
     * @brief 加性合成: streakTex x intensity -> hdrFbo
     *
     * 调用方内部 enable GL_BLEND (ONE, ONE), 结束后 disable.
     */
    virtual void DrawStreakComposite(uint32_t /*streakTex*/, uint32_t /*hdrFbo*/,
                                      int /*w*/, int /*h*/, float /*intensity*/) {}

    // ==================== Phase E.7 — Lens Flare (Ghost + Halo + Chromatic Aberration) ====================
    //
    // 算法: 屏幕空间 ghost-halo 镜头光晕模拟
    //   1. Bright pass  : 复用 DrawBloomBrightPass (相同 threshold + soft knee)
    //   2. Ghost + Halo : 新 shader 单次 fragment (lens flare ghost)
    //                    - Ghost: 朝画面中心反投 N 个不同尺寸的光圈 (uGhostCount, uGhostDispersal)
    //                    - Halo : 沿径向方向偏移采样形成环状光晕 (uHaloWidth)
    //                    - Chromatic Aberration: RGB 分量分别径向偏移采样 (uChromaticAberration)
    //   3. Composite    : 复用 DrawBloomComposite (additive)
    //
    // RT: ping-pong 对 (RGBA16F, srcW/2 x srcH/2, min 32x32)
    //   - tex[0]: bright pass 输出 (filtered)
    //   - tex[1]: ghost+halo 输出 (送 composite)
    //
    // Legacy 后端: 全部 no-op (SupportsLensFlare = false)

    /**
     * @brief 是否支持 Lens Flare
     *   要求: tonemap 可用 + Bloom bright/composite 可用 + lens flare ghost shader 编译成功
     *   默认实现 (Legacy): false
     */
    virtual bool SupportsLensFlare() const { return false; }

    /**
     * @brief 创建 lens flare ping-pong RT 对 (2 x RGBA16F + FBO, 同尺寸)
     *
     * 内部按 srcW/2, srcH/2 创建 (节省 fragment); 下限 32x32.
     *
     * @param srcW, srcH   HDR RT 输入尺寸
     * @param outFbos      [out] uint32_t[2] FBO id (2 ping-pong)
     * @param outTexs      [out] uint32_t[2] 颜色 tex id
     * @param outW, outH   [out] 实际创建尺寸
     * @return             true 成功; false 失败 (会清理已分配资源)
     */
    virtual bool CreateLensFlareTargets(int /*srcW*/, int /*srcH*/,
                                         uint32_t* /*outFbos*/,   // [2]
                                         uint32_t* /*outTexs*/,   // [2]
                                         int* /*outW*/, int* /*outH*/) { return false; }

    virtual void DeleteLensFlareTargets(uint32_t* /*fbos*/, uint32_t* /*texs*/) {}

    /**
     * @brief Ghost + Halo + Chromatic Aberration: brightTex -> dstFbo (覆盖写, 不开 blend)
     *
     * 算法详见类内注释; shader 静态循环 8 上限 + 早退 (GLES3 兼容).
     *
     * @param brightTex            输入 (bright pass 输出)
     * @param flareTex             Phase E.7.4 — 用户贴图 GL tex id (0 = 用 1x1 白 fallback)
     * @param dstFbo               输出 FBO (lens flare ping-pong 第 2 张)
     * @param w, h                 dst RT 尺寸
     * @param ghostCount           [0, 8] 整数; 0 = 不生成 ghost
     * @param ghostDispersal       [0, 2.0] 径向缩放
     * @param haloWidth            [0, 1.0] halo 环形半径 (UV 空间)
     * @param chromaticAberration  [0, 0.02] RGB 分量径向偏移
     * @param distortionEnabled    bool 是否启用色差 (false = RGB 同采)
     */
    virtual void DrawLensFlareGhost(uint32_t /*brightTex*/, uint32_t /*flareTex*/, uint32_t /*dstFbo*/,
                                     int /*w*/, int /*h*/,
                                     int /*ghostCount*/,
                                     float /*ghostDispersal*/,
                                     float /*haloWidth*/,
                                     float /*chromaticAberration*/,
                                     bool  /*distortionEnabled*/) {}

    // 注: bright pass 直接调用 DrawBloomBrightPass(...) 复用 Bloom 算法
    // 注: 最终 composite 直接调用 DrawBloomComposite(...) 复用 Bloom 加性合成
};

// ==================== 工厂函数 ====================

/// 创建渲染后端: 优先 GL 3.3, 回退 GL 1.x
/// 必须在 PlatformWindow 创建窗口和 GL context 之后调用
RenderBackend* CreateRenderBackend();

/// 全局渲染后端实例 (在 light_ui.cpp Window.Open 中初始化)
extern RenderBackend* g_render;
