/**
 * @file   auto_exposure_renderer.cpp
 * @brief  Phase E.5 — Auto Exposure (Eye Adaptation) 模块实现
 *
 * 详见 auto_exposure_renderer.h 的架构说明.
 *
 * 作者: ChocoLight Engine
 * 版本: Phase E.5.2
 */

#include "auto_exposure_renderer.h"
#include "render_backend.h"
#include "light.h"            // CC::Log

#include <cmath>              // log, exp2, log2

namespace {

/// 模块全局状态 (与 BloomRenderer / HDRRenderer 同风格, 匿名 namespace 封装)
struct State {
    RenderBackend* backend = nullptr;
    bool inited      = false;
    bool supported   = false;   // backend->SupportsAutoExposure() 缓存
    bool enabled     = false;   // Enable 成功且未 Disable
    int  width       = 0;       // 源 HDR RT 宽 (Enable 入参, 用于 Resize 同尺寸判断)
    int  height      = 0;

    // HDR 联动 flag (默认 false; 与 Bloom 默认 true 区别;
    //                 AE 改变 exposure 行为强烈, 不应默认接管)
    bool autoEnable  = false;

    // luminance RT 资源
    uint32_t lumFbo  = 0;
    uint32_t lumTex  = 0;
    int      lumW    = 0;       // backend 实际创建尺寸 (~srcW/4)
    int      lumH    = 0;
    int      lastMip = 0;       // 最后一层 mip 等级 = floor(log2(max(lumW, lumH)))

    // EV-based 参数 (默认值见 ALIGNMENT §3.6)
    float targetEV    = 0.0f;   // 中灰偏移 EV
    float speedUp     = 3.0f;   // 暗→亮 EV/sec
    float speedDown   = 1.0f;   // 亮→暗 EV/sec
    float minEV       = -8.0f;
    float maxEV       =  8.0f;

    // 运行时状态
    float currentEV       = 0.0f;   // 平滑后当前 EV
    float currentExposure = 1.0f;   // = 2^currentEV (cache, 避免每帧 exp2)
    float measuredLuma    = 0.0f;   // 上一帧 readback log luma (debug)
    bool  hasFirstSample  = false;  // 第一帧标记: targetEV = measured (无 history 跳变)
};

// ==================== Phase F.2.5 — Multi-Instance ====================
// 仿 BloomRenderer F.0.10.9.x.2: g_states[4] + #define g g_states[g_active]
// 用法: 调用方先 HDR.SetActiveInstance(pipId), 再 AE.SetActiveInstance(pipId).
// HDRRenderer::EndScene 内部 AE.Process / GetCurrentExposure 自然作用于当前 active.
static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };

#define g g_states[g_active]

/// 释放 luminance RT 资源 + 重置状态字段 (不改参数)
void ReleaseLuminanceRT() {
    if (g.backend && g.lumFbo) {
        g.backend->DeleteLuminanceTarget(g.lumFbo, g.lumTex);
    }
    g.lumFbo  = 0;
    g.lumTex  = 0;
    g.lumW    = 0;
    g.lumH    = 0;
    g.lastMip = 0;
    g.width   = 0;
    g.height  = 0;
    g.enabled = false;
    g.hasFirstSample = false;
}

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // anonymous namespace

namespace AutoExposureRenderer {

// ==================== 生命周期 ====================

void Init(RenderBackend* backend) {
    g_active = 0;
    g.backend   = backend;
    g.inited    = (backend != nullptr);
    g.supported = g.inited && backend->SupportsAutoExposure();
}

void Shutdown() {
    ReleaseLuminanceRT();
    g.backend   = nullptr;
    g.inited    = false;
    g.supported = false;
    g.enabled   = false;
    // 参数保留 (与 Bloom / HDR 同风格)
}

// ==================== Enable / Disable / Resize ====================

bool Enable(int w, int h) {
    if (!g.inited || !g.backend) {
        CC::Log(CC::LOG_WARN, "AutoExposureRenderer::Enable: 模块未 Init");
        return false;
    }
    if (!g.supported) {
        return false;
    }
    if (w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN, "AutoExposureRenderer::Enable: 非法尺寸 %dx%d", w, h);
        return false;
    }

    // 已启用且尺寸一致: no-op
    if (g.enabled) {
        if (g.width == w && g.height == h) return true;
        ReleaseLuminanceRT();
    }

    uint32_t fbo = 0, tex = 0;
    int      lumW = 0, lumH = 0;
    if (!g.backend->CreateLuminanceTarget(w, h, &fbo, &tex, &lumW, &lumH)) {
        CC::Log(CC::LOG_ERROR,
                "AutoExposureRenderer::Enable: CreateLuminanceTarget failed (%dx%d)",
                w, h);
        return false;
    }

    // 计算最后一层 mip = floor(log2(max(lumW, lumH)))
    int maxDim = (lumW > lumH) ? lumW : lumH;
    int lastMip = 0;
    while ((1 << (lastMip + 1)) <= maxDim) ++lastMip;

    g.lumFbo  = fbo;
    g.lumTex  = tex;
    g.lumW    = lumW;
    g.lumH    = lumH;
    g.lastMip = lastMip;
    g.width   = w;
    g.height  = h;
    g.enabled = true;
    g.hasFirstSample = false;   // 第一帧重新建立 baseline

    CC::Log(CC::LOG_INFO,
            "AutoExposureRenderer::Enable: src=%dx%d, lum=%dx%d, lastMip=%d",
            w, h, lumW, lumH, lastMip);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    ReleaseLuminanceRT();   // 内部会把 g.enabled 置 false
    CC::Log(CC::LOG_INFO, "AutoExposureRenderer::Disable");
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
    ReleaseLuminanceRT();
    return Enable(w, h);
}

// ==================== HDR 自动联动 ====================

void OnHDREnabled(int w, int h) {
    if (!g.autoEnable) return;
    Enable(w, h);
}

void OnHDRDisabled() {
    // HDR 关后 AE 无 hdrTex 输入, 强制关闭 (无论 autoEnable, 防 RT 悬挂)
    Disable();
}

void OnHDRResized(int w, int h) {
    if (!g.enabled) return;
    Resize(w, h);
}

void SetAutoEnable(bool flag) { g.autoEnable = flag; }
bool GetAutoEnable()          { return g.autoEnable; }

// ==================== EV-based 参数 ====================

void  SetTargetEV(float v) { g.targetEV = v; }
float GetTargetEV()        { return g.targetEV; }

void  SetSpeedUp(float v)   { g.speedUp   = clampf(v, 0.1f, 20.0f); }
float GetSpeedUp()          { return g.speedUp; }

void  SetSpeedDown(float v) { g.speedDown = clampf(v, 0.1f, 20.0f); }
float GetSpeedDown()        { return g.speedDown; }

void SetMinEV(float v) {
    g.minEV = v;
    // 不变量保护: min ≤ max; 越界时把 max 顶到 min
    if (g.maxEV < g.minEV) g.maxEV = g.minEV;
}
float GetMinEV() { return g.minEV; }

void SetMaxEV(float v) {
    g.maxEV = v;
    if (g.minEV > g.maxEV) g.minEV = g.maxEV;
}
float GetMaxEV() { return g.maxEV; }

// ==================== 调试 / OSD getter ====================

float GetCurrentEV()         { return g.enabled ? g.currentEV : 0.0f; }
float GetCurrentExposure()   { return g.enabled ? g.currentExposure : 1.0f; }
float GetMeasuredLuminance() { return g.enabled ? g.measuredLuma : 0.0f; }

// ==================== 管线调用 ====================

void Process(uint32_t hdrTex, float dt) {
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrTex) return;
    if (!g.lumFbo || !g.lumTex) return;

    // 1) Pass 1: log-luminance extract → lumFbo (R16F)
    g.backend->DrawLuminanceExtract(hdrTex, g.lumFbo, g.lumW, g.lumH);

    // 2) Pass 2: GPU mipmap reduce 到 1x1
    g.backend->GenerateLuminanceMipmap(g.lumTex);

    // 3) Pass 3: CPU 同步 readback
    float logLuma = g.backend->ReadbackLuminance1x1(g.lumFbo, g.lastMip);
    g.measuredLuma = logLuma;

    // 4) target EV 计算
    //   targetExposure = 0.18 / luma   (Reinhard key 0.18 中灰)
    //   targetEV       = log2(0.18 / luma) = log2(0.18) - log2(luma)
    //                  = log2(0.18) - logLuma / log(2)
    //   const log2(0.18) ≈ -2.473931
    constexpr float LOG2_KEY   = -2.473931f;
    constexpr float INV_LN2    =  1.442695f;   // 1 / log(2)
    float targetEVRaw = LOG2_KEY - logLuma * INV_LN2;
    // 用户可加 targetEV 偏移 (上调亮场景 / 下调暗场景)
    float targetEV    = clampf(targetEVRaw + g.targetEV, g.minEV, g.maxEV);

    // 5) 时间平滑 (限速 lerp; 双速度)
    if (!g.hasFirstSample) {
        // 第一帧: 直接设, 避免长 fade-in
        g.currentEV      = targetEV;
        g.hasFirstSample = true;
    } else {
        float deltaEV = targetEV - g.currentEV;
        float speed   = (deltaEV > 0.0f) ? g.speedUp : g.speedDown;
        float step    = speed * (dt < 0.0f ? 0.0f : (dt > 0.1f ? 0.1f : dt));
        if      (deltaEV >  step) g.currentEV += step;
        else if (deltaEV < -step) g.currentEV -= step;
        else                      g.currentEV  = targetEV;
        g.currentEV = clampf(g.currentEV, g.minEV, g.maxEV);
    }

    // 6) 缓存 exposure (= 2^currentEV)
    g.currentExposure = std::exp2(g.currentEV);
}

// ==================== Phase F.2.5 — Multi-Instance API ====================

int CreateInstance() {
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            g_states[i] = State{};
            g_states[i].backend   = g_states[0].backend;
            g_states[i].supported = g_states[0].supported;
            g_states[i].inited    = g_states[0].inited;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "AutoExposureRenderer::CreateInstance: id=%d (count=%d)", i, g_count);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN,
            "AutoExposureRenderer::CreateInstance: 槽位已满 (MAX=%d)", MAX_INSTANCES);
    return 0;
}

bool DestroyInstance(int id) {
    if (id <= 0 || id >= MAX_INSTANCES) return false;
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN,
                "AutoExposureRenderer::DestroyInstance: id=%d 未分配", id);
        return false;
    }
    const int saved = g_active;
    g_active = id;
    ReleaseLuminanceRT();
    g_states[id] = State{};
    g_slot_in_use[id] = false;
    --g_count;
    g_active = (saved == id) ? 0 : saved;
    CC::Log(CC::LOG_INFO,
            "AutoExposureRenderer::DestroyInstance: id=%d (count=%d, active=%d)",
            id, g_count, g_active);
    return true;
}

bool SetActiveInstance(int id) {
    if (id < 0 || id >= MAX_INSTANCES) return false;
    if (!g_slot_in_use[id]) return false;
    g_active = id;
    return true;
}

int GetActiveInstance() { return g_active; }
int GetInstanceCount()  { return g_count; }

int CloneInstance(int srcId) {
    if (srcId < 0 || srcId >= MAX_INSTANCES) return 0;
    if (!g_slot_in_use[srcId]) return 0;
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            g_states[i] = g_states[srcId];      // 复制全部参数 + 当前 EV
            // 复位 RT (新 instance 待自己 Enable)
            g_states[i].lumFbo = g_states[i].lumTex = 0;
            g_states[i].lumW = g_states[i].lumH = 0;
            g_states[i].lastMip = 0;
            g_states[i].width = g_states[i].height = 0;
            g_states[i].enabled = false;
            g_states[i].hasFirstSample = false;     // 新 instance 重新建立 baseline
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "AutoExposureRenderer::CloneInstance: srcId=%d -> id=%d (count=%d)",
                    srcId, i, g_count);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN, "AutoExposureRenderer::CloneInstance: 槽位已满");
    return 0;
}

} // namespace AutoExposureRenderer
