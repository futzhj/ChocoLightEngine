/**
 * @file   ssr_renderer.cpp
 * @brief  Phase E.9 — SSRRenderer 实现 (Phase E.10/E.11/E.12 增量)
 *
 * 模块结构 (与 SSAORenderer 同风格):
 *   - State (匿名 namespace, 模块单例)
 *   - 生命周期: Init/Shutdown/Enable/Disable/Resize/IsEnabled/IsSupported
 *   - HDR 联动: OnHDREnabled/OnHDRDisabled/OnHDRResized
 *   - 参数 setter/getter (13 对 — Phase E.9 7 对 + E.10/E.11/E.12) + AutoEnable
 *   - Process: blit depth -> SSR raw [+jitter] -> [temporal] -> [blur] -> composite
 *   - 辅助: InvertMat4 / Mat4Mul / Halton-2,3 8-sample 静态表
 *
 * Phase E.12/E.13: TAA-style temporal SSR.
 *   - full-res RGBA16F history × 2 ping-pong
 *   - Halton-2,3 8-sample jitter (±0.5 pixel) 默认启用
 *   - velocity buffer reprojection; missing velocity 时 fallback 到 depth reverse-reprojection
 *   - neighborhood AABB clip rejection 默认
 */

#include "ssr_renderer.h"
#include "render_backend.h"
#include "hdr_renderer.h"     // Phase E.18 — 取 dilated velocity tex
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

/// Phase E.12 — 列主序 4x4 矩阵乘法 (out = a * b)
/// 与 Mat4::operator* 保持一致, GL uniform 传 GL_FALSE 不转置.
static void Mat4Mul(const float* a, const float* b, float* out) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = s;
        }
    }
}

/// Phase E.12 — Halton(base=2,3) 8-sample, 行业标准 TAA jitter 表
/// 偏移范围: ±0.5 pixel (调用时 backend 除尺寸转 UV 空间)
static const float kHaltonJitter[8][2] = {
    { 0.0000f,  0.0000f},
    {-0.5000f,  0.3333f},
    { 0.2500f, -0.3333f},
    {-0.2500f,  0.1111f},
    { 0.3750f, -0.1111f},
    {-0.3750f,  0.4444f},
    { 0.1250f, -0.4444f},
    {-0.1250f,  0.2222f},
};

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

    // Phase E.12 — Temporal SSR (用户拍板 2026-05-14)
    bool      temporalEnabled    = true;        // 默认 ON (TAA-style 业界标准)
    float     temporalAlpha      = 0.9f;        // history 权重 clamp [0.5, 0.99]
    int       rejectionMode      = 1;           // 0=current-depth threshold, 1=neighborhood clip (默认)
    uint32_t  historyFbos[2]     = {0, 0};      // ping-pong FBO
    uint32_t  historyTexs[2]     = {0, 0};      // ping-pong tex (与 reflectTex 同尺寸 full-res)
    int       historyIdx         = 0;           // 当前 write 下标 (下帧为 read)
    float     prevViewProj[16]   = {0};         // 上一帧 viewProj 缓存 (用于 reprojection)
    bool      hasPrevViewProj    = false;       // 首帧标志 (false 时 shader 走 cur 路径)
    uint64_t  frameCounter       = 0;           // jitter 序列索引 (% 8)
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
    // Phase E.12 — history ping-pong
    if (g.historyFbos[0] || g.historyFbos[1] || g.historyTexs[0] || g.historyTexs[1]) {
        g.backend->DeleteSSRHistoryRT(g.historyFbos, g.historyTexs);
    }
    g.historyIdx       = 0;
    g.hasPrevViewProj  = false;
    g.frameCounter     = 0;
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

    // Phase E.12 — history ping-pong RT: full-res RGBA16F × 2 (与 reflectTex 同尺寸)
    // 失败不致命: temporal 自动降级 (Process 检查指针)
    if (!g.backend->CreateSSRHistoryRT(w, h, g.historyFbos, g.historyTexs)) {
        g.historyFbos[0] = g.historyFbos[1] = 0;
        g.historyTexs[0] = g.historyTexs[1] = 0;
        CC::Log(CC::LOG_INFO,
                "SSRRenderer: history RT 未分配 (backend 不支持 / OOM), temporal 降级为无累积");
    }
    g.historyIdx       = 0;
    g.hasPrevViewProj  = false;
    g.frameCounter     = 0;
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

// Phase E.12 — Temporal SSR 开关 (默认 true, TAA-style 业界标准)
void SetTemporalEnabled(bool f) {
    if (g.temporalEnabled == f) return;
    g.temporalEnabled = f;
    // 状态切换时重置 首帧标志, 避免失效的 prev 矩阵让 reproject 出错
    g.hasPrevViewProj = false;
}
bool GetTemporalEnabled() { return g.temporalEnabled; }

// Phase E.12 — History blend 权重 clamp [0.5, 0.99]
void  SetTemporalAlpha(float v) { g.temporalAlpha = clampf(v, 0.5f, 0.99f); }
float GetTemporalAlpha()         { return g.temporalAlpha; }

// Phase E.12 — Rejection 模式 clamp {0, 1}
void SetRejectionMode(int m) { g.rejectionMode = (m <= 0) ? 0 : 1; }
int  GetRejectionMode()       { return g.rejectionMode; }

// ==================== 调试 API ====================

uint32_t GetReflectionTexId() {
    return g.enabled ? g.reflectTex : 0;
}

// ==================== 管线 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // 转发到 region 版本 (0/0/0/0 = 全屏老路径, 零回归)
    Process(hdrFbo, hdrTex, 0, 0, 0, 0);
}

// Phase F.0.10.3 — Region 限定 SSR (split-screen 必备)
// 5 个 backend pass 全部加 region: depth blit / raw / temporal / blur×2 / composite
// blur pass 由 caller 负责 region 缩半 (full-res -> half-res 空间)
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH) {
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

    // Phase F.0.10.3 — half-res blur region (caller 处理 full-res → half-res 缩半)
    // 与 backend CreateSSRBlurRT 内部 max(1, full/2) 同模式
    const bool useRegion = (rgnW > 0 && rgnH > 0);
    int blurRgnX = 0, blurRgnY = 0, blurRgnW = 0, blurRgnH = 0;
    if (useRegion) {
        blurRgnX = rgnX / 2;
        blurRgnY = rgnY / 2;
        blurRgnW = (rgnW > 1) ? (rgnW / 2) : 1;
        blurRgnH = (rgnH > 1) ? (rgnH / 2) : 1;
    }

    // 0. 旁路核心: 从 HDR FBO 复制 depth 到 SSR depth tex (复用 SSAO blit 接口)
    g.backend->BlitHDRDepthToSSAO(hdrFbo, g.depthFbo, g.srcW, g.srcH,
                                   rgnX, rgnY, rgnW, rgnH);

    // 取当前 view + projection + 计算逆 (Phase E.12 需 viewProj 用于 reprojection)
    float view[16], proj[16];
    float invProj[16];
    g.backend->GetView(view);
    g.backend->GetProjection(proj);
    if (!InvertMat4(proj, invProj)) {
        // projection 退化 (如纯 2D 全 0 矩阵): 静默 skip
        return;
    }

    // Phase E.12 — jitter 计算 (仅 temporal 启用时生效; 其他时为 0 保持退化后等同于 Phase E.11)
    float jitterX = 0.0f, jitterY = 0.0f;
    const bool temporalActive = g.temporalEnabled
                              && g.historyFbos[0] && g.historyFbos[1]
                              && g.historyTexs[0] && g.historyTexs[1];
    if (temporalActive) {
        const int j = (int)(g.frameCounter & 7u);
        jitterX = kHaltonJitter[j][0];
        jitterY = kHaltonJitter[j][1];
    }

    // 1. SSR raw: depthTex + normalTex + hdrTex -> reflectFbo (RGBA16F, full-res)
    //    Phase E.12: + jitter (±0.5 pixel) 让多帧采样位置分散
    g.backend->DrawSSR(g.depthTex, normalTex, hdrTex, g.reflectFbo,
                       g.srcW, g.srcH, proj, invProj,
                       g.maxSteps, g.stepSize, g.thickness,
                       g.maxDistance, g.edgeFade,
                       jitterX, jitterY,
                       rgnX, rgnY, rgnW, rgnH);

    // Phase E.12 — Temporal pass: reproject + clip + blend
    // 输出: historyTexs[writeIdx] = 本帧 temporal 结果 (也是 blur 的输入)
    uint32_t srcForBlur = g.reflectTex;
    if (temporalActive) {
        const int writeIdx = g.historyIdx;
        const int readIdx  = 1 - writeIdx;

        // 计算 curViewProj + invCurViewProj + reprojMat
        float curViewProj[16];
        Mat4Mul(proj, view, curViewProj);

        float invCurViewProj[16];
        float reprojMat[16];
        if (InvertMat4(curViewProj, invCurViewProj)) {
            Mat4Mul(g.prevViewProj, invCurViewProj, reprojMat);

            // Phase E.14: 透传 dilation / scale / format，让 SSRTemporal shader 选解码 + 邻域路径
            // Phase E.18: 优先取 dilated velocity (HDR EndScene 已做过 9-tap),
            //             shader 内 uVelocityDilation 由 backend 据 dilationPassActive_ 自动置 0;
            //             dilated 不可用时 fallback raw velocityTex (shader 内 inline 9-tap)
            const uint32_t dilatedVTex = HDRRenderer::GetDilatedVelocityTexture();
            const uint32_t rawVTex     = g.backend->GetHDRVelocityTex(hdrFbo);
            const uint32_t velocityTex = dilatedVTex ? dilatedVTex : rawVTex;
            g.backend->DrawSSRTemporal(g.reflectTex,
                                       g.historyTexs[readIdx],
                                       g.depthTex,
                                       velocityTex,
                                       g.historyFbos[writeIdx],
                                       g.srcW, g.srcH,
                                       reprojMat, invProj,
                                       g.temporalAlpha,
                                       g.rejectionMode,
                                       g.hasPrevViewProj ? 1 : 0,
                                       g.backend->GetVelocityDilation(),
                                       g.backend->GetVelocityScale(),
                                       g.backend->GetActiveVelocityFormat(),
                                       rgnX, rgnY, rgnW, rgnH);
            srcForBlur = g.historyTexs[writeIdx];
            memcpy(g.prevViewProj, curViewProj, sizeof(curViewProj));
            g.hasPrevViewProj = true;
            g.historyIdx = readIdx;   // swap (下一帧 write 到原 read 的位置)
        } else {
            g.hasPrevViewProj = false;
        }
    }

    // 2. (Phase E.10) 可选 blur: separable Gaussian H+V on half-res ping-pong
    //    输入源: temporal 启用且成功 -> historyTexs[writeIdx]; 否则 -> reflectTex
    //    region: 已缩半 (blurRgn*)
    uint32_t finalReflectTex = srcForBlur;
    if (g.blurEnabled && g.blurFbos[0] && g.blurFbos[1] && g.blurW > 0 && g.blurH > 0) {
        // H pass: full-res srcForBlur -> half-res blurFbos[0]
        // Phase E.11: 额外传入 depthTex + bilateralEnabled + blurDepthSigma
        g.backend->DrawSSRBlur(srcForBlur, g.depthTex,
                                g.blurFbos[0], g.blurW, g.blurH,
                                0, g.blurRadius,
                                g.bilateralEnabled, g.blurDepthSigma,
                                blurRgnX, blurRgnY, blurRgnW, blurRgnH);
        // V pass: half-res blurTexs[0] -> half-res blurFbos[1]
        g.backend->DrawSSRBlur(g.blurTexs[0], g.depthTex,
                                g.blurFbos[1], g.blurW, g.blurH,
                                1, g.blurRadius,
                                g.bilateralEnabled, g.blurDepthSigma,
                                blurRgnX, blurRgnY, blurRgnW, blurRgnH);
        finalReflectTex = g.blurTexs[1];   // composite 用 half-res 模糊结果, 自动 bilinear upscale
    }

    // 3. composite: HDR += reflect.rgb * reflect.a * intensity (加性)
    //    backend 内部用临时 RT 解 feedback loop
    g.backend->DrawSSRComposite(finalReflectTex, hdrFbo, g.srcW, g.srcH, g.intensity,
                                 rgnX, rgnY, rgnW, rgnH);

    // Phase E.12 — 帧计数器推进 (仅 Halton 索引用, 不重置)
    ++g.frameCounter;
}

} // namespace SSRRenderer
