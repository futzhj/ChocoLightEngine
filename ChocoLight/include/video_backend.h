/**
 * @file video_backend.h
 * @brief 跨平台视频后端抽象接口
 * @note 与 RenderBackend 同风格, 平台选择由工厂函数自动决定
 *
 * 实现矩阵:
 *   桌面 (Win/Linux/macOS): VideoBackendFFmpeg  (FFmpeg 动态加载)
 *   Web  (Emscripten):      VideoBackendHTML5   (EM_JS + <video> + texImage2D)
 *   iOS:                    VideoBackendAVPlayer (AVPlayer + CVPixelBuffer)
 *   Android:                VideoBackendMedia    (JNI MediaPlayer + SurfaceTexture)
 *
 * 使用方式:
 *   VideoBackend* vb = CreateVideoBackend();
 *   vb->Open("intro.mp4", g_render);
 *   while (vb->IsPlaying()) { vb->Update(); vb->Draw(0,0,w,h); }
 *   vb->Close();
 *   delete vb;
 */
#pragma once

#include <cstdint>

class RenderBackend;

// ==================== 视频后端抽象 ====================

class VideoBackend {
public:
    virtual ~VideoBackend() = default;

    // ---- 生命周期 ----

    /// @brief 打开视频文件, 初始化解码器和 GL 纹理
    /// @param path 视频文件路径 (UTF-8)
    /// @param render 渲染后端 (用于创建/更新纹理)
    /// @return true=成功, false=失败 (CC::Log 输出详情)
    virtual bool Open(const char* path, RenderBackend* render) = 0;

    /// @brief 释放所有资源 (解码器 + 纹理 + 音频)
    virtual void Close() = 0;

    // ---- 每帧更新 ----

    /// @brief 解码下一帧 → 转换色彩空间 → 上传 GL 纹理
    /// @note 内部自带帧率控制, 未到时间则跳过解码
    virtual void Update() = 0;

    // ---- 绘制 ----

    /// @brief 获取当前帧的 GL 纹理 ID (供外部绘制)
    virtual uint32_t GetTextureId() const = 0;

    /// @brief 便捷绘制: 用渲染后端绘制视频帧四边形
    /// @param x,y 屏幕位置
    /// @param w,h 绘制尺寸 (默认=视频原始尺寸)
    virtual void Draw(float x, float y, float w, float h) = 0;

    // ---- 播放控制 ----

    virtual bool IsPlaying() const = 0;
    virtual bool IsFinished() const = 0;
    virtual void Stop() = 0;

    // ---- 属性查询 ----

    virtual int GetWidth() const = 0;
    virtual int GetHeight() const = 0;
};

// ==================== 工厂函数 ====================

/// @brief 根据当前平台创建视频后端
/// @return 新实例 (调用者拥有所有权, 需 delete)
///   桌面: VideoBackendFFmpeg
///   Web:  VideoBackendHTML5
///   iOS:  VideoBackendAVPlayer (M4)
///   Android: VideoBackendMedia (M3)
VideoBackend* CreateVideoBackend();
