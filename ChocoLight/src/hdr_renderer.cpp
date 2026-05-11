/**
 * @file hdr_renderer.cpp
 * @brief Phase E.3.2 — HDR 离屏渲染管线实现
 *
 * 与 header 约定: 所有 GL 操作经 RenderBackend; 本文件零 GL 依赖, 跨平台.
 */

#include "hdr_renderer.h"
#include "bloom_renderer.h"             // Phase E.4.2 — HDR Enable/Disable/Resize 联动回调
#include "auto_exposure_renderer.h"     // Phase E.5.2 — HDR Enable/Disable/Resize 联动回调 + EndScene exposure 覆盖
#include "render_backend.h"
#include "light.h"         // CC::Log

#include <chrono>

namespace HDRRenderer {

// ==================== 内部状态 ====================

namespace {

struct State {
    RenderBackend* backend   = nullptr;
    bool           inited    = false;  // Init() 调过 (不等于 HDR 启用)
    bool           supported = false;  // backend->SupportsHDR() 缓存

    // HDR RT 资源
    bool           enabled   = false;  // Enable() 成功 + 未 Disable
    bool           paused    = false;  // 被 SetCanvas 暂停
    uint32_t       fbo       = 0;      // 0 = 未创建
    uint32_t       sceneTex  = 0;      // RGBA16F 颜色纹理
    int            width     = 0;
    int            height    = 0;

    // Tonemap 参数 (Phase E.3.1 + E.3.4)
    float          exposure  = 1.0f;
    float          gamma     = 2.2f;
    int            tonemap   = 0;       // Phase E.3.4 — 0=ACES default
};

static State g;

// 内部辅助: 释放 RT 资源 (不改 exposure/gamma)
void ReleaseRT() {
    if (g.backend && (g.fbo || g.sceneTex)) {
        g.backend->DeleteHDRFBO(g.fbo, g.sceneTex);
    }
    g.fbo = 0;
    g.sceneTex = 0;
    g.width = 0;
    g.height = 0;
}

// 内部辅助: 创建 RT 资源 (失败返回 false 并清理)
bool CreateRT(int w, int h) {
    if (!g.backend) return false;
    if (w <= 0 || h <= 0) return false;
    uint32_t tex = 0;
    uint32_t fbo = g.backend->CreateHDRFBO(w, h, &tex);
    if (!fbo || !tex) {
        if (fbo || tex) g.backend->DeleteHDRFBO(fbo, tex);  // 部分失败兜底
        return false;
    }
    g.fbo = fbo;
    g.sceneTex = tex;
    g.width = w;
    g.height = h;
    return true;
}

} // anonymous namespace

// ==================== 生命周期 ====================

bool Init(RenderBackend* backend) {
    if (g.inited) {
        CC::Log(CC::LOG_WARN, "HDRRenderer::Init: already initialized, ignored");
        return true;
    }
    if (!backend) {
        CC::Log(CC::LOG_ERROR, "HDRRenderer::Init: backend is null");
        return false;
    }

    g.backend   = backend;
    g.supported = backend->SupportsHDR();
    g.inited    = true;

    if (g.supported) {
        CC::Log(CC::LOG_INFO, "HDRRenderer: ready (backend supports HDR)");
    } else {
        CC::Log(CC::LOG_INFO,
                "HDRRenderer: backend does NOT support HDR (Enable() will fail, LDR path active)");
    }
    return true;
}

void Shutdown() {
    if (!g.inited) return;
    ReleaseRT();
    g.enabled   = false;
    g.paused    = false;
    g.inited    = false;
    g.supported = false;
    g.backend   = nullptr;
}

bool IsInited() { return g.inited; }

// ==================== HDR 开关 ====================

bool Enable(int w, int h) {
    if (!g.inited) {
        CC::Log(CC::LOG_WARN, "HDRRenderer::Enable: Init() not called yet");
        return false;
    }
    if (!g.supported) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::Enable: backend does not support HDR (SupportsHDR=false)");
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::Enable: invalid size (%d, %d), must be > 0", w, h);
        return false;
    }

    // 如果已启用且尺寸相同, 什么都不做
    if (g.enabled && g.width == w && g.height == h) {
        return true;
    }

    // 释放旧 RT (如果有), 创建新尺寸
    ReleaseRT();
    if (!CreateRT(w, h)) {
        CC::Log(CC::LOG_ERROR, "HDRRenderer::Enable: CreateHDRFBO failed (%dx%d)", w, h);
        g.enabled = false;
        return false;
    }

    g.enabled = true;
    g.paused  = false;
    CC::Log(CC::LOG_INFO, "HDRRenderer::Enable: HDR RT created (%dx%d, fbo=%u, tex=%u)",
            w, h, g.fbo, g.sceneTex);

    // Phase E.4.2 — HDR 已启用, 通知 Bloom 模块 (autoEnable=true 时自动拉起)
    BloomRenderer::OnHDREnabled(w, h);
    // Phase E.5.2 — 同时通知 AE 模块 (autoEnable=false 时 no-op; 默认 manual exposure)
    AutoExposureRenderer::OnHDREnabled(w, h);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    // Phase E.5.2 — 先关 AE (AE 依赖 HDR RT 与 Bloom 同, 顺序任意, 安全先关)
    AutoExposureRenderer::OnHDRDisabled();
    // Phase E.4.2 — 先通知 Bloom 模块 (Bloom 依赖 HDR RT, 先关 Bloom 再关 HDR)
    BloomRenderer::OnHDRDisabled();
    ReleaseRT();
    g.enabled = false;
    g.paused  = false;
    CC::Log(CC::LOG_INFO, "HDRRenderer::Disable: HDR RT released");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.inited) return false;
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::Resize: invalid size (%d, %d)", w, h);
        return false;
    }
    if (!g.enabled) {
        // 未 Enable 时 Resize 等价于 Enable
        return Enable(w, h);
    }
    if (g.width == w && g.height == h) {
        return true;  // 尺寸相同, no-op
    }
    bool ok = Enable(w, h);  // Enable 内部会 ReleaseRT + CreateRT + OnHDREnabled
    // 注: Enable 已调过 OnHDREnabled, 该回调等价于 Bloom/AE Resize; 以下 OnHDRResized 重复调
    //     有 no-op 保护 (Resize 内部对于已同尺寸直接 return true)
    if (ok) {
        BloomRenderer::OnHDRResized(w, h);
        AutoExposureRenderer::OnHDRResized(w, h);   // Phase E.5.2
    }
    return ok;
}

// ==================== 主循环 hook ====================

void BeginScene() {
    // HDR 未启用 / 被 SetCanvas 暂停 / 资源失效: 静默 no-op
    if (!g.enabled || g.paused || !g.backend || !g.fbo) return;

    g.backend->BindFBO(g.fbo);
    g.backend->SetViewport(0, 0, g.width, g.height);
    // 清为透明黑 (HDR 中的 0,0,0,0 = 无光, tonemap 后仍是 0,0,0)
    g.backend->ClearCurrent(0.0f, 0.0f, 0.0f, 0.0f);
}

void EndScene() {
    // HDR 未启用 / 被 SetCanvas 暂停 / 资源失效: 静默 no-op
    if (!g.enabled || g.paused || !g.backend || !g.fbo || !g.sceneTex) return;

    // 解绑 HDR RT, 切到 default framebuffer
    g.backend->UnbindFBO();
    // 注意: 不复位 default fb 的 viewport; 调用方 (light_ui.cpp::Window_Call)
    // 一般在 SwapBuffers 前不再绘制, 下帧 BeginFrame 也不依赖 viewport. 若未来
    // tonemap 结果要与 LDR 其他内容合成, Lua 层可显式调 SetViewport.

    // Phase E.4.2 — Bloom 管线 (内部自检 IsEnabled; 未启用 no-op)
    // Process 将 bloom 结果 additive blend 到 g.fbo (HDR RT) 原地累加
    BloomRenderer::Process(g.fbo, g.sceneTex);

    // Bloom Process 内部结束时已 BindFramebuffer(0), 但为安全起见再 unbind 一次
    g.backend->UnbindFBO();

    // Phase E.5.2 — Auto Exposure (内部自检 IsEnabled; 未启用 no-op)
    // 测量 hdr+bloom 后的平均亮度, 更新内部 currentExposure 状态
    {
        static auto sLast = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - sLast).count();
        sLast = now;
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;   // clamp 防长时间挂起后跳变
        AutoExposureRenderer::Process(g.sceneTex, dt);
    }

    // Tonemap exposure: AE 开时覆盖 manual; AE 关时回归 manual SetExposure
    float exposure = AutoExposureRenderer::IsEnabled()
                        ? AutoExposureRenderer::GetCurrentExposure()
                        : g.exposure;

    // Tonemap + sRGB encode → default fb (E.3.4 多 operator; 输入已含 bloom 的 HDR RT)
    g.backend->DrawTonemapFullscreen(g.sceneTex, exposure, g.gamma, g.tonemap);
}

// ==================== 曝光 / Gamma ====================

void  SetExposure(float v) { g.exposure = v; }
float GetExposure()        { return g.exposure; }

void  SetGamma(float v)    { g.gamma = (v > 0.0001f) ? v : 0.0001f; }
float GetGamma()           { return g.gamma; }

// ==================== Phase E.3.4 — Tonemap Operator ====================

void SetTonemapper(int mode) {
    // 无效 mode 静默回退 ACES (0); 仅受理 0..3
    if (mode < TONEMAP_ACES || mode > TONEMAP_LINEAR) {
        g.tonemap = TONEMAP_ACES;
    } else {
        g.tonemap = mode;
    }
}

int GetTonemapper() { return g.tonemap; }

// ==================== 高级查询 ====================

uint32_t GetSceneTexture() { return g.sceneTex; }
int      GetWidth()        { return g.width; }
int      GetHeight()       { return g.height; }

// ==================== SetCanvas 兼容 ====================

void Pause()  { g.paused = true; }
void Resume() {
    if (!g.enabled) { g.paused = false; return; }
    // 恢复: 重新 BindFBO(HDR_RT)
    if (g.backend && g.fbo) {
        g.backend->BindFBO(g.fbo);
        g.backend->SetViewport(0, 0, g.width, g.height);
        // 注意: 不 Clear — 恢复时场景应继续累积 (SetCanvas 前绘制的内容保留)
    }
    g.paused = false;
}
bool IsPaused() { return g.paused; }

} // namespace HDRRenderer
