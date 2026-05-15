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

    int      width          = 0;            // sceneTex 全分辨率 (用户 Enable 传入, BlitTAAToHDR/Sharpen 输出用)
    int      height         = 0;
    int      historyW       = 0;            // Phase F.0.5: history RT 实际分辨率 (halfRes 时 = w/2, 否则 = w)
    int      historyH       = 0;            // 同上

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
    bool     antiFlicker      = true;        // Phase F.0.4: Karis luma weighting blend, 默认启用
    int      clipMode         = 1;           // Phase F.0.2/F.0.3: 0=RGB AABB / 1=YCoCg AABB (默认) / 2=YCoCg variance
    float    varianceGamma    = 1.0f;        // Phase F.0.3: variance clip 收紧系数 γ (Salvi 2016 / UE5 默认 1.0)
    bool     halfResHistory   = false;       // Phase F.0.5: history RT 半分辨率 (默认 false, 零回归)
    int      sharpenMode      = 0;           // Phase F.0.6/F.0.12: 0=unsharp (F.0.1 默认 4-tap) / 1=cas (5-tap AMD FSR1) / 2=rcas (5-tap AMD FSR2)
    float    motionGamma      = 1.5f;        // Phase F.0.8: motion-adaptive 高速区域 γ (UE5 高级形式, [0, 4])
    bool     motionAdaptiveGamma = false;    // Phase F.0.8: 默认 OFF (零回归, F.0.3 单 γ 行为)
    int      upscaleMode      = 0;           // Phase F.0.9: 0=bilinear (F.0.5 默认) / 1=bicubic Catmull-Rom
    bool     motionAdaptiveSharpness = false; // Phase F.0.13: 默认 OFF (零回归, sharpness 不随 motion 调整)
    float    motionSharpness  = 0.1f;        // Phase F.0.13: 高速运动时目标 sharpness (clamp [0, 2], 默认 0.1 减 trail)

    // jitter state
    uint64_t frameCounter   = 0;
    float    curJitterX     = 0.0f;
    float    curJitterY     = 0.0f;
};

static State g;

// ==================== 内部资源管理 ====================

/// Phase F.0.5: 计算 history RT 实际尺寸 (halfRes 时 = sceneW/2, 否则 = sceneW)
/// max(., 1) 防御极端小 sceneTex 除以 2 变 0 的边界条件
static int historySize_(int sceneSize, bool halfRes) {
    if (!halfRes) return sceneSize;
    const int half = sceneSize / 2;
    return half > 0 ? half : 1;
}

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
    g.historyW = g.historyH = 0;
}

/// 分配 history RT
/// @param sceneW/sceneH sceneTex 全分辨率 (用户传入)
/// Phase F.0.5: history RT 实际尺寸 = halfResHistory ? (w/2,h/2) : (w,h)
static bool AllocateRT(int sceneW, int sceneH) {
    if (!g.backend || sceneW <= 0 || sceneH <= 0) return false;
    const int hw = historySize_(sceneW, g.halfResHistory);
    const int hh = historySize_(sceneH, g.halfResHistory);
    if (!g.backend->CreateTAAHistoryRT(hw, hh, g.historyFbos, g.historyTexs)) {
        CC::Log(CC::LOG_WARN, "TAARenderer: CreateTAAHistoryRT failed (history=%dx%d, scene=%dx%d, halfRes=%d)",
                hw, hh, sceneW, sceneH, g.halfResHistory ? 1 : 0);
        return false;
    }
    g.historyIdx   = 0;
    g.hasHistory   = false;
    g.frameCounter = 0;
    g.width    = sceneW;
    g.height   = sceneH;
    g.historyW = hw;
    g.historyH = hh;
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

    // Phase F.0.5: TAA pass viewport = history RT 实际尺寸 (halfRes 时为 w/2, 否则为 w)
    //              shader 内邻域 sample 用 vUV 归一化 [0,1], sceneTex GL_LINEAR 自动 box-filter 预采样
    g.backend->DrawTAAPass(hdrTex,
                            g.historyTexs[readIdx],
                            velocityTex,
                            g.historyFbos[writeIdx],
                            g.historyW, g.historyH,     // Phase F.0.5: history RT 尺寸 (可能为 half-res)
                            g.blendAlpha,
                            g.neighborhoodClip ? 1 : 0,
                            g.hasHistory ? 1 : 0,
                            g.backend->GetVelocityDilation(),
                            g.backend->GetVelocityScale(),
                            g.backend->GetActiveVelocityFormat(),
                            g.antiFlicker ? 1 : 0,      // Phase F.0.4
                            g.clipMode,                 // Phase F.0.2/F.0.3
                            g.varianceGamma,            // Phase F.0.3 (static γ)
                            g.motionGamma,              // Phase F.0.8 (motion γ)
                            g.motionAdaptiveGamma ? 1 : 0); // Phase F.0.8 (开关)

    // Phase F.0.1/F.0.6/F.0.12: sharpness > 0 走 sharpen pass (in-place 写回 sceneTex);
    //                    否则保持 F.0 纯 blit 路径 (零 ALU 开销)
    // sharpenMode 三选一分支—— 0=unsharp (4-tap F.0.1) / 1=cas (5-tap FSR1) / 2=rcas (5-tap FSR2)
    //   sharpness 字段语义: unsharp/rcas [0, 2] / cas [0, 1] (各自后端 shader 本身范围 clamp)
    // Phase F.0.5:
    //   Sharpen: viewport=full-res, srcTex (history) GL_LINEAR sample 自动上采样→不需传 history 尺寸
    //   Blit:    src(history half-res) → dst(sceneTex full-res), backend 内检测尺寸不同走 GL_LINEAR stretch
    if (g.sharpness > 0.0f) {
        // Phase F.0.13: motion-adaptive sharpness—— 高速运动时 lerp 到 motionSharpness, 减 reprojection trail
        //                ComputeCameraMotionScalar 老 backend 默认返 0 (静默失效, 零回归)
        float effSharpness = g.sharpness;
        if (g.motionAdaptiveSharpness) {
            const float motion = g.backend->ComputeCameraMotionScalar();
            const float factor = clampf(motion * 0.5f, 0.0f, 1.0f);   // 经验归一化: ~1 = 50% factor, ~2 = 100%
            effSharpness = g.sharpness + (g.motionSharpness - g.sharpness) * factor;
        }
        if (g.sharpenMode == 2) {
            // Phase F.0.12: RCAS sharpness 接受完整 [0, 2] (FSR2 标准); 超出则 saturate 到 2.0
            const float rcasS = (effSharpness > 2.0f) ? 2.0f : effSharpness;
            g.backend->DrawTAARCASPass(g.historyTexs[writeIdx], hdrFbo,
                                        g.width, g.height, rcasS);
        } else if (g.sharpenMode == 1) {
            // Phase F.0.6: CAS sharpness clamp [0, 1] (FSR1 标准); 超出则 saturate 到 1.0
            const float casS = (effSharpness > 1.0f) ? 1.0f : effSharpness;
            g.backend->DrawTAACASPass(g.historyTexs[writeIdx], hdrFbo,
                                       g.width, g.height, casS);
        } else {
            // Phase F.0.1: unsharp mask, sharpness 接受完整 [0, 2] 范围
            g.backend->DrawTAASharpenPass(g.historyTexs[writeIdx], hdrFbo,
                                          g.width, g.height, effSharpness);
        }
    } else {
        // Phase F.0.9: sharpness=0 路径—— halfRes && upscaleMode==1 走 Catmull-Rom bicubic上采样
        //                                  其他走 F.0.5 老 BlitTAAToHDR (bilinear stretch 或 1:1 nearest)
        // 仅 halfRes=true 时需要上采样品质提升 (full-res 时 1:1 blit 零 ALU)
        if (g.halfResHistory && g.upscaleMode == 1) {
            g.backend->DrawTAAUpscalePass(g.historyTexs[writeIdx], hdrFbo,
                                           g.historyW, g.historyH,    // src = half-res
                                           g.width,    g.height);     // dst = full-res
        } else {
            g.backend->BlitTAAToHDR(g.historyTexs[writeIdx], hdrFbo,
                                     g.historyW, g.historyH,    // Phase F.0.5: src = history RT 实际尺寸
                                     g.width,    g.height);     // Phase F.0.5: dst = sceneTex full-res
        }
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

// Phase F.0.4 — Anti-flicker filter (Karis luma-weighted blend) 开关
void  SetAntiFlicker(bool on) { g.antiFlicker = on; }
bool  GetAntiFlicker()         { return g.antiFlicker; }

// Phase F.0.2/F.0.3 — 9-tap clip 色彩空间 + variance 模式
// 大小写不敏感解析: "rgb" → 0; "ycocg" → 1; "variance" → 2
// 未识别字符串静默保持当前 state (错误提示在 Lua 层返 nil+err)
static int parseClipMode_(const char* mode) {
    if (!mode) return -1;
    // 手写 case-insensitive 判别 (避免引入 <strings.h> / strcasecmp 跨平台问题)
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    };
    if (eq(mode, "rgb"))      return 0;
    if (eq(mode, "ycocg"))    return 1;
    if (eq(mode, "variance")) return 2;   // Phase F.0.3
    return -1;
}
void SetClipMode(const char* mode) {
    int parsed = parseClipMode_(mode);
    if (parsed >= 0) g.clipMode = parsed;   // 仅识别到才写入, 其他静默保持
}
const char* GetClipMode() {
    switch (g.clipMode) {
        case 0:  return "rgb";
        case 1:  return "ycocg";
        case 2:  return "variance";       // Phase F.0.3
        default: return "ycocg";          // 防御性默认 (不可达)
    }
}

// Phase F.0.3 — Variance clip 收紧系数 γ, clamp [0, 4]
void  SetVarianceGamma(float gamma) { g.varianceGamma = clampf(gamma, 0.0f, 4.0f); }
float GetVarianceGamma()             { return g.varianceGamma; }

// Phase F.0.5 — history RT 半分辨率开关
// 切换时立即重建 RT (避免分辨率不匹配的 reproject 花屏一帧)
// enabled=false 时仅修改 state, 下次 Enable 自动用新 halfRes 设置
void SetHalfResHistory(bool on) {
    if (on == g.halfResHistory) return;          // 早退: 无变化
    g.halfResHistory = on;
    if (!g.enabled) return;                       // 未启用: 下次 Enable 自动使用新 state
    // 运行时切换: 重建 history RT 到新分辨率
    const int sceneW = g.width;
    const int sceneH = g.height;
    if (sceneW <= 0 || sceneH <= 0) return;
    ReleaseRT();
    if (!AllocateRT(sceneW, sceneH)) {
        CC::Log(CC::LOG_WARN, "TAARenderer::SetHalfResHistory: 重建 RT 失败, TAA 已禁用");
        g.enabled = false;
        return;
    }
    // hasHistory 已被 AllocateRT 重置为 false (避免老分辨率 history 被 reproject)
    CC::Log(CC::LOG_INFO, "TAARenderer::SetHalfResHistory: %s, history RT = %dx%d (scene = %dx%d)",
            on ? "ON" : "OFF", g.historyW, g.historyH, sceneW, sceneH);
}
bool GetHalfResHistory() { return g.halfResHistory; }

// Phase F.0.6/F.0.12 — Sharpen mode ("unsharp" 4-tap F.0.1 / "cas" 5-tap FSR1 / "rcas" 5-tap FSR2)
// 大小写不敏感解析: "unsharp" → 0 / "cas" → 1 / "rcas" → 2; 未识别静默保持当前 state
// 仅修改 mode 字段, 不重建 RT (shader 在 backend 内部切分支)
// 复用 parseClipMode_ 的 lambda 模式, 避免 strcasecmp 跨平台问题 (Windows MSVC 无此函数, 需 _stricmp)
static int parseSharpenMode_(const char* mode) {
    if (!mode) return -1;
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    };
    if (eq(mode, "unsharp")) return 0;
    if (eq(mode, "cas"))     return 1;
    if (eq(mode, "rcas"))    return 2;   // Phase F.0.12
    return -1;
}
void SetSharpenMode(const char* mode) {
    int parsed = parseSharpenMode_(mode);
    if (parsed >= 0) g.sharpenMode = parsed;   // 仅识别到才写入 (与 SetClipMode 同模式)
}
// 返回不可变 C 字符串供 Lua/HUD 使用 (taa_funcs ABI 要求 const char*)
const char* GetSharpenMode() {
    if (g.sharpenMode == 2) return "rcas";   // Phase F.0.12
    if (g.sharpenMode == 1) return "cas";
    return "unsharp";
}

// Phase F.0.8 — motion-adaptive γ 双值 (static + motion) + 开关
// motionGamma clamp [0, 4] (与 varianceGamma 同范围); 默认 1.5 (UE5 推荐)
void  SetMotionGamma(float g_motion) { g.motionGamma = clampf(g_motion, 0.0f, 4.0f); }
float GetMotionGamma()                { return g.motionGamma; }
// motionAdaptive 默认 false; 切换即生效 (下一帧 shader 走 motion-adaptive 分支)
void  SetMotionAdaptive(bool on) { g.motionAdaptiveGamma = on; }
bool  GetMotionAdaptive()         { return g.motionAdaptiveGamma; }

// Phase F.0.13 — motion-adaptive sharpness (高速运动时动态降 sharpness 减 trail)
// motionSharpness clamp [0, 2] (与 sharpness 同范围); 默认 0.1 (高速时几乎不锐化)
void  SetMotionSharpness(float s) { g.motionSharpness = clampf(s, 0.0f, 2.0f); }
float GetMotionSharpness()         { return g.motionSharpness; }
// motionAdaptiveSharpness 默认 false; 切换即生效 (Process 内 effSharpness 切换 lerp 路径)
void  SetMotionAdaptiveSharpness(bool on) { g.motionAdaptiveSharpness = on; }
bool  GetMotionAdaptiveSharpness()         { return g.motionAdaptiveSharpness; }

// Phase F.0.9 — Custom upsampler ("bilinear" 默认 / "bicubic" Catmull-Rom 9-tap)
// 仅 sharpness=0 && halfRes=true 时生效; 复用 parseClipMode_ 手写 case-insensitive 模式
static int parseUpscaleMode_(const char* mode) {
    if (!mode) return -1;
    auto eq = [](const char* a, const char* b) {
        while (*a && *b) {
            char ca = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
            char cb = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
            if (ca != cb) return false;
            ++a; ++b;
        }
        return *a == '\0' && *b == '\0';
    };
    if (eq(mode, "bilinear")) return 0;
    if (eq(mode, "bicubic"))  return 1;
    return -1;
}
void SetUpscaleMode(const char* mode) {
    int parsed = parseUpscaleMode_(mode);
    if (parsed >= 0) g.upscaleMode = parsed;   // 仅识别到才写入 (与 SetClipMode/SetSharpenMode 同模式)
}
const char* GetUpscaleMode() {
    return (g.upscaleMode == 1) ? "bicubic" : "bilinear";
}

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
