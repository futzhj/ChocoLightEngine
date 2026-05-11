/**
 * @file   bloom_renderer.cpp
 * @brief  Phase E.4 — Bloom 后处理命名空间模块实现
 *
 * 详见 bloom_renderer.h 的架构说明.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.4.2
 */

#include "bloom_renderer.h"
#include "render_backend.h"
#include "cc_core.h"

namespace {

/// pyramid 层数上限 (DESIGN 规定 2..8)
static constexpr int MAX_LEVELS = 8;

/// 模块全局状态 (与 HDRRenderer 同风格, 匿名 namespace 封装)
struct State {
    RenderBackend* backend = nullptr;
    bool inited     = false;
    bool supported  = false;    // backend->SupportsBloom() 缓存
    bool enabled    = false;    // Enable 成功且未 Disable
    int  width      = 0;        // pyramid[0] 宽
    int  height     = 0;        // pyramid[0] 高

    // HDR 联动 flag (默认 true, 用户可通过 SetAutoEnable(false) 关闭)
    bool autoEnable = true;

    // pyramid 资源 (仅 [0..actualLevels-1] 有效)
    uint32_t fbos[MAX_LEVELS] = {0};
    uint32_t texs[MAX_LEVELS] = {0};
    int  actualLevels = 0;       // backend->CreateBloomPyramid 实际返回的层数

    // 参数 (默认值见 DESIGN §6.2)
    float threshold       = 1.0f;   // 亮度阈值
    float intensity       = 0.8f;   // 合成强度
    float radius          = 0.7f;   // 扩散半径
    int   requestedLevels = 5;      // 下次 Enable/Resize 应用
};

static State g;

/// 内部辅助: 释放 pyramid 资源 + 重置状态字段 (不改参数)
void ReleasePyramid() {
    if (g.backend && g.actualLevels > 0) {
        g.backend->DeleteBloomPyramid(g.fbos, g.texs, g.actualLevels);
    }
    for (int i = 0; i < MAX_LEVELS; ++i) {
        g.fbos[i] = 0;
        g.texs[i] = 0;
    }
    g.actualLevels = 0;
    g.width  = 0;
    g.height = 0;
    g.enabled = false;
}

/// clamp int 到 [lo, hi]
inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/// clamp float 到 [lo, hi]
inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // anonymous namespace

namespace BloomRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g.backend = backend;
    g.inited  = (backend != nullptr);
    g.supported = g.inited && backend->SupportsBloom();
    // 其它字段保持默认值 (见 State 默认初值)
}

void Shutdown() {
    ReleasePyramid();
    g.backend   = nullptr;
    g.inited    = false;
    g.supported = false;
    g.enabled   = false;
    // 参数保留 (下次 Init + Enable 继承; 与 HDRRenderer 同风格)
}

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "BloomRenderer::Enable: 模块未 Init");
        return false;
    }
    if (!g.supported) {
        // 低频 warn: 非 GL33 后端 / shader 编译失败
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "BloomRenderer::Enable: 非法尺寸 %dx%d", w, h);
        return false;
    }

    // 已启用时: 如果大小一致直接返回 true; 否则先 Disable 再重建
    if (g.enabled) {
        if (g.width == w && g.height == h) return true;
        ReleasePyramid();
    }

    int levels = clampi(g.requestedLevels, 2, MAX_LEVELS);
    int created = g.backend->CreateBloomPyramid(w, h, levels, g.fbos, g.texs);
    if (created < 2) {
        // 完全失败或只创建 1 级 (不够 pyramid 用): 全部回滚
        if (created > 0) {
            g.backend->DeleteBloomPyramid(g.fbos, g.texs, created);
            for (int i = 0; i < MAX_LEVELS; ++i) { g.fbos[i] = 0; g.texs[i] = 0; }
        }
        g.actualLevels = 0;
        CC::Log(CC::LOG_ERROR,
                "BloomRenderer::Enable: pyramid 创建失败 (请求 %d 级, 实际 %d 级)",
                levels, created);
        return false;
    }

    g.actualLevels = created;
    g.width   = w;
    g.height  = h;
    g.enabled = true;

    CC::Log(CC::LOG_INFO,
            "BloomRenderer::Enable: %dx%d, %d level pyramid", w, h, created);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleasePyramid();   // 内部会把 g.enabled 置 false
    CC::Log(CC::LOG_INFO, "BloomRenderer::Disable");
}

bool IsEnabled()   { return g.enabled; }
bool IsSupported() { return g.supported; }

bool Resize(int w, int h) {
    if (!g.inited || !g.supported) return false;
    if (w <= 0 || h <= 0) return false;
    // 未启用时 Resize 视作 Enable
    if (!g.enabled) return Enable(w, h);
    // 大小未变: no-op
    if (g.width == w && g.height == h) return true;
    // 重建
    ReleasePyramid();
    return Enable(w, h);
}

// ==================== HDR 自动联动 ====================

void OnHDREnabled(int w, int h) {
    if (!g.autoEnable) return;
    // 联动 Enable: 失败不中断 HDR 流程 (Bloom 有日志提示)
    Enable(w, h);
}

void OnHDRDisabled() {
    // HDR 关后 bloom 无输入, 强制关闭 (无论 autoEnable)
    Disable();
}

void OnHDRResized(int w, int h) {
    if (!g.enabled) return;    // 未启用时不跟随 Resize
    Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()          { return g.autoEnable; }

// ==================== 参数 ====================

void  SetThreshold(float v) { g.threshold = clampf(v, 0.0f, 1e9f); }
float GetThreshold()        { return g.threshold; }

void  SetIntensity(float v) { g.intensity = clampf(v, 0.0f, 1e9f); }
float GetIntensity()        { return g.intensity; }

void  SetRadius(float v)    { g.radius = clampf(v, 0.0f, 1.0f); }
float GetRadius()           { return g.radius; }

void SetLevels(int n) {
    // 边界外静默 clamp 到 [2, MAX_LEVELS]
    g.requestedLevels = clampi(n, 2, MAX_LEVELS);
    // 不立即重建; 下次 Enable/Resize 生效 (DESIGN §6.2 约定)
}

int GetLevels() { return g.requestedLevels; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrFbo, uint32_t hdrTex) {
    // 防御性检查: 未启用 / backend 无效 / pyramid 不足 → no-op
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrFbo || !hdrTex) return;
    if (g.actualLevels < 2) return;

    // 1. Bright Pass: HDR RT → pyramid[0]
    g.backend->DrawBloomBrightPass(hdrTex, g.fbos[0],
                                    g.width, g.height, g.threshold);

    // 2. Downsample: pyramid[0] → [1] → ... → [actualLevels-1]
    // 每级尺寸 = (前级宽 / 2, 前级高 / 2), 最小 1x1
    int prevW = g.width, prevH = g.height;
    for (int i = 1; i < g.actualLevels; ++i) {
        int curW = (prevW > 1) ? prevW / 2 : 1;
        int curH = (prevH > 1) ? prevH / 2 : 1;
        g.backend->DrawBloomDownsample(g.texs[i - 1], g.fbos[i], curW, curH);
        prevW = curW;
        prevH = curH;
    }

    // 3. Upsample + additive blend: 反向从最底层往回累加
    // pyramid[N-1] → [N-2] → ... → [0]
    // 每级 upsample 的 dst 大小 = 下级 * 2, 但最小 1x1
    for (int i = g.actualLevels - 1; i > 0; --i) {
        // 第 i-1 级的原始大小 (不依赖 prevW/prevH, 按 width >> (i-1) 反算)
        int dstW = g.width  >> (i - 1);
        int dstH = g.height >> (i - 1);
        if (dstW < 1) dstW = 1;
        if (dstH < 1) dstH = 1;
        g.backend->DrawBloomUpsample(g.texs[i], g.fbos[i - 1],
                                      dstW, dstH, g.radius);
    }

    // 4. Composite: pyramid[0] additive blend → hdrFbo (intensity 缩放)
    g.backend->DrawBloomComposite(g.texs[0], hdrFbo,
                                   g.width, g.height, g.intensity);
}

} // namespace BloomRenderer
