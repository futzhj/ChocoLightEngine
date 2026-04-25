/**
 * @file video_backend_mediaPlayer.cpp
 * @brief VideoBackend Android 实现 — MediaPlayer + SurfaceTexture → GL_TEXTURE_EXTERNAL_OES
 * @note 仅在 __ANDROID__ 编译
 *
 * 架构:
 *   MediaPlayer(Java) → SurfaceTexture(Java) → GL_TEXTURE_EXTERNAL_OES → 拷贝到 GL_TEXTURE_2D
 *   JNI 桥接: C++ 通过 SDL_AndroidGetJNIEnv() 获取 JNI 环境
 *
 * 帧上传流程:
 *   1. MediaPlayer 解码帧 → SurfaceTexture
 *   2. SurfaceTexture.updateTexImage() → 更新 OES 纹理
 *   3. FBO blit: OES 纹理 → 普通 TEXTURE_2D (供引擎绘制)
 */

#ifdef __ANDROID__

#include "video_backend.h"
#include "render_backend.h"
#include "light.h"

#include <SDL3/SDL.h>
#include <jni.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>  // GL_TEXTURE_EXTERNAL_OES
#include <android/log.h>

#define VB_TAG "VideoBackend"
#define VB_LOGI(...) __android_log_print(ANDROID_LOG_INFO, VB_TAG, __VA_ARGS__)
#define VB_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, VB_TAG, __VA_ARGS__)

// OES → TEXTURE_2D 拷贝着色器
static const char* s_oesVertSrc = R"GLSL(#version 300 es
layout(location=0) in vec2 aPos;
layout(location=1) in vec2 aUV;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = aUV;
}
)GLSL";

static const char* s_oesFragSrc = R"GLSL(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
in vec2 vUV;
out vec4 fragColor;
uniform samplerExternalOES uOESTex;
void main() {
    fragColor = texture(uOESTex, vUV);
}
)GLSL";

// ==================== VideoBackendMediaPlayer ====================

class VideoBackendMediaPlayer : public VideoBackend {
    RenderBackend* m_render = nullptr;

    // Java 对象 (全局引用)
    jobject m_mediaPlayer    = nullptr;  // android.media.MediaPlayer
    jobject m_surface        = nullptr;  // android.view.Surface
    jobject m_surfaceTexture = nullptr;  // android.graphics.SurfaceTexture

    // GL 资源
    GLuint  m_oesTex    = 0;   // GL_TEXTURE_EXTERNAL_OES (SurfaceTexture 目标)
    GLuint  m_tex2d     = 0;   // GL_TEXTURE_2D (引擎可用)
    GLuint  m_fbo       = 0;   // 用于 OES→2D 拷贝的 FBO
    GLuint  m_oesProgram = 0;  // OES 拷贝着色器

    int  m_width    = 0;
    int  m_height   = 0;
    bool m_opened   = false;
    bool m_playing  = false;
    bool m_finished = false;

    // JNI 方法 ID 缓存
    jmethodID m_midUpdateTexImage = nullptr;
    jmethodID m_midIsPlaying      = nullptr;

    // 编译着色器
    GLuint CompileShader(GLenum type, const char* src) {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            VB_LOGE("Shader compile error: %s", log);
            glDeleteShader(s);
            return 0;
        }
        return s;
    }

    // 初始化 OES→2D 拷贝管线
    bool InitOESCopy() {
        GLuint vs = CompileShader(GL_VERTEX_SHADER, s_oesVertSrc);
        GLuint fs = CompileShader(GL_FRAGMENT_SHADER, s_oesFragSrc);
        if (!vs || !fs) return false;

        m_oesProgram = glCreateProgram();
        glAttachShader(m_oesProgram, vs);
        glAttachShader(m_oesProgram, fs);
        glLinkProgram(m_oesProgram);
        glDeleteShader(vs);
        glDeleteShader(fs);

        GLint linked = 0;
        glGetProgramiv(m_oesProgram, GL_LINK_STATUS, &linked);
        if (!linked) {
            char log[512];
            glGetProgramInfoLog(m_oesProgram, sizeof(log), nullptr, log);
            VB_LOGE("Program link error: %s", log);
            glDeleteProgram(m_oesProgram);
            m_oesProgram = 0;
            return false;
        }
        return true;
    }

    // OES 纹理 → TEXTURE_2D (通过 FBO 拷贝)
    void CopyOESToTex2D() {
        if (!m_oesProgram || !m_oesTex || !m_tex2d || m_width <= 0) return;

        // 保存当前 FBO
        GLint prevFBO = 0;
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);
        GLint prevViewport[4];
        glGetIntegerv(GL_VIEWPORT, prevViewport);

        // 绑定拷贝 FBO → 目标 tex2d
        if (!m_fbo) {
            glGenFramebuffers(1, &m_fbo);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex2d, 0);
        glViewport(0, 0, m_width, m_height);

        // 绘制全屏四边形 (OES 纹理)
        glUseProgram(m_oesProgram);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_oesTex);
        glUniform1i(glGetUniformLocation(m_oesProgram, "uOESTex"), 0);

        // 全屏四边形顶点 (pos + uv)
        float quad[] = {
            -1, -1,  0, 0,
             1, -1,  1, 0,
            -1,  1,  0, 1,
             1,  1,  1, 1,
        };

        GLuint vao = 0, vbo = 0;
        glGenVertexArrays(1, &vao);
        glGenBuffers(1, &vbo);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glDeleteBuffers(1, &vbo);
        glDeleteVertexArrays(1, &vao);

        // 恢复
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);
        glViewport(prevViewport[0], prevViewport[1], prevViewport[2], prevViewport[3]);
        glUseProgram(0);
    }

public:
    ~VideoBackendMediaPlayer() override { Close(); }

    bool Open(const char* path, RenderBackend* render) override {
        m_render = render;

        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        if (!env) {
            VB_LOGE("Open: SDL_GetAndroidJNIEnv failed");
            return false;
        }

        // 创建 OES 纹理
        glGenTextures(1, &m_oesTex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, m_oesTex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Java: new SurfaceTexture(oesTexId)
        jclass clsST = env->FindClass("android/graphics/SurfaceTexture");
        jmethodID initST = env->GetMethodID(clsST, "<init>", "(I)V");
        jobject localST = env->NewObject(clsST, initST, (jint)m_oesTex);
        m_surfaceTexture = env->NewGlobalRef(localST);
        env->DeleteLocalRef(localST);

        // 缓存方法 ID
        m_midUpdateTexImage = env->GetMethodID(clsST, "updateTexImage", "()V");
        env->DeleteLocalRef(clsST);

        // Java: new Surface(surfaceTexture)
        jclass clsSurf = env->FindClass("android/view/Surface");
        jmethodID initSurf = env->GetMethodID(clsSurf, "<init>",
            "(Landroid/graphics/SurfaceTexture;)V");
        jobject localSurf = env->NewObject(clsSurf, initSurf, m_surfaceTexture);
        m_surface = env->NewGlobalRef(localSurf);
        env->DeleteLocalRef(localSurf);
        env->DeleteLocalRef(clsSurf);

        // Java: MediaPlayer.create(context, uri) 或 new MediaPlayer + setDataSource
        jclass clsMP = env->FindClass("android/media/MediaPlayer");
        jmethodID initMP = env->GetMethodID(clsMP, "<init>", "()V");
        jobject localMP = env->NewObject(clsMP, initMP);
        m_mediaPlayer = env->NewGlobalRef(localMP);
        env->DeleteLocalRef(localMP);

        // 缓存 isPlaying
        m_midIsPlaying = env->GetMethodID(clsMP, "isPlaying", "()Z");

        // 构建 asset:// 路径 或文件路径
        // Android assets 通过 AssetManager, 这里尝试 setDataSource(fd)
        // SDL 的 assets 路径前缀: 使用 SDL_GetBasePath() 指向 assets/
        // 对于 assets 文件, 通过 AssetManager 获取 fd
        jobject activity = (jobject)SDL_GetAndroidActivity();
        jclass clsActivity = env->GetObjectClass(activity);
        jmethodID midGetAssets = env->GetMethodID(clsActivity, "getAssets",
            "()Landroid/content/res/AssetManager;");
        jobject assetMgr = env->CallObjectMethod(activity, midGetAssets);

        // AssetManager.openFd(path) → AssetFileDescriptor
        jclass clsAM = env->GetObjectClass(assetMgr);
        jmethodID midOpenFd = env->GetMethodID(clsAM, "openFd",
            "(Ljava/lang/String;)Landroid/content/res/AssetFileDescriptor;");
        jstring jPath = env->NewStringUTF(path);

        jobject afd = nullptr;
        // openFd 可能抛异常 (文件不存在)
        afd = env->CallObjectMethod(assetMgr, midOpenFd, jPath);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            // 回退: 尝试作为绝对路径
            jmethodID midSetDS = env->GetMethodID(clsMP, "setDataSource",
                "(Ljava/lang/String;)V");
            env->CallVoidMethod(m_mediaPlayer, midSetDS, jPath);
            if (env->ExceptionCheck()) {
                env->ExceptionClear();
                VB_LOGE("Open: setDataSource failed for '%s'", path);
                env->DeleteLocalRef(jPath);
                Close();
                return false;
            }
        } else if (afd) {
            // 使用 AssetFileDescriptor
            jclass clsAFD = env->GetObjectClass(afd);
            jmethodID midGetFD = env->GetMethodID(clsAFD, "getFileDescriptor",
                "()Ljava/io/FileDescriptor;");
            jmethodID midGetOffset = env->GetMethodID(clsAFD, "getStartOffset", "()J");
            jmethodID midGetLen = env->GetMethodID(clsAFD, "getLength", "()J");

            jobject fd = env->CallObjectMethod(afd, midGetFD);
            jlong offset = env->CallLongMethod(afd, midGetOffset);
            jlong len = env->CallLongMethod(afd, midGetLen);

            jmethodID midSetDSFd = env->GetMethodID(clsMP, "setDataSource",
                "(Ljava/io/FileDescriptor;JJ)V");
            env->CallVoidMethod(m_mediaPlayer, midSetDSFd, fd, offset, len);

            env->DeleteLocalRef(fd);
            env->DeleteLocalRef(clsAFD);
            env->DeleteLocalRef(afd);
        }
        env->DeleteLocalRef(jPath);
        env->DeleteLocalRef(clsAM);
        env->DeleteLocalRef(assetMgr);
        env->DeleteLocalRef(clsActivity);

        // setSurface + prepare + start
        jmethodID midSetSurface = env->GetMethodID(
            env->GetObjectClass(m_mediaPlayer), "setSurface",
            "(Landroid/view/Surface;)V");
        env->CallVoidMethod(m_mediaPlayer, midSetSurface, m_surface);

        jmethodID midPrepare = env->GetMethodID(
            env->GetObjectClass(m_mediaPlayer), "prepare", "()V");
        env->CallVoidMethod(m_mediaPlayer, midPrepare);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            VB_LOGE("Open: MediaPlayer.prepare() failed");
            Close();
            return false;
        }

        // 获取视频尺寸
        jmethodID midGetW = env->GetMethodID(
            env->GetObjectClass(m_mediaPlayer), "getVideoWidth", "()I");
        jmethodID midGetH = env->GetMethodID(
            env->GetObjectClass(m_mediaPlayer), "getVideoHeight", "()I");
        m_width = env->CallIntMethod(m_mediaPlayer, midGetW);
        m_height = env->CallIntMethod(m_mediaPlayer, midGetH);

        env->DeleteLocalRef(clsMP);

        // 创建引擎用 TEXTURE_2D
        m_tex2d = render->CreateTexture(
            m_width > 0 ? m_width : 640,
            m_height > 0 ? m_height : 480,
            4, nullptr);

        // 初始化 OES→2D 拷贝着色器
        if (!InitOESCopy()) {
            VB_LOGE("Open: InitOESCopy failed");
            Close();
            return false;
        }

        // 开始播放
        jmethodID midStart = env->GetMethodID(
            env->FindClass("android/media/MediaPlayer"), "start", "()V");
        env->CallVoidMethod(m_mediaPlayer, midStart);

        m_opened = true;
        m_playing = true;
        VB_LOGI("Open: '%s' (%dx%d) oesTex=%u tex2d=%u", path, m_width, m_height, m_oesTex, m_tex2d);
        return true;
    }

    void Close() override {
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        if (env) {
            if (m_mediaPlayer) {
                jclass cls = env->GetObjectClass(m_mediaPlayer);
                jmethodID midRelease = env->GetMethodID(cls, "release", "()V");
                env->CallVoidMethod(m_mediaPlayer, midRelease);
                env->DeleteLocalRef(cls);
                env->DeleteGlobalRef(m_mediaPlayer);
                m_mediaPlayer = nullptr;
            }
            if (m_surface) {
                jclass cls = env->GetObjectClass(m_surface);
                jmethodID midRelease = env->GetMethodID(cls, "release", "()V");
                env->CallVoidMethod(m_surface, midRelease);
                env->DeleteLocalRef(cls);
                env->DeleteGlobalRef(m_surface);
                m_surface = nullptr;
            }
            if (m_surfaceTexture) {
                jclass cls = env->GetObjectClass(m_surfaceTexture);
                jmethodID midRelease = env->GetMethodID(cls, "release", "()V");
                env->CallVoidMethod(m_surfaceTexture, midRelease);
                env->DeleteLocalRef(cls);
                env->DeleteGlobalRef(m_surfaceTexture);
                m_surfaceTexture = nullptr;
            }
        }
        if (m_fbo) { glDeleteFramebuffers(1, &m_fbo); m_fbo = 0; }
        if (m_oesProgram) { glDeleteProgram(m_oesProgram); m_oesProgram = 0; }
        if (m_oesTex) { glDeleteTextures(1, &m_oesTex); m_oesTex = 0; }
        if (m_tex2d && m_render) { m_render->DeleteTexture(m_tex2d); m_tex2d = 0; }
        m_opened = false;
        m_playing = false;
    }

    void Update() override {
        if (!m_opened || !m_surfaceTexture) return;
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        if (!env) return;

        // SurfaceTexture.updateTexImage() → 更新 OES 纹理
        env->CallVoidMethod(m_surfaceTexture, m_midUpdateTexImage);
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            return;
        }

        // OES → TEXTURE_2D 拷贝
        CopyOESToTex2D();

        // 检查播放状态
        m_playing = (bool)env->CallBooleanMethod(m_mediaPlayer, m_midIsPlaying);
        if (!m_playing) m_finished = true;
    }

    uint32_t GetTextureId() const override { return m_tex2d; }

    void Draw(float x, float y, float w, float h) override {
        if (!m_tex2d || !m_render) return;
        m_render->PushMatrix();
        m_render->Translate(x, y, 0);
        m_render->SetColor(1, 1, 1, 1);
        m_render->BindTexture(m_tex2d);
        RenderVertex verts[4] = {
            {0, 0, 0,  0, 0,  1, 1, 1, 1},
            {w, 0, 0,  1, 0,  1, 1, 1, 1},
            {w, h, 0,  1, 1,  1, 1, 1, 1},
            {0, h, 0,  0, 1,  1, 1, 1, 1},
        };
        m_render->DrawArrays(DrawMode::Quads, verts, 4);
        m_render->UnbindTexture();
        m_render->PopMatrix();
    }

    bool IsPlaying()  const override { return m_playing; }
    bool IsFinished() const override { return m_finished; }

    void Stop() override {
        if (!m_opened || !m_mediaPlayer) return;
        JNIEnv* env = (JNIEnv*)SDL_GetAndroidJNIEnv();
        if (!env) return;
        jclass cls = env->GetObjectClass(m_mediaPlayer);
        jmethodID mid = env->GetMethodID(cls, "stop", "()V");
        env->CallVoidMethod(m_mediaPlayer, mid);
        env->DeleteLocalRef(cls);
        m_playing = false;
        m_finished = true;
    }

    int GetWidth()  const override { return m_width; }
    int GetHeight() const override { return m_height; }
};

// ==================== 工厂函数 (Android) ====================

VideoBackend* CreateVideoBackend() {
    return new VideoBackendMediaPlayer();
}

#endif // __ANDROID__
