/**
 * @file render_legacy.cpp
 * @brief OpenGL 1.x/2.x 固定管线渲染后端 (LegacyGLBackend)
 * @note 直接包装现有 GL 调用, 作为 GL 3.3 不可用时的回退方案
 */

#include "render_backend.h"
#include "light.h"
#include <vector>

#ifdef _WIN32
#include <GL/gl.h>
#elif defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include "platform_window.h"

// ==================== FBO 扩展函数指针 (从 canvas 迁入) ====================

#ifndef APIENTRY
#define APIENTRY
#endif

typedef void   (APIENTRY *PFN_glGenFramebuffers)(int, unsigned int*);
typedef void   (APIENTRY *PFN_glDeleteFramebuffers)(int, const unsigned int*);
typedef void   (APIENTRY *PFN_glBindFramebuffer)(unsigned int, unsigned int);
typedef void   (APIENTRY *PFN_glFramebufferTexture2D)(unsigned int, unsigned int, unsigned int, unsigned int, int);
typedef void   (APIENTRY *PFN_glGenRenderbuffers)(int, unsigned int*);
typedef void   (APIENTRY *PFN_glDeleteRenderbuffers)(int, const unsigned int*);
typedef void   (APIENTRY *PFN_glBindRenderbuffer)(unsigned int, unsigned int);
typedef void   (APIENTRY *PFN_glRenderbufferStorage)(unsigned int, unsigned int, int, int);
typedef void   (APIENTRY *PFN_glFramebufferRenderbuffer)(unsigned int, unsigned int, unsigned int, unsigned int);
typedef unsigned int (APIENTRY *PFN_glCheckFramebufferStatus)(unsigned int);

// GL FBO 常量
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER            0x8D40
#define GL_RENDERBUFFER           0x8D41
#define GL_COLOR_ATTACHMENT0      0x8CE0
#define GL_DEPTH_ATTACHMENT       0x8D00
#define GL_FRAMEBUFFER_COMPLETE   0x8CD5
#define GL_DEPTH_COMPONENT16      0x81A5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE          0x812F
#endif

// ==================== LegacyGLBackend ====================

class LegacyGLBackend : public RenderBackend {
    float curColor[4] = {1, 1, 1, 1};

    // FBO 扩展
    PFN_glGenFramebuffers         glGenFB   = nullptr;
    PFN_glDeleteFramebuffers      glDelFB   = nullptr;
    PFN_glBindFramebuffer         glBindFB  = nullptr;
    PFN_glFramebufferTexture2D    glFBTex2D = nullptr;
    PFN_glGenRenderbuffers        glGenRB   = nullptr;
    PFN_glDeleteRenderbuffers     glDelRB   = nullptr;
    PFN_glBindRenderbuffer        glBindRB  = nullptr;
    PFN_glRenderbufferStorage     glRBStore = nullptr;
    PFN_glFramebufferRenderbuffer glFBRB    = nullptr;
    PFN_glCheckFramebufferStatus  glCheckFB = nullptr;
    bool fboAvailable = false;

    void LoadFBOExtensions() {
        // 使用 PlatformWindow::GetGLProcAddress 跨平台加载 FBO 扩展
        auto getProc = [](const char* name) { return PlatformWindow::GetGLProcAddress(name); };
        glGenFB   = (PFN_glGenFramebuffers)getProc("glGenFramebuffers");
        glDelFB   = (PFN_glDeleteFramebuffers)getProc("glDeleteFramebuffers");
        glBindFB  = (PFN_glBindFramebuffer)getProc("glBindFramebuffer");
        glFBTex2D = (PFN_glFramebufferTexture2D)getProc("glFramebufferTexture2D");
        glGenRB   = (PFN_glGenRenderbuffers)getProc("glGenRenderbuffers");
        glDelRB   = (PFN_glDeleteRenderbuffers)getProc("glDeleteRenderbuffers");
        glBindRB  = (PFN_glBindRenderbuffer)getProc("glBindRenderbuffer");
        glRBStore = (PFN_glRenderbufferStorage)getProc("glRenderbufferStorage");
        glFBRB    = (PFN_glFramebufferRenderbuffer)getProc("glFramebufferRenderbuffer");
        glCheckFB = (PFN_glCheckFramebufferStatus)getProc("glCheckFramebufferStatus");
        fboAvailable = glGenFB && glDelFB && glBindFB && glFBTex2D && glCheckFB;
    }

    // DrawMode → GLenum 映射
    static GLenum ToGLMode(DrawMode mode) {
        switch (mode) {
            case DrawMode::Lines:       return GL_LINES;
            case DrawMode::LineLoop:    return GL_LINE_LOOP;
            case DrawMode::LineStrip:   return GL_LINE_STRIP;
            case DrawMode::Triangles:   return GL_TRIANGLES;
            case DrawMode::TriangleFan: return GL_TRIANGLE_FAN;
            case DrawMode::Quads:       return GL_QUADS;
        }
        return GL_TRIANGLES;
    }

public:
    bool Init() override {
        LoadFBOExtensions();
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        CC::Log(CC::LOG_INFO, "RenderBackend: LegacyGL initialized (FBO=%s)",
                fboAvailable ? "yes" : "no");
        return true;
    }

    void Shutdown() override {}

    const char* GetName() const override { return "LegacyGL"; }

    // ---- 帧控制 ----
    void BeginFrame(float cr, float cg, float cb, float ca) override {
        glClearColor(cr, cg, cb, ca);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }
    void EndFrame() override {}

    // ---- 状态 ----
    void SetColor(float r, float g, float b, float a) override {
        curColor[0] = r; curColor[1] = g; curColor[2] = b; curColor[3] = a;
        glColor4f(r, g, b, a);
    }
    void GetColor(float* r, float* g, float* b, float* a) override {
        *r = curColor[0]; *g = curColor[1]; *b = curColor[2]; *a = curColor[3];
    }
    void SetViewport(int x, int y, int w, int h) override {
        glViewport(x, y, w, h);
    }

    // ---- 变换栈 ----
    void PushMatrix() override  { glPushMatrix(); }
    void PopMatrix() override   { glPopMatrix(); }
    void Translate(float x, float y, float z) override { glTranslatef(x, y, z); }
    void Rotate(float angle, float ax, float ay, float az) override { glRotatef(angle, ax, ay, az); }
    void Scale(float sx, float sy, float sz) override { glScalef(sx, sy, sz); }
    void LoadOrtho(float l, float r, float b, float t, float n, float f) override {
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glOrtho(l, r, b, t, n, f);
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
    }

    // ---- 绘制 ----
    void DrawArrays(DrawMode mode, const RenderVertex* verts, int count) override {
        GLenum glMode = ToGLMode(mode);
        glBegin(glMode);
        for (int i = 0; i < count; ++i) {
            const auto& v = verts[i];
            glColor4f(v.r, v.g, v.b, v.a);
            glTexCoord2f(v.u, v.v);
            glVertex3f(v.x, v.y, v.z);
        }
        glEnd();
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
        GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_ALPHA;
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
        return tex;
    }
    void DeleteTexture(uint32_t texId) override {
        GLuint t = texId;
        glDeleteTextures(1, &t);
    }
    void BindTexture(uint32_t texId) override {
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texId);
    }
    void UnbindTexture() override {
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_TEXTURE_2D);
    }
    void UpdateTexture(uint32_t texId, int x, int y, int w, int h,
                       int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
        GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_ALPHA;
        glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, fmt, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    void ReplaceTexture(uint32_t texId, int w, int h, int channels, const void* pixels) override {
        glBindTexture(GL_TEXTURE_2D, texId);
        GLenum fmt = (channels == 4) ? GL_RGBA : (channels == 3) ? GL_RGB : GL_ALPHA;
        glTexImage2D(GL_TEXTURE_2D, 0, fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, pixels);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // ---- FBO ----
    uint32_t CreateFBO(int w, int h, uint32_t* outTex, uint32_t* outDepthRB) override {
        if (!fboAvailable) return 0;
        // 颜色纹理
        GLuint tex = CreateTexture(w, h, 4, nullptr);
        // 深度 renderbuffer
        GLuint depthRB = 0;
        glGenRB(1, &depthRB);
        glBindRB(GL_RENDERBUFFER, depthRB);
        glRBStore(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, w, h);
        glBindRB(GL_RENDERBUFFER, 0);
        // FBO
        GLuint fbo = 0;
        glGenFB(1, &fbo);
        glBindFB(GL_FRAMEBUFFER, fbo);
        glFBTex2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        if (glFBRB) glFBRB(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRB);
        unsigned int status = glCheckFB(GL_FRAMEBUFFER);
        glBindFB(GL_FRAMEBUFFER, 0);

        if (status != GL_FRAMEBUFFER_COMPLETE) {
            CC::Log(CC::LOG_ERROR, "LegacyGL: FBO incomplete (status=0x%X)", status);
            glDelFB(1, &fbo);
            GLuint t = tex; glDeleteTextures(1, &t);
            if (glDelRB) glDelRB(1, &depthRB);
            return 0;
        }
        *outTex = tex;
        *outDepthRB = depthRB;
        return fbo;
    }
    void DeleteFBO(uint32_t fbo, uint32_t tex, uint32_t depthRB) override {
        if (fbo && glDelFB) glDelFB(1, &fbo);
        if (tex) { GLuint t = tex; glDeleteTextures(1, &t); }
        if (depthRB && glDelRB) glDelRB(1, &depthRB);
    }
    void BindFBO(uint32_t fbo) override {
        if (glBindFB) glBindFB(GL_FRAMEBUFFER, fbo);
    }
    void UnbindFBO() override {
        if (glBindFB) glBindFB(GL_FRAMEBUFFER, 0);
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
};

// ==================== LegacyGLBackend 工厂 ====================

RenderBackend* CreateLegacyBackend() {
    auto* b = new LegacyGLBackend();
    if (b->Init()) return b;
    delete b;
    return nullptr;
}
