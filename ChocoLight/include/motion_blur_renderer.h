#pragma once
/**
 * @file   motion_blur_renderer.h
 * @brief  Phase E.15 — Velocity-driven Motion Blur 后处理命名空间模块
 *
 * 设计原则 (与 BloomRenderer / SSRRenderer 同模式):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsMotionBlur / CreateMotionBlurRT / DeleteMotionBlurRT / DrawMotionBlur)
 *   - 联动 HDR: AutoEnable=false (默认); 用户必须显式 Enable. HDR.Enable/Disable/Resize
 *               触发 OnHDREnabled/Disabled/Resized 内部回调
 *   - 显式可控: Lua Light.Graphics.MotionBlur.Enable / Disable / Set*
 *   - 向后兼容: 未 Enable 时所有 API 静默 no-op (HDR 管线仍工作, 无 motion blur)
 *   - Legacy 后端不支持: Enable() 返回 false + warn log
 *   - 纯壳模式: 模块本身不做 GL 调用, 全部转发给 backend
 *
 * 管线流程 (MotionBlur Enabled, 由 HDRRenderer::EndScene 内部调用):
 *   HDRRenderer::EndScene
 *     ↓ Bloom / LensDirt / Streak / SSAO / SSR / LensFlare 累积到 hdrFbo
 *     ↓ MotionBlurRenderer::Process(hdrFbo, hdrTex)
 *       ├─ Pass1 (shader): 沿 velocity 多采样 hdrTex → 写 motionBlurTex
 *       └─ Pass2 (blit):    motionBlurTex → 覆盖 hdrTex 内容
 *     ↓ DrawTonemapFullscreen(hdrTex, ...) → default fb
 *
 * 资源持有: motionBlurFbo + motionBlurTex (RGBA16F, 与 sceneTex 同尺寸)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.15
 */

#include <cstdint>

class RenderBackend;

namespace MotionBlurRenderer {

// ==================== 生命周期 ====================

/// 初始化 (缓存 backend 指针; 不分配 GPU 资源)
void Init(RenderBackend* backend);

/// 关闭 + 释放资源 (DeleteMotionBlurRT 配对; 清 flag)
void Shutdown();

// ==================== Enable / Disable / Resize ====================

/// 启用: 创建 motionBlurFbo + motionBlurTex (RGBA16F, w x h)
/// @return true = 成功; false = 后端不支持 / 参数非法 / 资源分配失败
bool Enable(int w, int h);

/// 关闭: 释放 motionBlurFbo + motionBlurTex
void Disable();

/// 是否已启用 (Enable 成功且未 Disable)
bool IsEnabled();

/// 后端是否支持 motion blur (shader 编译成功; Init 未调时返 false)
bool IsSupported();

/// 重建 RT 到新大小 (内部 = Disable + Enable)
bool Resize(int w, int h);

// ==================== HDR 自动联动 (内部 API, 不暴露 Lua) ====================

/// HDR.Enable 成功后回调; autoEnable=true 时自动 Enable (默认 false → no-op)
void OnHDREnabled(int w, int h);

/// HDR.Disable 回调: 强制 Disable (无视 autoEnable)
void OnHDRDisabled();

/// HDR.Resize 回调: 已启用时同步 Resize, 否则 no-op
void OnHDRResized(int w, int h);

/// 设置 HDR 联动开关 (默认 false; 与 LensDirt/SSAO/SSR 一致, Bloom 是唯一例外)
void SetAutoEnable(bool flag);
bool GetAutoEnable();

// ==================== 参数 ====================

/// 强度 (默认 1.0; clamp [0, 4])
/// 1.0 = velocity buffer 中位移直接做 blur, > 1.0 加强, 0 = 关闭 blur
void  SetStrength(float v);
float GetStrength();

/// 沿 velocity 采样数 (默认 8; clamp [1, 32])
/// 高质量 16~32, 性能优先 4~8
void SetSampleCount(int n);
int  GetSampleCount();

/// Phase E.16 — motion blur 模式 (默认 0; clamp [0, 2])
///   0 = combined     (camera + object 合一，与 Phase E.15 行为一致)
///   1 = camera_only  (仅相机运动造成的拖尾，物体在屏幕静止时不拖)
///   2 = object_only  (仅物体运动造成的拖尾，近似等于 combined - camera)
void SetMode(int m);
int  GetMode();

/// Phase E.17 — half-res motion blur 开关 (默认 false)
///   true  = motionBlurTex 改为 ((w+1)/2, (h+1)/2)，Pass2 用 GL_LINEAR 上采样
///           VRAM -75% (1080p RGBA16F: 8MB → 2MB), Pass1 fragment 性能 ~4×
///           视觉损失 ≤ 5%，依靠 bilinear 的低通效应自然平滑
///   false = full-res (与 Phase E.15/E.16 一致)
/// 已 Enable 时切换 → 立即 Resize 重建 RT; 未 Enable 时 → 下次 Enable 生效
void SetHalfRes(bool flag);
bool GetHalfRes();

// ==================== 管线调用 (HDRRenderer::EndScene 内部调) ====================

/**
 * @brief 执行 motion blur: Pass1 sceneTex+velocity→motionBlurTex, Pass2 blit 覆盖 sceneTex
 *
 * 内部自检 IsEnabled + backend + hdrFbo + hdrTex + velocityTex; 任一无效 silent skip.
 * @param hdrFbo  HDR FBO id (输出目标)
 * @param hdrTex  HDR scene 颜色 tex (输入 + 被覆盖)
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

/**
 * @brief Phase F.0.10.3 — Region 限定 motion blur (split-screen 必备 overload)
 *
 * 与无 region 版语义等价 (rgnW=0 || rgnH=0 时退化为全屏路径), 区别:
 *   1. Pass1 shader scissor 限定写 storage 空间子矩形
 *   2. Pass2 blit 用 src=(storageRgn) → dst=(rgn) sub-rect blit
 *   3. half-res storage (HalfRes=true) 时 storage region = dst region / 2
 *
 * 典型用法 (split-screen, HDR.SetAutoMotionBlur(false) 后手动调):
 *   MotionBlur.Process(0,    0, W/2, H)   -- 左半屏 player 1
 *   MotionBlur.Process(W/2,  0, W/2, H)   -- 右半屏 player 2
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH);

} // namespace MotionBlurRenderer
