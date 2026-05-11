/**
 * @file   lens_dirt_renderer.cpp
 * @brief  Phase E.6 — Lens Dirt 模块实现
 *
 * 详见 lens_dirt_renderer.h 的架构说明.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.6.2
 */

#include "lens_dirt_renderer.h"
#include "render_backend.h"
#include "light.h"            // CC::Log

namespace {

/// 模块全局状态 (与 BloomRenderer / AE Renderer 同风格, 匿名 namespace 封装)
struct State {
    RenderBackend* backend    = nullptr;
    bool     inited           = false;
    bool     supported        = false;  // backend->SupportsLensDirt() 缓存
    bool     enabled          = false;
    bool     autoEnable       = false;  // 默认 false (与 Bloom 默认 true 区别, 与 AE 同)

    uint32_t dirtTexId        = 0;      // 0 = fallback 1x1 白 (后端处理)
    float    intensity        = 0.4f;   // 默认值 (ALIGNMENT §3.6)
};

static State g;

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // anonymous namespace

namespace LensDirtRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.inited    = (backend != nullptr);
    g.supported = g.inited && backend->SupportsLensDirt();
}

void Shutdown() {
    g.backend    = nullptr;
    g.inited     = false;
    g.supported  = false;
    g.enabled    = false;
    // 参数保留 (下次 Init + Enable 继承)
}

// ==================== Enable / Disable ====================

bool Enable() {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "LensDirtRenderer::Enable: module not Init'd");
        return false;
    }
    if (!g.supported) {
        return false;
    }
    g.enabled = true;
    CC::Log(CC::LOG_INFO, "LensDirtRenderer::Enable: enabled (dirtTexId=%u, intensity=%.2f)",
            g.dirtTexId, g.intensity);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    g.enabled = false;
    CC::Log(CC::LOG_INFO, "LensDirtRenderer::Disable");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

// ==================== HDR 自动联动 ====================

void OnHDREnabled(int /*w*/, int /*h*/) {
    if (!g.autoEnable) return;
    Enable();
}

void OnHDRDisabled() {
    // HDR 关后 LensDirt 无 bloom 输入, 强制关闭
    Disable();
}

void OnHDRResized(int /*w*/, int /*h*/) {
    // LensDirt 无独立 RT; 尺寸直接从 Process 入参拿
    // no-op
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()          { return g.autoEnable; }

// ==================== 参数 ====================

void     SetDirtTextureId(uint32_t texId) { g.dirtTexId = texId; }
uint32_t GetDirtTextureId()               { return g.dirtTexId; }

void  SetIntensity(float v) { g.intensity = clampf(v, 0.0f, 1e9f); }
float GetIntensity()        { return g.intensity; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t bloomTex, int w, int h) {
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrFbo || !bloomTex) return;      // Bloom 未启用 → bloomTex=0 → no-op
    if (w <= 0 || h <= 0) return;

    // 后端 DrawLensDirtComposite 内部对 dirtTex=0 会 fallback 到 whiteTex1x1
    g.backend->DrawLensDirtComposite(bloomTex, g.dirtTexId,
                                      hdrFbo, w, h, g.intensity);
}

} // namespace LensDirtRenderer
