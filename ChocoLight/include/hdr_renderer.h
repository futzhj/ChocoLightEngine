/**
 * @file hdr_renderer.h
 * @brief Phase E.3.2 — HDR 离屏渲染管线 (RGBA16F RT + ACES tonemap)
 *
 * 设计原则 (与 BatchRenderer / LitBatchRenderer 同风格, 独立命名空间):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsHDR / CreateHDRFBO / DeleteHDRFBO / DrawTonemapFullscreen).
 *   - 显式开启: 不默认启用; Lua 调用 Light.Graphics.HDR.Enable(w, h) 才启用.
 *   - 向后兼容: 未 Enable 时所有 API 静默 no-op (LDR 路径正常工作).
 *   - Legacy 后端不支持: Enable() 返回 false + warn log.
 *
 * 管线流程 (HDR Enabled):
 *   BeginFrame → BindFBO(HDR_RT) → Clear → SetViewport(w, h)
 *     ↓ Lua Draw (所有 sprite/lit/几何绘到 HDR RT, RGBA16F, 可 > 1.0)
 *   EndScene → UnbindFBO() → DrawTonemapFullscreen(sceneTex, exposure, gamma)
 *     ↓ ACES shader: hdr × exposure → ACES fitted → sRGB encode → default fb
 *   SwapBuffers
 *
 * SetCanvas 兼容:
 *   - 用户调 Light.Graphics.SetCanvas(userCanvas): 切到 user FBO, HDR 状态保留但 "paused"
 *   - 用户调 Light.Graphics.SetCanvas(nil): 如果 HDR 启用, 自动 BindFBO(HDR_RT) 恢复
 *
 * 异常处理:
 *   - backend->SupportsHDR() = false → Init 返回 false, 后续所有 API no-op
 *   - Enable(w, h) 失败 (FBO 不完整 / OOM) → 清理后返回 false, IsEnabled = false
 *   - Resize(0,0) / Resize(负数) → 返回 false + warn
 */

#pragma once

#include <cstdint>

class RenderBackend;
// Phase E.14 — 前向声明，避免拉 render_backend.h 进 header (依赖面)
enum class VelocityFormat : uint8_t;

namespace HDRRenderer {

// ==================== 生命周期 ====================

/**
 * @brief 初始化 HDR 模块, 绑定 RenderBackend
 *
 * 只做绑定, 不创建 RT. 真正创建 RT 在 Enable(w, h).
 * 仅在 backend->SupportsHDR() = true 时返回 true.
 *
 * @param backend 必须非空
 * @return true = 后端支持 HDR 且绑定成功; false = 不支持 / 已初始化
 */
bool Init(RenderBackend* backend);

/// 释放 HDR RT (若已 Enable), 解绑 backend
void Shutdown();

/// 是否已调用 Init (不等于 HDR 启用)
bool IsInited();

// ==================== HDR 开关 ====================

/**
 * @brief 启用 HDR: 创建 RGBA16F RT
 *
 * 允许在同一进程多次 Enable (先 Disable 再 Enable) 以变更 RT 尺寸;
 * 等价于 Resize(w, h).
 *
 * @return true = RT 创建成功, 主循环下一帧起走 HDR 路径;
 *         false = backend->SupportsHDR() = false / Init 未调 / FBO 不完整
 */
bool Enable(int w, int h);

/// 关闭 HDR: 释放 RT; 后续主循环走 LDR 路径
void Disable();

/// HDR 当前是否启用 (Enable 成功 + 未 Disable)
bool IsEnabled();

/// backend->SupportsHDR() 转发 (Enable 前可查询; Init 未调时返回 false)
bool IsSupported();

/**
 * @brief 调整 HDR RT 尺寸 (窗口 resize 时调用方主动调用)
 *
 * 内部实现: Disable() + Enable(w, h). 如果之前未 Enable, 这调用等价于 Enable.
 * @return true 成功; false 资源创建失败或非法尺寸
 */
bool Resize(int w, int h);

// ==================== 主循环 hook (由 light_ui.cpp::Window_Call 调) ====================

/**
 * @brief 帧开始: 绑定 HDR RT + 清空 + 设置 viewport
 *
 * 必须在 backend->BeginFrame() 之后、Lua Draw 之前调用.
 * HDR 未启用或 paused 时静默 no-op.
 */
void BeginScene();

/**
 * @brief 帧结束: 解绑 HDR RT → tonemap blit 到 default framebuffer
 *
 * 必须在 BatchRenderer/LitBatchRenderer::EndFrame() 之后、SwapBuffers 之前调用.
 * HDR 未启用或 paused 时静默 no-op.
 */
void EndScene();

// ==================== 曝光 / Gamma ====================

/// 线性曝光预乘 (默认 1.0); LDR 模式下写入值但不影响渲染
void  SetExposure(float v);
float GetExposure();

/// sRGB encode gamma (默认 2.2); 同上
void  SetGamma(float v);
float GetGamma();

// ==================== Phase E.3.4 — Tonemap Operator ====================

/// Tonemap operator 常量 (与 shader uTonemapMode int 值对齐)
enum Tonemapper {
    TONEMAP_ACES       = 0,   ///< Narkowicz 2016 fitted (默认, 电影感)
    TONEMAP_REINHARD   = 1,   ///< x/(1+x) (简单基线)
    TONEMAP_UNCHARTED2 = 2,   ///< Hable filmic (含 white scale)
    TONEMAP_LINEAR     = 3,   ///< clamp(x, 0, 1) (调试用, 等同 LDR clip)
};

/// 设置 tonemap operator (无效 mode 静默回退 ACES)
void SetTonemapper(int mode);

/// 当前 operator (0..3)
int  GetTonemapper();

// ==================== 高级查询 ====================

/// 当前 HDR RT 的颜色纹理 id (Enable 未调时 = 0)
uint32_t GetSceneTexture();

/// 当前 HDR RT 的 FBO id (Enable 未调时 = 0); Phase E.8.x — SSAO/调试拿 normal tex
uint32_t GetFBO();

/// 当前 HDR RT 的 velocity 纹理 id (Enable 未调或后端不支持时 = 0)
uint32_t GetVelocityTexture();

/// Phase E.16 — 当前 HDR RT 的 camera-only velocity 纹理 id (slot 3)
/// Enable 未调 / 后端不支持 / CreateHDRFBO 未传 outCameraVelocityTex 时 = 0
uint32_t GetCameraVelocityTexture();

// ==================== Phase E.14 — Velocity dilation + 存储格式切换 ====================

/// dilation 开关：SSRTemporal 采样 velocity 时是否用 3x3 max-length 邻域（默认 ON）
/// @return true = 设置成功; false = backend 未初始化。
/// 切换不重建 RT，仅修改后端状态 + 下一帧 SSRTemporal draw 会读取。
bool          SetVelocityDilation(bool on);
bool          GetVelocityDilation();

/// Velocity 存储格式切换（RG16F 默认 vs RG8 低精度节 VRAM）
/// @return true = 切换成功（含 RT 重建）; false = 后端未启 / 重建失败。
/// HDR 未 Enable 时仅更新 state，下次 Enable 时生效。切换会隐含重置 velocity history。
bool           SetVelocityFormat(VelocityFormat fmt);
VelocityFormat GetVelocityFormat();

/// 当前 HDR RT 宽度 / 高度 (未 Enable 时 = 0)
int GetWidth();
int GetHeight();

// ==================== SetCanvas 兼容 (由 l_SetCanvas 内部调) ====================

/**
 * @brief 通知 HDR 模块: 用户调 SetCanvas(userFbo), HDR 暂停
 *
 * 不释放 HDR RT, 仅标记 paused; EndScene 仍会执行 tonemap (读 HDR RT 现有内容).
 */
void Pause();

/**
 * @brief 通知 HDR 模块: 用户调 SetCanvas(nil), HDR 恢复
 *
 * 如果 HDR enabled 且之前 paused, 重新 BindFBO(HDR_RT).
 */
void Resume();

/// 当前是否被 SetCanvas 暂停
bool IsPaused();

} // namespace HDRRenderer
