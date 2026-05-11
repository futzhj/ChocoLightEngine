#pragma once
/**
 * @file   lens_dirt_renderer.h
 * @brief  Phase E.6 — Lens Dirt 镜头脏污后处理 命名空间模块
 *
 * 设计原则 (与 BloomRenderer / AutoExposureRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口
 *     (SupportsLensDirt / DrawLensDirtComposite).
 *   - 联动 HDR: 默认 AutoEnable=false; HDR.Enable/Disable 可选自动拉起/关闭.
 *   - 无独立 RT: 不持有 GPU 资源 (仅一个 dirtTexId 引用); 1x1 白纹理 fallback
 *     由后端 (GL33Backend::whiteTex1x1) 提供, 当 dirtTexId=0 时自动使用.
 *   - 依赖 Bloom: Process 需要 BloomRenderer::GetPyramidTopTex() 作为 bloomTex 输入;
 *     Bloom 未启用时 Process 静默 no-op.
 *   - Legacy 后端不支持: Enable() 返回 false.
 *
 * 管线流程 (LensDirt Enabled, 由 HDRRenderer::EndScene 内部调用):
 *   HDRRenderer::EndScene 中 Bloom + AE 完成后:
 *     LensDirtRenderer::Process(hdrFbo, bloomTex, w, h)
 *       - DrawLensDirtComposite(bloomTex, dirtTex, hdrFbo, w, h, intensity)
 *       - shader: hdr += bloom × dirt × intensity (additive blend)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.6.2
 */

#include <cstdint>

class RenderBackend;

namespace LensDirtRenderer {

// ==================== 生命周期 ====================

/// 初始化 (缓存 backend; 不分配资源)
void Init(RenderBackend* backend);

/// 关闭模块 (当前无独立资源, 主要复位 flag)
void Shutdown();

// ==================== Enable / Disable ====================

/**
 * @brief 启用 Lens Dirt
 * @return true = 成功; false = backend 不支持 / Init 未调
 */
bool Enable();

void Disable();
bool IsEnabled();
bool IsSupported();

// ==================== HDR 自动联动 (内部 API) ====================

void OnHDREnabled(int w, int h);    ///< autoEnable=true 时自动调 Enable (无视 w/h)
void OnHDRDisabled();                ///< 强制 Disable (无论 autoEnable)
void OnHDRResized(int w, int h);     ///< no-op (LensDirt 无 RT)

void SetAutoEnable(bool flag);       ///< 默认 false
bool GetAutoEnable();

// ==================== 参数 ====================

/**
 * @brief 设置 dirt 纹理 GL id
 *
 * @param texId  原生 GL 纹理 id; 0 = 复位到内置 1x1 白纹理 fallback
 *               (乘白色 = bloom × intensity 效果)
 *
 * 注: 不持有 texId 引用计数, 调用方 (如 Lua 端 Image userdata)
 *     负责保证 tex 在 Disable 或换新前不被释放.
 */
void     SetDirtTextureId(uint32_t texId);

/// @return 当前 dirt tex id; 0 = 使用 fallback
uint32_t GetDirtTextureId();

/// @brief 合成强度 clamp [0, +inf), 默认 0.4
void  SetIntensity(float v);
float GetIntensity();

// ==================== 管线调用 (HDRRenderer::EndScene 内部调) ====================

/**
 * @brief 将 bloom × dirt × intensity 加到 HDR RT
 *
 * 需要 Bloom 启用 (bloomTex != 0); 否则静默 no-op.
 * 内部自检 IsEnabled.
 *
 * @param hdrFbo    HDR RT FBO id (目标, additive 写入)
 * @param bloomTex  BloomRenderer::GetPyramidTopTex() (输入)
 * @param w, h      hdrFbo 尺寸
 */
void Process(uint32_t hdrFbo, uint32_t bloomTex, int w, int h);

} // namespace LensDirtRenderer
