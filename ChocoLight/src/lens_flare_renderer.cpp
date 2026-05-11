/**
 * @file   lens_flare_renderer.cpp
 * @brief  Phase E.7 — Lens Flare 模块实现
 *
 * 详见 lens_flare_renderer.h 的架构说明.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.7.2
 */

#include "lens_flare_renderer.h"
#include "render_backend.h"
#include "light.h"            // CC::Log

namespace {

struct State {
    RenderBackend* backend = nullptr;
    bool inited      = false;
    bool supported   = false;
    bool enabled     = false;
    bool autoEnable  = false;     // 默认 false (与 Streak/LensDirt/AE 一致)

    // ping-pong RT
    uint32_t fbos[2]  = {0, 0};
    uint32_t texs[2]  = {0, 0};
    int      lumW     = 0;        // backend 实际创建尺寸 (~srcW/2)
    int      lumH     = 0;
    int      srcW     = 0;        // composite 还原到原 HDR 尺寸
    int      srcH     = 0;

    // 参数 (默认值见 ALIGNMENT §3.2)
    float threshold           = 1.0f;
    float intensity           = 0.4f;
    int   ghostCount          = 4;
    float ghostDispersal      = 0.4f;
    float haloWidth           = 0.5f;
    float chromaticAberration = 0.005f;
    bool  distortionEnabled   = true;
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
        g.backend->DeleteLensFlareTargets(g.fbos, g.texs);
    }
    g.fbos[0] = g.fbos[1] = 0;
    g.texs[0] = g.texs[1] = 0;
    g.lumW = g.lumH = 0;
    g.srcW = g.srcH = 0;
    g.enabled = false;
}

} // anonymous namespace

namespace LensFlareRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.inited    = (backend != nullptr);
    g.supported = g.inited && backend->SupportsLensFlare();
}

void Shutdown() {
    ReleaseRT();
    g.backend   = nullptr;
    g.inited    = false;
    g.supported = false;
    g.enabled   = false;
    // 参数保留 (下次 Init+Enable 继承)
}

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "LensFlareRenderer::Enable: module not Init'd");
        return false;
    }
    if (!g.supported) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "LensFlareRenderer::Enable: invalid size %dx%d", w, h);
        return false;
    }
    if (g.enabled) {
        if (g.srcW == w && g.srcH == h) return true;
        ReleaseRT();
    }

    uint32_t fbos[2] = {0, 0};
    uint32_t texs[2] = {0, 0};
    int lumW = 0, lumH = 0;
    if (!g.backend->CreateLensFlareTargets(w, h, fbos, texs, &lumW, &lumH)) {
        CC::Log(CC::LOG_ERROR,
                "LensFlareRenderer::Enable: CreateLensFlareTargets failed (%dx%d)", w, h);
        return false;
    }

    g.fbos[0] = fbos[0]; g.fbos[1] = fbos[1];
    g.texs[0] = texs[0]; g.texs[1] = texs[1];
    g.lumW    = lumW;    g.lumH    = lumH;
    g.srcW    = w;       g.srcH    = h;
    g.enabled = true;

    CC::Log(CC::LOG_INFO,
            "LensFlareRenderer::Enable: src=%dx%d, ping-pong RT=%dx%d, fbos=[%u,%u]",
            w, h, lumW, lumH, g.fbos[0], g.fbos[1]);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleaseRT();
    CC::Log(CC::LOG_INFO, "LensFlareRenderer::Disable");
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

void OnHDRDisabled() { Disable(); }

void OnHDRResized(int w, int h) {
    if (!g.enabled) return;
    Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()          { return g.autoEnable; }

// ==================== 参数 ====================

void  SetThreshold(float v)            { g.threshold = clampf(v, 0.0f, 1e9f); }
float GetThreshold()                   { return g.threshold; }

void  SetIntensity(float v)            { g.intensity = clampf(v, 0.0f, 1e9f); }
float GetIntensity()                   { return g.intensity; }

void  SetGhostCount(int n)             { g.ghostCount = clampi(n, 0, 8); }
int   GetGhostCount()                  { return g.ghostCount; }

void  SetGhostDispersal(float v)       { g.ghostDispersal = clampf(v, 0.0f, 2.0f); }
float GetGhostDispersal()              { return g.ghostDispersal; }

void  SetHaloWidth(float v)            { g.haloWidth = clampf(v, 0.0f, 1.0f); }
float GetHaloWidth()                   { return g.haloWidth; }

void  SetChromaticAberration(float v)  { g.chromaticAberration = clampf(v, 0.0f, 0.02f); }
float GetChromaticAberration()         { return g.chromaticAberration; }

void SetDistortionEnabled(bool flag)   { g.distortionEnabled = flag; }
bool GetDistortionEnabled()            { return g.distortionEnabled; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrFbo || !hdrTex) return;
    if (!g.fbos[0] || !g.fbos[1]) return;

    // 1. Bright pass: hdrTex → lfRT[0] (复用 Bloom 算法 + soft knee)
    g.backend->DrawBloomBrightPass(hdrTex, g.fbos[0], g.lumW, g.lumH, g.threshold);

    // 2. Ghost + Halo + Chromatic Aberration: lfRT[0] → lfRT[1]
    g.backend->DrawLensFlareGhost(g.texs[0], g.fbos[1],
                                    g.lumW, g.lumH,
                                    g.ghostCount, g.ghostDispersal,
                                    g.haloWidth, g.chromaticAberration,
                                    g.distortionEnabled);

    // 3. Composite: lfRT[1] additive → hdrFbo (复用 Bloom composite + intensity)
    g.backend->DrawBloomComposite(g.texs[1], hdrFbo,
                                    g.srcW, g.srcH, g.intensity);
}

} // namespace LensFlareRenderer
