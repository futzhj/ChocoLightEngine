/**
 * @file taa_renderer.cpp
 * @brief Phase F.0 — TAA 主管线实现
 *
 * 与 header 约定: 所有 GL 操作经 RenderBackend; 本文件零 GL 依赖, 跨平台.
 */

#include "taa_renderer.h"
#include "render_backend.h"        // VelocityFormat 完整定义 + backend 接口
#include "hdr_renderer.h"          // 取 dilated velocity tex (E.18 输出)
#include "light.h"                 // CC::Log
#include <cstring>                 // memcpy

namespace TAARenderer {

namespace {

// ==================== 内部辅助 ====================

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// Phase F.0 — Halton(base=2,3) 8-sample, 与 SSRRenderer 中 kHaltonJitter 完全一致
/// 偏移范围: ±0.5 pixel (调用时除尺寸转 NDC 偏移)
/// 业界标准 TAA jitter 表 (UE/Unity/Frostbite 都用此或扩展)
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
    RenderBackend* backend  = nullptr;
    bool     inited         = false;
    bool     supported      = false;        // backend->SupportsTAA() 缓存
    bool     enabled        = false;
    bool     autoEnable     = false;        // 与 Phase E 模块一致, 用户主动 Enable

    int      width          = 0;
    int      height         = 0;

    // history ping-pong (与 SSR Temporal / Motion Blur 同模式)
    uint32_t historyFbos[2] = {0, 0};
    uint32_t historyTexs[2] = {0, 0};
    int      historyIdx     = 0;            // 当前 write 下标 (下帧为 read)
    bool     hasHistory     = false;        // 首帧 = false → shader 内 uHasHistory=0 输出 cur

    // 参数 (CONSENSUS §1.4 决策矩阵)
    float    blendAlpha       = 0.92f;       // history 权重 (略高于 SSR Temporal 的 0.9, 主管线累积更稳)
    bool     neighborhoodClip = true;        // 默认启用 9-tap AABB clip
    bool     jitterEnabled    = true;        // TAA Enable 时随之拉起 jitter
    float    sharpness        = 0.5f;        // Phase F.0.1: 4-tap unsharp mask 强度 [0, 2], 默认 0.5

    // jitter state
    uint64_t frameCounter   = 0;
    float    curJitterX     = 0.0f;
    float    curJitterY     = 0.0f;
};

static State g;

// ==================== 内部资源管理 ====================

/// 释放 history RT (与 backend 配对)
static void ReleaseRT() {
    if (!g.backend) return;
    if (g.historyFbos[0] || g.historyFbos[1] || g.historyTexs[0] || g.historyTexs[1]) {
        g.backend->DeleteTAAHistoryRT(g.historyFbos, g.historyTexs);
    }
    g.historyIdx    = 0;
    g.hasHistory    = false;
    g.frameCounter  = 0;
    g.curJitterX    = 0.0f;
    g.curJitterY    = 0.0f;
    g.width = g.height = 0;
}

/// 分配 history RT (RGBA16F × 2, 与 sceneTex 同尺寸)
static bool AllocateRT(int w, int h) {
    if (!g.backend || w <= 0 || h <= 0) return false;
    if (!g.backend->CreateTAAHistoryRT(w, h, g.historyFbos, g.historyTexs)) {
        CC::Log(CC::LOG_WARN, "TAARenderer: CreateTAAHistoryRT failed (%dx%d)", w, h);
        return false;
    }
    g.historyIdx   = 0;
    g.hasHistory   = false;
    g.frameCounter = 0;
    g.width  = w;
    g.height = h;
    return true;
}

} // anonymous namespace

// ==================== 生命周期 ====================

bool Init(RenderBackend* backend) {
    if (g.inited) {
        CC::Log(CC::LOG_WARN, "TAARenderer::Init: already initialized, ignored");
        return true;
    }
    if (!backend) {
        CC::Log(CC::LOG_ERROR, "TAARenderer::Init: backend is null");
        return false;
    }
    g.backend   = backend;
    g.supported = backend->SupportsTAA();
    g.inited    = true;
    if (g.supported) {
        CC::Log(CC::LOG_INFO, "TAARenderer: ready (backend supports TAA)");
    } else {
        CC::Log(CC::LOG_INFO,
                "TAARenderer: backend does NOT support TAA (Enable() will fail; silent fallback)");
    }
    return true;
}

void Shutdown() {
    if (!g.inited) return;
    ReleaseRT();
    g.enabled   = false;
    g.inited    = false;
    g.supported = false;
    g.backend   = nullptr;
}

bool IsInited() { return g.inited; }

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "TAARenderer::Enable: 模块未 Init");
        return false;
    }
    if (!g.supported) {
        CC::Log(CC::LOG_WARN, "TAARenderer::Enable: backend 不支持 TAA (shader 编译失败 / 无 RGBA16F)");
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "TAARenderer::Enable: invalid size (%d, %d)", w, h);
        return false;
    }
    if (g.enabled && g.width == w && g.height == h) return true;

    ReleaseRT();
    if (!AllocateRT(w, h)) {
        g.enabled = false;
        return false;
    }
    g.enabled = true;
    CC::Log(CC::LOG_INFO, "TAARenderer::Enable: TAA history RT created (%dx%d, RGBA16F × 2)", w, h);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleaseRT();
    g.enabled = false;
    // jitter 状态在 backend 内复位 (Process 末尾会 ClearJitteredProjection)
    if (g.backend) g.backend->ClearJitteredProjection();
    CC::Log(CC::LOG_INFO, "TAARenderer::Disable: TAA history RT released");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.inited) return false;
    if (!g.enabled) return Enable(w, h);
    if (g.width == w && g.height == h) return true;
    return Enable(w, h);   // Enable 内部会 ReleaseRT + AllocateRT
}

// ==================== HDR 联动 ====================

void OnHDREnabled(int w, int h) {
    if (g.autoEnable) Enable(w, h);
}

void OnHDRDisabled() {
    // HDR 关 → TAA 必须关 (依赖 HDR sceneTex + velocityTex)
    if (g.enabled) Disable();
}

void OnHDRResized(int w, int h) {
    if (g.enabled) Resize(w, h);
}

// ==================== 主循环 hook ====================

void ApplyJitter() {
    // 防御: TAA 未启用 / jitter 关 / backend 失效 → 不做任何事 (raster 用原始 projection)
    if (!g.enabled || !g.jitterEnabled || !g.backend) return;
    if (g.width <= 0 || g.height <= 0) return;

    // 取当前 unjittered projection (用户 SetPerspective 设置过的)
    float proj[16];
    g.backend->GetProjection(proj);

    // 取本帧 Halton 偏移 (±0.5 pixel 范围)
    const int j = (int)(g.frameCounter & 7u);
    g.curJitterX = kHaltonJitter[j][0];
    g.curJitterY = kHaltonJitter[j][1];

    // NDC 偏移 = 像素偏移 × 2 / RT 尺寸 (NDC 范围 [-1, +1] 对应 width/height 像素)
    const float ndcOffX = g.curJitterX * 2.0f / (float)g.width;
    const float ndcOffY = g.curJitterY * 2.0f / (float)g.height;

    // 修改 column-major 4x4 projection: m[8] (col 2, row 0) 和 m[9] (col 2, row 1)
    // 这等价于让 clip_pos.x += ndcOff.x * clip_pos.z (sub-pixel NDC 平移)
    // 业界标准方法 (UE/Frostbite 通用)
    float jitteredProj[16];
    memcpy(jitteredProj, proj, sizeof(proj));
    jitteredProj[8] += ndcOffX;
    jitteredProj[9] += ndcOffY;

    g.backend->LoadJitteredProjection(jitteredProj);
}

// ==================== 管线 Process ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    if (!g.enabled || !g.supported || !g.backend) {
        // 即使未启用, 也复位 backend jitter (防御性: ApplyJitter 后用户切关 TAA)
        if (g.backend) g.backend->ClearJitteredProjection();
        return;
    }
    if (!hdrFbo || !hdrTex) return;
    if (!g.historyFbos[0] || !g.historyFbos[1]) return;

    const int writeIdx = g.historyIdx;
    const int readIdx  = 1 - writeIdx;

    // 取 dilated velocity (Phase E.18 输出); fallback raw velocity
    // - dilation pass active 时: 单点采 dilatedTex (shader 内 uVelocityDilation=0 by backend)
    // - dilation pass 未跑时:    inline 9-tap raw velocity (shader 内 uVelocityDilation=velocityDilation)
    const uint32_t dilatedV  = HDRRenderer::GetDilatedVelocityTexture();
    const uint32_t rawV      = g.backend->GetHDRVelocityTex(hdrFbo);
    const uint32_t velocityTex = dilatedV ? dilatedV : rawV;

    // 跑 TAA shader: 输出到 historyFbos[writeIdx]
    g.backend->DrawTAAPass(hdrTex,
                            g.historyTexs[readIdx],
                            velocityTex,
                            g.historyFbos[writeIdx],
                            g.width, g.height,
                            g.blendAlpha,
                            g.neighborhoodClip ? 1 : 0,
                            g.hasHistory ? 1 : 0,
                            g.backend->GetVelocityDilation(),
                            g.backend->GetVelocityScale(),
                            g.backend->GetActiveVelocityFormat());

    // Phase F.0.1: sharpness > 0 走 4-tap unsharp mask sharpen pass (in-place 写回 sceneTex);
    //              否则保持 F.0 纯 blit 路径 (零 ALU 开销)
    if (g.sharpness > 0.0f) {
        g.backend->DrawTAASharpenPass(g.historyTexs[writeIdx], hdrFbo,
                                     g.width, g.height, g.sharpness);
    } else {
        g.backend->BlitTAAToHDR(g.historyTexs[writeIdx], hdrFbo, g.width, g.height);
    }

    // 状态推进
    g.historyIdx   = readIdx;             // ping-pong swap
    g.hasHistory   = true;
    g.frameCounter++;

    // 复位 jitter (下帧 BeginScene 后 ApplyJitter 重新设)
    g.backend->ClearJitteredProjection();
}

// ==================== 参数 ====================

void  SetBlendAlpha(float alpha) { g.blendAlpha = clampf(alpha, 0.0f, 1.0f); }
float GetBlendAlpha()            { return g.blendAlpha; }

void  SetNeighborhoodClip(bool on) { g.neighborhoodClip = on; }
bool  GetNeighborhoodClip()        { return g.neighborhoodClip; }

void  SetSharpness(float s) { g.sharpness = clampf(s, 0.0f, 2.0f); }
float GetSharpness()         { return g.sharpness; }

void  SetJitterEnabled(bool on) {
    g.jitterEnabled = on;
    // 若运行时关 jitter, 立即复位 backend 状态, 避免下帧仍用旧 jittered projection
    if (!on && g.backend) g.backend->ClearJitteredProjection();
}
bool  GetJitterEnabled() { return g.jitterEnabled; }

// ==================== 内部状态查询 ====================

int GetFrameCounter() { return (int)(g.frameCounter & 7u); }

void GetCurrentJitter(float* outX, float* outY) {
    if (outX) *outX = g.curJitterX;
    if (outY) *outY = g.curJitterY;
}

} // namespace TAARenderer
