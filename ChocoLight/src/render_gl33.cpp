/**
 * @file render_gl33.cpp
 * @brief OpenGL 3.3 Core Profile 渲染后端 (GL33Backend)
 * @note VAO/VBO + shader pipeline, 自管理矩阵栈
 */

#include "render_backend.h"
#include "light.h"

// GL 头文件: GLES3 (Web/移动) vs glad (桌面)
#if defined(__EMSCRIPTEN__)
#include <GLES3/gl3.h>
#elif defined(__ANDROID__)
#include <GLES3/gl3.h>
#elif defined(CHOCO_PLATFORM_IOS)
#include <OpenGLES/ES3/gl.h>
#else
#include <glad/gl.h>
#endif

#include "platform_window.h"
#include <vector>
#include <cstring>
#include <unordered_map>

// ==================== 内嵌 Shader 源码 ====================

// GLES3 / GL33 共用 Shader, 仅版本声明不同
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
static const char* VS_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;
uniform mat4 uMVP;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

static const char* FS_SOURCE = R"(#version 300 es
precision mediump float;
in vec2 vTexCoord;
in vec4 vColor;
uniform sampler2D uTexture;
uniform int uUseTexture;
layout(location=0) out vec4 FragColor;
void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
}
)";
#else
static const char* VS_SOURCE = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec2 aTexCoord;
layout(location=2) in vec4 aColor;

uniform mat4 uMVP;

out vec2 vTexCoord;
out vec4 vColor;

void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vTexCoord = aTexCoord;
    vColor = aColor;
}
)";

static const char* FS_SOURCE = R"(
#version 330 core
in vec2 vTexCoord;
in vec4 vColor;

uniform sampler2D uTexture;
uniform int uUseTexture;

out vec4 FragColor;

void main() {
    if (uUseTexture == 1) {
        FragColor = vColor * texture(uTexture, vTexCoord);
    } else {
        FragColor = vColor;
    }
}
)";
#endif

// ==================== Phase AS.2 — 3D 默认 shader ====================
// 简单 Lambert 光照 + diffuse texture, 单方向光 (从右上前方照射)
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
static const char* VS3D_SOURCE = R"(#version 300 es
precision highp float;
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormalW;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    // 法线经模型矩阵变换 (简化: 不做严格 normal matrix)
    vNormalW = mat3(uModel) * aNormal;
    vTexCoord = aUV;
    vColor = aColor;
}
)";
static const char* FS3D_SOURCE = R"(#version 300 es
precision mediump float;
in vec3 vNormalW;
in vec2 vTexCoord;
in vec4 vColor;
uniform sampler2D uTexture;
uniform int uUseTexture;
uniform vec3 uLightDir;     // 已归一化, 指向光源
uniform vec3 uLightColor;
uniform vec3 uAmbient;
layout(location=0) out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormalW);
    float ndl = max(dot(N, uLightDir), 0.0);
    vec3 lit = uAmbient + uLightColor * ndl;
    vec4 base = vColor;
    if (uUseTexture == 1) base = base * texture(uTexture, vTexCoord);
    FragColor = vec4(base.rgb * lit, base.a);
}
)";
#else
static const char* VS3D_SOURCE = R"(
#version 330 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
layout(location=3) in vec4 aColor;
uniform mat4 uMVP;
uniform mat4 uModel;
out vec3 vNormalW;
out vec2 vTexCoord;
out vec4 vColor;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vNormalW = mat3(uModel) * aNormal;
    vTexCoord = aUV;
    vColor = aColor;
}
)";
static const char* FS3D_SOURCE = R"(
#version 330 core
in vec3 vNormalW;
in vec2 vTexCoord;
in vec4 vColor;
uniform sampler2D uTexture;
uniform int uUseTexture;
uniform vec3 uLightDir;
uniform vec3 uLightColor;
uniform vec3 uAmbient;
out vec4 FragColor;
void main() {
    vec3 N = normalize(vNormalW);
    float ndl = max(dot(N, uLightDir), 0.0);
    vec3 lit = uAmbient + uLightColor * ndl;
    vec4 base = vColor;
    if (uUseTexture == 1) base = base * texture(uTexture, vTexCoord);
    FragColor = vec4(base.rgb * lit, base.a);
}
)";
#endif

// Phase AS.2 — Mesh GPU 资源
struct MeshGPU {
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    int    indexCount;
};

// ==================== GL33Backend ====================

class GL33Backend : public RenderBackend {
    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;             // Phase A5: 静态索引缓冲, 与 BatchRenderer 配合
    GLuint program = 0;
    GLint  locMVP = -1;
    GLint  locUseTexture = -1;

    // Phase A5: EBO 容量 (索引数), 由 BatchRenderer 一次性上传后永久不变
    int eboCapacity = 0;

    // 当前颜色
    float curColor[4] = {1, 1, 1, 1};

    // 自管理矩阵栈
    std::vector<Mat4> matStack;
    Mat4 projection;
    Mat4 modelview;

    // 当前绑定的纹理 (0 = 无)
    GLuint boundTex = 0;

    // 动态 VBO 容量 (顶点数)
    int vboCapacity = 0;
    static constexpr int INITIAL_VBO_CAPACITY = 1024;

    // ---- Phase AS.2 — 3D mesh 资源 ----
    GLuint program3D       = 0;       // 3D 默认 shader (Lambert + diffuse)
    GLint  loc3D_MVP       = -1;
    GLint  loc3D_Model     = -1;
    GLint  loc3D_Texture   = -1;
    GLint  loc3D_UseTexture= -1;
    GLint  loc3D_LightDir  = -1;
    GLint  loc3D_LightColor= -1;
    GLint  loc3D_Ambient   = -1;
    bool   userShaderActive = false;  // 用户 Shader:Use 时为 true; UseDefaultShader 时复位
    bool   depthTestEnabled = false;  // 当前深度测试状态 (默认关, 与 2D 兼容)
    Mat4   viewMatrix;                // Phase AS.2 — 视图矩阵 (LookAt 结果)
    bool   hasView          = false;  // 是否已 LoadView (false 表示用 modelview 直接)

    // Mesh 资源池
    std::unordered_map<uint32_t, MeshGPU> meshes;
    uint32_t                              nextMeshId = 1;

    // 编译 shader, 返回 0 表示失败
    static GLuint CompileShader(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, 512, nullptr, log);
            CC::Log(CC::LOG_ERROR, "GL33: shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    // 链接 program
    static GLuint LinkProgram(GLuint vs, GLuint fs) {
        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetProgramInfoLog(p, 512, nullptr, log);
            CC::Log(CC::LOG_ERROR, "GL33: program link error: %s", log);
            glDeleteProgram(p);
            return 0;
        }
        return p;
    }

    // 上传 MVP uniform (2D shader, 简单的 projection * modelview)
    void FlushMVP() {
        Mat4 mvp = projection * modelview;
        glUniformMatrix4fv(locMVP, 1, GL_FALSE, mvp.m);
    }

    // 计算 3D MVP: projection * view * modelview (含相机 transform)
    Mat4 ComputeMVP3D() const {
        Mat4 vm = hasView ? (viewMatrix * modelview) : modelview;
        return projection * vm;
    }

    // 确保 VBO 容量足够
    void EnsureVBOCapacity(int vertexCount) {
        if (vertexCount <= vboCapacity) return;
        int newCap = vboCapacity ? vboCapacity : INITIAL_VBO_CAPACITY;
        while (newCap < vertexCount) newCap *= 2;
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, newCap * sizeof(RenderVertex), nullptr, GL_DYNAMIC_DRAW);
        vboCapacity = newCap;
    }

public:
    bool Init() override {
        // 编译链接 shader
        GLuint vs = CompileShader(GL_VERTEX_SHADER, VS_SOURCE);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, FS_SOURCE);
        if (!vs || !fs) return false;
        program = LinkProgram(vs, fs);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!program) return false;

        locMVP = glGetUniformLocation(program, "uMVP");
        locUseTexture = glGetUniformLocation(program, "uUseTexture");

        // 创建 VAO + VBO + EBO (Phase A5)
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glGenBuffers(1, &ebo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, INITIAL_VBO_CAPACITY * sizeof(RenderVertex),
                     nullptr, GL_DYNAMIC_DRAW);
        vboCapacity = INITIAL_VBO_CAPACITY;
        // EBO 绑定到 VAO, BatchRenderer 首次 DrawIndexed 时上传索引数据
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);

        // 顶点属性: aPos(0), aTexCoord(1), aColor(2)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, u));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex),
                              (void*)offsetof(RenderVertex, r));

        glBindVertexArray(0);

        // 初始矩阵
        projection = Mat4::Identity();
        modelview = Mat4::Identity();
        matStack.reserve(32);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

        // ---- Phase AS.2 — 编译 3D 默认 shader ----
        GLuint vs3D = CompileShader(GL_VERTEX_SHADER, VS3D_SOURCE);
        GLuint fs3D = CompileShader(GL_FRAGMENT_SHADER, FS3D_SOURCE);
        if (vs3D && fs3D) {
            program3D = LinkProgram(vs3D, fs3D);
            if (program3D) {
                loc3D_MVP        = glGetUniformLocation(program3D, "uMVP");
                loc3D_Model      = glGetUniformLocation(program3D, "uModel");
                loc3D_Texture    = glGetUniformLocation(program3D, "uTexture");
                loc3D_UseTexture = glGetUniformLocation(program3D, "uUseTexture");
                loc3D_LightDir   = glGetUniformLocation(program3D, "uLightDir");
                loc3D_LightColor = glGetUniformLocation(program3D, "uLightColor");
                loc3D_Ambient    = glGetUniformLocation(program3D, "uAmbient");
            } else {
                CC::Log(CC::LOG_WARN, "GL33: 3D shader link failed (Mesh:Draw will silently no-op)");
            }
        } else {
            CC::Log(CC::LOG_WARN, "GL33: 3D shader compile failed");
        }
        if (vs3D) glDeleteShader(vs3D);
        if (fs3D) glDeleteShader(fs3D);

        CC::Log(CC::LOG_INFO, "RenderBackend: GL33 Core initialized (GL %s)%s",
                (const char*)glGetString(GL_VERSION),
                program3D ? ", 3D mesh enabled" : "");
        return true;
    }

    void Shutdown() override {
        if (program) glDeleteProgram(program);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        if (vao) glDeleteVertexArrays(1, &vao);
        program = vao = vbo = ebo = 0;
        eboCapacity = 0;

        // Phase AS.2 — 释放 3D 资源
        if (program3D) {
            glDeleteProgram(program3D);
            program3D = 0;
        }
        for (auto& kv : meshes) {
            const MeshGPU& m = kv.second;
            if (m.ebo) glDeleteBuffers(1, &m.ebo);
            if (m.vbo) glDeleteBuffers(1, &m.vbo);
            if (m.vao) glDeleteVertexArrays(1, &m.vao);
        }
        meshes.clear();
    }

    const char* GetName() const override { return "GL33Core"; }

    // ---- 帧控制 ----
    void BeginFrame(float cr, float cg, float cb, float ca) override {
        glClearColor(cr, cg, cb, ca);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glUseProgram(program);
        glBindVertexArray(vao);
    }
    void EndFrame() override {
        glBindVertexArray(0);
        glUseProgram(0);
    }

    // ---- 状态 ----
    void SetColor(float r, float g, float b, float a) override {
        curColor[0] = r; curColor[1] = g; curColor[2] = b; curColor[3] = a;
    }
    void GetColor(float* r, float* g, float* b, float* a) override {
        *r = curColor[0]; *g = curColor[1]; *b = curColor[2]; *a = curColor[3];
    }
    void SetViewport(int x, int y, int w, int h) override {
        glViewport(x, y, w, h);
    }

    // ---- 变换栈 ----
    void PushMatrix() override {
        matStack.push_back(modelview);
    }
    void PopMatrix() override {
        if (!matStack.empty()) {
            modelview = matStack.back();
            matStack.pop_back();
        }
    }
    void Translate(float x, float y, float z) override {
        modelview = modelview * Mat4::Translate(x, y, z);
    }
    void Rotate(float angle, float ax, float ay, float az) override {
        modelview = modelview * Mat4::Rotate(angle, ax, ay, az);
    }
    void Scale(float sx, float sy, float sz) override {
        modelview = modelview * Mat4::Scale(sx, sy, sz);
    }
    void LoadOrtho(float l, float r, float b, float t, float n, float f) override {
        projection = Mat4::Ortho(l, r, b, t, n, f);
        modelview = Mat4::Identity();
    }

    // ---- Phase A5: 索引绘制 (BatchRenderer 走此路径) ----
    void DrawIndexed(const RenderVertex* verts, int vertexCount,
                     const uint16_t* indices, int indexCount,
                     uint32_t textureId) override {
        if (!verts || !indices || vertexCount <= 0 || indexCount <= 0) return;

        // 更新 MVP + 纹理 uniform
        FlushMVP();
        if (textureId) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, (GLuint)textureId);
            glUniform1i(locUseTexture, 1);
            boundTex = (GLuint)textureId;
        } else {
            glUniform1i(locUseTexture, 0);
        }

        // 上传顶点数据
        EnsureVBOCapacity(vertexCount);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, vertexCount * sizeof(RenderVertex), verts);

        // 上传索引数据 (EBO 已在 VAO 中绑定)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
        if (indexCount > eboCapacity) {
            // 扩容 (按 2 倍增长)
            int newCap = eboCapacity ? eboCapacity : 1024;
            while (newCap < indexCount) newCap *= 2;
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, newCap * sizeof(uint16_t),
                         nullptr, GL_DYNAMIC_DRAW);
            eboCapacity = newCap;
        }
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, indexCount * sizeof(uint16_t), indices);

        // 一次性 indexed draw
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_SHORT, (void*)0);

        if (textureId) {
            glBindTexture(GL_TEXTURE_2D, 0);
            boundTex = 0;
        }
    }

    // ---- 绘制 ----
    void DrawArrays(DrawMode mode, const RenderVertex* verts, int count) override {
        if (count <= 0) return;

        // 更新 MVP
        FlushMVP();

        // 纹理状态
        glUniform1i(locUseTexture, boundTex ? 1 : 0);

        // 上传顶点数据
        EnsureVBOCapacity(count);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferSubData(GL_ARRAY_BUFFER, 0, count * sizeof(RenderVertex), verts);

        // GL 3.3 Core 没有 GL_QUADS, 需要拆分
        if (mode == DrawMode::Quads) {
            // 每 4 个顶点拆为 2 个三角形 (0,1,2 + 0,2,3)
            int quadCount = count / 4;
            int triVertCount = quadCount * 6;

            // 构建索引或直接生成三角形顶点
            // 使用临时缓冲拆分 (性能足够, 2D 场景顶点少)
            std::vector<RenderVertex> triVerts;
            triVerts.reserve(triVertCount);
            for (int i = 0; i < quadCount; ++i) {
                int base = i * 4;
                triVerts.push_back(verts[base + 0]);
                triVerts.push_back(verts[base + 1]);
                triVerts.push_back(verts[base + 2]);
                triVerts.push_back(verts[base + 0]);
                triVerts.push_back(verts[base + 2]);
                triVerts.push_back(verts[base + 3]);
            }
            EnsureVBOCapacity(triVertCount);
            glBufferSubData(GL_ARRAY_BUFFER, 0, triVertCount * sizeof(RenderVertex), triVerts.data());
            glDrawArrays(GL_TRIANGLES, 0, triVertCount);
        } else {
            GLenum glMode;
            switch (mode) {
                case DrawMode::Lines:       glMode = GL_LINES; break;
                case DrawMode::LineLoop:    glMode = GL_LINE_LOOP; break;
                case DrawMode::LineStrip:   glMode = GL_LINE_STRIP; break;
                case DrawMode::Triangles:   glMode = GL_TRIANGLES; break;
                case DrawMode::TriangleFan: glMode = GL_TRIANGLE_FAN; break;
                default:                    glMode = GL_TRIANGLES; break;
            }
            glDrawArrays(glMode, 0, count);
        }
    }

    // ---- 纹理 ----
    uint32_t CreateTexture(int w, int h, int channels, const void* pixels) override {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        // GL 3.3 Core: 内部格式必须使用 sized format
        GLenum intFmt, fmt;
        if (channels == 4) { intFmt = GL_RGBA8; fmt = GL_RGBA; }
        else if (channels == 3) { intFmt = GL_RGB8; fmt = GL_RGB; }
        else { intFmt = GL_R8; fmt = GL_RED; }
        glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        // 单通道纹理: 映射 RED → ALPHA, RGB=1 (兼容旧 GL_ALPHA 行为, 用于字体图集)
        if (channels == 1) {
#if defined(__EMSCRIPTEN__)
            // WebGL2 不支持 texture swizzle, 展开 R8 → RGBA (白底 + alpha)
            if (pixels) {
                std::vector<uint8_t> rgba(w * h * 4);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int i = 0; i < w * h; ++i) {
                    rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                    rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            return tex;
#elif defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
            // GLES3: 单独设置 swizzle (GL_TEXTURE_SWIZZLE_RGBA 不在 GLES 3.0 规范中)
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_ONE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_RED);
#else
            GLint swizzle[] = {GL_ONE, GL_ONE, GL_ONE, GL_RED};
            glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle);
#endif
        }
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }
    void DeleteTexture(uint32_t texId) override {
        GLuint t = texId;
        glDeleteTextures(1, &t);
    }
    void BindTexture(uint32_t texId) override {
        boundTex = texId;
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texId);
    }
    void UnbindTexture() override {
        boundTex = 0;
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void UpdateTexture(uint32_t texId, int x, int y, int w, int h,
                       int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
#if defined(__EMSCRIPTEN__)
        if (channels == 1 && pixels) {
            // 内部纹理已为 RGBA8, 展开后上传
            std::vector<uint8_t> rgba(w * h * 4);
            const uint8_t* src = (const uint8_t*)pixels;
            for (int i = 0; i < w * h; ++i) {
                rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        } else {
            GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
        }
#else
        GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_RED;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
#endif
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void ReplaceTexture(uint32_t texId, int w, int h, int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
#if defined(__EMSCRIPTEN__)
        if (channels == 1) {
            if (pixels) {
                std::vector<uint8_t> rgba(w * h * 4);
                const uint8_t* src = (const uint8_t*)pixels;
                for (int i = 0; i < w * h; ++i) {
                    rgba[i*4+0] = 255; rgba[i*4+1] = 255;
                    rgba[i*4+2] = 255; rgba[i*4+3] = src[i];
                }
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            } else {
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            }
        } else {
            GLenum intFmt = (channels == 4) ? GL_RGBA8 : GL_RGB8;
            GLenum fmt    = (channels == 4) ? GL_RGBA  : GL_RGB;
            glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        }
#else
        GLenum intFmt, fmt;
        if (channels == 4) { intFmt = GL_RGBA8; fmt = GL_RGBA; }
        else if (channels == 3) { intFmt = GL_RGB8; fmt = GL_RGB; }
        else { intFmt = GL_R8; fmt = GL_RED; }
        glTexImage2D(GL_TEXTURE_2D, 0, intFmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
#endif
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ---- FBO ----
    uint32_t CreateFBO(int w, int h, uint32_t* outTex, uint32_t* outDepthRB) override {
        GLuint tex = CreateTexture(w, h, 4, nullptr);
        GLuint depthRB = 0;
        glGenRenderbuffers(1, &depthRB);
        glBindRenderbuffer(GL_RENDERBUFFER, depthRB);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);

        GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "GL33: FBO incomplete (status=0x%X)", status);
            glDeleteFramebuffers(1, &fbo);
            GLuint t = tex; glDeleteTextures(1, &t);
            glDeleteRenderbuffers(1, &depthRB);
            return 0;
        }
        *outTex = tex;
        *outDepthRB = depthRB;
        return fbo;
    }
    void DeleteFBO(uint32_t fbo, uint32_t tex, uint32_t depthRB) override {
        if (fbo) glDeleteFramebuffers(1, &fbo);
        if (tex) { GLuint t = tex; glDeleteTextures(1, &t); }
        if (depthRB) glDeleteRenderbuffers(1, &depthRB);
    }
    void BindFBO(uint32_t fbo) override {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }
    void UnbindFBO() override {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    // ---- 裁剪 ----
    void SetScissor(bool enable, int x, int y, int w, int h) override {
        if (enable) {
            glEnable(GL_SCISSOR_TEST);
            glScissor(x, y, w, h);
        } else {
            glDisable(GL_SCISSOR_TEST);
        }
    }

    // ---- 用户 Shader 支持 ----
    bool SupportsShaders() const override { return true; }

    uint32_t CreateShader(const char* vertexSrc, const char* fragmentSrc,
                          char* errLog, int errLogSize) override {
        if (!vertexSrc || !fragmentSrc) return 0;

        auto tryCompile = [&](GLenum type, const char* src) -> GLuint {
            GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &src, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok) {
                if (errLog && errLogSize > 0) {
                    glGetShaderInfoLog(s, errLogSize - 1, nullptr, errLog);
                    errLog[errLogSize - 1] = '\0';
                }
                glDeleteShader(s);
                return 0;
            }
            return s;
        };

        GLuint vs = tryCompile(GL_VERTEX_SHADER, vertexSrc);
        if (!vs) return 0;
        GLuint fs = tryCompile(GL_FRAGMENT_SHADER, fragmentSrc);
        if (!fs) { glDeleteShader(vs); return 0; }

        GLuint p = glCreateProgram();
        glAttachShader(p, vs);
        glAttachShader(p, fs);
        // 绑定默认属性位置, 让用户 shader 复用引擎的 VAO
        glBindAttribLocation(p, 0, "aPos");
        glBindAttribLocation(p, 1, "aTexCoord");
        glBindAttribLocation(p, 2, "aColor");
        glLinkProgram(p);
        GLint ok = 0;
        glGetProgramiv(p, GL_LINK_STATUS, &ok);
        glDeleteShader(vs);
        glDeleteShader(fs);
        if (!ok) {
            if (errLog && errLogSize > 0) {
                glGetProgramInfoLog(p, errLogSize - 1, nullptr, errLog);
                errLog[errLogSize - 1] = '\0';
            }
            glDeleteProgram(p);
            return 0;
        }
        return (uint32_t)p;
    }

    void DeleteShader(uint32_t shaderId) override {
        if (shaderId) glDeleteProgram((GLuint)shaderId);
    }

    bool UseShader(uint32_t shaderId) override {
        if (!shaderId) return false;
        glUseProgram((GLuint)shaderId);
        userShaderActive = true;  // Phase AS.2 — 标记用户 shader 激活
        // 自动上传 MVP 到约定名 uMVP (若存在)
        GLint locMVPUser = glGetUniformLocation((GLuint)shaderId, "uMVP");
        if (locMVPUser >= 0) {
            Mat4 mvp = projection * modelview;
            glUniformMatrix4fv(locMVPUser, 1, GL_FALSE, mvp.m);
        }
        return true;
    }

    void UseDefaultShader() override {
        glUseProgram(program);
        userShaderActive = false;  // Phase AS.2 — 复位标志
        FlushMVP();
    }

    int GetUniformLocation(uint32_t shaderId, const char* name) override {
        if (!shaderId || !name) return -1;
        return glGetUniformLocation((GLuint)shaderId, name);
    }

    void SetUniform1f(int loc, float v) override {
        if (loc >= 0) glUniform1f(loc, v);
    }
    void SetUniform2f(int loc, float x, float y) override {
        if (loc >= 0) glUniform2f(loc, x, y);
    }
    void SetUniform3f(int loc, float x, float y, float z) override {
        if (loc >= 0) glUniform3f(loc, x, y, z);
    }
    void SetUniform4f(int loc, float x, float y, float z, float w) override {
        if (loc >= 0) glUniform4f(loc, x, y, z, w);
    }
    void SetUniform1i(int loc, int v) override {
        if (loc >= 0) glUniform1i(loc, v);
    }
    void SetUniformMat4(int loc, const float* m) override {
        if (loc >= 0 && m) glUniformMatrix4fv(loc, 1, GL_FALSE, m);
    }

    // ---- Phase AS.1 新增 uniform setter ----
    void SetUniformMat3(int loc, const float* m) override {
        if (loc >= 0 && m) glUniformMatrix3fv(loc, 1, GL_FALSE, m);
    }
    void SetUniform2i(int loc, int x, int y) override {
        if (loc >= 0) glUniform2i(loc, x, y);
    }
    void SetUniform3i(int loc, int x, int y, int z) override {
        if (loc >= 0) glUniform3i(loc, x, y, z);
    }
    void SetUniform4i(int loc, int x, int y, int z, int w) override {
        if (loc >= 0) glUniform4i(loc, x, y, z, w);
    }
    void SetUniform1fv(int loc, int count, const float* v) override {
        if (loc >= 0 && v && count > 0) glUniform1fv(loc, count, v);
    }
    void SetUniform2fv(int loc, int count, const float* v) override {
        if (loc >= 0 && v && count > 0) glUniform2fv(loc, count, v);
    }
    void SetUniformSampler(int loc, int slot, uint32_t texId) override {
        if (loc < 0 || !texId) return;
        // 限制 slot 在合理范围 (大多数 GPU 至少支持 16 个 texture unit)
        if (slot < 0) slot = 0;
        if (slot > 15) slot = 15;
        glActiveTexture(GL_TEXTURE0 + (GLenum)slot);
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        glUniform1i(loc, slot);
        // 恢复活动 texture unit 到 slot 0, 与引擎默认绘制一致
        glActiveTexture(GL_TEXTURE0);
    }
    void GenerateMipmap(uint32_t texId) override {
        if (!texId) return;
        glBindTexture(GL_TEXTURE_2D, (GLuint)texId);
        glGenerateMipmap(GL_TEXTURE_2D);
        // mipmap 生成后, 需要让 min filter 支持 mipmap 才有效
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void ClearCurrent(float r, float g, float b, float a) override {
        glClearColor(r, g, b, a);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // ==================== Phase AS.2 — 3D mesh + 深度测试 + camera ====================

    bool Supports3D() const override { return program3D != 0; }

    uint32_t CreateMesh(const RenderVertex3D* verts, int vCount,
                        const uint32_t* indices, int iCount) override {
        if (!verts || vCount <= 0 || !indices || iCount <= 0) return 0;
        if (!program3D) return 0;  // 无 3D shader 时不能创建 (Init 失败兜底)

        MeshGPU m;
        glGenVertexArrays(1, &m.vao);
        glGenBuffers(1, &m.vbo);
        glGenBuffers(1, &m.ebo);
        glBindVertexArray(m.vao);

        // VBO: 上传顶点
        glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
        glBufferData(GL_ARRAY_BUFFER, vCount * sizeof(RenderVertex3D), verts, GL_STATIC_DRAW);

        // EBO: 上传索引 (uint32, 兼容 mesh 顶点数 > 65536)
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, iCount * sizeof(uint32_t), indices, GL_STATIC_DRAW);

        // 顶点属性 layout: pos(0), normal(1), uv(2), color(3)
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, x));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, nx));
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, u));
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                              (void*)offsetof(RenderVertex3D, r));

        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

        m.indexCount = iCount;
        uint32_t id = nextMeshId++;
        meshes[id] = m;
        return id;
    }

    void DeleteMesh(uint32_t meshId) override {
        auto it = meshes.find(meshId);
        if (it == meshes.end()) return;
        const MeshGPU& m = it->second;
        if (m.ebo) glDeleteBuffers(1, &m.ebo);
        if (m.vbo) glDeleteBuffers(1, &m.vbo);
        if (m.vao) glDeleteVertexArrays(1, &m.vao);
        meshes.erase(it);
    }

    void DrawMesh(uint32_t meshId, uint32_t textureId) override {
        auto it = meshes.find(meshId);
        if (it == meshes.end()) return;
        const MeshGPU& m = it->second;
        if (!m.vao || m.indexCount <= 0) return;

        // 选择 shader: 用户 shader 激活时不切, 否则用引擎 3D 默认
        if (!userShaderActive && program3D) {
            glUseProgram(program3D);
            // 上传 MVP / Model / Light uniforms
            Mat4 mvp = ComputeMVP3D();
            glUniformMatrix4fv(loc3D_MVP,   1, GL_FALSE, mvp.m);
            glUniformMatrix4fv(loc3D_Model, 1, GL_FALSE, modelview.m);
            // 默认光: 从 (0.5, 1, 0.5) 方向照射 (归一化)
            float lx = 0.408f, ly = 0.816f, lz = 0.408f;
            glUniform3f(loc3D_LightDir,   lx, ly, lz);
            glUniform3f(loc3D_LightColor, 0.9f, 0.9f, 0.85f);
            glUniform3f(loc3D_Ambient,    0.2f, 0.2f, 0.25f);
            // 纹理
            if (textureId) {
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, textureId);
                glUniform1i(loc3D_Texture,    0);
                glUniform1i(loc3D_UseTexture, 1);
            } else {
                glUniform1i(loc3D_UseTexture, 0);
            }
        } else if (textureId) {
            // 用户 shader: 仅绑定纹理到 slot 0, 用户负责 sampler uniform
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, textureId);
        }

        // 临时启用深度测试 (如果用户没启用), 绘制完恢复
        bool tempDepth = !depthTestEnabled;
        if (tempDepth) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        }

        glBindVertexArray(m.vao);
        glDrawElements(GL_TRIANGLES, m.indexCount, GL_UNSIGNED_INT, (void*)0);
        glBindVertexArray(0);

        if (tempDepth) {
            glDisable(GL_DEPTH_TEST);
        }

        // 切回默认 2D shader (避免 3D shader 残留影响后续 2D 绘制)
        if (!userShaderActive && program3D) {
            glUseProgram(program);
            // 重新绑定 2D VAO 以便后续 2D 绘制
            glBindVertexArray(vao);
        }
    }

    void SetDepthTest(bool enable) override {
        if (enable == depthTestEnabled) return;
        depthTestEnabled = enable;
        if (enable) {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_LEQUAL);
        } else {
            glDisable(GL_DEPTH_TEST);
        }
    }

    void SetDepthFunc(int func) override {
        // 0=Less, 1=LEqual, 2=Greater, 3=GEqual, 4=Equal, 5=NotEqual, 6=Always, 7=Never
        GLenum gl_func;
        switch (func) {
            case 0: gl_func = GL_LESS; break;
            case 1: gl_func = GL_LEQUAL; break;
            case 2: gl_func = GL_GREATER; break;
            case 3: gl_func = GL_GEQUAL; break;
            case 4: gl_func = GL_EQUAL; break;
            case 5: gl_func = GL_NOTEQUAL; break;
            case 6: gl_func = GL_ALWAYS; break;
            case 7: gl_func = GL_NEVER; break;
            default: gl_func = GL_LEQUAL; break;
        }
        glDepthFunc(gl_func);
    }

    void LoadView(const float* viewMat4) override {
        if (!viewMat4) {
            hasView = false;
            return;
        }
        memcpy(viewMatrix.m, viewMat4, sizeof(viewMatrix.m));
        hasView = true;
    }

    void LoadProjection(const float* projMat4) override {
        if (!projMat4) return;
        memcpy(projection.m, projMat4, sizeof(projection.m));
    }
};

// ==================== GL33Backend 工厂 ====================

RenderBackend* CreateGL33Backend() {
    auto* b = new GL33Backend();
    if (b->Init()) return b;
    delete b;
    return nullptr;
}

// ==================== 运行时自动选择工厂 ====================

RenderBackend* CreateRenderBackend() {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__) || defined(CHOCO_PLATFORM_IOS)
    // GLES3: 直接创建, 无需 glad 加载
    RenderBackend* gl33 = CreateGL33Backend();
    if (gl33) return gl33;
    CC::Log(CC::LOG_ERROR, "GLES3 backend init failed!");
    return nullptr;
#else
    // 桌面: glad 加载 + Legacy 回退
    extern RenderBackend* CreateLegacyBackend();
    if (gladLoadGL((GLADloadfunc)PlatformWindow::GetGLProcAddress)) {
        if (GLAD_GL_VERSION_3_3) {
            RenderBackend* gl33 = CreateGL33Backend();
            if (gl33) return gl33;
            CC::Log(CC::LOG_WARN, "GL33 backend init failed, falling back to Legacy");
        } else {
            CC::Log(CC::LOG_INFO, "GL version < 3.3, using Legacy backend");
        }
    } else {
        CC::Log(CC::LOG_WARN, "glad failed to load GL, using Legacy backend");
    }
    RenderBackend* legacy = CreateLegacyBackend();
    if (legacy) return legacy;
    CC::Log(CC::LOG_ERROR, "No render backend available!");
    return nullptr;
#endif
}
