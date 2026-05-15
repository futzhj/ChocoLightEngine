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
    int            mode        = 0;         // Phase E.16: 0=combined / 1=camera / 2=object
    bool           halfRes     = false;     // Phase E.17: half-res motion blur 开关
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

// Phase E.17 — 内部辅助: 从逻辑尺寸 (w, h) 推出 motionBlurTex 实际存储尺寸
// halfRes=true → (w+1)/2, (h+1)/2 (向上取整防奇数丢边)
static inline void ComputeStorageSize(int w, int h, int& sw, int& sh) {
    if (g.halfRes) {
        sw = (w + 1) / 2;
        sh = (h + 1) / 2;
    } else {
        sw = w;
        sh = h;
    }
}

// 内部辅助: 创建 RT 资源 (失败返回 false 并清理半成品)
bool CreateRT(int w, int h) {
    if (!g.backend) return false;
    if (w <= 0 || h <= 0) return false;
    uint32_t tex = 0;
    // Phase E.17: 透传实际存储尺寸 (full-res 时 sw==w, sh==h, 与 Phase E.16 等价)
    int sw = 0, sh = 0;
    ComputeStorageSize(w, h, sw, sh);
    uint32_t fbo = g.backend->CreateMotionBlurRT(w, h, &tex, sw, sh);
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

// Phase E.16 — mode 参数 (clamp [0, 2])
//   0=combined (与 Phase E.15 行为一致); 1=camera_only; 2=object_only
void SetMode(int m)           { g.mode = ClampI(m, 0, 2); }
int  GetMode()                { return g.mode; }

// Phase E.17 — half-res 开关
//   已 Enable 时切换 → 立即 Resize 重建 RT (用户体验连贯)
//   未 Enable 时切换 → 仅更新状态，下次 Enable 时生效
//   同值为 no-op (避免重建同尺寸 RT)
void SetHalfRes(bool flag) {
    if (g.halfRes == flag) return;       // no-op: 状态未变
    g.halfRes = flag;
    if (g.enabled) {
        // 立即重建 RT (逻辑尺寸不变，仅实际存储尺寸变)
        Resize(g.width, g.height);
    }
    // 未 Enable 时状态字段已更新，后续 Enable 会用新值
}
bool GetHalfRes()             { return g.halfRes; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // 转发到 region 版本 (0/0/0/0 = 全屏老路径, 零回归)
    Process(hdrFbo, hdrTex, 0, 0, 0, 0);
}

// Phase F.0.10.3 — Region 限定 motion blur (split-screen 必备)
// rgnW=0 || rgnH=0 时退化为全屏路径 (与老 Process 等价)
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH) {
    // 防御: 5 个先决条件任一不满足 → silent skip
    if (!g.enabled || !g.backend || !hdrFbo || !hdrTex) return;
    if (!g.fbo || !g.tex) return;

    // velocity buffer 来源: HDRRenderer 提供
    // Phase E.18: 优先取 dilated tex (HDR EndScene 已做过 9-tap), shader 走单点采路径;
    //             dilated 不可用时 fallback 到 raw velocityTex (shader 内 inline 9-tap)
    const uint32_t dilatedTex = HDRRenderer::GetDilatedVelocityTexture();
    const uint32_t rawTex     = HDRRenderer::GetVelocityTexture();
    const uint32_t velocityTex = dilatedTex ? dilatedTex : rawTex;
    if (!velocityTex) return;   // 后端不支持 velocity buffer 或 HDR 未启 → silent skip

    // Phase E.16 — 取 camera-only velocity (mode=1/2 需；mode=0 也传入 backend 作为占位)
    // 不存在时 backend safeMode 会自动 fallback 到 mode=0。
    // Phase E.18: camera-only velocity 同样优先取 dilated, fallback raw
    const uint32_t dilatedCamTex   = HDRRenderer::GetDilatedCameraVelocityTexture();
    const uint32_t rawCamTex       = HDRRenderer::GetCameraVelocityTexture();
    const uint32_t cameraVelocityTex = dilatedCamTex ? dilatedCamTex : rawCamTex;

    // Phase E.17 — 计算 motionBlurTex 实际尺寸 (full-res 时 == g.width/g.height)
    int rtW = 0, rtH = 0;
    ComputeStorageSize(g.width, g.height, rtW, rtH);

    // Phase F.0.10.3 — region 透传到 backend, half-res 缩半逻辑由 backend 内部处理
    g.backend->DrawMotionBlur(hdrTex, velocityTex,
                               cameraVelocityTex,                  // ★ Phase E.16
                               g.fbo, g.tex,
                               hdrFbo,
                               g.width, g.height,
                               g.strength, g.sampleCount,
                               g.mode,                             // ★ Phase E.16
                               rtW, rtH,                           // ★ Phase E.17
                               rgnX, rgnY, rgnW, rgnH);            // ★ Phase F.0.10.3
}

} // namespace MotionBlurRenderer
