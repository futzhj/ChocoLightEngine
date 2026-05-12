#pragma once
/**
 * @file   ssao_renderer.h
 * @brief  Phase E.8 — SSAO (Screen-Space Ambient Occlusion) 模块
 *
 * 设计原则 (与 BloomRenderer / LensFlareRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口.
 *   - 联动 HDR: 默认 autoEnable=false; HDR.Enable/Disable/Resize 可选自动拉起/关闭.
 *   - 双 RT 旁路 (用户选择 2026-05-12, 不动 HDR RT):
 *       * SSAO 自管独立 depth texture + 小 FBO (full-res, 仅 GL_DEPTH_ATTACHMENT)
 *       * 每帧 Process() 入口先用 glBlitFramebuffer 从 HDR FBO 复制 depth
 *       * AO ping-pong RT 对 (R16F, HDR RT / 2 大小, 下限 32x32)
 *       * Composite 用内部临时 RT 解 feedback loop (读 HDR + 写 HDR)
 *   - Legacy 后端不支持: Enable 返 false (SupportsSSAO 默认 no-op).
 *
 * 管线 (由 HDRRenderer::EndScene 在 Bloom 之前调; AO 必须先于 bright pass):
 *   SSAORenderer::Process(hdrFbo, hdrTex):
 *     0. BlitHDRDepthToSSAO(hdrFbo, depthFbo, srcW, srcH)        ← 旁路 depth 复制
 *     1. DrawSSAO(depthTex, noiseTex, fbos[0], rtW, rtH,
 *                  proj, invProj, kernel, kernelSize, ...)        ← raw AO (R16F)
 *     2. DrawSSAOBlur(texs[0], depthTex, fbos[1], rtW, rtH, 0)    ← 水平 blur (blurEnabled)
 *     3. DrawSSAOBlur(texs[1], depthTex, fbos[0], rtW, rtH, 1)    ← 垂直 blur
 *     4. DrawSSAOComposite(texs[0], hdrFbo, srcW, srcH, intensity) ← HDR *= mix(1,ao,intensity)
 *
 * 适用范围 (用户确认 2026-05-12):
 *   仅 3D mesh + 显式 SetDepthTest(true) 场景生效.
 *   纯 2D 场景 (z=0 平面) AO 输出全 1, 存在但不影响画面.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.8.2
 */

#include <cstdint>

class RenderBackend;

namespace SSAORenderer {

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

void SetAutoEnable(bool flag);   ///< 默认 false (同 LensDirt/Streak/LensFlare/AE)
bool GetAutoEnable();

// ==================== 参数 ====================

/// 采样半径 clamp [0.05, 5.0], 默认 0.5 (view space 单位)
void  SetRadius(float v);
float GetRadius();

/// 防自遮蔽偏移 clamp [0.0, 0.2], 默认 0.025
void  SetBias(float v);
float GetBias();

/// AO 强度乘子 clamp [0, 4.0], 默认 1.0 (composite 时用 mix(1.0, ao, intensity))
void  SetIntensity(float v);
float GetIntensity();

/// 采样 kernel 数 {8, 16}, 默认 16 (其他值 clamp 到最近)
void SetKernelSize(int n);
int  GetKernelSize();

/// AO 对比度幂 clamp [0.5, 8.0], 默认 2.0 (pow(ao, power))
void  SetPower(float v);
float GetPower();

/// 是否启用双边分离 blur, 默认 true
void SetBlurEnabled(bool flag);
bool GetBlurEnabled();

// ==================== 管线调用 ====================

/**
 * @brief 执行 SSAO 完整管线: blit depth -> raw AO -> blur (2 pass) -> composite
 *
 * 内部自检 + 静默 no-op 条件:
 *   - g.enabled / g.supported / backend / hdrFbo / hdrTex 任一为 0/false
 *   - g.depthFbo / g.depthTex / g.fbos[0] / g.fbos[1] / g.noiseTex 任一为 0
 *
 * 内部自动从 backend->GetProjection / GetView 取矩阵; invProj 由 Mat4::Inverse 计算.
 *
 * 双 RT 旁路: 调用方不再传 depthTex; SSAO 管自己的 depthTex.
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

} // namespace SSAORenderer
