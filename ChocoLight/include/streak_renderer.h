#pragma once
/**
 * @file   streak_renderer.h
 * @brief  Phase E.6 — Streak (Anamorphic Flare) 横向条纹光晕模块
 *
 * 设计原则 (与 BloomRenderer / AutoExposureRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口 (5 个 Streak 虚接口).
 *   - 联动 HDR: 默认 AutoEnable=false; HDR.Enable/Disable/Resize 可选自动拉起/关闭.
 *   - 持有资源: ping-pong RT 对 (2 个 RGBA16F, HDR RT / 2 大小).
 *   - 依赖 Bloom: v1 实现 Bright Pass 复用 Bloom programBloomBright; 若 Bloom
 *     后端不支持, Streak Process 降级 (DrawStreakBright 内部自检 bloomSupported).
 *   - Legacy 后端不支持: Enable() 返回 false.
 *
 * 管线流程 (Streak Enabled, 由 HDRRenderer::EndScene 内部调用):
 *   StreakRenderer::Process(hdrFbo, hdrTex)
 *     1. DrawStreakBright(hdrTex -> streakRT[0], threshold)
 *     2. for i in 1..iterations: DrawStreakBlur(streakRT[src] -> streakRT[dst],
 *                                                length × 2^(i-1), direction)
 *     3. DrawStreakComposite(streakRT[final] additive -> hdrFbo, intensity)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.6.2
 */

#include <cstdint>

class RenderBackend;

namespace StreakRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend);
void Shutdown();

// ==================== Enable / Disable / Resize ====================

/**
 * @brief 启用 Streak, 分配 ping-pong RT 对
 *
 * @param w, h  HDR RT 尺寸 (内部 backend 会按 /2 创建, 下限 32x32)
 * @return true 成功; false 失败 (backend 不支持 / 参数非法 / 资源失败)
 */
bool Enable(int w, int h);

void Disable();
bool IsEnabled();
bool IsSupported();

/**
 * @brief 重建 RT 到新尺寸 (同尺寸 no-op)
 */
bool Resize(int w, int h);

// ==================== HDR 联动 (内部) ====================

void OnHDREnabled(int w, int h);
void OnHDRDisabled();
void OnHDRResized(int w, int h);

void SetAutoEnable(bool flag);   ///< 默认 false
bool GetAutoEnable();

// ==================== 参数 ====================

/// 亮度阈值 clamp [0, +inf), 默认 1.0
void  SetThreshold(float v);
float GetThreshold();

/// 合成强度 clamp [0, +inf), 默认 0.3
void  SetIntensity(float v);
float GetIntensity();

/// 单步 UV 距离 clamp [0, 0.1], 默认 0.02
void  SetLength(float v);
float GetLength();

/**
 * @brief 方向向量 (shader 内 normalize)
 *
 * (0, 0) 会被拒绝 (保留旧值), 防 NaN.
 * 默认 (1.0, 0.0) 水平.
 */
void SetDirection(float x, float y);

/// 多返回: outX, outY
void GetDirection(float& outX, float& outY);

/// 迭代次数 clamp [1, 8], 默认 5; 步长倍距扩展 (2^i)
void SetIterations(int n);
int  GetIterations();

// ==================== 管线调用 ====================

/**
 * @brief 执行 streak 完整管线: bright + N iteration blur + composite
 *
 * 内部自检 IsEnabled + Bloom.SupportsBloom (bright pass 依赖).
 * 未启用 / 资源失效 / bloomTex 无效 时 no-op.
 *
 * @param hdrFbo  HDR RT FBO id (最终 composite 目标)
 * @param hdrTex  HDR RT 颜色 tex (bright pass 输入)
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

} // namespace StreakRenderer
