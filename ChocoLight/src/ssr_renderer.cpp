/**
 * @file   ssr_renderer.cpp
 * @brief  Phase E.9 — SSRRenderer 实现
 *
 * 模块结构 (与 SSAORenderer 同风格, 简化为单 reflectRT 无 ping-pong):
 *   - State (匿名 namespace, 模块单例)
 *   - 生命周期: Init/Shutdown/Enable/Disable/Resize/IsEnabled/IsSupported
 *   - HDR 联动: OnHDREnabled/OnHDRDisabled/OnHDRResized
 *   - 参数 setter/getter (7 对) + AutoEnable + 调试 GetReflectionTexId
 *   - Process: blit depth -> SSR raw -> composite
 *   - 辅助: InvertMat4 (内部, 与 SSAO 同实现)
 *
 * 高质量方案 (用户拍板 2026-05-12): full-res RGBA16F + 64 步 ray march.
 */

#include "ssr_renderer.h"
#include "render_backend.h"
#include "light.h"   // CC::Log

#include <cstring>

namespace SSRRenderer {

namespace {

// ==================== 内部辅助 ====================

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// 4x4 矩阵求逆 (列主序, 与 ssao_renderer.cpp 同实现)
/// 失败时 (det ≈ 0) 返回 false, out 不修改
static bool InvertMat4(const float* m, float* out) {
    float inv[16];
    inv[ 0] =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[ 4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[ 8] =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[ 9];
    inv[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[ 9];
    inv[ 1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[ 5] =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[ 9] = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[ 9];
    inv[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[ 9];
    inv[ 2] =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[ 6];
    inv[ 6] = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[ 6];
    inv[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[ 5];
    inv[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[ 5];
    inv[ 3] = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[ 9]*m[2]*m[ 7] + m[ 9]*m[3]*m[ 6];
    inv[ 7] =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[ 8]*m[2]*m[ 7] - m[ 8]*m[3]*m[ 6];
    inv[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[ 9] - m[ 8]*m[1]*m[ 7] + m[ 8]*m[3]*m[ 5];
    inv[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[ 9] + m[ 8]*m[1]*m[ 6] - m[ 8]*m[2]*m[ 5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-10f && det < 1e-10f) return false;
    float invDet = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * invDet;
    return true;
}

// ==================== 模块状态 ====================

struct State {
    bool     enabled        = false;
    bool     supported      = false;
    bool     autoEnable     = false;

    RenderBackend* backend  = nullptr;

    // depth RT 旁路 (full-res, 复用 SSAO BlitHDRDepthToSSAO 接口)
    uint32_t depthFbo       = 0;
    uint32_t depthTex       = 0;

    // 反射 RT (单 RGBA16F, full-res)
    uint32_t reflectFbo     = 0;
    uint32_t reflectTex     = 0;
    int      srcW           = 0;
    int      srcH           = 0;

    // 参数 (默认值见 CONSENSUS §3 / DESIGN §3.4)
    int     maxSteps        = 64;
    float   stepSize        = 0.1f;
    float   thickness       = 0.5f;
    float   maxDistance     = 50.0f;
    float   intensity       = 0.7f;
    float   edgeFade        = 0.1f;
    bool    blurEnabled     = false;   // Phase E.10 已激活: true 时 Process 走 H+V 模糊

    // Phase E.10 — half-res blur ping-pong (用户拍板 2026-05-12)
    uint32_t blurFbos[2]    = {0, 0};   // [0] = H pass dst, [1] = V pass dst
    uint32_t blurTexs[2]    = {0, 0};
    int      blurW          = 0;        // half-res 宽 (max(1, srcW/2))
    int      blurH          = 0;
    float    blurRadius     = 1.5f;     // texel 半径乘子, clamp [0.5, 4.0]

    // Phase E.11 — depth-aware bilateral 选项
    bool     bilateralEnabled = true;   // 默认 true (默认享用最佳质量), false 切回 E.10 Gaussian
    float    blurDepthSigma   = 200.0f; // bilateral 深度权重 σ, clamp [50, 500]
};

static State g;

// ==================== 内部资源管理 ====================

/// 释放所有动态资源 (depth RT + reflect RT + Phase E.10 blur RT × 2), 不动 backend / 参数
static void DestroyResources() {
    if (!g.backend) return;
    if (g.depthFbo || g.depthTex) {
        g.backend->DeleteSSRDepthRT(g.depthFbo, g.depthTex);
        g.depthFbo = g.depthTex = 0;
    }
    if (g.reflectFbo || g.reflectTex) {
        g.backend->DeleteSSRTargets(&g.reflectFbo, &g.reflectTex);
    }
    // Phase E.10 — half-res blur ping-pong
    if (g.blurFbos[0] || g.blurFbos[1] || g.blurTexs[0] || g.blurTexs[1]) {
        g.backend->DeleteSSRBlurRT(g.blurFbos, g.blurTexs);
    }
    g.blurW = g.blurH = 0;
    g.srcW = g.srcH = 0;
}

/// 分配所有资源 (depth RT full-res + reflect RT full-res + Phase E.10 blur RT half-res)
/// blur RT 即使 BlurEnabled=false 也分配 (简化生命周期; 1080p 仅 ~4MB 代价)
static bool AllocateResources(int w, int h) {
    if (!g.backend || w <= 0 || h <= 0) return false;

    // depth RT: full-res
    if (!g.backend->CreateSSRDepthRT(w, h, &g.depthFbo, &g.depthTex)) {
        CC::Log(CC::LOG_WARN, "SSRRenderer: CreateSSRDepthRT failed (%dx%d)", w, h);
        return false;
    }
    // reflect RT: full-res RGBA16F
    if (!g.backend->CreateSSRTargets(w, h, &g.reflectFbo, &g.reflectTex)) {
        CC::Log(CC::LOG_WARN, "SSRRenderer: CreateSSRTargets failed (%dx%d)", w, h);
        g.backend->DeleteSSRDepthRT(g.depthFbo, g.depthTex);
        g.depthFbo = g.depthTex = 0;
        return false;
    }
    // Phase E.10 — blur ping-pong RT: half-res RGBA16F × 2 (失败不致命, silent fallback 到无 blur)
    if (!g.backend->CreateSSRBlurRT(w, h, g.blurFbos, g.blurTexs, &g.blurW, &g.blurH)) {
        // backend 不支持 SSR Blur (旧 GL / Legacy) -> blur 自动 no-op, 不影响 reflect 主路径
        g.blurFbos[0] = g.blurFbos[1] = 0;
        g.blurTexs[0] = g.blurTexs[1] = 0;
        g.blurW = g.blurH = 0;
    }
    g.srcW = w;
    g.srcH = h;
    return true;
}

} // anonymous namespace

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.supported = (backend && backend->SupportsSSR());
    if (g.supported) {
        CC::Log(CC::LOG_INFO, "SSRRenderer: initialized (supported=yes)");
    } else {
        CC::Log(CC::LOG_INFO, "SSRRenderer: initialized (supported=no, backend=%s)",
                backend ? backend->GetName() : "null");
    }
}

void Shutdown() {
    DestroyResources();
    g.enabled   = false;
    g.backend   = nullptr;
    g.supported = false;
}

bool Enable(int w, int h) {
    if (!g.backend || !g.supported) return false;
    if (g.enabled) {
        // 幂等: 已启用 -> 视尺寸是否变化重建
        if (w == g.srcW && h == g.srcH) return true;
        DestroyResources();
        g.enabled = false;
    }
    if (!AllocateResources(w, h)) return false;
    g.enabled = true;
    return true;
}

void Disable() {
    if (!g.enabled) return;
    DestroyResources();
    g.enabled = false;
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.enabled) return false;
    if (w == g.srcW && h == g.srcH) return true;   // fast path
    DestroyResources();
    if (!AllocateResources(w, h)) {
        g.enabled = false;
        return false;
    }
    return true;
}

// ==================== HDR 联动 ====================

void OnHDREnabled(int w, int h) {
    if (g.autoEnable && !g.enabled) {
        Enable(w, h);
    }
}

void OnHDRDisabled() {
    // SSR 必须先关 (HDR RT 消亡前清自己资源, 防 blit 用到失效 fbo)
    Disable();
}

void OnHDRResized(int w, int h) {
    if (g.enabled) Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()           { return g.autoEnable; }

// ==================== 参数 (7 对) ====================

void SetMaxSteps(int n)      { g.maxSteps = clampi(n, 8, 128); }
int  GetMaxSteps()            { return g.maxSteps; }

void  SetStepSize(float v)   { g.stepSize = clampf(v, 0.01f, 1.0f); }
float GetStepSize()           { return g.stepSize; }

void  SetThickness(float v)  { g.thickness = clampf(v, 0.01f, 5.0f); }
float GetThickness()          { return g.thickness; }

void  SetMaxDistance(float v){ g.maxDistance = clampf(v, 1.0f, 1000.0f); }
float GetMaxDistance()        { return g.maxDistance; }

void  SetIntensity(float v)  { g.intensity = clampf(v, 0.0f, 2.0f); }
float GetIntensity()          { return g.intensity; }

void  SetEdgeFade(float v)   { g.edgeFade = clampf(v, 0.0f, 0.5f); }
float GetEdgeFade()           { return g.edgeFade; }

void SetBlurEnabled(bool f)  { g.blurEnabled = f; }
bool GetBlurEnabled()         { return g.blurEnabled; }

// Phase E.10 — 反射模糊半径 [0.5, 4.0] (texel 单位)
void  SetBlurRadius(float v) { g.blurRadius = clampf(v, 0.5f, 4.0f); }
float GetBlurRadius()         { return g.blurRadius; }

// Phase E.11 — depth-aware bilateral 开关 (默认 true)
void SetBilateralEnabled(bool f) { g.bilateralEnabled = f; }
bool GetBilateralEnabled()        { return g.bilateralEnabled; }

// Phase E.11 — bilateral 深度权重 σ, clamp [50, 500] (与 SSAO bilateral 一致)
void  SetBlurDepthSigma(float v) { g.blurDepthSigma = clampf(v, 50.0f, 500.0f); }
float GetBlurDepthSigma()         { return g.blurDepthSigma; }

// ==================== 调试 API ====================

uint32_t GetReflectionTexId() {
    return g.enabled ? g.reflectTex : 0;
}

// ==================== 管线 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    if (!g.enabled || !g.supported || !g.backend) return;
    if (!hdrFbo || !hdrTex) return;
    if (!g.depthFbo || !g.depthTex || !g.reflectFbo || !g.reflectTex) return;

    // Phase E.8.x: 拿 HDR FBO 关联的 G-buffer normal tex (MRT slot 1).
    // backend 不支持 MRT 或 fbo 未带 normal 时返回 0 -> silent fallback.
    uint32_t normalTex = g.backend->GetHDRNormalTex(hdrFbo);
    if (!normalTex) {
        // 仅首次 skip 时打 warning (避免每帧刷屏)
        static bool warned = false;
        if (!warned) {
            CC::Log(CC::LOG_WARN,
                    "SSRRenderer::Process: HDR FBO 无 G-buffer normal RT, 跳过 SSR (后端不支持 MRT 或 FBO 旧版本)");
            warned = true;
        }
        return;
    }

    // 0. 旁路核心: 从 HDR FBO 复制 depth 到 SSR depth tex (复用 SSAO blit 接口)
    g.backend->BlitHDRDepthToSSAO(hdrFbo, g.depthFbo, g.srcW, g.srcH);

    // 取当前 projection 矩阵 + 计算逆
    float proj[16];
    float invProj[16];
    g.backend->GetProjection(proj);
    if (!InvertMat4(proj, invProj)) {
        // projection 退化 (如纯 2D 全 0 矩阵): 静默 skip
        return;
    }

    // 1. SSR raw: depthTex + normalTex + hdrTex -> reflectFbo (RGBA16F, full-res)
    g.backend->DrawSSR(g.depthTex, normalTex, hdrTex, g.reflectFbo,
                       g.srcW, g.srcH, proj, invProj,
                       g.maxSteps, g.stepSize, g.thickness,
                       g.maxDistance, g.edgeFade);

    // 2. (Phase E.10) 可选 blur: separable Gaussian H+V on half-res ping-pong
    //    blur 资源未分配 (旧 backend) 时 silent skip, composite 仍用 full-res reflect
    uint32_t finalReflectTex = g.reflectTex;
    if (g.blurEnabled && g.blurFbos[0] && g.blurFbos[1] && g.blurW > 0 && g.blurH > 0) {
        // H pass: full-res reflectTex -> half-res blurFbos[0]
        //   (一举完成 downsample + horizontal blur, 由硬件 bilinear filter 隐式 downsample)
        // Phase E.11: 额外传入 depthTex + bilateralEnabled + blurDepthSigma,
        //             shader runtime 选择 Gaussian/Bilateral 路径
        g.backend->DrawSSRBlur(g.reflectTex, g.depthTex,
                                g.blurFbos[0], g.blurW, g.blurH,
                                0, g.blurRadius,
                                g.bilateralEnabled, g.blurDepthSigma);
        // V pass: half-res blurTexs[0] -> half-res blurFbos[1]
        g.backend->DrawSSRBlur(g.blurTexs[0], g.depthTex,
                                g.blurFbos[1], g.blurW, g.blurH,
                                1, g.blurRadius,
                                g.bilateralEnabled, g.blurDepthSigma);
        finalReflectTex = g.blurTexs[1];   // composite 用 half-res 模糊结果, 自动 bilinear upscale
    }

    // 3. composite: HDR += reflect.rgb * reflect.a * intensity (加性)
    //    backend 内部用临时 RT 解 feedback loop
    g.backend->DrawSSRComposite(finalReflectTex, hdrFbo, g.srcW, g.srcH, g.intensity);
}

} // namespace SSRRenderer
