/**
 * @file   streak_renderer.cpp
 * @brief  Phase E.6 — Streak (Anamorphic Flare) 模块实现
 *
 * 详见 streak_renderer.h 的架构说明.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.6.2
 */

#include "streak_renderer.h"
#include "render_backend.h"
#include "light.h"            // CC::Log

#include <cmath>              // std::pow

namespace {

/// 模块全局状态
struct State {
    RenderBackend* backend = nullptr;
    bool inited      = false;
    bool supported   = false;
    bool enabled     = false;
    bool autoEnable  = false;     // 默认 false (同 AE / LensDirt)

    // ping-pong RT 对
    uint32_t fbos[2]  = {0, 0};
    uint32_t texs[2]  = {0, 0};
    int      lumW     = 0;        // backend 实际创建尺寸 (~srcW/2)
    int      lumH     = 0;
    int      srcW     = 0;        // Enable 入参 (用于 HDR RT composite 尺寸)
    int      srcH     = 0;

    // 参数 (默认值见 ALIGNMENT §3.6)
    float threshold   = 1.0f;
    float intensity   = 0.3f;
    float length      = 0.02f;
    float dirX        = 1.0f;     // 水平 (最经典 anamorphic)
    float dirY        = 0.0f;
    int   iterations  = 5;
};

static State g;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void ReleaseRT() {
    if (g.backend && (g.fbos[0] || g.fbos[1])) {
        g.backend->DeleteStreakTargets(g.fbos, g.texs);
    }
    g.fbos[0] = g.fbos[1] = 0;
    g.texs[0] = g.texs[1] = 0;
    g.lumW = g.lumH = 0;
    g.srcW = g.srcH = 0;
    g.enabled = false;
}

} // anonymous namespace

namespace StreakRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.inited    = (backend != nullptr);
    g.supported = g.inited && backend->SupportsStreak();
}

void Shutdown() {
    ReleaseRT();
    g.backend   = nullptr;
    g.inited    = false;
    g.supported = false;
    g.enabled   = false;
    // 参数保留
}

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "StreakRenderer::Enable: module not Init'd");
        return false;
    }
    if (!g.supported) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "StreakRenderer::Enable: invalid size %dx%d", w, h);
        return false;
    }

    if (g.enabled) {
        if (g.srcW == w && g.srcH == h) return true;
        ReleaseRT();
    }

    uint32_t fbos[2] = {0, 0};
    uint32_t texs[2] = {0, 0};
    int lumW = 0, lumH = 0;
    if (!g.backend->CreateStreakTargets(w, h, fbos, texs, &lumW, &lumH)) {
        CC::Log(CC::LOG_ERROR,
                "StreakRenderer::Enable: CreateStreakTargets failed (%dx%d)", w, h);
        return false;
    }

    g.fbos[0] = fbos[0];
    g.fbos[1] = fbos[1];
    g.texs[0] = texs[0];
    g.texs[1] = texs[1];
    g.lumW    = lumW;
    g.lumH    = lumH;
    g.srcW    = w;
    g.srcH    = h;
    g.enabled = true;

    CC::Log(CC::LOG_INFO,
            "StreakRenderer::Enable: src=%dx%d, ping-pong RT=%dx%d, fbos=[%u,%u]",
            w, h, lumW, lumH, g.fbos[0], g.fbos[1]);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleaseRT();
    CC::Log(CC::LOG_INFO, "StreakRenderer::Disable");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.inited || !g.supported) return false;
    if (w <= 0 || h <= 0) return false;
    if (!g.enabled) return Enable(w, h);
    if (g.srcW == w && g.srcH == h) return true;
    ReleaseRT();
    return Enable(w, h);
}

// ==================== HDR 联动 ====================

void OnHDREnabled(int w, int h) {
    if (!g.autoEnable) return;
    Enable(w, h);
}

void OnHDRDisabled() {
    Disable();
}

void OnHDRResized(int w, int h) {
    if (!g.enabled) return;
    Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()          { return g.autoEnable; }

// ==================== 参数 ====================

void  SetThreshold(float v) { g.threshold = clampf(v, 0.0f, 1e9f); }
float GetThreshold()        { return g.threshold; }

void  SetIntensity(float v) { g.intensity = clampf(v, 0.0f, 1e9f); }
float GetIntensity()        { return g.intensity; }

void  SetLength(float v)    { g.length = clampf(v, 0.0f, 0.1f); }
float GetLength()           { return g.length; }

void SetDirection(float x, float y) {
    // (0, 0) 会让 shader normalize 得 NaN, 保留旧值
    if (std::fabs(x) < 1e-6f && std::fabs(y) < 1e-6f) return;
    g.dirX = x;
    g.dirY = y;
}

void GetDirection(float& outX, float& outY) {
    outX = g.dirX;
    outY = g.dirY;
}

void SetIterations(int n) { g.iterations = clampi(n, 1, 8); }
int  GetIterations()      { return g.iterations; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrFbo || !hdrTex) return;
    if (!g.fbos[0] || !g.fbos[1]) return;

    // 1. Bright pass: hdrTex → streakRT[0] (阈值提取, 共用 Bloom shader)
    g.backend->DrawStreakBright(hdrTex, g.fbos[0], g.lumW, g.lumH, g.threshold);

    // 2. N 次 ping-pong 方向倍距模糊
    int src = 0;
    for (int i = 0; i < g.iterations; ++i) {
        int dst = 1 - src;
        float stepLen = g.length * std::pow(2.0f, (float)i);   // 倍距扩展
        g.backend->DrawStreakBlur(g.texs[src], g.fbos[dst],
                                    g.lumW, g.lumH,
                                    stepLen, g.dirX, g.dirY);
        src = dst;
    }

    // 3. 加性合成: streakRT[final] × intensity → hdrFbo
    g.backend->DrawStreakComposite(g.texs[src], hdrFbo,
                                    g.srcW, g.srcH, g.intensity);
}

} // namespace StreakRenderer
