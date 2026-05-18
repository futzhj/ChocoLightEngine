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
#include <cmath>                   // Phase F.1: std::lround for renderW/H 取整

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
//
// Phase F.0.10 — 多实例化重构 (multi-instance):
//   - 老 `static State g;` 单例 → `static State g_states[MAX_INSTANCES]; static int g_active = 0;`
//   - 通过 macro `#define g g_states[g_active]` 让现有 35 fn 零改动继续访问 active instance
//   - g_states[0] 是 default singleton (老 namespace API 行为完全等价 F.0~F.0.14)
//   - 新加 5 fn (CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount)
//   - 每 instance 独立: backend ptr / enabled / RT / 14 个 sub-phase 参数 / jitter state
//   - MAX_INSTANCES=4: default + 3 user (split-screen 双人/四人足够)
//
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

    // Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 (默认 false, 等同 F.0 单尺寸路径零回归)
    bool     taauEnabled    = false;         // ★ TAAU 总开关 (Set 触发 HDR 重建 + history 重建)
    float    renderScale    = 1.0f;          // [0.5, 1.0]; 仅 taauEnabled 时影响 renderW/H
    int      upscalePreset  = 3;             // 0=Performance(0.5) / 1=Balanced(0.667) / 2=Quality(0.75) / 3=Native(1.0)
    int      renderW        = 0;             // = lround(width * renderScale) when taauEnabled, else 0 (== width 隐含)
    int      renderH        = 0;

    // jitter state
    uint64_t frameCounter   = 0;
    float    curJitterX     = 0.0f;
    float    curJitterY     = 0.0f;
};

// Phase F.0.10 — multi-instance support
static constexpr int MAX_INSTANCES = 4;          // default + 3 user instance (split-screen 多人足够)
static State g_states[MAX_INSTANCES];
static int   g_active = 0;                       // 当前 active instance 索引 [0, MAX_INSTANCES)
static int   g_count  = 1;                       // 已分配 instance 数 (>=1, g_states[0]=default 永远占用)
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };

// 现有 35 fn 内部沿用 `g.X` 写法; macro 透明展开到 active instance
// 注意: 仅在 taa_renderer.cpp 文件内有效, 不污染外部命名空间
#define g g_states[g_active]

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
    // Phase G.1 — VRAM Tracking: history RT 是 ping-pong, 一次 Create 注册 2 个
    if ((g.historyTexs[0] || g.historyTexs[1]) && g.historyW > 0 && g.historyH > 0) {
        LT::GpuMem::Untrack("TAA history", "RGBA16F", g.historyW, g.historyH);
        LT::GpuMem::Untrack("TAA history", "RGBA16F", g.historyW, g.historyH);
    }
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
    // Phase G.1 — VRAM Tracking: history ping-pong 2 张 RGBA16F
    LT::GpuMem::Track("TAA history", "RGBA16F", hw, hh);
    LT::GpuMem::Track("TAA history", "RGBA16F", hw, hh);
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
    // Phase F.0.10: 把 backend ptr / supported / inited 写入所有 MAX_INSTANCES 槽
    // (每个 instance 共享 backend 能力, 但独立 enabled / RT / 参数)
    const bool supported = backend->SupportsTAA();
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        g_states[i].backend   = backend;
        g_states[i].supported = supported;
        g_states[i].inited    = true;
    }
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
    // Phase F.0.10: 遍历所有 instance 释放 RT + 重置所有 state 字段
    // (active instance 在循环中会临时切换, 最终复位到 0)
    const int saved_active = g_active;
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        g_active = i;
        if (g_states[i].inited) {
            ReleaseRT();        // 作用于当前 g_active 槽的 RT
        }
        g_states[i].enabled   = false;
        g_states[i].inited    = false;
        g_states[i].supported = false;
        g_states[i].backend   = nullptr;
    }
    // 复位多实例分配状态: 仅 default 槽存在
    g_active = 0;
    g_count  = 1;
    for (int i = 0; i < MAX_INSTANCES; ++i) g_slot_in_use[i] = (i == 0);
    (void)saved_active;  // saved_active 不需保留 (Shutdown 后默认回 0)
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

    // NDC 偏移 = 像素偏移 × 2 / 尺寸
    // Phase F.1 TAAU: 当 taauEnabled=true 时, raster 在 render-res 进行,
    //   jitter 必须按 render-res pixel (NDC offset 因此更大: render 1 px = output (output/render) px)
    //   F.0 模式 renderW/H == 0, 退化到 width/height (零回归)
    const int jitterW = (g.taauEnabled && g.renderW > 0) ? g.renderW : g.width;
    const int jitterH = (g.taauEnabled && g.renderH > 0) ? g.renderH : g.height;
    const float ndcOffX = g.curJitterX * 2.0f / (float)jitterW;
    const float ndcOffY = g.curJitterY * 2.0f / (float)jitterH;

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
    // 老接口: 全屏 process, 转发到 region 版本 (rgn=0 → 等价无 scissor 全屏)
    Process(hdrFbo, hdrTex, 0, 0, 0, 0);
}

// Phase F.0.10.2 — region 变体: rgnW/rgnH > 0 时启用 GL_SCISSOR_TEST 限制写入子矩形
// 与无参 Process 共用同一份累积/blit 逻辑, 仅多透传 region 4 参数到 backend
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH) {
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

    // Phase F.0.10.5 — 算 uvBounds (vec4: uMin.xy, uMax.xy) 给 shader 内 ClampUV 用
    // 防御性: 全屏路径 (rgnW=0 || rgnH=0) 时 uvBoundsPtr=nullptr, backend 自动上传 (0,0,1,1) = 无 clamp
    // region 路径: 加 0.5 texel inset 防线性插值越界 (业界标准, UE 同模式)
    float uvBoundsBuf[4];
    const float* uvBoundsPtr = nullptr;
    if (rgnW > 0 && rgnH > 0 && g.historyW > 0 && g.historyH > 0) {
        const float invW = 1.0f / (float)g.historyW;
        const float invH = 1.0f / (float)g.historyH;
        uvBoundsBuf[0] = ((float)rgnX        + 0.5f) * invW;
        uvBoundsBuf[1] = ((float)rgnY        + 0.5f) * invH;
        uvBoundsBuf[2] = ((float)(rgnX + rgnW) - 0.5f) * invW;
        uvBoundsBuf[3] = ((float)(rgnY + rgnH) - 0.5f) * invH;
        uvBoundsPtr = uvBoundsBuf;
    }

    // Phase F.0.5: TAA pass viewport = history RT 实际尺寸 (halfRes 时为 w/2, 否则为 w)
    //              shader 内邻域 sample 用 vUV 归一化 [0,1], sceneTex GL_LINEAR 自动 box-filter 预采样
    // Phase F.0.10.2: rgnX/Y/W/H 透传到 backend, rgnW/rgnH=0 时 backend 内部跳过 scissor (零回归)
    // Phase F.0.10.5: uvBoundsPtr 透传到 shader (nullptr = 全屏 (0,0,1,1) no-op)
    // Phase F.1 TAAU: 当 taauEnabled=true 时, history RT 是 output-res (renderW/H 通过参数透传给 backend
    //                 让 shader 的 uTexel 按 render-res 上传, 邻域采样在 render 像素步进)
    const int taauRenderW = g.taauEnabled ? g.renderW : 0;
    const int taauRenderH = g.taauEnabled ? g.renderH : 0;
    const int taauOutputW = g.taauEnabled ? g.width   : 0;   // F.1 时 g.width == outputW (与 historyW 同)
    const int taauOutputH = g.taauEnabled ? g.height  : 0;
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
                            g.motionAdaptiveGamma ? 1 : 0, // Phase F.0.8 (开关)
                            rgnX, rgnY, rgnW, rgnH,     // Phase F.0.10.2 region
                            uvBoundsPtr,                // Phase F.0.10.5 uvBounds
                            taauRenderW, taauRenderH,   // Phase F.1 TAAU render-res (0 = F.0 行为)
                            taauOutputW, taauOutputH,   // Phase F.1 TAAU output-res (0 = F.0 行为)
                            g.taauEnabled ? 1 : 0);     // Phase F.1 TAAU 开关

    // Phase F.1 TAAU: sharpen/blit 的目标 FBO 在 taauEnabled 时是 outputSceneFbo (output-res),
    //   否则是 hdrFbo (F.0 行为)。outputSceneFbo 由 HDRRenderer::OnTAAURenderScaleChanged 创建,
    //   通过 HDRRenderer::GetSceneFboForOutput() 查询。
    //   sharpen 写入目标 FBO 大小: TAAU 模式 g.width == outputW (因为 SetTAAUEnabled 时 g.width 已被
    //   设为 outputW; see TAA::Enable & SetTAAUEnabled 注释)。
    const uint32_t finalDstFbo = g.taauEnabled ? HDRRenderer::GetSceneFboForOutput() : hdrFbo;

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
            g.backend->DrawTAARCASPass(g.historyTexs[writeIdx], finalDstFbo,
                                        g.width, g.height, rcasS,
                                        rgnX, rgnY, rgnW, rgnH);    // Phase F.0.10.2
        } else if (g.sharpenMode == 1) {
            // Phase F.0.6: CAS sharpness clamp [0, 1] (FSR1 标准); 超出则 saturate 到 1.0
            const float casS = (effSharpness > 1.0f) ? 1.0f : effSharpness;
            g.backend->DrawTAACASPass(g.historyTexs[writeIdx], finalDstFbo,
                                       g.width, g.height, casS,
                                       rgnX, rgnY, rgnW, rgnH);     // Phase F.0.10.2
        } else {
            // Phase F.0.1: unsharp mask, sharpness 接受完整 [0, 2] 范围
            // Phase F.0.10.5: Sharpen 写回 sceneTex (full-res), uvBounds 基于 g.width/g.height 算
            float sharpenUvBounds[4];
            const float* sharpenUvBoundsPtr = nullptr;
            if (rgnW > 0 && rgnH > 0 && g.width > 0 && g.height > 0) {
                const float invW = 1.0f / (float)g.width;
                const float invH = 1.0f / (float)g.height;
                sharpenUvBounds[0] = ((float)rgnX        + 0.5f) * invW;
                sharpenUvBounds[1] = ((float)rgnY        + 0.5f) * invH;
                sharpenUvBounds[2] = ((float)(rgnX + rgnW) - 0.5f) * invW;
                sharpenUvBounds[3] = ((float)(rgnY + rgnH) - 0.5f) * invH;
                sharpenUvBoundsPtr = sharpenUvBounds;
            }
            g.backend->DrawTAASharpenPass(g.historyTexs[writeIdx], finalDstFbo,
                                          g.width, g.height, effSharpness,
                                          rgnX, rgnY, rgnW, rgnH,   // Phase F.0.10.2
                                          sharpenUvBoundsPtr);      // Phase F.0.10.5
        }
    } else {
        // Phase F.0.9/F.0.14: sharpness=0 路径—— halfRes && upscaleMode 三选一
        //   upscaleMode==1 走 Catmull-Rom 9-tap bicubic (F.0.9)
        //   upscaleMode==2 走 Lanczos-2 25-tap 5x5 (F.0.14, 超高画质)
        //   其他 (==0 / halfRes=false) 走 F.0.5 老 BlitTAAToHDR (bilinear stretch 或 1:1 nearest)
        // 仅 halfRes=true 时需要上采样品质提升 (full-res 时 1:1 blit 零 ALU)
        if (g.halfResHistory && g.upscaleMode == 1) {
            g.backend->DrawTAAUpscalePass(g.historyTexs[writeIdx], finalDstFbo,
                                           g.historyW, g.historyH,    // src = half-res
                                           g.width,    g.height,      // dst = full-res
                                           rgnX, rgnY, rgnW, rgnH);   // Phase F.0.10.2
        } else if (g.halfResHistory && g.upscaleMode == 2) {
            g.backend->DrawTAALanczosPass(g.historyTexs[writeIdx], finalDstFbo,
                                           g.historyW, g.historyH,
                                           g.width,    g.height,
                                           rgnX, rgnY, rgnW, rgnH);   // Phase F.0.10.2
        } else {
            g.backend->BlitTAAToHDR(g.historyTexs[writeIdx], finalDstFbo,
                                     g.historyW, g.historyH,    // Phase F.0.5: src = history RT 实际尺寸
                                     g.width,    g.height,      // Phase F.0.5: dst = sceneTex full-res
                                     rgnX, rgnY, rgnW, rgnH);   // Phase F.0.10.2
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

// Phase F.0.9/F.0.14 — Custom upsampler ("bilinear" 默认 / "bicubic" Catmull-Rom 9-tap / "lanczos" Lanczos-2 25-tap)
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
    if (eq(mode, "lanczos"))  return 2;   // Phase F.0.14
    return -1;
}
void SetUpscaleMode(const char* mode) {
    int parsed = parseUpscaleMode_(mode);
    if (parsed >= 0) g.upscaleMode = parsed;   // 仅识别到才写入 (与 SetClipMode/SetSharpenMode 同模式)
}
const char* GetUpscaleMode() {
    if (g.upscaleMode == 2) return "lanczos";   // Phase F.0.14
    if (g.upscaleMode == 1) return "bicubic";
    return "bilinear";
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

// ==================== Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 ====================

// 预设字符串 ↔ scale 映射 (单一来源, 避免散落 magic number)
//   "performance" 0.5   /  "balanced" 0.667  /  "quality" 0.75  /  "native" 1.0
// 数值与 FSR2 公开档位一致 (idx 0..3)
static const float kPresetScale[4]   = { 0.5f, 0.6667f, 0.75f, 1.0f };
static const char* kPresetName [4]   = { "performance", "balanced", "quality", "native" };

static int presetIdxFromScale_(float scale) {
    // 容差 0.01 匹配预设; 用户自定 scale (如 0.6) 找最近档
    int best = 0;
    float bestDelta = 1e30f;
    for (int i = 0; i < 4; ++i) {
        const float d = scale > kPresetScale[i] ? (scale - kPresetScale[i])
                                                : (kPresetScale[i] - scale);
        if (d < bestDelta) { bestDelta = d; best = i; }
    }
    return best;
}

static float clampRenderScale_(float s) {
    if (s < 0.5f) return 0.5f;
    if (s > 1.0f) return 1.0f;
    return s;
}

// 内部: 根据 outputW/H + renderScale 重算 renderW/H, 返 (w, h) 整数对
static void computeRenderRes_(int outputW, int outputH, float scale,
                               int* outRenderW, int* outRenderH) {
    int rw = (int)std::lround((double)outputW * (double)scale);
    int rh = (int)std::lround((double)outputH * (double)scale);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (outRenderW) *outRenderW = rw;
    if (outRenderH) *outRenderH = rh;
}

// 内部: 触发 HDR 重建 (TAAU 模式) + 重置 history. 失败时 taauEnabled 已先被设过, 需清回.
// 仅在 enabled + active 是 default instance (g_active==0) 时调用 (上层保护).
static void updateMipBias_();   // Phase F.1.1 forward decl (impl 在 §F.1.1 节)
static bool applyTAAUChange_() {
    if (!g.enabled || !g.backend) return false;
    if (g.width <= 0 || g.height <= 0) return false;

    if (g.taauEnabled) {
        // 重算 renderW/H + 通知 HDR 切到双尺寸
        computeRenderRes_(g.width, g.height, g.renderScale, &g.renderW, &g.renderH);
        if (!HDRRenderer::OnTAAURenderScaleChanged(g.renderW, g.renderH,
                                                    g.width, g.height)) {
            CC::Log(CC::LOG_WARN, "TAARenderer: HDR 切 TAAU 失败, 回退 F.0 路径");
            g.taauEnabled = false;
            g.renderW = 0;
            g.renderH = 0;
            return false;
        }
    } else {
        // 切回 F.0 路径
        HDRRenderer::OnTAAUDisabled();
        g.renderW = 0;
        g.renderH = 0;
    }

    // 重置 history (新旧路径 history 尺寸/语义不混用)
    g.hasHistory   = false;
    g.historyIdx   = 0;
    g.frameCounter = 0;

    // Phase F.1.1: 任何 taauEnabled / renderScale 变化都会经由本函数, 此处统一 push mipBias
    updateMipBias_();
    return true;
}

bool SetTAAUEnabled(bool flag) {
    if (g.taauEnabled == flag) return true;   // no-op

    // Phase F.1.0.1: 移除 F.1.0 的 `g_active != 0` 限制, 让 user instance 也能启用 TAAU.
    //   调用方需保证 HDR g_active 与 TAA g_active 一致 (否则 GetSceneFboForOutput 返错 fbo).
    //   典型用法: HDR.SetActiveInstance(pipId); TAA.SetActiveInstance(pipId); TAA.SetTAAUEnabled(true)

    if (!g.supported) {
        CC::Log(CC::LOG_WARN, "TAARenderer::SetTAAUEnabled: backend 不支持 TAA, 忽略");
        return false;
    }

    if (flag) {
        // Q5 仲裁: TAAU + HalfResHistory 互斥, 自动关 HalfResHistory
        if (g.halfResHistory) {
            CC::Log(CC::LOG_WARN,
                    "TAARenderer::SetTAAUEnabled: HalfResHistory 与 TAAU 互斥, 自动关 HalfResHistory");
            g.halfResHistory = false;
            // 已 Enable 时同步重建 history (历史尺寸变 full-res)
            if (g.enabled) {
                ReleaseRT();
                if (!AllocateRT(g.width, g.height)) {
                    CC::Log(CC::LOG_ERROR, "TAARenderer::SetTAAUEnabled: 关 HalfResHistory 后 history 重建失败");
                    return false;
                }
            }
        }
    }

    g.taauEnabled = flag;
    return applyTAAUChange_();
}
bool GetTAAUEnabled() { return g.taauEnabled; }

void SetRenderScale(float scale) {
    const float clamped = clampRenderScale_(scale);
    if (scale != clamped) {
        CC::Log(CC::LOG_INFO,
                "TAARenderer::SetRenderScale: %f clamped to [0.5, 1.0] -> %f",
                (double)scale, (double)clamped);
    }
    if (g.renderScale == clamped) return;   // no-op

    g.renderScale = clamped;
    g.upscalePreset = presetIdxFromScale_(clamped);

    // 仅 taauEnabled 时触发 HDR 重建 (Phase F.1.0.1: 移除 g_active==0 限制, user instance 也支持)
    if (g.taauEnabled && g.enabled) {
        applyTAAUChange_();
    }
}
float GetRenderScale() { return g.renderScale; }

void SetUpscalePreset(const char* preset) {
    if (!preset) return;
    for (int i = 0; i < 4; ++i) {
        // 不区分大小写匹配 (容忍 "Balanced" / "BALANCED")
        const char* a = preset;
        const char* b = kPresetName[i];
        bool match = true;
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
            if (ca != cb) { match = false; break; }
            ++a; ++b;
        }
        if (match && *a == 0 && *b == 0) {
            SetRenderScale(kPresetScale[i]);
            // SetRenderScale 已设 upscalePreset, 这里冗余 set 保险 (浮点容差边界)
            g.upscalePreset = i;
            return;
        }
    }
    CC::Log(CC::LOG_WARN,
            "TAARenderer::SetUpscalePreset: 未知预设 \"%s\" (合法: performance/balanced/quality/native), 忽略",
            preset);
}
const char* GetUpscalePreset() {
    int idx = g.upscalePreset;
    if (idx < 0 || idx >= 4) idx = 3;   // 默认 native
    return kPresetName[idx];
}

void GetRenderResolution(int* outW, int* outH) {
    // taauEnabled=true: 返 renderW/H; 否则 == outputW/H (g.width/height)
    const int rw = (g.taauEnabled && g.renderW > 0) ? g.renderW : g.width;
    const int rh = (g.taauEnabled && g.renderH > 0) ? g.renderH : g.height;
    if (outW) *outW = rw;
    if (outH) *outH = rh;
}

void GetOutputResolution(int* outW, int* outH) {
    // 总是返 g.width/g.height (用户 Enable 入参; TAAU 模式 history 尺寸 == g.width/g.height)
    if (outW) *outW = g.width;
    if (outH) *outH = g.height;
}

// ==================== Phase F.1.1 — Mipmap LOD Bias ====================
//
// 设计:
//   - autoMipBias_ 是单一全局标志 (不是 per-instance), 与 backend mipBias_ 配对
//   - 在 active instance 的 taauEnabled / renderScale / 切换 active 时自动 push 新 bias
//   - 用户 SetMipBias 仅在 autoMipBias=false 时持久生效 (auto 模式下被 hook 覆盖)
//   - 公式: bias = log2(renderScale) - 0.7 当 TAAU 启用; 否则 bias = 0
//     0.667 -> -1.285  /  0.5 -> -1.7  /  0.75 -> -1.115  /  1.0 -> 0 (退回, 等同 F.0)

static bool autoMipBias_ = true;   // 全局: 默认 ON, TAAU 启用时自动调

// 内部: 计算并推 mipBias 给 backend.
//   autoMipBias=false 时不动 backend (用户手动 SetMipBias 决定).
//   autoMipBias=true  时按当前 active instance 的 taauEnabled + renderScale 算 bias.
static void updateMipBias_() {
    if (!g.backend) return;
    if (!autoMipBias_) return;   // 手动模式: 不覆盖

    float bias = 0.0f;   // TAAU 关闭 / native scale 时 = 0 (零回归)
    if (g.taauEnabled && g.renderScale > 0.0f && g.renderScale < 1.0f) {
        // FSR2 / DLSS 风格: bias = log2(scale) - 0.7, 介于 UE4 (-0.0) 与 FSR2 (-1.0) 之间
        bias = std::log2(g.renderScale) - 0.7f;
    }
    g.backend->SetMipBias(bias);
}

void SetAutoMipBias(bool flag) {
    if (autoMipBias_ == flag) return;
    autoMipBias_ = flag;
    if (flag) {
        // 切回 auto: 立即 sync 一次, 让 bias 与当前 TAAU 状态对齐
        updateMipBias_();
    } else {
        // 关 auto: 复位 backend bias 为 0 (用户后续可 SetMipBias 手动设值)
        if (g.backend) g.backend->SetMipBias(0.0f);
    }
}
bool GetAutoMipBias() { return autoMipBias_; }

void SetMipBias(float bias) {
    if (!g.backend) return;
    // backend 内部 clamp [-4, +4]; 这里仅透传
    g.backend->SetMipBias(bias);
}

float GetMipBias() {
    if (!g.backend) return 0.0f;
    return g.backend->GetMipBias();
}

// ==================== Phase F.0.10 — Multi-Instance API ====================
//
// 设计说明:
//   - g_states[0] 是 default singleton, 永远占用; 老 35 fn 默认作用于 [0]
//   - CreateInstance() 找空闲槽 [1, MAX_INSTANCES-1] 返 ID, 槽满返 0
//   - 新 instance 继承 default 的 backend/supported/inited (来自 Init() 时全量写入)
//   - SetActiveInstance(id) 切换 active, 后续 namespace fn 作用于 [id]
//   - DestroyInstance(id) 释放该槽 RT + 标空闲, 若 active 是该 id 自动切回 0
//   - 不能销毁 id=0 (default), 不能 SetActiveInstance(无效 id)

int CreateInstance() {
    // Phase F.0.10: CreateInstance 仅分配槽位 + 复位 state, 不要求 default Init() 完成
    // (Init 由 light_ui 在 window 创建时调用; headless smoke 环境也能创建 instance,
    //  仅后续 Enable() 因 backend=nullptr 失败, 这是符合预期的)
    // 找第一个空闲槽 (跳过 [0])
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 复位为干净 default state (struct 默认值), 然后继承 default 的 backend / supported / inited
            g_states[i] = State{};
            g_states[i].backend   = g_states[0].backend;     // 可能为 nullptr (headless 环境)
            g_states[i].supported = g_states[0].supported;
            g_states[i].inited    = g_states[0].inited;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO, "TAARenderer::CreateInstance: 创建 instance id=%d (count=%d, inited=%d)",
                    i, g_count, g_states[0].inited ? 1 : 0);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN, "TAARenderer::CreateInstance: 槽位已满 (MAX_INSTANCES=%d)", MAX_INSTANCES);
    return 0;
}

bool DestroyInstance(int id) {
    if (id <= 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN, "TAARenderer::DestroyInstance: 非法 id=%d (合法范围 [1, %d])", id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "TAARenderer::DestroyInstance: id=%d 未分配", id);
        return false;
    }
    // 释放 RT: 临时切到该 instance, 调 ReleaseRT, 然后回原 active
    const int saved = g_active;
    g_active = id;
    ReleaseRT();
    g_states[id] = State{};       // 清空所有字段 (RT handle 等)
    g_slot_in_use[id] = false;
    --g_count;
    // 若被销毁的是 active, 切回 default
    g_active = (saved == id) ? 0 : saved;
    CC::Log(CC::LOG_INFO, "TAARenderer::DestroyInstance: 销毁 instance id=%d (count=%d, active=%d)", id, g_count, g_active);
    return true;
}

bool SetActiveInstance(int id) {
    if (id < 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN, "TAARenderer::SetActiveInstance: 非法 id=%d (合法范围 [0, %d])", id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "TAARenderer::SetActiveInstance: id=%d 未分配", id);
        return false;
    }
    g_active = id;
    // Phase F.1.1: 新 active instance 可能 taauEnabled/renderScale 不同, 重算 mipBias
    updateMipBias_();
    return true;
}

int GetActiveInstance() {
    return g_active;
}

int GetInstanceCount() {
    return g_count;
}

// ==================== Phase F.0.10.9.x.3 — Clone (1-line setup) ====================

int CloneInstance(int srcId) {
    if (srcId < 0 || srcId >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "TAARenderer::CloneInstance: 非法 srcId=%d (合法范围 [0, %d])",
                srcId, MAX_INSTANCES - 1);
        return 0;
    }
    if (!g_slot_in_use[srcId]) {
        CC::Log(CC::LOG_WARN, "TAARenderer::CloneInstance: srcId=%d 未分配", srcId);
        return 0;
    }
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 全字段复制 (含 backend/inited/supported/blendAlpha/halfResHistory/sharpenMode/...)
            g_states[i] = g_states[srcId];
            // 复位 backend 创建的 RT (history × 2)
            g_states[i].historyFbos[0] = 0; g_states[i].historyFbos[1] = 0;
            g_states[i].historyTexs[0] = 0; g_states[i].historyTexs[1] = 0;
            g_states[i].width   = 0; g_states[i].height   = 0;
            g_states[i].historyW = 0; g_states[i].historyH = 0;
            g_states[i].enabled = false;
            // 复位 temporal state (新 instance 第一帧 fallback 走 cur 路径)
            g_states[i].historyIdx = 0;
            g_states[i].hasHistory = false;
            // Phase F.1.0.1: clone 仍清 taauEnabled (新 instance 走自己 Enable + SetTAAUEnabled),
            //   但保留 renderScale / upscalePreset 让用户调过的设置传过去
            g_states[i].taauEnabled   = false;
            g_states[i].renderW       = 0;
            g_states[i].renderH       = 0;
            // renderScale / upscalePreset 保留 (源 instance 调过的设置, 复制过来,
            //   新 instance SetTAAUEnabled(true) 会按此 scale 重算 renderW/H)
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "TAARenderer::CloneInstance: srcId=%d -> id=%d (count=%d)",
                    srcId, i, g_count);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN, "TAARenderer::CloneInstance: 槽位已满");
    return 0;
}

} // namespace TAARenderer
