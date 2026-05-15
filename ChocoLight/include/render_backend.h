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

// ==================== Phase E.14 — Velocity Buffer Format ====================

/// Phase E.14 — HDR FBO velocity attachment 的存储格式
/// RG16F（8MB/1080p）默认；RG8（2MB/1080p）可选低精度，需 shader 配合 uVelocityScale 编码
enum class VelocityFormat : uint8_t {
    RG16F = 0,   // 默认: 16-bit float, 直接存 currentUV - prevUV
    RG8   = 1    // 可选: 8-bit UNORM, [-uVelocityScale, +uVelocityScale] 编码为 [0, 1]
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
    virtual void ResetVelocityHistory() {}
    virtual void CommitVelocityHistory() {}

    // ---- Phase E.14 — Velocity dilation 与编码 scale ----
    /// dilation 开关 (默认 ON：SSRTemporal shader 会用 3x3 max-length 邻域)
    virtual void  SetVelocityDilation(bool /*enabled*/) {}
    virtual bool  GetVelocityDilation() const { return true; }
    /// RG8 模式下 shader 编/解码 velocity 用的尺度因子
    /// raw_velocity = clamp(uv_delta / (2*scale) + 0.5, 0, 1)
    /// decoded = (raw - 0.5) * 2 * scale
    /// 返回 0.25 = ±0.25 UV / frame, 1080p 下±540 pixels, 足以覆盖 >90% 实际运动
    virtual float GetVelocityScale() const { return 0.25f; }
    /// 当前 HDR FBO 的 velocity 存储格式 (SSRTemporal draw 时供 shader 选解码路径)
    /// HDRRenderer 同时只会有 1 张 HDR FBO，由最近一次 CreateHDRFBO 设定。
    virtual VelocityFormat GetActiveVelocityFormat() const { return VelocityFormat::RG16F; }

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
    virtual void SetNextPreviousModelMatrix(const float* prevModelMat4) {}
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
                                          const float* jointMatrices, int jointCount,
                                          const float* prevJointMatrices = nullptr,
                                          int prevJointCount = 0) {}

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
                                                 const float* morphWeights, int morphTargetCount,
                                                 const float* prevJointMatrices = nullptr,
                                                 int prevJointCount = 0,
                                                 const float* prevMorphWeights = nullptr,
                                                 int prevMorphTargetCount = 0) {}

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
     *
     * Phase E.8.x 升级: 可选 outNormalTex 参数启用 MRT (G-buffer view-space normal).
     *  - outNormalTex == nullptr: 仅创建单 RT (Phase E.3 原行为)
     *  - outNormalTex != nullptr: 创建 MRT (RGBA16F color + RG16F view-normal),
     *                              glDrawBuffers(2, [COLOR_0, COLOR_1])
     * normalTex 同 depthRB 一样由后端内部 map 关联管理, DeleteHDRFBO 释放.
     * 创建失败时 (RG16F 不支持 / FBO incomplete) 返回 0, 已分配的 GL 对象全释放.
     *
     * 默认实现 (Legacy): return 0.
     */
    /**
     * Phase E.16 扩展: 可选 outCameraVelocityTex 参数启用双 velocity MRT
     *  - outCameraVelocityTex == nullptr: 根本不创建第二张 velocity tex (Phase E.15 原行为)
     *  - outCameraVelocityTex != nullptr: 创建 camera-only velocity tex 于
     *                                       COLOR_ATTACHMENT3, 与 outVelocityTex 同格式.
     *                                       3D shader FS 同时写 FragVelocity (slot 2 combined)
     *                                       + FragCameraVelocity (slot 3 camera-only).
     *                                       MotionBlurRenderer mode=1/2 时读取.
     */
    virtual uint32_t CreateHDRFBO(int /*w*/, int /*h*/,
                                   uint32_t* /*outColorTex*/,
                                   uint32_t* /*outNormalTex*/ = nullptr,
                                   uint32_t* /*outVelocityTex*/ = nullptr,
                                   VelocityFormat /*velocityFormat*/ = VelocityFormat::RG16F,
                                   uint32_t* /*outCameraVelocityTex*/ = nullptr) { return 0; }

    /**
     * @brief 释放 HDR FBO 资源
     *
     * 释放 fbo + 颜色纹理 + 内部管理的 depth RBO + (Phase E.8.x) normalTex.
     * 与 CreateHDRFBO 配对调用.
     *
     * 默认实现 (Legacy): no-op.
     */
    virtual void DeleteHDRFBO(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /**
     * @brief 取 HDR FBO 关联的 G-buffer view-space normal 纹理 id (Phase E.8.x)
     *
     * @param fbo 由 CreateHDRFBO 返回的 fbo id
     * @return    fbo 关联的 RG16F view-space normal tex id
     *            (0 = 该 fbo 不带 MRT, 或 fbo 已释放, 或后端不支持)
     *
     * 设计意图:
     *   解耦 SSAORenderer 与 HDR 内部资源管理. SSAO 只持有 hdrFbo,
     *   不需要也不应该感知 normalTex 的 lifetime.
     *
     * 默认实现 (Legacy / 未启用 MRT): return 0.
     */
    virtual uint32_t GetHDRNormalTex(uint32_t /*fbo*/) const { return 0; }
    virtual uint32_t GetHDRVelocityTex(uint32_t /*fbo*/) const { return 0; }
    /// Phase E.16 — 查询 HDR FBO 关联的 camera-only velocity tex (slot 3)
    /// 返 0 表示 HDR FBO 创建时未请求第二张、后端不支持、或 fbo 已释放.
    /// MotionBlurRenderer mode=1 (camera) / mode=2 (object) 时读。
    virtual uint32_t GetHDRCameraVelocityTex(uint32_t /*fbo*/) const { return 0; }

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

    // ==================== Phase E.8 — SSAO (Screen-Space Ambient Occlusion) ====================
    //
    // 双 RT 旁路方案 (用户选择 2026-05-12):
    //   HDR RT 保持不变 (renderbuffer depth 原封); SSAO 分配独立 depth tex + 小 FBO,
    //   每帧 Process 入口用 glBlitFramebuffer 从 HDR FBO 复制 depth 到 SSAO FBO.
    //
    // 管线 (blurEnabled=true 时):
    //   0. BlitHDRDepthToSSAO(hdrFbo, depthFbo, w, h)       — 旁路 depth 复制
    //   1. DrawSSAO(depthTex, noiseTex, fbos[0], ...)       — raw AO (R16F, 1/2 res)
    //   2. DrawSSAOBlur(texs[0], depthTex, fbos[1], axis=0) — 水平 bilateral blur
    //   3. DrawSSAOBlur(texs[1], depthTex, fbos[0], axis=1) — 垂直 bilateral blur
    //   4. DrawSSAOComposite(texs[0], hdrFbo, srcW, srcH, intensity) — HDR *= mix(1, ao, intensity)
    //
    // Legacy backend 永 no-op (SupportsSSAO = false).

    /// 是否支持 SSAO (shader 编译 + HDR/Bloom + blit probe 全通过)
    virtual bool SupportsSSAO() const { return false; }

    /// Phase E.8 双 RT 旁路: 创建 SSAO 专用 depth tex + FBO (无 color attachment)
    /// @param w,h      与 HDR RT 同尺寸 (full-res blit 目标)
    /// @param outFbo   输出 FBO (仅 GL_DEPTH_ATTACHMENT, glDrawBuffer=GL_NONE)
    /// @param outTex   输出 depth texture (NEAREST + CLAMP_TO_EDGE)
    /// @return true=成功; 失败时 outFbo/outTex = 0
    virtual bool CreateSSAODepthRT(int /*w*/, int /*h*/,
                                    uint32_t* /*outFbo*/, uint32_t* /*outTex*/) { return false; }
    virtual void DeleteSSAODepthRT(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /// Phase E.8 旁路核心: 用 glBlitFramebuffer 从 HDR FBO 复制 depth 到 SSAO FBO
    /// @note 在每帧 SSAORenderer::Process() 入口调用；GL_DEPTH_BUFFER_BIT + GL_NEAREST
    ///       HDR RT 的 depth renderbuffer -> SSAO 专用 depth texture
    virtual void BlitHDRDepthToSSAO(uint32_t /*hdrFbo*/, uint32_t /*ssaoDepthFbo*/,
                                     int /*w*/, int /*h*/) {}

    /// 创建 SSAO AO ping-pong RT:
    ///   [0] raw AO    (R16F, 半分辨率)
    ///   [1] blur temp (R16F, 半分辨率)
    /// @param outW,outH 实际 RT 尺寸 (半分辨率, 最小 32x32 clamp)
    /// @return true=成功; 失败时 fbos/texs 清零
    virtual bool CreateSSAOTargets(int /*w*/, int /*h*/,
                                    uint32_t* /*fbos*/, uint32_t* /*texs*/,
                                    int* /*outW*/, int* /*outH*/) { return false; }
    virtual void DeleteSSAOTargets(uint32_t* /*fbos*/, uint32_t* /*texs*/) {}

    /// 创建 4x4 RGBA8 noise texture (REPEAT wrap + NEAREST filter)
    /// 每像素 RGB = 归一化随机 (x, y, 0) 向量 (z=0 保证半球采样切空间不出界)
    virtual uint32_t CreateSSAONoiseTex() { return 0; }
    virtual void     DeleteSSAONoiseTex(uint32_t /*tex*/) {}

    /// SSAO raw pass: depthTex + normalTex -> dstFbo (R16F, 半分辨率)
    /// @param normalTex  Phase E.8.x G-buffer view-space normal tex (取代 ddx/ddy 重建)
    /// @param kernel     vec3[kernelSize] CPU 侧预生成半球采样方向 (tangent space)
    /// @param kernelSize 8 或 16
    virtual void DrawSSAO(uint32_t /*depthTex*/, uint32_t /*noiseTex*/,
                          uint32_t /*normalTex*/,
                          uint32_t /*dstFbo*/,
                          int /*w*/, int /*h*/,
                          const float* /*projMat4*/, const float* /*invProjMat4*/,
                          const float* /*kernel*/, int /*kernelSize*/,
                          float /*radius*/, float /*bias*/, float /*power*/) {}

    /// 双边分离滤波: srcAOTex + depthTex -> dstFbo
    /// @param axis 0=水平; 1=垂直 (depth-aware 权重 exp(-|cDepth - sampDepth| * 200))
    virtual void DrawSSAOBlur(uint32_t /*srcAOTex*/, uint32_t /*depthTex*/, uint32_t /*dstFbo*/,
                              int /*w*/, int /*h*/, int /*axis*/) {}

    /// Composite: HDR *= mix(1.0, aoTex.r, intensity) -> 覆盖写 HDR RT
    /// @note 读 HDR 写 HDR 的 feedback loop 由内部临时 tex 中转规避
    virtual void DrawSSAOComposite(uint32_t /*aoTex*/, uint32_t /*dstFbo*/,
                                    int /*w*/, int /*h*/,
                                    float /*intensity*/) {}

    /// Phase E.8 辅助: 获取当前 projection / view 矩阵 (SSAO 重建 view pos 需要)
    /// @param out16 输出 mat4 (列主序 16 floats)
    virtual void GetProjection(float* /*out16*/) const {}
    virtual void GetView(float* /*out16*/) const {}

    /// Phase F.0.13 — 返回相机帧间运动幅度标量 (用于 motion-adaptive sharpness)
    /// 实现策略: Frobenius distance of viewProj(t) vs viewProj(t-1)
    /// 返回值: [0, +∞), 0 = 静止, ~1 = 中等速度, >2 = 高速
    /// 老 backend 默认返 0 (motion-adaptive sharpness 静默失效, 零回归)
    /// 首帧或 hasPrevViewProj=false 时也返 0 (避免误判为高速)
    virtual float ComputeCameraMotionScalar() const { return 0.0f; }

    // ==================== Phase E.9 — SSR (Screen Space Reflection) ====================
    //
    // 屏幕空间反射: linear ray march in view space.
    // 复用 Phase E.8.x 的 G-buffer view-space normal MRT (GetHDRNormalTex)
    // + 独立 depth RT 旁路 (与 SSAO 平行设计, 复用 BlitHDRDepthToSSAO 接口语义).
    //
    // 管线 (Phase E.12 完整路径):
    //   0. BlitHDRDepthToSSAO(hdrFbo, ssrDepthFbo, w, h)        — 旁路 depth 复制
    //                                                            (接口名带 SSAO 仅历史命名,
    //                                                             语义=HDR depth blit 到任意 dst,
    //                                                             SSR/SSAO 都可调用)
    //   1. DrawSSR(depthTex, normalTex, hdrTex, ssrFbo, ...)    — raw 反射 (RGBA16F, full-res, 可带 jitter)
    //   2. DrawSSRTemporal(cur, history, depth, velocity, ...)  — optional temporal accumulation
    //   3. DrawSSRBlur(src, depth, ...)                         — optional Gaussian / Bilateral blur
    //   4. DrawSSRComposite(reflectTex, hdrFbo, ..., intensity) — HDR += reflect.rgb * reflect.a * intensity
    //                                                            (内部用 temp RT 解 feedback loop)
    //
    // Legacy backend 永 no-op (SupportsSSR = false).
    // 高质量方案 (用户拍板 2026-05-12): full-res RGBA16F + 64 步默认 (clamp [8, 128]).

    /// 是否支持 SSR (shader 编译 + RGBA16F + view-space normal MRT 全可用)
    virtual bool SupportsSSR() const { return false; }

    /// Phase E.9 旁路: 创建 SSR 专用 depth tex + FBO (无 color attachment, full-res)
    /// @note 与 SSAO 的 CreateSSAODepthRT 平行; SSR 不复用 SSAO 资源以保持模块独立
    /// @param w,h    与 HDR RT 同尺寸
    /// @param outFbo 输出 FBO (仅 GL_DEPTH_ATTACHMENT, glDrawBuffer=GL_NONE)
    /// @param outTex 输出 depth texture (NEAREST + CLAMP_TO_EDGE)
    /// @return true=成功; 失败时 outFbo/outTex = 0
    virtual bool CreateSSRDepthRT(int /*w*/, int /*h*/,
                                   uint32_t* /*outFbo*/, uint32_t* /*outTex*/) { return false; }
    virtual void DeleteSSRDepthRT(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /// 创建 SSR 反射 RT: full-res RGBA16F + GL_LINEAR + GL_CLAMP_TO_EDGE, 无 depth
    /// @return true=成功; 失败时 outFbo/outTex = 0
    virtual bool CreateSSRTargets(int /*w*/, int /*h*/,
                                   uint32_t* /*outFbo*/, uint32_t* /*outTex*/) { return false; }
    virtual void DeleteSSRTargets(uint32_t* /*fbo*/, uint32_t* /*tex*/) {}

    /// SSR raw pass: 反射结果写入 dstFbo (RGBA16F)
    /// @param depthTex   HDR depth tex (full-res, 已 blit)
    /// @param normalTex  HDR G-buffer view-space normal (Phase E.8.x)
    /// @param hdrTex     HDR color tex (反射采样源)
    /// @param dstFbo     SSR 反射 RT (本 backend 创建的 ssrFbo)
    /// @param w,h        full-res
    /// @param projMat4 / invProjMat4  column-major, 16 floats each
    /// @param maxSteps   ray march 步数 [8, 128]
    /// @param stepSize   每步 view-space units
    /// @param thickness  深度命中容差 (view-space units)
    /// @param maxDist    距离上限
    /// @param edgeFade   屏幕边缘 UV 空间 fade 宽度 [0, 0.5]
    /// @param jitterX,jitterY  Phase E.12: ray march 起点像素单位偏移 (±0.5 pixel 范围)
    ///                  TemporalEnabled=false 时调用方传 0.0 即旧行为
    virtual void DrawSSR(uint32_t /*depthTex*/, uint32_t /*normalTex*/, uint32_t /*hdrTex*/,
                         uint32_t /*dstFbo*/,
                         int /*w*/, int /*h*/,
                         const float* /*projMat4*/, const float* /*invProjMat4*/,
                         int /*maxSteps*/, float /*stepSize*/, float /*thickness*/,
                         float /*maxDist*/, float /*edgeFade*/,
                         float /*jitterX*/, float /*jitterY*/) {}

    /// SSR composite: hdrFbo += reflectTex.rgb * reflectTex.a * intensity
    /// 后端内部用临时 RT 解 feedback loop (HDR 既读又写).
    virtual void DrawSSRComposite(uint32_t /*reflectTex*/, uint32_t /*hdrFbo*/,
                                   int /*w*/, int /*h*/, float /*intensity*/) {}

    // ==================== Phase E.10 — SSR Blur (反射模糊, 粗糙度模拟) ====================
    //
    // 用户拍板 (2026-05-12): half-res ping-pong RGBA16F + separable Gaussian 5-tap.
    //   1080p full-res reflect RT ~8 MB,  blur ping-pong ~2 MB × 2 = ~4 MB 额外.
    //   反射 upscale 由 composite 阶段硬件 bilinear filter 自动处理.
    //
    // Process 中插入位置: DrawSSR 之后, DrawSSRComposite 之前.
    // 仅当 SSRRenderer.blurEnabled=true 且 blur RT 分配成功才执行.

    /// 创建 half-res blur ping-pong RT (RGBA16F × 2)
    /// @param wFull, hFull   full-res 输入 (内部自动 max(1, w/2))
    /// @param outFbos[2]     输出 FBO 数组 (axis=0 dst + axis=1 dst)
    /// @param outTexs[2]     输出 tex 数组 (GL_LINEAR + GL_CLAMP_TO_EDGE)
    /// @param outW, outH     实际 half-res 尺寸 (外部读取, 用于 viewport / uTexel)
    /// @return true=成功, 失败时所有 out* 清零
    virtual bool CreateSSRBlurRT(int /*wFull*/, int /*hFull*/,
                                  uint32_t* /*outFbos*/, uint32_t* /*outTexs*/,
                                  int* /*outW*/, int* /*outH*/) { return false; }
    virtual void DeleteSSRBlurRT(uint32_t* /*fbos*/, uint32_t* /*texs*/) {}

    /// separable blur pass: srcTex -> dstFbo  (Phase E.11 Bilateral 升级)
    ///
    /// Phase E.10: 纯 Gaussian 5-tap
    /// Phase E.11: 单 shader 双模式, runtime 通过 bilateralEnabled 切换
    ///   - false: Phase E.10 Gaussian 路径（向后兼容）
    ///   - true:  Bilateral 路径, 用 depthTex 计算跨深度边权重衰减
    ///
    /// @param srcTex           源 tex (full-res reflect 或 half-res blur 中间, 由 caller 控制)
    /// @param depthTex         SSR depth tex (full-res, NEAREST; Phase E.11 bilateral 必需;
    ///                         bilateralEnabled=false 时仍建议传有效值以避免 driver 差异)
    /// @param dstFbo           目标 FBO (half-res blurFbos[axis])
    /// @param dstW, dstH       目标 RT 尺寸 (uTexel = 1/dstSize)
    /// @param axis             0=horizontal, 1=vertical
    /// @param radius           texel 半径乘子 [0.5, 4.0]
    /// @param bilateralEnabled Phase E.11: true=Bilateral, false=Gaussian (Phase E.10 行为)
    /// @param depthSigma       Phase E.11: bilateral 深度权重 σ [50, 500]
    ///                         (bilateralEnabled=false 时 ignored)
    virtual void DrawSSRBlur(uint32_t /*srcTex*/, uint32_t /*depthTex*/,
                              uint32_t /*dstFbo*/, int /*dstW*/, int /*dstH*/,
                              int /*axis*/, float /*radius*/,
                              bool /*bilateralEnabled*/, float /*depthSigma*/) {}

    // ==================== Phase E.12 — Temporal SSR (时序累积降噪) ====================
    //
    // 用户拍板 (2026-05-14): full-res RGBA16F history × 2 ping-pong + Halton-2,3 8-sample
    // jitter + neighborhood clip rejection. 业界标准 TAA-style temporal SSR.
    //
    // 管线插入位置: DrawSSR -> DrawSSRTemporal -> DrawSSRBlur -> DrawSSRComposite
    // 仅当 SSRRenderer.temporalEnabled=true 且 history RT 分配成功才执行.
    //
    // Reprojection 算法:
    //   - 有 velocityTex: prevUV = vUV - velocityTex.rg
    //   - 无 velocityTex: 当前帧像素 (vUV, depth) -> NDC -> uReprojectMat -> prevUV
    //   - uReprojectMat = prevViewProj * invCurViewProj (CPU 预乘, fallback 路径)
    //   - 越界 / 首帧 -> 强制 cur (不混合 history)
    //   - rejectionMode=0: current-depth threshold; rejectionMode=1: 9-tap neighborhood AABB clip

    /// 创建 SSR temporal history ping-pong RT (full-res RGBA16F × 2)
    /// @note 与 reflectTex 同尺寸; 永远 full-res (不复用 blur 的 half-res)
    /// @param w, h        full-res 尺寸
    /// @param outFbos[2]  输出 FBO 数组 (仅 color attachment, 无 depth)
    /// @param outTexs[2]  输出 tex 数组 (GL_LINEAR + GL_CLAMP_TO_EDGE)
    /// @return true=成功, 失败时 outFbos/outTexs 全清零
    virtual bool CreateSSRHistoryRT(int /*w*/, int /*h*/,
                                     uint32_t* /*outFbos2*/,
                                     uint32_t* /*outTexs2*/) { return false; }
    virtual void DeleteSSRHistoryRT(uint32_t* /*fbos2*/, uint32_t* /*texs2*/) {}

    /// Temporal pass: reproject + reject + blend
    /// 输入: 当前帧 SSR raw + 上一帧 SSR temporal + depth + 可选 velocity
    /// 输出: dstFbo (即下一帧的 history read 源)
    ///
    /// @param curReflectTex  当前帧 SSR raw (slot 0)
    /// @param historyTex     上一帧 SSR temporal 输出 (slot 1)
    /// @param depthTex       SSR depth tex (slot 2, full-res, NEAREST)
    /// @param velocityTex    HDR velocity tex (slot 3, RG16F; 0 时使用矩阵 fallback)
    /// @param dstFbo         目标 FBO (history ping-pong write 目标)
    /// @param w, h           full-res 尺寸
    /// @param reprojectMat4  prevViewProj * invCurViewProj (column-major 16 floats, CPU 预乘)
    /// @param invProjMat4    invProj (重建 view pos 备用, 当前 shader 内可不用)
    /// @param blendAlpha     history 权重 [0.5, 0.99]
    /// @param rejectionMode  0 = current-depth threshold, 1 = neighborhood AABB clip
    /// @param hasHistory     0 = 首帧禁用 temporal (输出=cur), 1 = 正常累积
    virtual void DrawSSRTemporal(uint32_t /*curReflectTex*/,
                                  uint32_t /*historyTex*/,
                                  uint32_t /*depthTex*/,
                                  uint32_t /*velocityTex*/,
                                  uint32_t /*dstFbo*/,
                                  int /*w*/, int /*h*/,
                                  const float* /*reprojectMat4*/,
                                  const float* /*invProjMat4*/,
                                  float /*blendAlpha*/,
                                  int   /*rejectionMode*/,
                                  int   /*hasHistory*/,
                                  bool           /*velocityDilation*/ = true,
                                  float          /*velocityScale*/    = 0.25f,
                                  VelocityFormat /*velocityFormat*/   = VelocityFormat::RG16F) {}

    // ==================== Phase E.15 — Motion Blur 虚接口 ====================
    // 设计 (与 Bloom/SSR 同模式):
    //   1. SupportsMotionBlur()  — shader 编译成功才返 true (Legacy 永 false)
    //   2. CreateMotionBlurRT()  — 独立 RGBA16F ping-pong RT (与 sceneTex 同尺寸)
    //   3. DeleteMotionBlurRT()  — 配对释放
    //   4. DrawMotionBlur()      — 2 pass: ① 采 sceneTex+velocityTex 写 motionBlurTex
    //                                       ② blit motionBlurTex 覆盖 sceneTex
    //
    // velocity format / scale / dilation 由 backend 内部从 Phase E.14 状态字段取
    // (SetVelocityDilation/GetActiveVelocityFormat/GetVelocityScale)，不增加参数。

    /// 后端是否支持 motion blur (GL33 = shader 编译成功才 true)
    virtual bool SupportsMotionBlur() const { return false; }

    /// 创建 motion blur ping-pong RT (RGBA16F, color-only, 无 depth)
    /// @param  w, h               逻辑尺寸 (与 HDR sceneTex 同)
    /// @param  outTex             返回 GL tex id (失败为 0)
    /// @param  storageW, storageH Phase E.17 — RT 实际分配尺寸 (0 = 沿用 w/h)
    ///                            half-res 下为 (w+1)/2, (h+1)/2
    /// @return GL fbo id (失败为 0); 失败时 outTex 也保证为 0
    virtual uint32_t CreateMotionBlurRT(int /*w*/, int /*h*/, uint32_t* outTex,
                                         int /*storageW*/ = 0,    // ★ Phase E.17
                                         int /*storageH*/ = 0) {  // ★ Phase E.17
        if (outTex) *outTex = 0;
        return 0;
    }

    /// 释放 motion blur RT (与 CreateMotionBlurRT 配对)
    virtual void DeleteMotionBlurRT(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /// 执行 motion blur 完整 2-pass 流程
    ///   Pass1 (shader): bind motionBlurFbo → 沿 velocity 多采样 → 写 motionBlurTex
    ///   Pass2 (blit):   motionBlurTex → dstFbo (覆盖 sceneTex 内容)
    /// @param sceneTex            HDR scene 颜色 tex (HDR + 所有后处理累积)
    /// @param velocityTex         Phase E.13 velocity buffer (RG16F / RG8) — combined (camera+object)
    /// @param cameraVelocityTex   Phase E.16 — camera-only velocity tex (mode=1/2 时读;
    ///                            0 表示 fbo 创建时未请求, backend 内部 fallback 到 mode=0)
    /// @param motionBlurFbo       ping-pong fbo (由 MotionBlurRenderer 持有)
    /// @param motionBlurTex       ping-pong tex
    /// @param dstFbo              HDR fbo (输出目标，sceneTex 所属 fbo)
    /// @param w, h                分辨率
    /// @param strength            用户调节 [0, 4] (clamp 由调用方做)
    /// @param sampleCount         沿 velocity 采样数 [1, 32] (clamp 由调用方做)
    /// @param mode                Phase E.16 — 0=combined / 1=camera_only / 2=object_only
    /// @param rtW, rtH            Phase E.17 — motionBlurTex 实际尺寸 (0 = 沿用 w/h)
    ///                            Pass1 viewport 用 (rtW, rtH); Pass2 blit src=(rtW, rtH) dst=(w, h)
    ///                            (rtW < w || rtH < h) → 自动选 GL_LINEAR 上采样; 否则 GL_NEAREST
    virtual void DrawMotionBlur(uint32_t /*sceneTex*/, uint32_t /*velocityTex*/,
                                 uint32_t /*cameraVelocityTex*/,
                                 uint32_t /*motionBlurFbo*/, uint32_t /*motionBlurTex*/,
                                 uint32_t /*dstFbo*/,
                                 int /*w*/, int /*h*/,
                                 float /*strength*/, int /*sampleCount*/,
                                 int /*mode*/,
                                 int /*rtW*/ = 0, int /*rtH*/ = 0) {}  // ★ Phase E.17

    // ==================== Phase E.18 — Independent Velocity Dilation Pass ====================
    //
    // 背景: Phase E.14 起, SSR Temporal / Motion Blur shader 各自内嵌 9-tap max-length
    //       dilation, 双消费者场景下同一 velocityTex 被重复 9-tap, 浪费 GPU.
    //
    // 方案: 抽出独立 dilation pass — 在 HDR EndScene 内对 raw velocityTex 做一次 9-tap
    //       max-length, 输出 dilatedVelocityTex (RG16F, 始终 decode 后的 float), 后续
    //       SSR Temporal / Motion Blur shader 走单点采样路径 (uVelocityDilation=0).
    //
    // 资源:
    //   - dilatedVelocityFbo/Tex      与 raw velocityTex 同尺寸 (full-res), RG16F
    //   - dilatedCameraVelocityFbo/Tex 与 cameraVelocityTex 同条件 (Phase E.16 双 MRT)
    //   - 创建在 HDRRenderer::CreateRT, 释放在 ReleaseRT
    //
    // 状态:
    //   - dilationPassActive_ (backend 内部字段, 每帧 EndScene 由 HDRRenderer 设置)
    //   - true:  DrawSSRTemporal/DrawMotionBlur 内强制 uVelocityDilation=0 (单点采样)
    //   - false: 沿用 Phase E.14 行为 (shader 内 9-tap inline)
    //
    // Consumer fallback:
    //   - dilationPassActive=true  → consumer 绑 dilatedTex
    //   - dilationPassActive=false → consumer 绑 rawTex (兼容旧路径)

    /// 后端是否支持 velocity dilation pass (GL33 = shader 编译成功才 true)
    virtual bool SupportsVelocityDilation() const { return false; }

    /// 创建 velocity dilation ping-pong RT (RG16F, color-only, 无 depth)
    /// @param  w, h    logical 尺寸 (velocityTex full-res, 保留供未来 sanity check; 当前 backend 未使用)
    /// @param  sw, sh  storage 尺寸 (dilatedTex 实际 RT 尺寸)
    ///                 - Phase E.18:   sw=w, sh=h (full-res)
    ///                 - Phase E.18.1: halfRes=true 时 sw=((w+1)/2), sh=((h+1)/2)
    ///                 由调用方 (HDRRenderer) 计算并传入, backend 仅按 sw/sh 创建 RT
    /// @param  outTex  返回 GL tex id (失败为 0)
    /// @return GL fbo id (失败为 0); 失败时 outTex 也保证为 0
    /// @note   dilatedTex 永远 RG16F (无视 raw velocity format); shader 内统一 decode 写 float
    virtual uint32_t CreateVelocityDilateRT(int /*w*/, int /*h*/,
                                            int /*sw*/, int /*sh*/,
                                            uint32_t* outTex) {
        if (outTex) *outTex = 0;
        return 0;
    }

    /// 释放 velocity dilation RT (与 CreateVelocityDilateRT 配对)
    virtual void DeleteVelocityDilateRT(uint32_t /*fbo*/, uint32_t /*tex*/) {}

    /// 执行 dilation pass (single pass, 全屏 9-tap max-length)
    /// @param srcVelocityTex  输入 (RG16F 或 RG8, full-res, shader 内据 velocityFormat decode)
    /// @param dstFbo          dilation 输出 fbo (绑定 dilatedTex)
    /// @param sw, sh          dilatedTex storage 尺寸 (= viewport 尺寸; uTexel = 1/(sw, sh))
    ///                        - Phase E.18:   sw=w, sh=h (full-res, 邻域物理覆盖 3 raw px)
    ///                        - Phase E.18.1: half-res 时 sw=halfW, sh=halfH (邻域物理覆盖 6 raw px, max-filter 更鲁棒)
    /// @note  shader 内据 backend 当前 velocityFormat/Scale state decode raw,
    ///        最终写入 dilatedTex (RG16F float)
    virtual void DrawVelocityDilate(uint32_t /*srcVelocityTex*/,
                                     uint32_t /*dstFbo*/,
                                     int /*sw*/, int /*sh*/) {}

    /// 设置 dilation pass 当前帧是否激活 (HDRRenderer EndScene 每帧调用)
    /// true  → SSR Temporal / Motion Blur shader 强制 uVelocityDilation=0 (单点)
    /// false → 沿用 SetVelocityDilation 配置 (shader 内 inline 9-tap)
    virtual void SetDilationPassActive(bool /*active*/) {}

    /// 查询 dilation pass 当前激活状态 (用于调试 / consumer 决定绑 dilated/raw tex)
    virtual bool GetDilationPassActive() const { return false; }

    // ==================== Phase F.0 — TAA Master Pipeline 虚接口 ====================
    //
    // 设计 (与 Phase E 系列模块同模式):
    //   1. SupportsTAA()              — 后端能力位 (Legacy 永 false)
    //   2. CreateTAAHistoryRT() / DeleteTAAHistoryRT() — RGBA16F × 2 ping-pong (与 sceneTex 同尺寸)
    //   3. DrawTAAPass()              — reproject + neighborhood AABB clip + alpha blend
    //   4. BlitTAAToHDR()             — 把 TAA 输出 blit 回 HDR sceneTex (让 Tonemap 用 TAA 后内容)
    //
    // Jitter 注入 (backend 双 projection state):
    //   5. LoadJitteredProjection()       — TAA Enable 时每帧 BeginScene 前调; 后续 raster 用 jittered
    //   6. ClearJitteredProjection()      — TAA Process 末尾调; 复位至 unjittered (下帧 BeginScene 重设)
    //   7. IsJitteredProjectionActive()   — debug HUD 用
    //
    // 关键不变量:
    //   - GetProjection(out) 始终返 unjittered projection (SSR/SSAO/SSR Temporal 零改动)
    //   - velocity buffer 用 unjittered uCurViewProj 计算 (vertex shader 已上传)
    //     vertex shader 内 vCurClip = uCurViewProj * (uModel * pos) 而不用 gl_Position (含 jitter)

    /// 后端是否支持 TAA (RGBA16F + MRT velocity + shader 编译成功)
    virtual bool SupportsTAA() const { return false; }

    /// 创建 TAA history ping-pong RT (RGBA16F × 2, color-only, 无 depth)
    /// @param  w, h     与 HDR sceneTex 同尺寸
    /// @param  fbos     out: 2-element fbo array (失败时全 0)
    /// @param  texs     out: 2-element tex array (失败时全 0)
    /// @return true on success
    virtual bool CreateTAAHistoryRT(int /*w*/, int /*h*/,
                                     uint32_t* /*fbos*/, uint32_t* /*texs*/) { return false; }

    /// 释放 TAA history RT (与 CreateTAAHistoryRT 配对)
    virtual void DeleteTAAHistoryRT(uint32_t* /*fbos*/, uint32_t* /*texs*/) {}

    /// 执行 TAA pass (single pass, 全屏 reproject + clip + blend)
    /// @param curHdrTex          本帧 HDR scene (jittered raster 输出)
    /// @param historyTex         上帧 TAA 输出 (首帧时 backend 内部用 curHdrTex 占位)
    /// @param velocityTex        Phase E.18 dilated 优先, fallback raw velocity
    /// @param dstFbo             本帧 TAA 输出目标 (写入新 history slot)
    /// @param w, h               sceneTex 尺寸
    /// @param blendAlpha         history 权重 [0.5, 0.99]
    /// @param neighborhoodClip   1=启用 9-tap AABB clip, 0=纯 reproject+blend
    /// @param hasHistory         0=首帧 (输出 cur 不混合), 1=累积
    /// @param velocityDilation   0=单点 (E.18 dilated 路径), 1=inline 9-tap (fallback)
    /// @param velocityScale      RG8 模式下 decode 尺度
    /// @param velocityFormat     RG16F / RG8
    /// @param antiFlicker        Phase F.0.4: 0=纯 alpha blend (F.0 行为), 1=Karis luma-weighted blend
    ///                           高 luma 像素降权重压制 firefly 闪烁; 默认 1 与 F.0.1 sharpening 配合自然
    /// @param clipMode           Phase F.0.2/F.0.3: 0=RGB AABB clip (F.0), 1=YCoCg AABB (F.0.2), 2=YCoCg variance (F.0.3)
    ///                           YCoCg AABB 在亮度+色度独立约束; variance clip = mean ± γ·σ 代替 min/max
    /// @param varianceGamma      Phase F.0.3: variance clip 收紧系数 γ，仅 clipMode==2 生效
    ///                           Salvi 2016 / UE5 推荐 1.0；调用方应 clamp 到 [0, 4]。 越大 clip 越宽松 (接近无 clip)；0=极端激进 (mn=mx=mean)
    /// @param motionGamma        Phase F.0.8: motion-adaptive 高速区域 γ (UE5 高级形式), 仅 clipMode==2 && motionAdaptiveGamma==1 生效
    ///                           默认 1.5; 调用方应 clamp 到 [0, 4]; 静止区域用 varianceGamma, 高速运动 lerp 至 motionGamma
    /// @param motionAdaptiveGamma Phase F.0.8: 0=仅用 varianceGamma (F.0.3 行为, 零回归), 1=按 |velocity| 长度 lerp 两 γ
    virtual void DrawTAAPass(uint32_t /*curHdrTex*/, uint32_t /*historyTex*/,
                             uint32_t /*velocityTex*/, uint32_t /*dstFbo*/,
                             int /*w*/, int /*h*/,
                             float /*blendAlpha*/, int /*neighborhoodClip*/,
                             int /*hasHistory*/,
                             bool /*velocityDilation*/,
                             float /*velocityScale*/,
                             VelocityFormat /*velocityFormat*/,
                             int   /*antiFlicker*/        = 1,
                             int   /*clipMode*/           = 1,
                             float /*varianceGamma*/      = 1.0f,
                             float /*motionGamma*/        = 1.5f,   // Phase F.0.8 default = 1.5
                             int   /*motionAdaptiveGamma*/= 0)      // Phase F.0.8 default = OFF (零回归)
                             {}

    /// 把 TAA 输出 blit 回 HDR sceneTex (覆盖, 让后续 Tonemap 用 TAA 后内容)
    /// @param srcTex    TAA 输出 tex (= history 新 slot tex)
    /// @param dstFbo    HDR FBO (绑定的 sceneTex 是 blit 目标)
    /// @param srcW, srcH  src 尺寸 (history RT 实际尺寸，可能为 half-res，Phase F.0.5)
    /// @param dstW, dstH  dst 尺寸 (sceneTex 尺寸，full-res)；默认 0=与 src 同尺寸 (向后兼容老调用)
    /// Phase F.0.5: src/dst 尺寸不同时自动走 GL_LINEAR stretch (上采样)；同尺寸保持 GL_NEAREST (零回归)
    virtual void BlitTAAToHDR(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                              int /*srcW*/, int /*srcH*/,
                              int /*dstW*/ = 0, int /*dstH*/ = 0) {}

    /// Phase F.0.1 — TAA Sharpen pass: 4-tap unsharp mask, 替代 BlitTAAToHDR (in-place 写回 sceneTex)
    /// 调用方在 sharpness > 0 时调; sharpness <= 0 时直接走 BlitTAAToHDR (零 ALU 开销).
    /// shader 编译失败时此函数空实现, 调用方需 fallback 到 BlitTAAToHDR (基类默认 do nothing 已满足).
    /// @param srcTex     TAA 输出 tex (= history 新 slot tex)
    /// @param dstFbo     HDR FBO (绑定的 sceneTex 是写回目标)
    /// @param w, h       尺寸 (full-res)
    /// @param sharpness  unsharp mask 强度, 推荐 [0, 2]; 已由调用方 clamp
    virtual void DrawTAASharpenPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                    int /*w*/, int /*h*/, float /*sharpness*/) {}

    /// Phase F.0.6 — TAA CAS pass: 5-tap contrast-adaptive sharpening (AMD FidelityFX FSR1)
    /// 与 F.0.1 unsharp 共存, 用户通过 SetSharpenMode("cas"/"unsharp") 切换.
    /// shader 编译失败时此函数空实现, 调用方需 fallback 到 BlitTAAToHDR.
    /// @param srcTex     TAA 输出 tex (= history 新 slot tex)
    /// @param dstFbo     HDR FBO
    /// @param w, h       尺寸 (full-res)
    /// @param sharpness  CAS [0, 1] (FSR1 标准): 0→peak=-1/8 弱, 1→peak=-1/5 强; 已由调用方 clamp
    virtual void DrawTAACASPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                int /*w*/, int /*h*/, float /*sharpness*/) {}

    /// Phase F.0.12 — TAA RCAS pass: 5-tap robust contrast-adaptive sharpening (AMD FidelityFX FSR2)
    /// FSR1 CAS 的高级形式: noise detection (range<1/64 跳过) + edge protection (lobe sqrt 限制).
    /// 与 F.0.1 unsharp / F.0.6 cas 共存, 用户通过 SetSharpenMode("rcas") 切换.
    /// shader 编译失败时此函数空实现, 调用方需 fallback 到 BlitTAAToHDR.
    /// @param srcTex     TAA 输出 tex (= history 新 slot tex)
    /// @param dstFbo     HDR FBO
    /// @param w, h       尺寸 (full-res)
    /// @param sharpness  RCAS [0, 2] (FSR2 标准): 0→peak=-1/16 弱, 2→peak=-1/4 强; 已由调用方 clamp
    virtual void DrawTAARCASPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                 int /*w*/, int /*h*/, float /*sharpness*/) {}

    /// Phase F.0.14 — TAA Lanczos-2 25-tap 5x5 上采样 (高画质替代 F.0.9 Catmull-Rom)
    /// 仅 halfRes=true && sharpness=0 && upscaleMode==2 (lanczos) 路径使用
    /// 算法: Lanczos kernel L(x) = sinc(x) * sinc(x/2) for |x|<2 else 0
    /// 性能: ~0.07 ms @ 1080p (vs Catmull-Rom ~0.03 ms, +0.04 ms)
    /// 视觉: -10% blur vs Catmull-Rom (-55% vs bilinear), 适合 4K/桌面 GPU 超高画质
    /// 默认 no-op (老 backend 不支持时静默 fallback 到 BlitTAAToHDR)
    /// @param srcTex     history half-res tex
    /// @param dstFbo     HDR FBO
    /// @param srcW, srcH src 分辨率 (history half-res)
    /// @param dstW, dstH dst 分辨率 (sceneTex full-res)
    virtual void DrawTAALanczosPass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                    int /*srcW*/, int /*srcH*/,
                                    int /*dstW*/, int /*dstH*/) {}

    /// Phase F.0.9 — TAA Custom Upscale pass: Catmull-Rom 9-tap bicubic (Sigggraph 2018 Filmic SMAA)
    /// 仅 halfRes=true && sharpness=0 && upscaleMode==1 路径使用, 替代 BlitTAAToHDR 的 GL_LINEAR stretch.
    /// 视觉收益: -50% blur vs bilinear; 性能: ~+0.025 ms @ 1080p.
    /// shader 编译失败时此函数空实现, 调用方需 fallback 到 BlitTAAToHDR.
    /// @param srcTex     history half-res tex
    /// @param dstFbo     HDR FBO
    /// @param srcW, srcH src 分辨率 (history half-res)
    /// @param dstW, dstH dst 分辨率 (sceneTex full-res)
    virtual void DrawTAAUpscalePass(uint32_t /*srcTex*/, uint32_t /*dstFbo*/,
                                    int /*srcW*/, int /*srcH*/,
                                    int /*dstW*/, int /*dstH*/) {}

    /// 设置 jittered projection matrix (TAA 启用时每帧 BeginScene 前调)
    /// 调用后, ComputeMVP3D() 用 jitteredProjection 替代原 projection (raster 路径).
    /// GetProjection() 仍返 unjittered (SSR/SSAO 等 view-space reconstruction 不受影响).
    /// @param jitteredProj  16 floats (column-major)
    virtual void LoadJitteredProjection(const float* /*jitteredProj*/) {}

    /// 清除 jittered projection 模式 (复位为 unjittered raster)
    /// TAA Process 末尾调, 让下帧 BeginScene 重新设
    virtual void ClearJitteredProjection() {}

    /// 查询当前是否启用了 jittered projection (debug HUD 用)
    virtual bool IsJitteredProjectionActive() const { return false; }
};

// ==================== 工厂函数 ====================

/// 创建渲染后端: 优先 GL 3.3, 回退 GL 1.x
/// 必须在 PlatformWindow 创建窗口和 GL context 之后调用
RenderBackend* CreateRenderBackend();

/// 全局渲染后端实例 (在 light_ui.cpp Window.Open 中初始化)
extern RenderBackend* g_render;
