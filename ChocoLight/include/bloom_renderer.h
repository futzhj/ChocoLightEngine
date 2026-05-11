#pragma once
/**
 * @file   bloom_renderer.h
 * @brief  Phase E.4 — Bloom 后处理命名空间模块
 *
 * 设计原则 (与 HDRRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsBloom / CreateBloomPyramid / DeleteBloomPyramid /
 *      DrawBloomBrightPass / DrawBloomDownsample / DrawBloomUpsample /
 *      DrawBloomComposite).
 *   - 联动 HDR: 默认 AutoEnable=true; HDR.Enable/Disable/Resize 自动拉起/关闭/重建.
 *   - 显式可控: Lua 可手动 Enable/Disable; SetAutoEnable(false) 禁用联动.
 *   - 向后兼容: 未 Enable 时所有 API 静默 no-op (HDR 管线仍工作, 无 bloom).
 *   - Legacy 后端不支持: Enable() 返回 false + warn log.
 *
 * 管线流程 (Bloom Enabled, 由 HDRRenderer::EndScene 内部调用):
 *   HDRRenderer::EndScene
 *     ↓ UnbindFBO (切回 default fb)
 *     ↓ BloomRenderer::Process(hdrFbo, hdrTex)
 *       ├─ BrightPass    : hdrTex -> pyramid[0]       (threshold + soft knee)
 *       ├─ Downsample x N: [0]->[1]->...->[N-1]       (13-tap COD AW)
 *       ├─ Upsample  x N-1: [N-1]->[N-2]->...->[0]    (tent 3x3 + GL blend ONE/ONE)
 *       └─ Composite     : [0] -> hdrFbo              (intensity-scaled additive)
 *     ↓ UnbindFBO (防御性)
 *     ↓ DrawTonemapFullscreen(hdrTex, ...) (HDR + bloom 合成后的结果 tonemap 到 default fb)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.4.2
 */

#include <cstdint>

class RenderBackend;

namespace BloomRenderer {

// ==================== 生命周期 ====================

/**
 * @brief 初始化 Bloom 模块 (缓存 backend 指针, 不分配 GPU 资源)
 *
 * 应在 HDRRenderer::Init() 之后调用. 不自动启用 Bloom.
 */
void Init(RenderBackend* backend);

/**
 * @brief 关闭模块 + 释放所有资源 (pyramid + 清 flag)
 */
void Shutdown();

// ==================== Enable / Disable / Resize ====================

/**
 * @brief 启用 Bloom 管线, 分配 pyramid (当前 levels 层)
 *
 * @param w  pyramid[0] 宽 (通常 = HDR RT 宽)
 * @param h  pyramid[0] 高
 * @return   成功 true; 失败 false (backend 不支持 / 参数非法 / 部分分配失败)
 */
bool Enable(int w, int h);

/**
 * @brief 关闭 Bloom 管线, 释放 pyramid
 */
void Disable();

/// 是否启用中 (Enable 成功且未 Disable)
bool IsEnabled();

/// 后端是否支持 Bloom (GL33 + shader 编译成功)
bool IsSupported();

/**
 * @brief 重建 pyramid 到新大小 (先 Disable 再 Enable)
 * @return 成功 true; 失败 false (例如 w/h 非法)
 */
bool Resize(int w, int h);

// ==================== HDR 自动联动 (内部 API, 不暴露 Lua) ====================

/**
 * @brief HDR.Enable 成功后的回调
 *
 * 当 autoEnable=true 时自动调 Enable(w, h); 否则 no-op.
 */
void OnHDREnabled(int w, int h);

/// HDR.Disable 的回调: 自动 Disable (无论 autoEnable)
void OnHDRDisabled();

/// HDR.Resize 的回调: 当前已启用时 Resize(w, h), 否则 no-op
void OnHDRResized(int w, int h);

/// 设置 HDR 联动开关 (默认 true)
void SetAutoEnable(bool flag);

/// 查询 HDR 联动开关
bool GetAutoEnable();

// ==================== 参数 ====================

/// 亮度阈值 (L > threshold 时保留, 默认 1.0; clamp [0, +∞))
void  SetThreshold(float v);
float GetThreshold();

/// 合成强度 (默认 0.8; clamp [0, +∞))
void  SetIntensity(float v);
float GetIntensity();

/// 扩散半径 (默认 0.7; clamp [0, 1])
void  SetRadius(float v);
float GetRadius();

/**
 * @brief 设置 pyramid 层数 (默认 5; clamp [2, 8])
 *
 * 不立即生效; 下次 Enable / Resize 重建 pyramid 时才应用新值.
 * (当前已启用 pyramid 不受影响, 避免半帧切换导致视觉跳变.)
 */
void SetLevels(int n);
int  GetLevels();

// ==================== 管线调用 (HDRRenderer::EndScene 内部调) ====================

/**
 * @brief 执行 Bloom 完整管线: bright + downsample + upsample + composite
 *
 * 输出合成到 hdrFbo (即 HDR RT 原地加亮), 继续走 Tonemap.
 * 内部自检 IsEnabled; 未启用时 no-op.
 *
 * @param hdrFbo  HDR RT 的 FBO id
 * @param hdrTex  HDR RT 颜色纹理 id
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex);

// ==================== 高级查询 (Phase E.6 — Lens Dirt 输入源) ====================

/**
 * @brief 取 pyramid 顶层 (level 0) 颜色 tex id
 *
 * 供 LensDirtRenderer 读取 bloom 输出 (高斯柔化后的亮像素) 作为 dirt 乘数.
 * Bloom 未启用 / pyramid 未建时返回 0.
 */
uint32_t GetPyramidTopTex();

} // namespace BloomRenderer
