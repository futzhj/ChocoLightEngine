/**
 * @file taa_renderer.h
 * @brief Phase F.0 — TAA (Temporal Anti-Aliasing) 主管线
 *
 * 对整个 HDR scene 做时序累积超采样 + neighborhood AABB clip + alpha blend。
 * 复用 Phase E 系列已有资产：
 *   - Halton-2,3 8-sample jitter 表 (与 SSR Temporal 共用)
 *   - HDR FBO 关联的 velocity buffer (E.13/E.14) + dilated velocity (E.18)
 *   - history ping-pong RT 模式 (与 SSR Temporal 同构)
 *
 * 集成位置 (HDR EndScene):
 *   SSAO → dilation → SSR → LensFlare → MotionBlur → **TAA** → Tonemap
 *
 * Jitter 注入 (backend 双 projection):
 *   BeginScene 后用 backend->LoadJitteredProjection(jitteredProj) 让本帧 3D raster 用 jittered;
 *   GetProjection() 始终返 unjittered (SSR/SSAO/velocity 零改动);
 *   Process 末尾用 ClearJitteredProjection() 复位。
 *
 * Lua API 入口: Light.Graphics.TAA.*
 *
 * 与 SSR Temporal 关系: 共存 (用户自负责); 同开时反射会被 temporal 两次。
 *   推荐启用 TAA 时手动 Light.Graphics.SSR.SetTemporalEnabled(false)。
 *
 * @copyright Choco Light Engine
 */
#pragma once

#include <cstdint>

class RenderBackend;

namespace TAARenderer {

// ==================== 生命周期 ====================

/// 初始化模块: 缓存 backend 指针 + 查询 backend->SupportsTAA() 能力位
bool Init(RenderBackend* backend);

/// 释放所有资源 (history RT + state 复位)
void Shutdown();

/// 已 Init() 过 (不等于 TAA 已 Enable)
bool IsInited();

// ==================== HDR 开关 ====================

/// 启用 TAA: 创建 history ping-pong RT (RGBA16F × 2, 与 sceneTex 同尺寸)
/// @return false 当 backend 不支持 TAA / RT 创建失败 / 参数非法
bool Enable(int w, int h);

/// 释放 history RT (state 保留, 下次 Enable 还可用)
void Disable();

bool IsEnabled();
bool IsSupported();

/// 调整尺寸: 已 Enable 时重建 RT (与 SSR/MotionBlur 同模式)
bool Resize(int w, int h);

// ==================== HDR 联动 hook ====================

/// HDR Enable 自动调; autoEnable=false 时 no-op (与 Phase E 模块一致)
void OnHDREnabled(int w, int h);

/// HDR Disable 自动调; 释放 history RT
void OnHDRDisabled();

/// HDR Resize 自动调
void OnHDRResized(int w, int h);

// ==================== 主循环 hook ====================

/// 在 BeginScene 之后、用户 Draw 之前调用:
/// 若 TAA 启用 + jitter 启用, 计算本帧 Halton sub-pixel 偏移,
/// 通过 backend->LoadJitteredProjection() 让本帧 3D raster 用 jittered projection.
/// 用户透明 (Lua 层不需感知).
void ApplyJitter();

/// 主 pass: 在 HDR EndScene 内 MotionBlur 之后、Tonemap 之前调用.
/// 读 cur HDR sceneTex + 上帧 history + dilated/raw velocity → 写新 history + blit 回 sceneTex.
void Process(uint32_t hdrFbo, uint32_t hdrTex);

// ==================== 参数 ====================

/// history 权重 [0.5, 0.99], 默认 0.92
void  SetBlendAlpha(float alpha);
float GetBlendAlpha();

/// 是否启用 9-tap neighborhood AABB clip (默认 true)
void  SetNeighborhoodClip(bool on);
bool  GetNeighborhoodClip();

/// 是否启用 sub-pixel projection jitter (默认 true)
/// 关闭后 TAA 退化为纯时序 stability filter, 无 super-sampling 效果
void  SetJitterEnabled(bool on);
bool  GetJitterEnabled();

/// Phase F.0.1 — TAA Sharpening 强度 [0, 2], 默认 0.5
/// 算法: 4-tap unsharp mask, 弥补 TAA 引入的 sub-pixel 模糊
/// 0 = 纯 blit (零 ALU 开销); > 0 启用 sharpen pass (~0.03ms @ 1080p)
/// 推荐值 0.3~0.8; > 1.5 易产生 ringing 伪影 / firefly 加剧
void  SetSharpness(float s);
float GetSharpness();

/// Phase F.0.4 — Anti-flicker filter (Karis luma weighting blend), 默认 true
/// 算法: 高 luma 像素在 history blend 阶段赋予低权重，压制 firefly 闪烁伪影
/// false = 纯 alpha blend (Phase F.0 原始行为, 低亮部与 Karis 路径几乎同结果)
/// 与 F.0.1 sharpening 配合使用收益更佳 (sharpness > 1.0 时 firefly 加剧问题)
/// 性能: +0.01ms @ 1080p (2 dot + 4 div + 1 div / px)
void  SetAntiFlicker(bool on);
bool  GetAntiFlicker();

/// Phase F.0.2/F.0.3 — 9-tap clip 色彩空间: "rgb" (F.0) / "ycocg" (F.0.2 默认 AABB) / "variance" (F.0.3 mean±γσ)
/// 算法: YCoCg lift 形式转换 (与 FXAA / Inside / UE5 同); RGB 路径 = F.0 三通道 min/max
/// variance 路径 (Salvi 2016 / UE5 default): m1 = mean(N9); m2 = mean(N9^2); σ = sqrt(max(0, m2-m1^2)); clip = [m1-γσ, m1+γσ]
/// 与 AABB 相比 variance 对 single-outlier 更鲁棒 (均值不受单点影响), clip 盒更紧凑, ghost 抑制更强
/// 大小写不敏感; 非法值静默忽略 (Lua 层返 nil+err)
/// 性能: ycocg AABB +0.05ms / variance +0.07ms @ 1080p
void        SetClipMode(const char* mode);
const char* GetClipMode();

/// Phase F.0.3 — Variance clip 收紧系数 γ, 仅 ClipMode=="variance" 生效, 默认 1.0
/// Salvi 2016 推荐 1.0; UE5 默认 1.0; 常用范围 [0.75, 1.5]
///   γ 越小 → clip 越严 → ghost 抑制越强, 但可能出现 trail/over-smoothing
///   γ 越大 → clip 越宽松 → 接近无 clip
///   γ = 0  → 极端激进 (mn=mx=mean), history 被强制贴近邻域均值
void  SetVarianceGamma(float gamma);   // clamp [0, 4]
float GetVarianceGamma();

/// Phase F.0.5 — TAA history RT 是否半分辨率 (默认 false, 零回归)
/// 算法: history RT 尺寸 = (w/2, h/2), VRAM -75%; TAA pass viewport=(w/2, h/2);
///       BlitTAAToHDR 自动 GL_LINEAR stretch 上采样 history → sceneTex (full-res);
///       Sharpen pass 不变 (走 fragment shader 上采样, sample srcTex 自动 GL_LINEAR)
/// 视觉影响: history bilinear 上采样引入 ~1px 模糊, 默认 sharpness=0.5 完全弥补;
///          邻域 clip 在 box-filtered 邻域上做, firefly 反而被预压制
/// 性能: VRAM -75% (1080p 33.2MB → 8.3MB; 4K 132.7MB → 33.2MB)
///       TAA pass -75% 像素 (~0.04ms vs 0.10ms); Blit +0.01ms (stretch);
///       Sharpen 不变. 总体 ~0.09ms vs 0.13ms (-30%)
/// 切换时机: 立即重建 history RT + invalidate hasHistory (避免分辨率不匹配 reproject 花屏)
/// 推荐场景: 移动 4K / VRAM 紧张 / 长开 TAA 项目; 静态高端不必启用
void SetHalfResHistory(bool on);
bool GetHalfResHistory();

/// Phase F.0.6 — TAA Sharpen Mode (4-tap unsharp mask vs 5-tap CAS)
/// "unsharp" (默认) — F.0.1 4-tap unsharp mask, sharpness ∈ [0, 2], ~0.03 ms
/// "cas"             — AMD FidelityFX FSR1 5-tap contrast-adaptive sharpening
///                     sharpness ∈ [0, 1] (FSR1 标准范围, TAARenderer 内部 clamp 到 1)
///                     contrast-adaptive: 平滑区域不锁牰 + HDR safe + perceptual gamma
///                     性能: ~0.05 ms (+0.02 ms vs unsharp)
/// 大小写不敏感: "CAS" / "Cas" / "cas" 等价; "unsharp" / "Unsharp" / "UNSHARP" 等价
/// 未识别字符串保持当前 state (Lua 层 raise error 提示)
/// shader 切换零开销, sharpness 字段共享 (语义按 mode 自适应)
void SetSharpenMode(const char* mode);
const char* GetSharpenMode();

// ==================== 内部状态查询 (debug HUD 用) ====================

/// 当前帧 Halton 索引 (% 8), 累加帧计数器低 3 位
int  GetFrameCounter();

/// 当前帧 sub-pixel jitter (±0.5 pixel 范围), 仅 jitterEnabled+enabled 时非零
void GetCurrentJitter(float* outX, float* outY);

} // namespace TAARenderer
