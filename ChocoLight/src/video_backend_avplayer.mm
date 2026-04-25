/**
 * @file video_backend_avplayer.mm
 * @brief VideoBackend iOS 实现 — AVPlayer + OpenGLES 纹理上传
 * @note 仅在 CHOCO_PLATFORM_IOS 编译
 *       当前为存根实现, 返回 nullptr (视频功能待后续版本)
 */

#ifdef CHOCO_PLATFORM_IOS

#include "video_backend.h"

// TODO: 完整实现 AVPlayer + CVOpenGLESTextureCacheCreateTextureFromImage
// 需要: <AVFoundation/AVFoundation.h>, <CoreVideo/CVOpenGLESTextureCache.h>

VideoBackend* CreateVideoBackend() {
    return nullptr;
}

#endif // CHOCO_PLATFORM_IOS
