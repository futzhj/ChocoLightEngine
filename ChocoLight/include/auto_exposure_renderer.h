#pragma once
/**
 * @file   auto_exposure_renderer.h
 * @brief  Phase E.5 — Auto Exposure (Eye Adaptation) 命名空间模块
 *
 * 设计原则 (与 BloomRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsAutoExposure / CreateLuminanceTarget / DeleteLuminanceTarget /
 *      DrawLuminanceExtract / GenerateLuminanceMipmap / ReadbackLuminance1x1).
 *   - 联动 HDR: 默认 AutoEnable=false (与 Bloom 默认 true 区别);
 *               HDR.Enable/Disable/Resize 可选自动拉起/关闭/重建.
 *   - 显式可控: Lua 可手动 Enable/Disable; 默认 manual exposure 不被影响.
 *   - 向后兼容: 未 Enable 时所有 API 静默 no-op (HDR 管线仍工作, manual exposure 生效).
 *   - Legacy 后端不支持: Enable() 返回 false + warn log.
 *
 * 管线流程 (AE Enabled, 由 HDRRenderer::EndScene 内部调用):
 *   HDRRenderer::EndScene
 *     ↓ UnbindFBO
 *     ↓ BloomRenderer::Process(hdrFbo, hdrTex)
 *     ↓ UnbindFBO
 *     ↓ AutoExposureRenderer::Process(hdrTex, dt)
 *       ├─ DrawLuminanceExtract  : hdrTex -> lumFbo (R16F log luma)
 *       ├─ GenerateLuminanceMipmap: lumTex -> 1x1 平均
 *       ├─ ReadbackLuminance1x1   : 1x1 R 通道 -> CPU
 *       └─ 时间平滑 lerp(currentEV, targetEV, dt × speed)
 *     ↓ exposure = AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
 *     ↓ DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.5.2
 */

#include <cstdint>

class RenderBackend;

namespace AutoExposureRenderer {

// ==================== 生命周期 ====================

/**
 * @brief 初始化 AE 模块 (缓存 backend 指针, 不分配 GPU 资源)
 *
 * 应在 HDRRenderer::Init() / BloomRenderer::Init() 之后调用. 不自动启用 AE.
 */
void Init(RenderBackend* backend);

/**
 * @brief 关闭模块 + 释放所有资源 (luminance RT + 清 flag)
 */
void Shutdown();

// ==================== Enable / Disable / Resize ====================

/**
 * @brief 启用 AE 管线, 分配 luminance RT (R16F mipmap-able)
 *
 * @param w  HDR RT 宽 (通常 = HDR RT 宽)
 * @param h  HDR RT 高
 * @return   成功 true; 失败 false (backend 不支持 / 参数非法 / 资源失败)
 */
bool Enable(int w, int h);

/**
 * @brief 关闭 AE 管线, 释放 luminance RT
 *
 * Disable 后 manual SetExposure (HDR 模块) 立即回归生效.
 */
void Disable();

/// 是否启用中 (Enable 成功且未 Disable)
bool IsEnabled();

/// 后端是否支持 AE (GL33 + R16F + glReadPixels 编译期检测)
bool IsSupported();

/**
 * @brief 重建 luminance RT 到新大小 (先 Disable 再 Enable)
 * @return 成功 true; 同尺寸 no-op 也返 true; 失败 false (例如 w/h 非法)
 */
bool Resize(int w, int h);

// ==================== HDR 自动联动 (内部 API, 不暴露 Lua) ====================

/**
 * @brief HDR.Enable 成功后的回调
 *
 * 当 autoEnable=true 时自动调 Enable(w, h); 否则 no-op.
 */
void OnHDREnabled(int w, int h);

/// HDR.Disable 的回调: 自动 Disable (无论 autoEnable, 防 RT 悬挂)
void OnHDRDisabled();

/// HDR.Resize 的回调: 当前已启用时 Resize(w, h), 否则 no-op
void OnHDRResized(int w, int h);

/// 设置 HDR 联动开关 (默认 false; 与 Bloom 默认 true 区别)
void SetAutoEnable(bool flag);

/// 查询 HDR 联动开关
bool GetAutoEnable();

// ==================== EV-based 参数 ====================

/// 中灰目标 EV (默认 0.0 = 中灰反射率 18%); 内部加到测量 EV 上作偏移
void  SetTargetEV(float v);
float GetTargetEV();

/// 暗→亮 适应速度 EV/sec (默认 3.0; clamp [0.1, 20]); 模拟人眼 0.5s 亮适应
void  SetSpeedUp(float v);
float GetSpeedUp();

/// 亮→暗 适应速度 EV/sec (默认 1.0; clamp [0.1, 20]); 模拟人眼 30s 暗适应 (但游戏中加快)
void  SetSpeedDown(float v);
float GetSpeedDown();

/// 当前 EV 下限 (默认 -8.0); 防夜场过曝
void  SetMinEV(float v);
float GetMinEV();

/// 当前 EV 上限 (默认 +8.0); 防强光欠曝
void  SetMaxEV(float v);
float GetMaxEV();

// ==================== 调试 / OSD getter ====================

/// 平滑后当前 EV (last frame); IsEnabled=false 时返 0
float GetCurrentEV();

/// 当前 EV 转 exposure 倍率 (= 2^GetCurrentEV()); HDR.Tonemap 实际用值
float GetCurrentExposure();

/// 上一帧 ReadbackLuminance1x1 测得的 log luma (debug); IsEnabled=false 时返 0
float GetMeasuredLuminance();

// ==================== 管线调用 (HDRRenderer::EndScene 内部调) ====================

/**
 * @brief 执行 AE 管线: extract + mipmap reduce + readback + 时间平滑
 *
 * 仅更新内部 currentEV / currentExposure 状态; 不直接改 HDR exposure.
 * HDRRenderer 在 tonemap 调用时主动取 GetCurrentExposure() 覆盖 manual.
 * 内部自检 IsEnabled; 未启用时 no-op.
 *
 * @param hdrTex  HDR RT 颜色纹理 id (输入)
 * @param dt      自上次 Process 的秒数 (用于时间平滑); clamp [0, 0.1]
 */
void Process(uint32_t hdrTex, float dt);

} // namespace AutoExposureRenderer
