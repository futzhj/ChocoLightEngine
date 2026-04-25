/**
 * @file video_backend_avplayer.mm
 * @brief VideoBackend iOS 实现 — AVPlayer + CVPixelBuffer → OpenGLES 纹理
 * @note 仅在 CHOCO_PLATFORM_IOS 编译
 *
 * 架构:
 *   AVPlayer → AVPlayerItemVideoOutput → CVPixelBuffer → glTexImage2D → TEXTURE_2D
 *   每帧通过 CVPixelBufferLockBaseAddress 取像素, 上传到引擎纹理
 */

#ifdef CHOCO_PLATFORM_IOS

#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>

#include "video_backend.h"
#include "render_backend.h"
#include "light.h"
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>

// GLES3 可能不定义 GL_BGRA_EXT (Apple 扩展值 0x80E1)
#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif

// ==================== VideoBackendAVPlayer ====================

class VideoBackendAVPlayer : public VideoBackend {
    RenderBackend* m_render = nullptr;

    // AV 对象 (用 __bridge_retained 持有)
    void* m_player     = nullptr;  // AVPlayer*
    void* m_playerItem = nullptr;  // AVPlayerItem*
    void* m_videoOutput = nullptr; // AVPlayerItemVideoOutput*

    uint32_t m_texId   = 0;
    int      m_width   = 0;
    int      m_height  = 0;
    bool     m_opened  = false;
    bool     m_playing = false;

public:
    ~VideoBackendAVPlayer() override { Close(); }

    bool Open(const char* path, RenderBackend* render) override {
        m_render = render;

        @autoreleasepool {
            // 构建 URL: 先尝试 bundle 路径, 再尝试绝对路径
            NSString* nsPath = [NSString stringWithUTF8String:path];
            NSURL* url = nil;

            // 检查 bundle 中的 assets
            NSString* bundlePath = [[NSBundle mainBundle]
                pathForResource:[nsPath stringByDeletingPathExtension]
                ofType:[nsPath pathExtension]
                inDirectory:@"assets"];
            if (bundlePath) {
                url = [NSURL fileURLWithPath:bundlePath];
            } else {
                // 绝对路径或 URL
                if ([nsPath hasPrefix:@"http"]) {
                    url = [NSURL URLWithString:nsPath];
                } else {
                    url = [NSURL fileURLWithPath:nsPath];
                }
            }

            // 创建 AVPlayerItem + AVPlayer
            AVPlayerItem* item = [AVPlayerItem playerItemWithURL:url];
            AVPlayer* player = [AVPlayer playerWithPlayerItem:item];

            // 配置视频输出 (BGRA 格式, 直接可上传 GL)
            NSDictionary* attrs = @{
                (NSString*)kCVPixelBufferPixelFormatTypeKey:
                    @(kCVPixelFormatType_32BGRA)
            };
            AVPlayerItemVideoOutput* output =
                [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:attrs];
            [item addOutput:output];

            // 等待 tracks 加载
            NSArray<AVAssetTrack*>* videoTracks =
                [item.asset tracksWithMediaType:AVMediaTypeVideo];
            if (videoTracks.count > 0) {
                CGSize size = videoTracks[0].naturalSize;
                m_width = (int)size.width;
                m_height = (int)size.height;
            }

            // 持有引用
            m_player = (__bridge_retained void*)player;
            m_playerItem = (__bridge_retained void*)item;
            m_videoOutput = (__bridge_retained void*)output;

            // 创建引擎纹理
            m_texId = render->CreateTexture(
                m_width > 0 ? m_width : 640,
                m_height > 0 ? m_height : 480,
                4, nullptr);

            // 开始播放
            [player play];
            m_opened = true;
            m_playing = true;

            CC::Log(CC::LOG_INFO, "Video(AVPlayer): opened '%s' (%dx%d) tex=%u",
                     path, m_width, m_height, m_texId);
        }
        return true;
    }

    void Close() override {
        @autoreleasepool {
            if (m_player) {
                AVPlayer* player = (__bridge_transfer AVPlayer*)m_player;
                [player pause];
                m_player = nullptr;
            }
            if (m_playerItem) {
                AVPlayerItem* item = (__bridge_transfer AVPlayerItem*)m_playerItem;
                if (m_videoOutput) {
                    AVPlayerItemVideoOutput* output =
                        (__bridge_transfer AVPlayerItemVideoOutput*)m_videoOutput;
                    [item removeOutput:output];
                    m_videoOutput = nullptr;
                }
                m_playerItem = nullptr;
            }
        }
        if (m_texId && m_render) { m_render->DeleteTexture(m_texId); m_texId = 0; }
        m_opened = false;
        m_playing = false;
    }

    void Update() override {
        if (!m_opened || !m_videoOutput) return;

        @autoreleasepool {
            AVPlayerItemVideoOutput* output =
                (__bridge AVPlayerItemVideoOutput*)m_videoOutput;
            AVPlayerItem* item = (__bridge AVPlayerItem*)m_playerItem;

            CMTime time = item.currentTime;
            if (![output hasNewPixelBufferForItemTime:time]) return;

            CVPixelBufferRef pixelBuffer =
                [output copyPixelBufferForItemTime:time itemTimeForDisplay:nil];
            if (!pixelBuffer) return;

            CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            void* baseAddr = CVPixelBufferGetBaseAddress(pixelBuffer);
            int w = (int)CVPixelBufferGetWidth(pixelBuffer);
            int h = (int)CVPixelBufferGetHeight(pixelBuffer);

            // 尺寸变化时重建纹理
            if (w != m_width || h != m_height) {
                m_width = w;
                m_height = h;
                if (m_texId) m_render->DeleteTexture(m_texId);
                m_texId = m_render->CreateTexture(m_width, m_height, 4, nullptr);
            }

            // BGRA 数据上传到 GL 纹理
            glBindTexture(GL_TEXTURE_2D, m_texId);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, baseAddr);

            CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            CVPixelBufferRelease(pixelBuffer);

            // 检查播放状态
            AVPlayer* player = (__bridge AVPlayer*)m_player;
            m_playing = (player.rate > 0.0f);
        }
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

    bool IsPlaying() const override { return m_playing; }

    bool IsFinished() const override {
        if (!m_playerItem) return true;
        @autoreleasepool {
            AVPlayerItem* item = (__bridge AVPlayerItem*)m_playerItem;
            return CMTimeCompare(item.currentTime, item.duration) >= 0;
        }
    }

    void Stop() override {
        if (!m_player) return;
        @autoreleasepool {
            AVPlayer* player = (__bridge AVPlayer*)m_player;
            [player pause];
            [player seekToTime:kCMTimeZero];
        }
        m_playing = false;
    }

    int GetWidth()  const override { return m_width; }
    int GetHeight() const override { return m_height; }
};

// ==================== 工厂函数 (iOS) ====================

VideoBackend* CreateVideoBackend() {
    return new VideoBackendAVPlayer();
}

#endif // CHOCO_PLATFORM_IOS
