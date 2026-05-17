#pragma once
/**
 * @file   lens_flare_renderer.h
 * @brief  Phase E.7 — Lens Flare (Ghost + Halo + Chromatic Aberration) 模块
 *
 * 设计原则 (与 BloomRenderer / StreakRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口.
 *   - 联动 HDR: 默认 autoEnable=false; HDR.Enable/Disable/Resize 可选自动拉起/关闭.
 *   - 持有资源: ping-pong RT 对 (2 个 RGBA16F, HDR RT / 2 大小, 下限 32x32).
 *   - 复用 Bloom: bright pass + composite 直接调 backend 的 Bloom 虚接口
 *                 (DrawBloomBrightPass / DrawBloomComposite).
 *   - Legacy 后端不支持: Enable 返 false.
 *
 * 管线 (由 HDRRenderer::EndScene 在 Streak 之后调):
 *   LensFlareRenderer::Process(hdrFbo, hdrTex)
 *     1. DrawBloomBrightPass(hdrTex -> lfRT[0], threshold)            (复用)
 *     2. DrawLensFlareGhost(lfTex[0] -> lfFbo[1],
 *                           w, h, ghostCount, dispersal, haloWidth,
 *                           chromaticAberration, distortionEnabled)
 *     3. DrawBloomComposite(lfTex[1] -> hdrFbo, intensity)             (复用 additive)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.7.2
 */

#include <cstdint>

class RenderBackend;

namespace LensFlareRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend);
void Shutdown();

bool Enable(int w, int h);
void Disable();
bool IsEnabled();
bool IsSupported();
bool Resize(int w, int h);

// ==================== HDR 联动 (内部 API) ====================

void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);

void SetAutoEnable(bool flag);   ///< 默认 false (同 LensDirt/Streak/AE)
bool GetAutoEnable();

// ==================== 参数 ====================

/// 亮度阈值 clamp [0, +inf), 默认 1.0
void  SetThreshold(float v);
float GetThreshold();

/// 合成强度 clamp [0, +inf), 默认 0.4
void  SetIntensity(float v);
float GetIntensity();

/// Ghost 数量 clamp [0, 8], 默认 4; 0 = 关 ghost 只留 halo
void SetGhostCount(int n);
int  GetGhostCount();

/// Ghost 径向缩放 clamp [0, 2.0], 默认 0.4
void  SetGhostDispersal(float v);
float GetGhostDispersal();

/// Halo 环形半径 clamp [0, 1.0] UV, 默认 0.5; 0 = 关 halo
void  SetHaloWidth(float v);
float GetHaloWidth();

/// 色差偏移 clamp [0, 0.02], 默认 0.005
void  SetChromaticAberration(float v);
float GetChromaticAberration();

/// 是否启用色差 (false 时 RGB 同采, 省 ~3x 带宽)
void SetDistortionEnabled(bool flag);
bool GetDistortionEnabled();

// ==================== Phase E.7.4 — 用户贴图 ====================

/**
 * @brief 设置用户提供的 lens flare 贴图 GL tex id
 * @param texId  GL tex id; 0 = 用 1x1 白 fallback (回到纯 procedural 行为)
 *
 * 行为: shader 在每个像素采 flareTex(vUV).rgb 后整体相乘 result.
 *       1x1 白 fallback 使 result 不变, 完全向后兼容.
 *       用户可提供:
 *         - 纯白图     -> 等于纯 procedural (但浪费 1 个采样)
 *         - 渐变 LUT    -> 染色 ghost (类似 grading)
 *         - 中心亮 vignette -> 边缘衰减
 *         - 星芒贴图   -> star flare 视觉
 *         - 彩虹纹理   -> 极致幻彩 ghost
 *
 * 注意: tex id 仅存 uint32_t; Lua 端 Image userdata 由用户负责生命周期.
 *       绑定后需保持 GL 资源有效, 否则下次 Process 会读到无效纹理.
 */
void     SetFlareTextureId(uint32_t texId);
uint32_t GetFlareTextureId();

// ==================== 管线调用 ====================

/**
 * @brief 执行 lens flare 完整管线: bright + ghost+halo+aberration + composite
 *
 * 内部自检 IsEnabled + Bloom.SupportsBloom (bright/composite 依赖).
 * 未启用 / 资源失效 / hdrTex 无效 时 no-op.
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

// ==================== Phase F.2.2 — Multi-Instance ====================
// 4 instance: default + 3 user. 每 instance 独立 ping-pong RT + 8 个 ghost/halo 参数 + flareTexId.
int  CreateInstance();
bool DestroyInstance(int id);
bool SetActiveInstance(int id);
int  GetActiveInstance();
int  GetInstanceCount();
int  CloneInstance(int srcId);

} // namespace LensFlareRenderer
