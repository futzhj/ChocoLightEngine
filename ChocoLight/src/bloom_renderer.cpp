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
#include "light.h"            // CC::Log
#include <algorithm>          // std::max (Phase F.0.10.3 — region mip clamp)

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

// ==================== Phase F.0.10.9.x.2 — Multi-instance support ====================
//
// 老 `static State g;` 单例 → `static State g_states[MAX_INSTANCES]; static int g_active = 0;`
// 通过 macro `#define g g_states[g_active]` 让现有 fn 零改动继续访问 active instance
//   - g_states[0] 是 default singleton (老 namespace API 行为完全等价 Phase E.4~F.0.10.10)
//   - 新加 5 fn (CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount)
//   - 每 instance 独立: backend ptr / supported / inited / enabled / pyramid (fbos[8] + texs[8] + actualLevels) /
//                       width/height / threshold/intensity/radius/requestedLevels / autoEnable
//   - MAX_INSTANCES=4: default + 3 user (与 HDR/TAA 一致)
static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };

// 现有 fn 沿用 `g.X` 写法; macro 透明展开到 active instance
// 仅在 bloom_renderer.cpp 内部有效
#define g g_states[g_active]

/// 内部辅助: 释放 pyramid 资源 + 重置状态字段 (不改参数)
void ReleasePyramid() {
    // Phase G.1.1 — VRAM Tracking: 在 backend Delete 前 Untrack 各级 (此时 g.width/height/actualLevels 仍有效)
    //   每级尺寸递推 = w/2^i × h/2^i (与 backend->CreateBloomPyramid 算法严格一致, 见 render_gl33.cpp:5688-5689)
    if (g.actualLevels > 0 && g.width > 0 && g.height > 0) {
        int curW = g.width, curH = g.height;
        for (int i = 0; i < g.actualLevels; ++i) {
            LT::GpuMem::Untrack("Bloom pyramid", "RGBA16F", curW, curH);
            curW = (curW > 1) ? curW / 2 : 1;
            curH = (curH > 1) ? curH / 2 : 1;
        }
    }
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
    // Phase F.0.10.9.x.2: 仅初始化 g_states[0] (default singleton)
    g_active = 0;       // 显式回到 default, 后续 g.X 即作用于 [0]
    g.backend = backend;
    g.inited  = (backend != nullptr);
    g.supported = g.inited && backend->SupportsBloom();
    // 其它字段保持默认值 (见 State 默认初值)
}

void Shutdown() {
    // Phase F.0.10.9.x.2: 反向遍历, 销毁所有 user instance, 最后清 default
    for (int i = MAX_INSTANCES - 1; i >= 1; --i) {
        if (g_slot_in_use[i]) {
            const int saved = g_active;
            g_active = i;
            ReleasePyramid();
            g_states[i] = State{};
            g_slot_in_use[i] = false;
            g_active = saved;
        }
    }
    // 清 default (i=0)
    g_active = 0;
    ReleasePyramid();
    g.backend   = nullptr;
    g.inited    = false;
    g.supported = false;
    g.enabled   = false;
    g_count = 1;        // 仅 default 占用
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

    // Phase G.1.1 — VRAM Tracking: 逐级 Track, 与 ReleasePyramid 严格对称
    //   各级尺寸 w/2^i × h/2^i (min 1×1), RGBA16F (backend 固定格式, 见 render_gl33.cpp:5658)
    {
        int curW = w, curH = h;
        for (int i = 0; i < created; ++i) {
            LT::GpuMem::Track("Bloom pyramid", "RGBA16F", curW, curH);
            curW = (curW > 1) ? curW / 2 : 1;
            curH = (curH > 1) ? curH / 2 : 1;
        }
    }

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
    // 转发到 region 版本 (0/0/0/0 = 全屏老路径, 零回归)
    Process(hdrFbo, hdrTex, 0, 0, 0, 0);
}

// Phase F.0.10.3 — Region 限定 bloom (split-screen 必备)
// rgnW=0 || rgnH=0 时退化为全屏路径 (与老 Process 等价, 零回归)
// region 在 mip 链中按 >>i (downsample) / <<i (upsample) 缩放
// Phase F.0.10.5 — 加 uvBounds 上传到 Down/Up/Composite shader, 完美防 ~1px 边界泄漏
void Process(uint32_t hdrFbo, uint32_t hdrTex,
             int rgnX, int rgnY, int rgnW, int rgnH) {
    // 防御性检查: 未启用 / backend 无效 / pyramid 不足 → no-op
    if (!g.enabled || !g.backend || !g.supported) return;
    if (!hdrFbo || !hdrTex) return;
    if (g.actualLevels < 2) return;

    // useRegion: rgnW/rgnH > 0 才启 scissor; 否则退化为全屏老路径
    const bool useRegion = (rgnW > 0 && rgnH > 0);

    // Phase F.0.10.5 — uvBounds 算 helper (lambda) — 把 region 转换为 src 空间 normalized UV
    // 加 0.5 texel inset 防线性插值越界 (业界标准, UE 同模式)
    auto calcUvBounds = [](int srcW, int srcH,
                            int srcRgnX, int srcRgnY, int srcRgnW, int srcRgnH,
                            float out[4]) {
        const float invW = 1.0f / (float)srcW;
        const float invH = 1.0f / (float)srcH;
        out[0] = ((float)srcRgnX           + 0.5f) * invW;
        out[1] = ((float)srcRgnY           + 0.5f) * invH;
        out[2] = ((float)(srcRgnX + srcRgnW) - 0.5f) * invW;
        out[3] = ((float)(srcRgnY + srcRgnH) - 0.5f) * invH;
    };

    // 1. Bright Pass: HDR RT → pyramid[0]  (region 同输入坐标, mip-0 full-res)
    //    Bright shader 单点采 → 不需 uvBounds (老 backend 默认 nullptr OK)
    g.backend->DrawBloomBrightPass(hdrTex, g.fbos[0],
                                    g.width, g.height, g.threshold,
                                    rgnX, rgnY, rgnW, rgnH);

    // 2. Downsample: pyramid[0] → [1] → ... → [actualLevels-1]
    // 每级尺寸 = (前级宽 / 2, 前级高 / 2), 最小 1x1; region 同步缩半
    // Phase F.0.10.5: shader 内 ClampUV 用 src (i-1) 空间 UV, 故 uvBounds 算 src region
    int prevW = g.width, prevH = g.height;
    int curRgnX = rgnX, curRgnY = rgnY, curRgnW = rgnW, curRgnH = rgnH;
    for (int i = 1; i < g.actualLevels; ++i) {
        int curW = (prevW > 1) ? prevW / 2 : 1;
        int curH = (prevH > 1) ? prevH / 2 : 1;
        // Phase F.0.10.5: src region = 当前 mip-(i-1) region (即缩半前 region) — 算 uvBounds
        float uvBoundsBuf[4];
        const float* uvBoundsPtr = nullptr;
        if (useRegion) {
            calcUvBounds(prevW, prevH, curRgnX, curRgnY, curRgnW, curRgnH, uvBoundsBuf);
            uvBoundsPtr = uvBoundsBuf;
        }
        // dst region 缩半 (>>1); useRegion=false 时保持 0/0/0/0
        if (useRegion) {
            curRgnX >>= 1;
            curRgnY >>= 1;
            curRgnW = (curRgnW > 1) ? (curRgnW >> 1) : 1;
            curRgnH = (curRgnH > 1) ? (curRgnH >> 1) : 1;
        }
        g.backend->DrawBloomDownsample(g.texs[i - 1], g.fbos[i], curW, curH,
                                        curRgnX, curRgnY, curRgnW, curRgnH,
                                        uvBoundsPtr);   // Phase F.0.10.5
        prevW = curW;
        prevH = curH;
    }

    // 3. Upsample + additive blend: 反向从最底层往回累加
    // pyramid[N-1] → [N-2] → ... → [0]
    // 每级 upsample 的 dst 大小 = 下级 * 2; region 按 (i-1) 层反算 (不递推, 避免误差)
    // Phase F.0.10.5: src 是 mip-i, uvBounds 算 mip-i 空间 region
    for (int i = g.actualLevels - 1; i > 0; --i) {
        // 第 i-1 级的原始大小 (按 width >> (i-1) 反算)
        int dstW = g.width  >> (i - 1);
        int dstH = g.height >> (i - 1);
        if (dstW < 1) dstW = 1;
        if (dstH < 1) dstH = 1;
        // src mip-i 大小 (按 >> i 反算)
        int srcW = g.width  >> i;
        int srcH = g.height >> i;
        if (srcW < 1) srcW = 1;
        if (srcH < 1) srcH = 1;
        // region 在 i-1 级 (dst) 和 i 级 (src) 各自的坐标 (按 >> 反算, 不递推保证一致性)
        int dRgnX = useRegion ? (rgnX >> (i - 1)) : 0;
        int dRgnY = useRegion ? (rgnY >> (i - 1)) : 0;
        int dRgnW = useRegion ? ((rgnW > 0) ? std::max(1, rgnW >> (i - 1)) : 0) : 0;
        int dRgnH = useRegion ? ((rgnH > 0) ? std::max(1, rgnH >> (i - 1)) : 0) : 0;
        // Phase F.0.10.5: uvBounds 在 src mip-i 空间
        float uvBoundsBuf[4];
        const float* uvBoundsPtr = nullptr;
        if (useRegion) {
            int sRgnX = rgnX >> i;
            int sRgnY = rgnY >> i;
            int sRgnW = std::max(1, rgnW >> i);
            int sRgnH = std::max(1, rgnH >> i);
            calcUvBounds(srcW, srcH, sRgnX, sRgnY, sRgnW, sRgnH, uvBoundsBuf);
            uvBoundsPtr = uvBoundsBuf;
        }
        g.backend->DrawBloomUpsample(g.texs[i], g.fbos[i - 1],
                                      dstW, dstH, g.radius,
                                      dRgnX, dRgnY, dRgnW, dRgnH,
                                      uvBoundsPtr);   // Phase F.0.10.5
    }

    // 4. Composite: pyramid[0] additive blend → hdrFbo (intensity 缩放), region 同输入坐标
    // Phase F.0.10.5: src = pyramid[0] mip-0 (g.width × g.height), uvBounds 同 region
    float compositeUvBoundsBuf[4];
    const float* compositeUvBoundsPtr = nullptr;
    if (useRegion) {
        calcUvBounds(g.width, g.height, rgnX, rgnY, rgnW, rgnH, compositeUvBoundsBuf);
        compositeUvBoundsPtr = compositeUvBoundsBuf;
    }
    g.backend->DrawBloomComposite(g.texs[0], hdrFbo,
                                   g.width, g.height, g.intensity,
                                   rgnX, rgnY, rgnW, rgnH,
                                   compositeUvBoundsPtr);   // Phase F.0.10.5
}

// ==================== Phase E.6 — 高级查询 ====================

uint32_t GetPyramidTopTex() {
    // 未启用 / pyramid 未建 / 资源失效: 返回 0 (调用方视作 "no bloom")
    if (!g.enabled || g.actualLevels < 1) return 0;
    return g.texs[0];
}

// ==================== Phase F.0.10.9.x.2 — Multi-Instance API ====================
//
// 设计说明 (与 HDR/TAA multi-instance 完全一致):
//   - g_states[0] 是 default singleton, 永远占用; 老 namespace API 默认作用于 [0]
//   - CreateInstance() 找空闲槽 [1, MAX_INSTANCES-1] 返 ID, 槽满返 0
//   - 新 instance 继承 default 的 backend/supported/inited (来自 Init)
//   - SetActiveInstance(id) 切换 active, 后续 namespace fn 作用于 [id]
//   - DestroyInstance(id) 释放该槽 pyramid + 标空闲, 若 active=该 id 自动切回 0
//   - 不能销毁 id=0 (default), 不能 SetActiveInstance(无效 id)

int CreateInstance() {
    // 找第一个空闲槽 (跳过 [0])
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 复位为干净 default state, 然后继承 default 的 backend / supported / inited
            g_states[i] = State{};
            g_states[i].backend   = g_states[0].backend;
            g_states[i].supported = g_states[0].supported;
            g_states[i].inited    = g_states[0].inited;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "BloomRenderer::CreateInstance: 创建 instance id=%d (count=%d, inited=%d)",
                    i, g_count, g_states[0].inited ? 1 : 0);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN,
            "BloomRenderer::CreateInstance: 槽位已满 (MAX_INSTANCES=%d)", MAX_INSTANCES);
    return 0;
}

bool DestroyInstance(int id) {
    if (id <= 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "BloomRenderer::DestroyInstance: 非法 id=%d (合法范围 [1, %d])",
                id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "BloomRenderer::DestroyInstance: id=%d 未分配", id);
        return false;
    }
    // 释放 pyramid: 临时切到该 instance, 调 ReleasePyramid, 然后回原 active
    const int saved = g_active;
    g_active = id;
    ReleasePyramid();
    g_states[id] = State{};            // 清空所有字段 (RT handle 等)
    g_slot_in_use[id] = false;
    --g_count;
    // 若被销毁的是 active, 切回 default
    g_active = (saved == id) ? 0 : saved;
    CC::Log(CC::LOG_INFO,
            "BloomRenderer::DestroyInstance: 销毁 instance id=%d (count=%d, active=%d)",
            id, g_count, g_active);
    return true;
}

bool SetActiveInstance(int id) {
    if (id < 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "BloomRenderer::SetActiveInstance: 非法 id=%d (合法范围 [0, %d])",
                id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "BloomRenderer::SetActiveInstance: id=%d 未分配", id);
        return false;
    }
    g_active = id;
    return true;
}

int GetActiveInstance() { return g_active; }
int GetInstanceCount()  { return g_count; }

// ==================== Phase F.0.10.9.x.3 — Clone (1-line setup) ====================

int CloneInstance(int srcId) {
    // 边界检查
    if (srcId < 0 || srcId >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "BloomRenderer::CloneInstance: 非法 srcId=%d (合法范围 [0, %d])",
                srcId, MAX_INSTANCES - 1);
        return 0;
    }
    if (!g_slot_in_use[srcId]) {
        CC::Log(CC::LOG_WARN, "BloomRenderer::CloneInstance: srcId=%d 未分配", srcId);
        return 0;
    }
    // 找空闲槽
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 全字段复制 (含 backend/supported/inited/threshold/intensity/radius/requestedLevels)
            g_states[i] = g_states[srcId];
            // 复位 backend 创建的 RT (新 instance 待自己 Enable 重建 pyramid)
            for (int k = 0; k < MAX_LEVELS; ++k) {
                g_states[i].fbos[k] = 0;
                g_states[i].texs[k] = 0;
            }
            g_states[i].actualLevels = 0;
            g_states[i].enabled = false;
            g_states[i].width   = 0;
            g_states[i].height  = 0;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "BloomRenderer::CloneInstance: srcId=%d -> id=%d (count=%d)",
                    srcId, i, g_count);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN, "BloomRenderer::CloneInstance: 槽位已满");
    return 0;
}

} // namespace BloomRenderer
