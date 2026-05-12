/**
 * @file   ssao_renderer.cpp
 * @brief  Phase E.8 — SSAORenderer 实现
 *
 * 模块结构 (与 LensFlareRenderer 同风格, 增加 SSAO 专用 depth RT + kernel/noise 资源):
 *   - State (匿名 namespace, 模块单例)
 *   - 生命周期: Init/Shutdown/Enable/Disable/Resize/IsEnabled/IsSupported
 *   - HDR 联动: OnHDREnabled/OnHDRDisabled/OnHDRResized
 *   - 参数 setter/getter (6 对) + AutoEnable
 *   - Process: blit -> raw -> blur (h+v) -> composite
 *   - 辅助: GenerateHemisphereKernel + InvertMat4 (内部)
 */

#include "ssao_renderer.h"
#include "render_backend.h"
#include "light.h"   // CC::Log

#include <cmath>
#include <cstring>

namespace SSAORenderer {

namespace {

// ==================== 内部辅助 ====================

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// 生成 16 个半球采样方向 (tangent space, z >= 0 半球内)
/// 用 Hammersley 风格序列 + 距离重分布 (近密远疏, lerp(0.1, 1.0, (i/N)^2))
/// 输出格式: float[16 * 3], 每 3 个为一个 vec3
static void GenerateHemisphereKernel(float* outKernel, int n) {
    // 简单 LCG (deterministic, 不污染外部 srand)
    unsigned int s = 12345u;
    auto rnd01 = [&s]() -> float {
        s = s * 1664525u + 1013904223u;
        return (s & 0xFFFFFF) / float(0x1000000);   // [0, 1)
    };

    for (int i = 0; i < n; ++i) {
        // x, y in [-1, 1]; z in [0, 1] (上半球)
        float x = rnd01() * 2.0f - 1.0f;
        float y = rnd01() * 2.0f - 1.0f;
        float z = rnd01();
        // 归一化到单位向量
        float len = sqrtf(x * x + y * y + z * z);
        if (len < 1e-4f) { x = 0.0f; y = 0.0f; z = 1.0f; len = 1.0f; }
        x /= len; y /= len; z /= len;

        // 距离分布: 近密远疏
        float scale = (float)i / (float)n;
        scale = 0.1f + (scale * scale) * 0.9f;   // lerp(0.1, 1.0, scale^2)
        x *= scale; y *= scale; z *= scale;

        outKernel[i * 3 + 0] = x;
        outKernel[i * 3 + 1] = y;
        outKernel[i * 3 + 2] = z;
    }
}

/// 4x4 矩阵求逆 (列主序, GLU 风格伴随矩阵 + 行列式)
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

// ==================== 模块状态 ====================

struct State {
    bool     enabled        = false;
    bool     supported      = false;
    bool     autoEnable     = false;

    RenderBackend* backend  = nullptr;

    // 双 RT 旁路: full-res 独立 depth tex + 小 FBO
    uint32_t depthFbo       = 0;
    uint32_t depthTex       = 0;

    // AO ping-pong (R16F, 半分辨率)
    uint32_t fbos[2]        = {0, 0};   // [0]: raw AO, [1]: blur temp
    uint32_t texs[2]        = {0, 0};
    uint32_t noiseTex       = 0;
    int      rtW            = 0;
    int      rtH            = 0;        // 半分辨率 (full-res / 2)
    int      srcW           = 0;
    int      srcH           = 0;        // full-res HDR RT

    // 16 半球采样方向 (3 floats × 16 = 48)
    float    kernel[16 * 3] = {0};

    // 参数 (默认值见 ALIGNMENT §3.2)
    float    radius         = 0.5f;
    float    bias           = 0.025f;
    float    intensity      = 1.0f;
    int      kernelSize     = 16;
    float    power          = 2.0f;
    bool     blurEnabled    = true;
};

static State g;

// ==================== 内部资源管理 ====================

/// 释放所有动态资源 (depth/AO RT + noise tex), 不动 backend 指针 / 参数
static void DestroyResources() {
    if (!g.backend) return;
    if (g.depthFbo || g.depthTex) {
        g.backend->DeleteSSAODepthRT(g.depthFbo, g.depthTex);
        g.depthFbo = g.depthTex = 0;
    }
    if (g.fbos[0] || g.fbos[1] || g.texs[0] || g.texs[1]) {
        g.backend->DeleteSSAOTargets(g.fbos, g.texs);
    }
    if (g.noiseTex) {
        g.backend->DeleteSSAONoiseTex(g.noiseTex);
        g.noiseTex = 0;
    }
    g.rtW = g.rtH = g.srcW = g.srcH = 0;
}

/// 分配所有资源 (depth RT + AO RT + noise tex)
static bool AllocateResources(int w, int h) {
    if (!g.backend || w <= 0 || h <= 0) return false;

    // depth RT: full-res, 与 HDR 同尺寸
    if (!g.backend->CreateSSAODepthRT(w, h, &g.depthFbo, &g.depthTex)) {
        CC::Log(CC::LOG_WARN, "SSAORenderer: CreateSSAODepthRT failed (%dx%d)", w, h);
        return false;
    }
    // AO ping-pong: 半分辨率
    if (!g.backend->CreateSSAOTargets(w, h, g.fbos, g.texs, &g.rtW, &g.rtH)) {
        CC::Log(CC::LOG_WARN, "SSAORenderer: CreateSSAOTargets failed (%dx%d)", w, h);
        g.backend->DeleteSSAODepthRT(g.depthFbo, g.depthTex);
        g.depthFbo = g.depthTex = 0;
        return false;
    }
    // noise tex (deterministic, 一次创建)
    if (!g.noiseTex) {
        g.noiseTex = g.backend->CreateSSAONoiseTex();
        if (!g.noiseTex) {
            CC::Log(CC::LOG_WARN, "SSAORenderer: CreateSSAONoiseTex failed");
            g.backend->DeleteSSAOTargets(g.fbos, g.texs);
            g.backend->DeleteSSAODepthRT(g.depthFbo, g.depthTex);
            g.depthFbo = g.depthTex = 0;
            return false;
        }
    }
    g.srcW = w;
    g.srcH = h;
    return true;
}

} // anonymous namespace

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend   = backend;
    g.supported = (backend && backend->SupportsSSAO());
    GenerateHemisphereKernel(g.kernel, 16);   // 一次生成 lifetime 不变
    if (g.supported) {
        CC::Log(CC::LOG_INFO, "SSAORenderer: initialized (supported=yes)");
    } else {
        CC::Log(CC::LOG_INFO, "SSAORenderer: initialized (supported=no, backend=%s)",
                backend ? backend->GetName() : "null");
    }
}

void Shutdown() {
    DestroyResources();
    g.enabled = false;
    g.backend = nullptr;
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
    // SSAO 必须先关 (HDR RT 消亡前清自己资源, 防 blit 用到失效 fbo)
    Disable();
}

void OnHDRResized(int w, int h) {
    if (g.enabled) Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()           { return g.autoEnable; }

// ==================== 参数 ====================

void  SetRadius(float v)       { g.radius = clampf(v, 0.05f, 5.0f); }
float GetRadius()               { return g.radius; }

void  SetBias(float v)          { g.bias = clampf(v, 0.0f, 0.2f); }
float GetBias()                  { return g.bias; }

void  SetIntensity(float v)    { g.intensity = clampf(v, 0.0f, 4.0f); }
float GetIntensity()            { return g.intensity; }

void SetKernelSize(int n) {
    // 仅接受 8 或 16 (其他值 clamp 到最近)
    g.kernelSize = (n <= 12) ? 8 : 16;
}
int GetKernelSize() { return g.kernelSize; }

void  SetPower(float v)         { g.power = clampf(v, 0.5f, 8.0f); }
float GetPower()                 { return g.power; }

void SetBlurEnabled(bool flag) { g.blurEnabled = flag; }
bool GetBlurEnabled()           { return g.blurEnabled; }

// ==================== 管线 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    if (!g.enabled || !g.supported || !g.backend) return;
    if (!hdrFbo || !hdrTex) return;
    if (!g.depthFbo || !g.depthTex || !g.fbos[0] || !g.fbos[1] || !g.noiseTex) return;
    (void)hdrTex;   // composite 内部直接读 dstFbo (HDR RT) 内容; hdrTex 仅校验

    // Phase E.8.x: 拿 HDR FBO 关联的 G-buffer normal tex (MRT slot 1).
    // backend 不支持 MRT 或 fbo 未带 normal 时返回 0 -> silent fallback (skip Process,
    // 不绘制 SSAO; HDR 颜色保持原样; 用户视觉上等同 SSAO 未启用).
    uint32_t normalTex = g.backend->GetHDRNormalTex(hdrFbo);
    if (!normalTex) {
        // 仅首次 skip 时打 warning (避免每帧刷屏): 用静态 once flag.
        static bool warned = false;
        if (!warned) {
            CC::Log(CC::LOG_WARN,
                    "SSAORenderer::Process: HDR FBO 无 G-buffer normal RT, 跳过 SSAO (后端不支持 MRT 或 FBO 旧版本)");
            warned = true;
        }
        return;
    }

    // 0. 旁路核心: 从 HDR FBO 复制 depth 到 SSAO depth tex
    g.backend->BlitHDRDepthToSSAO(hdrFbo, g.depthFbo, g.srcW, g.srcH);

    // 取当前 projection 矩阵 + 计算逆
    float proj[16];
    float invProj[16];
    g.backend->GetProjection(proj);
    if (!InvertMat4(proj, invProj)) {
        // projection 退化 (如纯 2D 全 0 矩阵): 静默 skip
        return;
    }

    // 1. raw AO: depthTex + noiseTex + normalTex -> fbos[0] (R16F, 半分辨率)
    g.backend->DrawSSAO(g.depthTex, g.noiseTex, normalTex, g.fbos[0],
                         g.rtW, g.rtH,
                         proj, invProj, g.kernel, g.kernelSize,
                         g.radius, g.bias, g.power);

    // 2. blur (可选 2-pass separable)
    uint32_t aoTex = g.texs[0];
    if (g.blurEnabled) {
        // 水平: texs[0] -> fbos[1]
        g.backend->DrawSSAOBlur(g.texs[0], g.depthTex, g.fbos[1], g.rtW, g.rtH, 0);
        // 垂直: texs[1] -> fbos[0]
        g.backend->DrawSSAOBlur(g.texs[1], g.depthTex, g.fbos[0], g.rtW, g.rtH, 1);
        aoTex = g.texs[0];
    }

    // 3. composite: HDR *= mix(1.0, ao, intensity) (覆盖写 HDR RT)
    //    backend 内部用临时 RT 解 feedback loop (读 HDR + 写 HDR)
    g.backend->DrawSSAOComposite(aoTex, hdrFbo, g.srcW, g.srcH, g.intensity);
}

} // namespace SSAORenderer
