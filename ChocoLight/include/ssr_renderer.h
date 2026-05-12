#pragma once
/**
 * @file   ssr_renderer.h
 * @brief  Phase E.9 — SSR (Screen-Space Reflection) 模块
 *
 * 设计原则 (与 SSAORenderer / BloomRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口.
 *   - 联动 HDR: 默认 autoEnable=false; HDR.Enable/Disable/Resize 可选自动拉起/关闭.
 *   - 高质量方案 (用户拍板 2026-05-12): full-res RGBA16F + 64 步 ray march
 *       * SSR 自管独立 depth texture + 小 FBO (full-res, 仅 GL_DEPTH_ATTACHMENT)
 *       * 每帧 Process() 入口先用 glBlitFramebuffer 从 HDR FBO 复制 depth (复用 BlitHDRDepthToSSAO 接口)
 *       * 反射 RT 单 RGBA16F (full-res, 与 HDR RT 同尺寸)
 *       * Composite 用内部临时 RT 解 feedback loop (读 HDR + 加性写 HDR)
 *   - Legacy 后端不支持: Enable 返 false (SupportsSSR 默认 no-op).
 *
 * 复用 Phase E.8.x G-buffer view-space normal:
 *   - 从 backend->GetHDRNormalTex(hdrFbo) 取 RG16F normal slot 1
 *   - 缺 normalTex 时 once-warn 并 silent skip (不影响其他后处理)
 *
 * 管线 (由 HDRRenderer::EndScene 在 Bloom 之前、SSAO 之后调):
 *   SSRRenderer::Process(hdrFbo, hdrTex):
 *     0. BlitHDRDepthToSSAO(hdrFbo, depthFbo, srcW, srcH)         ← 旁路 depth 复制 (复用 SSAO 接口)
 *     1. GetHDRNormalTex(hdrFbo) -> normalTex  (缺则 silent skip)
 *     2. DrawSSR(depthTex, normalTex, hdrTex, reflectFbo, w, h,
 *                 proj, invProj, maxSteps, stepSize, thickness,
 *                 maxDist, edgeFade)                              ← raw reflection (RGBA16F)
 *     3. DrawSSRComposite(reflectTex, hdrFbo, w, h, intensity)    ← HDR += reflect.rgb * reflect.a * intensity
 *
 * 适用范围:
 *   仅 3D mesh + 显式 SetDepthTest(true) 场景生效.
 *   纯 2D 场景 (z=0 平面) 反射全 0, 不影响画面.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.9
 */

#include <cstdint>

class RenderBackend;

namespace SSRRenderer {

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

void SetAutoEnable(bool flag);   ///< 默认 false (同 SSAO/LensDirt/Streak/AE)
bool GetAutoEnable();

// ==================== 参数 (7 对 setter/getter) ====================

/// ray march 步数 clamp [8, 128], 默认 64
void SetMaxSteps(int n);
int  GetMaxSteps();

/// 每步 view-space 距离 clamp [0.01, 1.0], 默认 0.1
void  SetStepSize(float v);
float GetStepSize();

/// 深度命中容差 clamp [0.01, 5.0], 默认 0.5 (view space 单位)
void  SetThickness(float v);
float GetThickness();

/// ray march 距离上限 clamp [1.0, 1000.0], 默认 50.0
void  SetMaxDistance(float v);
float GetMaxDistance();

/// composite 强度乘子 clamp [0.0, 2.0], 默认 0.7 (HDR += reflect.rgb * reflect.a * intensity)
void  SetIntensity(float v);
float GetIntensity();

/// 屏幕边缘 fade 区域宽度 clamp [0.0, 0.5], 默认 0.1
void  SetEdgeFade(float v);
float GetEdgeFade();

/// 是否启用反射 blur, 默认 false. Phase E.10 已激活: 启用后 Process 执行 H+V 两 pass 高斯模糊.
void SetBlurEnabled(bool flag);
bool GetBlurEnabled();

/// Phase E.10 — 反射模糊半径 (texel 空间), clamp [0.5, 4.0], 默认 1.5.
/// 仅在 BlurEnabled=true 时生效. 调大模拟更粗糙的金属表面.
void  SetBlurRadius(float v);
float GetBlurRadius();

// ==================== 调试 API ====================

/// 当前反射 RT id (0 = 未启用), 供 Lua 端可视化反射纹理
uint32_t GetReflectionTexId();

// ==================== 管线调用 ====================

/**
 * @brief 执行 SSR 完整管线: blit depth -> SSR raw -> composite
 *
 * 内部自检 + 静默 no-op 条件:
 *   - g.enabled / g.supported / backend / hdrFbo / hdrTex 任一为 0/false
 *   - g.depthFbo / g.depthTex / g.reflectFbo / g.reflectTex 任一为 0
 *   - GetHDRNormalTex(hdrFbo) 返 0 (Phase E.8.x normal MRT 未启用) → once warn + skip
 *
 * 内部自动从 backend->GetProjection 取矩阵; invProj 由 Mat4::Inverse 计算.
 * normalTex 不缓存, 每帧重新查询 (尊重 HDR FBO 重建).
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

} // namespace SSRRenderer
