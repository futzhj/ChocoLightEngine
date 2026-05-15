/**
 * @file motion_blur_renderer.cpp
 * @brief Phase E.15 — Velocity-driven Motion Blur 实现
 *
 * 设计：纯壳模式，所有 GL 调用经 backend 转发。模块仅管理 state 与生命周期。
 */

#include "motion_blur_renderer.h"
#include "render_backend.h"        // RenderBackend 4 个 motion blur 虚接口
#include "hdr_renderer.h"          // GetVelocityTexture (取 velocity buffer GL id)
#include "light.h"                 // CC::Log

namespace MotionBlurRenderer {

// ==================== 内部状态 ====================

namespace {

struct State {
    RenderBackend* backend     = nullptr;
    bool           inited      = false;
    bool           supported   = false;     // backend->SupportsMotionBlur() 缓存
    bool           enabled     = false;     // Enable 成功 + 未 Disable
    bool           autoEnable  = false;     // 默认 false (与 LensDirt/SSR 同, Bloom 例外)

    // ping-pong RT 资源
    uint32_t       fbo         = 0;
    uint32_t       tex         = 0;
    int            width       = 0;
    int            height      = 0;

    // 调参
    float          strength    = 1.0f;      // [0, 4]
    int            sampleCount = 8;         // [1, 32]
};

static State g;

// 内部辅助: 释放 RT 资源 (不改 strength/sampleCount)
void ReleaseRT() {
    if (g.backend && (g.fbo || g.tex)) {
        g.backend->DeleteMotionBlurRT(g.fbo, g.tex);
    }
    g.fbo    = 0;
    g.tex    = 0;
    g.width  = 0;
    g.height = 0;
}

// 内部辅助: 创建 RT 资源 (失败返回 false 并清理半成品)
bool CreateRT(int w, int h) {
    if (!g.backend) return false;
    if (w <= 0 || h <= 0) return false;
    uint32_t tex = 0;
    uint32_t fbo = g.backend->CreateMotionBlurRT(w, h, &tex);
    if (!fbo || !tex) {
        if (fbo || tex) g.backend->DeleteMotionBlurRT(fbo, tex);   // 部分失败兜底
        return false;
    }
    g.fbo    = fbo;
    g.tex    = tex;
    g.width  = w;
    g.height = h;
    return true;
}

// 简单 clamp (避免 #include <algorithm> 的 std::clamp 模板膨胀)
inline float ClampF(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
inline int ClampI(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

} // anonymous namespace

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.inited    = (backend != nullptr);
    g.supported = g.inited && backend->SupportsMotionBlur();
    if (g.supported) {
        CC::Log(CC::LOG_INFO, "MotionBlurRenderer: ready (backend supports motion blur)");
    } else if (g.inited) {
        CC::Log(CC::LOG_INFO,
                "MotionBlurRenderer: backend does NOT support motion blur (Enable will fail)");
    }
}

void Shutdown() {
    ReleaseRT();
    g.backend    = nullptr;
    g.inited     = false;
    g.supported  = false;
    g.enabled    = false;
    // 参数保留 (与 Bloom/HDR 同风格, 下次 Init+Enable 继承)
}

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "MotionBlurRenderer::Enable: 模块未 Init");
        return false;
    }
    if (!g.supported) {
        // 低频 warn: 非 GL33 后端 / shader 编译失败
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "MotionBlurRenderer::Enable: 非法尺寸 %dx%d", w, h);
        return false;
    }

    // 已启用且尺寸相同 → no-op
    if (g.enabled && g.width == w && g.height == h) return true;

    // 不同尺寸或未启用 → 释放旧 RT 重建
    ReleaseRT();
    if (!CreateRT(w, h)) {
        CC::Log(CC::LOG_ERROR,
                "MotionBlurRenderer::Enable: CreateMotionBlurRT failed (%dx%d)", w, h);
        g.enabled = false;
        return false;
    }
    g.enabled = true;
    CC::Log(CC::LOG_INFO,
            "MotionBlurRenderer::Enable: RT created (%dx%d, fbo=%u, tex=%u)",
            w, h, g.fbo, g.tex);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleaseRT();
    g.enabled = false;
    CC::Log(CC::LOG_INFO, "MotionBlurRenderer::Disable: RT released");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.inited) return false;
    if (w <= 0 || h <= 0) return false;
    if (!g.enabled) return Enable(w, h);
    if (g.width == w && g.height == h) return true;   // 同尺寸 no-op
    return Enable(w, h);                               // Enable 内部会 ReleaseRT + CreateRT
}

// ==================== HDR 联动 ====================

void OnHDREnabled(int w, int h) {
    if (g.autoEnable) Enable(w, h);
    // autoEnable=false 时仅记下 HDR 启用事实, 等用户显式 Enable
}

void OnHDRDisabled() {
    // HDR 关 → motion blur 必须关 (依赖 HDR sceneTex + velocityTex)
    if (g.enabled) Disable();
}

void OnHDRResized(int w, int h) {
    if (g.enabled) Resize(w, h);
    // 否则 no-op (用户没启 motion blur)
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()           { return g.autoEnable; }

// ==================== 参数 ====================

void  SetStrength(float v)   { g.strength = ClampF(v, 0.0f, 4.0f); }
float GetStrength()           { return g.strength; }

void SetSampleCount(int n)    { g.sampleCount = ClampI(n, 1, 32); }
int  GetSampleCount()         { return g.sampleCount; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // 防御: 5 个先决条件任一不满足 → silent skip
    if (!g.enabled || !g.backend || !hdrFbo || !hdrTex) return;
    if (!g.fbo || !g.tex) return;

    // velocity buffer 来源: HDRRenderer 提供
    uint32_t velocityTex = HDRRenderer::GetVelocityTexture();
    if (!velocityTex) return;   // 后端不支持 velocity buffer 或 HDR 未启 → silent skip

    g.backend->DrawMotionBlur(hdrTex, velocityTex,
                               g.fbo, g.tex,
                               hdrFbo,
                               g.width, g.height,
                               g.strength, g.sampleCount);
}

} // namespace MotionBlurRenderer
