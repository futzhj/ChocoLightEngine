/**
 * @file video_backend_html5.cpp
 * @brief VideoBackend HTML5 实现 — Web/Emscripten 平台
 * @note 利用浏览器 <video> 元素解码, gl.texImage2D 上传帧到 WebGL 纹理
 *       音频由 <video> 元素原生处理, 无需单独管理
 */

#ifdef __EMSCRIPTEN__

#include "video_backend.h"
#include "render_backend.h"
#include "light.h"
#include <emscripten.h>
#include <emscripten/html5.h>
#include <GLES3/gl3.h>

// ==================== JS 桥接 (EM_JS) ====================

// 创建 <video> 元素, 返回 JS 端 ID
EM_JS(int, js_video_create, (const char* pathPtr), {
    var path = UTF8ToString(pathPtr);
    var video = document.createElement('video');
    video.src = path;
    video.crossOrigin = 'anonymous';
    video.playsInline = true;
    video.preload = 'auto';
    video.style.display = 'none';
    document.body.appendChild(video);
    if (!Module._videos) Module._videos = [];
    var id = Module._videos.length;
    Module._videos.push(video);
    return id;
});

EM_JS(void, js_video_play, (int id), {
    if (Module._videos && Module._videos[id]) {
        Module._videos[id].play().catch(function(e) {
            console.warn('Video play failed:', e.message);
        });
    }
});

EM_JS(int, js_video_is_playing, (int id), {
    if (!Module._videos || !Module._videos[id]) return 0;
    var v = Module._videos[id];
    return (!v.paused && !v.ended) ? 1 : 0;
});

EM_JS(int, js_video_is_ended, (int id), {
    if (!Module._videos || !Module._videos[id]) return 1;
    return Module._videos[id].ended ? 1 : 0;
});

EM_JS(int, js_video_get_width, (int id), {
    if (!Module._videos || !Module._videos[id]) return 0;
    return Module._videos[id].videoWidth || 0;
});

EM_JS(int, js_video_get_height, (int id), {
    if (!Module._videos || !Module._videos[id]) return 0;
    return Module._videos[id].videoHeight || 0;
});

EM_JS(void, js_video_stop, (int id), {
    if (Module._videos && Module._videos[id]) {
        Module._videos[id].pause();
        Module._videos[id].currentTime = 0;
    }
});

EM_JS(void, js_video_destroy, (int id), {
    if (Module._videos && Module._videos[id]) {
        Module._videos[id].pause();
        Module._videos[id].remove();
        Module._videos[id] = null;
    }
});

// 将 <video> 当前帧上传到指定 GL 纹理
// 利用 Emscripten 内部 GL.textures 映射获取 WebGL 纹理对象
EM_JS(int, js_video_upload_frame, (int id, int glTexId), {
    if (!Module._videos || !Module._videos[id]) return 0;
    var video = Module._videos[id];
    if (video.readyState < 2) return 0; // HAVE_CURRENT_DATA
    var gl = Module['ctx'] || Module['canvas'].getContext('webgl2');
    if (!gl) return 0;
    var tex = GL.textures[glTexId];
    if (!tex) return 0;
    gl.bindTexture(gl.TEXTURE_2D, tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, video);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    return 1;
});

// ==================== VideoBackendHTML5 ====================

class VideoBackendHTML5 : public VideoBackend {
    RenderBackend* m_render = nullptr;
    int      m_videoJsId = -1;
    uint32_t m_texId  = 0;
    int      m_width  = 0;
    int      m_height = 0;
    bool     m_opened = false;

public:
    ~VideoBackendHTML5() override { Close(); }

    bool Open(const char* path, RenderBackend* render) override {
        m_render = render;
        m_videoJsId = js_video_create(path);
        if (m_videoJsId < 0) {
            CC::Log(CC::LOG_WARN, "Video(HTML5): failed to create <video> for '%s'", path);
            return false;
        }
        // 占位纹理 (尺寸在首帧到达后更新)
        m_texId = render->CreateTexture(1, 1, 4, nullptr);
        js_video_play(m_videoJsId);
        m_opened = true;
        CC::Log(CC::LOG_INFO, "Video(HTML5): opened '%s' (jsId=%d)", path, m_videoJsId);
        return true;
    }

    void Close() override {
        if (m_videoJsId >= 0) { js_video_destroy(m_videoJsId); m_videoJsId = -1; }
        if (m_texId && m_render) { m_render->DeleteTexture(m_texId); m_texId = 0; }
        m_opened = false;
    }

    void Update() override {
        if (!m_opened || m_videoJsId < 0) return;
        // 检测视频尺寸 (首帧加载后生效)
        int w = js_video_get_width(m_videoJsId);
        int h = js_video_get_height(m_videoJsId);
        if (w > 0 && h > 0 && (w != m_width || h != m_height)) {
            m_width = w; m_height = h;
            // 重建纹理以匹配视频尺寸
            if (m_texId) m_render->DeleteTexture(m_texId);
            m_texId = m_render->CreateTexture(m_width, m_height, 4, nullptr);
            CC::Log(CC::LOG_INFO, "Video(HTML5): texture resized %dx%d", m_width, m_height);
        }
        // 浏览器解码帧 → texImage2D → GL 纹理
        js_video_upload_frame(m_videoJsId, (int)m_texId);
    }

    uint32_t GetTextureId() const override { return m_texId; }

    void Draw(float x, float y, float w, float h) override {
        if (!m_texId || !m_render) return;
        m_render->PushMatrix();
        m_render->Translate(x, y, 0);
        m_render->SetColor(1, 1, 1, 1);
        m_render->BindTexture(m_texId);
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

    bool IsPlaying()  const override { return m_opened && js_video_is_playing(m_videoJsId); }
    bool IsFinished() const override { return !m_opened || js_video_is_ended(m_videoJsId); }
    void Stop() override { if (m_videoJsId >= 0) js_video_stop(m_videoJsId); }
    int  GetWidth()   const override { return m_width; }
    int  GetHeight()  const override { return m_height; }
};

// ==================== 工厂函数 (Emscripten) ====================

VideoBackend* CreateVideoBackend() {
    return new VideoBackendHTML5();
}

#endif // __EMSCRIPTEN__
