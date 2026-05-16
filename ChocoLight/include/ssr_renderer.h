#pragma once
/**
 * @file   ssr_renderer.h
 * @brief  Phase E.9 — SSR (Screen-Space Reflection) 模块（Phase E.10/E.11/E.12 增量）
 *
 * 设计原则 (与 SSAORenderer / BloomRenderer 同风格):
 *   - 后端无关: 所有 GL 操作经 RenderBackend 虚接口.
 *   - 联动 HDR: 默认 autoEnable=false; HDR.Enable/Disable/Resize 可选自动拉起/关闭.
 *   - 高质量方案 (用户拍板 2026-05-12 起): full-res RGBA16F + 64 步 ray march
 *       * SSR 自管独立 depth texture + 小 FBO (full-res, 仅 GL_DEPTH_ATTACHMENT)
 *       * 每帧 Process() 入口先用 glBlitFramebuffer 从 HDR FBO 复制 depth (复用 BlitHDRDepthToSSAO 接口)
 *       * 反射 RT 单 RGBA16F (full-res, 与 HDR RT 同尺寸)
 *       * Composite 用内部临时 RT 解 feedback loop (读 HDR + 加性写 HDR)
 *       * Phase E.10: 可选 half-res Gaussian blur
 *       * Phase E.11: 可选 depth-aware bilateral blur
 *       * Phase E.12/E.13: 可选 Temporal SSR (jitter + velocity/fallback reprojection + history ping-pong)
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
 *                 maxDist, edgeFade, jitterX, jitterY)             ← raw reflection (RGBA16F)
 *     3. DrawSSRTemporal(reflectTex, historyTex, depthTex, velocityTex, ...)
 *                                                                    ← optional temporal accumulation
 *     4. DrawSSRBlur(srcForBlur, depthTex, ...)                    ← optional blur
 *     5. DrawSSRComposite(finalReflectTex, hdrFbo, w, h, intensity)← HDR += reflect.rgb * reflect.a * intensity
 *
 * 适用范围:
 *   仅 3D mesh + 显式 SetDepthTest(true) 场景生效.
 *   纯 2D 场景 (z=0 平面) 反射全 0, 不影响画面.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.12
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

/// Phase E.11 — 切换 bilateral 深度感知模式, 默认 true.
/// true  = depth-aware bilateral (跨深度边权重衰减, 消除 leak)
/// false = 纯 Gaussian (Phase E.10 行为, 向后兼容; 用于 A/B 对比)
/// 仅在 BlurEnabled=true 时影响最终视觉.
void SetBilateralEnabled(bool flag);
bool GetBilateralEnabled();

/// Phase E.11 — bilateral 深度权重灵敏度 sigma, clamp [50.0, 500.0], 默认 200.0.
/// σ 越大: 跨深度边模糊衰减越快 (锐利边缘保留更好)
/// σ 越小: 跨深度边权重宽容 (行为接近 Gaussian)
/// 仅在 BilateralEnabled=true && BlurEnabled=true 时影响视觉.
void  SetBlurDepthSigma(float v);
float GetBlurDepthSigma();

/// Phase E.12 — Temporal SSR 时序累积降噪, 默认 true (TAA-style 业界标准).
/// 关闭后行为完全等同 Phase E.11 (raw → blur → composite, 无 history).
/// 开启时: SSR raw 增加 Halton-2,3 8-sample jitter, 配合 reverse-reprojection
///         history 累积 + neighborhood AABB clip rejection.
void SetTemporalEnabled(bool flag);
bool GetTemporalEnabled();

/// Phase E.12 — History blend 权重 (history 占比) clamp [0.5, 0.99], 默认 0.9.
/// 越高 → 历史权重大, 去噪强但响应慢
/// 越低 → 响应快, 但去噪弱
/// 仅在 TemporalEnabled=true 时影响视觉.
void  SetTemporalAlpha(float v);
float GetTemporalAlpha();

/// Phase E.12 — Rejection 模式 clamp {0, 1}, 默认 1.
/// 0 = current-depth threshold rejection
/// 1 = neighborhood AABB clip (9-tap min/max history clip, 抗 ghost)
/// 仅在 TemporalEnabled=true 时影响视觉.
void SetRejectionMode(int mode);
int  GetRejectionMode();

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

/**
 * @brief Phase F.0.10.3 — Region 限定 SSR (split-screen 必备 overload)
 *
 * 与无 region 版语义等价 (rgnW=0 || rgnH=0 时退化为全屏路径), 区别:
 *   1. BlitHDRDepthToSSAO: sub-rect blit (节省 IO + 防越界)
 *   2. DrawSSR raw:  scissor 限定写区域 (ray march shader 内仍全屏, 允许跨边界采样反射)
 *   3. DrawSSRTemporal:   scissor 限定 history write 区域 (防写脏邻 region history;
 *                         reproject 允许跨 region 读 history, shader 不动)
 *   4. DrawSSRBlur × 2:   half-res region (caller 缩半: ((x+1)/2, (y+1)/2, max(1, w/2), max(1, h/2)))
 *   5. DrawSSRComposite:  sub-rect blit + scissor 限定 additive 写区域
 *
 * 典型用法 (split-screen, HDR.SetAutoSSR(false) 后手动调):
 *   SSR.Process(0,    0, W/2, H)   -- 左半屏 player 1
 *   SSR.Process(W/2,  0, W/2, H)   -- 右半屏 player 2
 *
 * 注意: 反射 ray march 跨 region 是物理正确的 (player 1 屏上可能看到 player 2 区位的反射),
 *       这与 SSR 屏幕空间本质一致, 不视为缺陷
 */
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH);

// ==================== Phase F.0.10.9.x.2 — Multi-Instance ====================
//
// 仿 HDRRenderer multi-instance 模型: 4 instance (default + 3 user),
// 每 instance 各自独立 depth/reflect/blur/history RT + 参数 + temporal state (prevViewProj).
// 切 active instance 后, 现有 namespace fn 自动作用于该 instance.
// 用途: split-screen 4 player 各自 SSR profile (强度/temporal/blur 全独立).
//
// 与 HDR/TAA/Bloom/MotionBlur multi-instance API 命名/语义完全一致.

/// 创建新 SSR instance, 返 id [1, 3] / 0 (槽满)
int CreateInstance();

/// 销毁 user instance (id 1..3); 不能销毁 id=0
bool DestroyInstance(int id);

/// 切换 active instance, 后续 namespace fn 作用于该 instance
bool SetActiveInstance(int id);

/// 当前 active instance id (默认 0)
int GetActiveInstance();

/// 已分配 instance 总数 (default 0 占 1, 范围 [1, 4])
int GetInstanceCount();

// ==================== Phase F.0.10.9.x.3 — Clone (1-line setup) ====================
//
// 复制 srcId 全部调参字段 (max_steps/intensity/blur_radius/temporal_alpha/...) 到新 instance,
// 全部 RT (depth/reflect/blur×2/history×2) + temporal state (prevViewProj/hasPrevViewProj/frameCounter/historyIdx)
// 不复制 — 新 instance 待自己调 Enable + 第一帧从 cur 路径走.
// 失败条件: srcId 非法 / srcId 未分配 / 槽满 → 返 0.
int CloneInstance(int srcId);

} // namespace SSRRenderer
