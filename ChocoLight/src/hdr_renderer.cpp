/**
 * @file hdr_renderer.cpp
 * @brief Phase E.3.2 — HDR 离屏渲染管线实现
 *
 * 与 header 约定: 所有 GL 操作经 RenderBackend; 本文件零 GL 依赖, 跨平台.
 */

#include "hdr_renderer.h"
#include "render_backend.h"             // Phase E.14 — 需 VelocityFormat 完整定义 (header 仅 fwd decl)
#include "bloom_renderer.h"             // Phase E.4.2 — HDR Enable/Disable/Resize 联动回调
#include "auto_exposure_renderer.h"     // Phase E.5.2 — HDR Enable/Disable/Resize 联动回调 + EndScene exposure 覆盖
#include "lens_dirt_renderer.h"         // Phase E.6.2 — Lens Dirt 后处理联动
#include "streak_renderer.h"            // Phase E.6.2 — Streak anamorphic flare 联动
#include "lens_flare_renderer.h"        // Phase E.7.2 — Lens Flare (ghost + halo + chromatic) 联动
#include "ssao_renderer.h"               // Phase E.8.2 — SSAO (屏幕空间环境光遮蔽) 联动
#include "ssr_renderer.h"                // Phase E.9 — SSR (屏幕空间反射) 联动
#include "motion_blur_renderer.h"        // Phase E.15 — Velocity-driven Motion Blur 联动
#include "taa_renderer.h"                // Phase F.0 — TAA 主管线联动
#include "light.h"         // CC::Log

#include <chrono>
#include <vector>          // Phase F.0.10.8.1 — .cube parser byte buffer
#include <cstdio>          // Phase F.0.10.8.1 — snprintf
#include <cstdlib>         // Phase F.0.10.8.1 — strtof / strtol
#include <cstring>         // Phase F.0.10.8.1 — strncmp
#include <cstdarg>         // Phase F.0.10.8.1 — writeErr_ vsnprintf
#include <string>          // Phase F.0.10.8.3 — WatchEntry.path
#include <SDL3/SDL.h>      // Phase F.0.10.8.1 — SDL_LoadFile / SDL_free / SDL_GetPathInfo
#include "stb_image.h"     // Phase F.0.10.8.2 — HALD CLUT 图像 LUT 解码 (实现在 third_party/stb_impl.c)

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
    int            width     = 0;      // Phase F.1: 当 taauActive=true 时 == renderW (与 fbo/sceneTex 同尺寸); 否则 == outputW
    int            height    = 0;      // 同上

    // Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦
    //   taauActive=false: 沿用 F.0 单尺寸 (renderW/H 等同 width/height = outputW/H)
    //   taauActive=true:  fbo/sceneTex/velocityTex/normalTex/depth 在 (renderW, renderH);
    //                     outputSceneTex + history 在 (outputW, outputH); sharpen/tonemap 读 outputSceneTex
    int            outputW   = 0;      // 用户 Enable 入参 (== window 尺寸)
    int            outputH   = 0;
    bool           taauActive = false; // 由 TAARenderer::SetTAAUEnabled 通过 OnTAAURenderScaleChanged 通知
    uint32_t       outputSceneFbo = 0; // output-res sceneTex FBO (RGBA16F); 仅 taauActive 时分配
    uint32_t       outputSceneTex = 0; // output-res sceneTex; sharpen/tonemap 输入

    // Tonemap 参数 (Phase E.3.1 + E.3.4)
    float          exposure  = 1.0f;
    float          gamma     = 2.2f;
    int            tonemap   = 0;       // Phase E.3.4 — 0=ACES default

    // Phase E.14 — Velocity dilation / format
    // dilation 默认 ON；backend 实际状态在 backend.velocityDilation_，本缓存供 Get 使用
    bool           velocityDilation = true;
    VelocityFormat velocityFormat   = VelocityFormat::RG16F;

    // Phase E.18 — Independent Velocity Dilation Pass RT (与 HDR FBO 同生命周期)
    // 创建条件: backend->SupportsVelocityDilation() && velocityTex 存在 (CreateRT 内判定)
    // 内容:     9-tap max-length 后已 decode 的 RG16F float velocity
    // 消费:     MotionBlurRenderer / SSRRenderer 走单点采样
    uint32_t       dilatedVelocityFbo       = 0;  // combined velocity dilation 输出 fbo
    uint32_t       dilatedVelocityTex       = 0;  // 与 g.fbo velocityTex 同尺寸 (或 half-res) RG16F
    uint32_t       dilatedCameraVelocityFbo = 0;  // camera-only dilation 输出 fbo (与 cameraVelocityTex 同条件)
    uint32_t       dilatedCameraVelocityTex = 0;

    // Phase E.18.1 — dilation pass 半分辨率开关 (默认 false = full-res = Phase E.18 行为)
    //   true: dilatedTex storage = ((W+1)/2, (H+1)/2); VRAM -75%, dilation pass perf +4×
    //   仅在 dilation pass 启用时有意义 (否则 dilatedFbo 未创建, 字段被保存但不生效)
    //   切换时若已 Enable → ReleaseDilationRT + RebuildDilationRT (双 RT 同步)
    bool           dilationHalfRes          = false;

    // Phase E.18.2 — dilation pass 自动跳过单消费者场景开关 (默认 false = Phase E.18.1 行为)
    //   true: EndScene 检测仅 SSR Temporal 启用 + MB 未启用 → 本帧跳过 DrawVelocityDilate
    //         (consumer fallback inline 9-tap, 单消费者 SSR 场景省 1 fetch/px)
    //   受益场景: 仅 SSR Temporal 单消费者;
    //   其他场景 (仅 MB / SSR+MB / 都不启) autoSkip 不会跳过
    bool           dilationAutoSkip         = false;
    // 内部: once-log 状态追踪 (避免每帧 spam, 仅 active↔skip 转变时打一次日志)
    bool           lastDilationActiveLog    = true;

    // Phase F.0.10.2 — Auto-TAA 开关 (默认 true = 零回归; false = 让用户手动 TAA.Process 控时序)
    //   split-screen 场景: 用户需要 TAA.SetActiveInstance + TAA.Process(region) 分次处理, 不能让 EndScene 全屏覆盖
    bool           autoTAA                  = true;
    // Phase F.0.10.3 — Auto-Bloom/SSR/MotionBlur 开关 (默认 true = 零回归; false = 用户手动 .Process(rgn) 控时序)
    //   split-screen 场景: 多 player 各自独立后处理时, 必关这 3 个开关, 用 Lua API 手动 region 处理
    bool           autoBloom                = true;
    bool           autoSSR                  = true;
    bool           autoMotionBlur           = true;
    // Phase F.0.10.6 — Auto-Tonemap 开关 (默认 true = 零回归; false = 用户手动 HDR.Tonemap(rgn))
    //   split-screen 场景 P1 黄昏 vs P2 冷夜: 必关, 用 HDR.Tonemap(rgn, params) 为每 region 独立 tonemap
    bool           autoTonemap              = true;
    // Phase F.0.10.8 — Per-instance grading LUT (每 instance 独立 LUT 应用)
    //   每 player / region 可挂不同 LUT 做差异化调色 (P1 黄昏暖调 / P2 冷夜蓝调)
    //   lutTexId=0 或 lutStrength=0 → shader 跳过 LUT (uniform branch 短路, 性能 0 损)
    uint32_t       lutTexId                 = 0;
    float          lutStrength              = 0.0f;   // clamp [0, 1]
};

// Phase F.0.10.8.3 — WatchEntry: 一条 watched LUT 文件记录
//   path     注册时传入的路径 (后续 Poll 也用此 path, 不做规范化)
//   lastMtime 上次成功 reload (或初次 Watch) 时的 modify_time, nanoseconds since epoch
//   lutId    当前 GL tex id (reload 时更新)
//   isHald   true = HALD image (.png/.jpg/.bmp/.tga); false = .cube
struct WatchEntry {
    std::string path;
    SDL_Time    lastMtime = 0;
    uint32_t    lutId     = 0;
    bool        isHald    = false;
};

// ==================== Phase F.0.10.9 — Multi-instance support ====================
//
// 老 `static State g;` 单例 → `static State g_states[MAX_INSTANCES]; static int g_active = 0;`
// 通过 macro `#define g g_states[g_active]` 让现有 100+ fn 零改动继续访问 active instance
//   - g_states[0] 是 default singleton (老 namespace API 行为完全等价 F.0~F.0.10.8.6)
//   - 新加 5 fn (CreateInstance / DestroyInstance / SetActiveInstance / GetActiveInstance / GetInstanceCount)
//   - 每 instance 独立: backend ptr / enabled / FBO / sceneTex / dilation RT / tonemap params /
//                       autoXXX 5 flags / velocityFormat / per-instance LUT 应用
//   - MAX_INSTANCES=4: default + 3 user (split-screen 4 人足够)
//
// 全 instance 共享 (g_global 单一全局):
//   - lutHotReload     系统级开关 (Poll 由用户主动调, 不区分 instance)
//   - lutReloadCb/User 单一回调 (用户自己 multiplex by path)
//   - lutWatchList     全局 LUT 注册表 (LUT id 全局, 跨 instance 共享, 由 instance 各自 SetGradingLUT 引用)
struct GlobalState {
    bool                    lutHotReload    = true;
    LUTReloadCallback       lutReloadCb     = nullptr;
    void*                   lutReloadCbUser = nullptr;
    std::vector<WatchEntry> lutWatchList;
};

static constexpr int MAX_INSTANCES = 4;
static State g_states[MAX_INSTANCES];
static int   g_active = 0;
static int   g_count  = 1;
static bool  g_slot_in_use[MAX_INSTANCES] = { true, false, false, false };
static GlobalState g_global;

// 现有 100+ fn 内部沿用 `g.X` 写法; macro 透明展开到 active instance
// 注意: 仅在 hdr_renderer.cpp 文件内有效, 不污染外部命名空间
#define g g_states[g_active]

// Phase E.18.1: 内部辅助 — 计算 dilation RT 实际存储尺寸
// halfRes=true 时 ((W+1)/2, (H+1)/2) 向上取整 (与 Phase E.17 motion blur 同模式)
// halfRes=false 时 (W, H) 与 Phase E.18 行为等价
static inline void ComputeDilationStorageSize(int w, int h, int& sw, int& sh) {
    if (g.dilationHalfRes) {
        sw = (w + 1) / 2;
        sh = (h + 1) / 2;
    } else {
        sw = w;
        sh = h;
    }
}

// Phase E.18.1: 内部辅助 — 仅释放 dilation RT 部分 (不动 HDR scene/normal/velocity MRT)
// 供 SetVelocityDilationHalfRes 切换时避免重建整个 HDR FBO
void ReleaseDilationRT() {
    if (!g.backend) return;
    if (g.dilatedVelocityFbo || g.dilatedVelocityTex) {
        g.backend->DeleteVelocityDilateRT(g.dilatedVelocityFbo, g.dilatedVelocityTex);
        g.dilatedVelocityFbo = 0;
        g.dilatedVelocityTex = 0;
    }
    if (g.dilatedCameraVelocityFbo || g.dilatedCameraVelocityTex) {
        g.backend->DeleteVelocityDilateRT(g.dilatedCameraVelocityFbo, g.dilatedCameraVelocityTex);
        g.dilatedCameraVelocityFbo = 0;
        g.dilatedCameraVelocityTex = 0;
    }
    // 通知 backend dilation 已停用 (可能重建后才重新启用)
    g.backend->SetDilationPassActive(false);
}

// Phase E.18.1: 内部辅助 — 重建 dilation RT (仅在 HDR 已 Enable + backend 支持 + raw velocityTex 存在时)
// 取 raw cameraVelocityTex 是否存在决定是否同时创建 dilatedCamRT
void RebuildDilationRT(int w, int h) {
    if (!g.backend || !g.fbo || w <= 0 || h <= 0) return;
    if (!g.backend->SupportsVelocityDilation()) return;
    const uint32_t rawVelocity = g.backend->GetHDRVelocityTex(g.fbo);
    if (!rawVelocity) return;

    int dsw = 0, dsh = 0;
    ComputeDilationStorageSize(w, h, dsw, dsh);

    uint32_t dilatedTex = 0;
    const uint32_t dilatedFbo = g.backend->CreateVelocityDilateRT(w, h, dsw, dsh, &dilatedTex);
    if (dilatedFbo && dilatedTex) {
        g.dilatedVelocityFbo = dilatedFbo;
        g.dilatedVelocityTex = dilatedTex;
        CC::Log(CC::LOG_INFO,
                "HDRRenderer: Phase E.18.1 rebuilt dilated combined velocity RT (storage=%dx%d, halfRes=%s)",
                dsw, dsh, g.dilationHalfRes ? "ON" : "OFF");
    }
    const uint32_t rawCamera = g.backend->GetHDRCameraVelocityTex(g.fbo);
    if (rawCamera) {
        uint32_t dilatedCamTex = 0;
        const uint32_t dilatedCamFbo = g.backend->CreateVelocityDilateRT(w, h, dsw, dsh, &dilatedCamTex);
        if (dilatedCamFbo && dilatedCamTex) {
            g.dilatedCameraVelocityFbo = dilatedCamFbo;
            g.dilatedCameraVelocityTex = dilatedCamTex;
            CC::Log(CC::LOG_INFO,
                    "HDRRenderer: Phase E.18.1 rebuilt dilated camera velocity RT (storage=%dx%d, halfRes=%s)",
                    dsw, dsh, g.dilationHalfRes ? "ON" : "OFF");
        }
    }
}

// 内部辅助: 释放 RT 资源 (不改 exposure/gamma)
void ReleaseRT() {
    if (g.backend) {
        // Phase E.18: 先释放 dilation RT (与 HDR FBO 同生命周期, 在 raw velocityTex 释放前清)
        if (g.dilatedVelocityFbo || g.dilatedVelocityTex) {
            g.backend->DeleteVelocityDilateRT(g.dilatedVelocityFbo, g.dilatedVelocityTex);
        }
        if (g.dilatedCameraVelocityFbo || g.dilatedCameraVelocityTex) {
            g.backend->DeleteVelocityDilateRT(g.dilatedCameraVelocityFbo, g.dilatedCameraVelocityTex);
        }
        // 通知 backend dilation pass 已停用 (consumer 自动回 fallback)
        g.backend->SetDilationPassActive(false);

        if (g.fbo || g.sceneTex) {
            g.backend->DeleteHDRFBO(g.fbo, g.sceneTex);
        }
        // Phase F.1 TAAU: 释放 output sceneTex (与 HDR FBO 同生命周期)
        if (g.outputSceneFbo || g.outputSceneTex) {
            g.backend->DeleteOutputSceneTex(g.outputSceneFbo, g.outputSceneTex);
        }
    }
    g.fbo                     = 0;
    g.sceneTex                = 0;
    g.dilatedVelocityFbo      = 0;
    g.dilatedVelocityTex      = 0;
    g.dilatedCameraVelocityFbo = 0;
    g.dilatedCameraVelocityTex = 0;
    g.width  = 0;
    g.height = 0;
    // Phase F.1 TAAU: 复位双尺寸字段 (Disable 流程下次 Enable 重新走 F.0 单尺寸)
    g.outputSceneFbo = 0;
    g.outputSceneTex = 0;
    g.outputW        = 0;
    g.outputH        = 0;
    g.taauActive     = false;
    if (g.backend) g.backend->ResetVelocityHistory();
}

// 内部辅助: 创建 RT 资源 (失败返回 false 并清理)
// Phase E.8.x: 默认请求 MRT (color + normal). 若 backend 不支持则 silent fallback
// 到 single-RT (normalTex=0). SSAO 模块在 normalTex=0 时自动跳过 Process.
bool CreateRT(int w, int h) {
    if (!g.backend) return false;
    if (w <= 0 || h <= 0) return false;
    uint32_t tex = 0;
    uint32_t normalTex = 0;
    uint32_t velocityTex = 0;
    uint32_t cameraVelocityTex = 0;   // Phase E.16: 第二张 velocity tex (slot 3)
    // Phase E.14: 透传当前 velocity format (默认 RG16F)
    // Phase E.16: 额外请求 cameraVelocityTex 供 MotionBlur mode=1/2 使用
    //             (mode=0 下 backend 仅创建不读、零回归)
    uint32_t fbo = g.backend->CreateHDRFBO(w, h, &tex, &normalTex, &velocityTex,
                                            g.velocityFormat,
                                            &cameraVelocityTex);
    if (!fbo || !tex) {
        if (fbo || tex) g.backend->DeleteHDRFBO(fbo, tex);  // 部分失败兜底
        return false;
    }
    g.fbo = fbo;
    g.sceneTex = tex;
    g.width = w;
    g.height = h;
    // normalTex / velocityTex 由 backend 内部 map 管理; 此处不需 cache.
    g.backend->ResetVelocityHistory();
    // Phase E.14: 同步 dilation 状态到 backend (Init 之后可能被用户 Set 过)
    g.backend->SetVelocityDilation(g.velocityDilation);

    // Phase E.18: 若 backend 支持 dilation pass 且 raw velocityTex 已创建,
    //             同时创建 dilatedVelocityFbo/Tex (combined 始终; camera-only 与 cameraVelocityTex 同条件).
    //             创建失败 silent fallback — EndScene 内 dilation pass 自动 skip,
    //             consumer 走 inline 9-tap 旧路径 (零回归).
    // Phase E.18.1: 透传实际存储尺寸 dsw/dsh (full-res 或 half-res)
    if (g.backend->SupportsVelocityDilation() && velocityTex) {
        int dsw = 0, dsh = 0;
        ComputeDilationStorageSize(w, h, dsw, dsh);

        uint32_t dilatedTex = 0;
        const uint32_t dilatedFbo = g.backend->CreateVelocityDilateRT(w, h, dsw, dsh, &dilatedTex);
        if (dilatedFbo && dilatedTex) {
            g.dilatedVelocityFbo = dilatedFbo;
            g.dilatedVelocityTex = dilatedTex;
            CC::Log(CC::LOG_INFO,
                    "HDRRenderer: Phase E.18 dilated combined velocity RT created (storage=%dx%d, logical=%dx%d, halfRes=%s, fbo=%u, tex=%u)",
                    dsw, dsh, w, h, g.dilationHalfRes ? "ON" : "OFF", dilatedFbo, dilatedTex);
        }
        if (cameraVelocityTex) {
            uint32_t dilatedCamTex = 0;
            const uint32_t dilatedCamFbo = g.backend->CreateVelocityDilateRT(w, h, dsw, dsh, &dilatedCamTex);
            if (dilatedCamFbo && dilatedCamTex) {
                g.dilatedCameraVelocityFbo = dilatedCamFbo;
                g.dilatedCameraVelocityTex = dilatedCamTex;
                CC::Log(CC::LOG_INFO,
                        "HDRRenderer: Phase E.18 dilated camera velocity RT created (storage=%dx%d, logical=%dx%d, halfRes=%s, fbo=%u, tex=%u)",
                        dsw, dsh, w, h, g.dilationHalfRes ? "ON" : "OFF", dilatedCamFbo, dilatedCamTex);
            }
        }
    }
    return true;
}

// Phase F.0.10.9.x.1 — 跨 instance LUT id 引用同步 (防悬挂)
//   多 HDR instance 各自 SetGradingLUT(id) 可能引用同一 lut id;
//   id 释放 (DeleteLUT3D / UnwatchLUT 全量删除路径) 时必须遍历所有 instance
//   清相同引用, 防止其他 instance 的 lutTexId 指向已 free 的 GL tex (悬挂);
//   id 替换 (PollLUTReloads hot reload 路径) 时把所有 oldId 引用 remap 到 newId,
//   让所有 instance 自动看到新 LUT.
//
// 参数语义:
//   oldId  待清 / 替换的旧 GL tex id (0 时直接返, 无意义)
//   newId  0 = 释放路径 (同时清 lutStrength=0); 非 0 = remap (strength 保留)
static void RemapLUTIdAcrossInstances(uint32_t oldId, uint32_t newId) {
    if (oldId == 0u) return;
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        if (g_states[i].lutTexId == oldId) {
            g_states[i].lutTexId = newId;
            if (newId == 0u) g_states[i].lutStrength = 0.0f;
        }
    }
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

    // Phase F.0.10.9: 把 backend ptr / supported / inited 写入所有 MAX_INSTANCES 槽
    //   (每个 instance 共享 backend 能力, 但独立 enabled / RT / 参数)
    const bool supported = backend->SupportsHDR();
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        g_states[i].backend   = backend;
        g_states[i].supported = supported;
        g_states[i].inited    = true;
    }
    // Phase E.14: backend 默认 dilation ON，与 g.velocityDilation 初值一致
    backend->SetVelocityDilation(g.velocityDilation);

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
    // Phase F.0.10.9: 遍历所有 instance 释放 RT + 重置所有 state 字段
    //   (active instance 在循环中临时切换, 最终复位到 0)
    const int saved_active = g_active;
    for (int i = 0; i < MAX_INSTANCES; ++i) {
        g_active = i;
        if (g_states[i].inited) {
            ReleaseRT();        // 作用于当前 g_active 槽的 RT
        }
        g_states[i] = State{};  // 完全清空
    }
    // 复位多实例分配状态: 仅 default 槽存在
    g_active = 0;
    g_count  = 1;
    for (int i = 0; i < MAX_INSTANCES; ++i) g_slot_in_use[i] = (i == 0);
    (void)saved_active;
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
    // Phase F.1 TAAU: outputW/H 是用户传入的目标输出尺寸 (与 width/height 同步当 taauActive=false)
    if (g.enabled && g.outputW == w && g.outputH == h && !g.taauActive) {
        return true;
    }

    // 释放旧 RT (如果有), 创建新尺寸
    ReleaseRT();
    // Phase F.1 TAAU: Enable 始终走 F.0 单尺寸路径; TAAU 由 TAARenderer 后续 SetTAAUEnabled
    //   触发 OnTAAURenderScaleChanged 切换到双尺寸模式 (重建 fbo 为 render-res)
    if (!CreateRT(w, h)) {
        CC::Log(CC::LOG_ERROR, "HDRRenderer::Enable: CreateHDRFBO failed (%dx%d)", w, h);
        g.enabled = false;
        return false;
    }
    g.outputW    = w;        // Phase F.1: 用户期望的输出尺寸
    g.outputH    = h;
    g.taauActive = false;    // Phase F.1: F.0 路径 (TAAU 由 SetTAAUEnabled 切换)

    g.enabled = true;
    g.paused  = false;
    CC::Log(CC::LOG_INFO, "HDRRenderer::Enable: HDR RT created (%dx%d, fbo=%u, tex=%u)",
            w, h, g.fbo, g.sceneTex);

    // Phase E.4.2 — HDR 已启用, 通知 Bloom 模块 (autoEnable=true 时自动拉起)
    BloomRenderer::OnHDREnabled(w, h);
    // Phase E.5.2 — 同时通知 AE 模块 (autoEnable=false 时 no-op; 默认 manual exposure)
    AutoExposureRenderer::OnHDREnabled(w, h);
    // Phase E.6.2 — 通知 LensDirt + Streak (autoEnable=false 时 no-op)
    LensDirtRenderer::OnHDREnabled(w, h);
    StreakRenderer::OnHDREnabled(w, h);
    // Phase E.7.2 — 通知 LensFlare (autoEnable=false 时 no-op)
    LensFlareRenderer::OnHDREnabled(w, h);
    // Phase E.8.2 — 通知 SSAO (autoEnable=false 时 no-op)
    SSAORenderer::OnHDREnabled(w, h);
    // Phase E.9 — 通知 SSR (autoEnable=false 时 no-op)
    SSRRenderer::OnHDREnabled(w, h);
    // Phase E.15 — 通知 Motion Blur (autoEnable=false 时 no-op)
    MotionBlurRenderer::OnHDREnabled(w, h);
    // Phase F.0 — 通知 TAA (autoEnable=false 时 no-op, 用户主动 Enable)
    TAARenderer::OnHDREnabled(w, h);
    return true;
}

void Disable() {
    if (!g.enabled) return;
    // Phase F.0 — TAA 依赖 HDR sceneTex + velocity + dilation, 管线最末端 → 最先关闭
    TAARenderer::OnHDRDisabled();
    // Phase E.15 — Motion Blur 依赖 HDR sceneTex + velocityTex, 最先关闭 (管线末端)
    MotionBlurRenderer::OnHDRDisabled();
    // Phase E.9 — SSR 依赖 HDR RT depth + normal (blit 源), 必须在 HDR RT 销毁前先释放 (在 SSAO 之前, 与容释放顺序无冲突)
    SSRRenderer::OnHDRDisabled();
    // Phase E.8.2 — SSAO 依赖 HDR RT depth (blit 源), 必须在 HDR RT 销毁前先释放
    SSAORenderer::OnHDRDisabled();
    // Phase E.7.2 — 管线末端模块最先关 (LensFlare 依赖 Bloom + HDR RT)
    LensFlareRenderer::OnHDRDisabled();
    // Phase E.6.2 — 先关 LensFx 模块 (依赖 HDR RT + Bloom 的上层; 安全先关)
    StreakRenderer::OnHDRDisabled();
    LensDirtRenderer::OnHDRDisabled();
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
    // Phase F.1 TAAU: 比较应基于 outputW/H (用户的目标尺寸), 不是 width/height (TAAU 模式下是 render-res)
    if (g.outputW == w && g.outputH == h) {
        return true;  // 尺寸相同, no-op
    }
    bool ok = Enable(w, h);  // Enable 内部会 ReleaseRT + CreateRT + OnHDREnabled
    // 注: Enable 已调过 OnHDREnabled, 该回调等价于 Bloom/AE/LensFx Resize; 以下 OnHDRResized 重复调
    //     有 no-op 保护 (Resize 内部对于已同尺寸直接 return true)
    if (ok) {
        BloomRenderer::OnHDRResized(w, h);
        AutoExposureRenderer::OnHDRResized(w, h);   // Phase E.5.2
        LensDirtRenderer::OnHDRResized(w, h);       // Phase E.6.2 (no-op, 无 RT)
        StreakRenderer::OnHDRResized(w, h);         // Phase E.6.2
        LensFlareRenderer::OnHDRResized(w, h);      // Phase E.7.2
        SSAORenderer::OnHDRResized(w, h);            // Phase E.8.2 — SSAO depth/AO RT 同步尺寸
        SSRRenderer::OnHDRResized(w, h);             // Phase E.9 — SSR depth/reflect RT 同步尺寸
        MotionBlurRenderer::OnHDRResized(w, h);      // Phase E.15 — motion blur RT 同步尺寸
        TAARenderer::OnHDRResized(w, h);             // Phase F.0 — TAA history RT 同步尺寸
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
    // Phase F.0.10.3: autoBloom=false 时跳过自动 Bloom, 让用户手动 Bloom.Process(rgn) (split-screen 必备)
    if (g.autoBloom) {
        BloomRenderer::Process(g.fbo, g.sceneTex);
    }

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

    // Phase E.6.2 — Lens Dirt (内部自检 IsEnabled; 未启用 no-op)
    // 需 Bloom 启用 (GetPyramidTopTex 返 0 时 no-op, LensDirt 本身再防御)
    LensDirtRenderer::Process(g.fbo,
                               BloomRenderer::GetPyramidTopTex(),
                               g.width, g.height);

    // Phase E.6.2 — Streak (内部自检 IsEnabled; 未启用 no-op)
    StreakRenderer::Process(g.fbo, g.sceneTex);

    // Phase E.8.2 — SSAO (“阴调”, 必须在 Bloom 之前, 否则被 bloom 提亮抹平)
    // 但为避免在该点插入时打乱现有顺序, 本 phase 选择插在 LensFlare 之后:
    // SSAO 主要作用在几何体阴调, 与机 Bloom/AE 有轻微交互但可接受
    SSAORenderer::Process(g.fbo, g.sceneTex);

    // Phase E.18 — Independent Velocity Dilation Pass (在 SSR / MotionBlur 之前)
    //   条件: velocityDilation 开启 && backend 支持 && dilated RT 创建成功
    //   作用: 对 raw velocityTex/cameraVelocityTex 做一次 9-tap max-length,
    //         后续 SSR Temporal / Motion Blur shader 走单点采样 (避免重复 9-tap)
    //   失败兜底: dilation RT 不存在或 raw velocityTex=0 → dilationActive=false,
    //             consumer fallback inline 9-tap (零回归)
    // Phase E.18.1: 预先计算 dilation storage 尺寸 (full-res 或 half-res), 给 DrawVelocityDilate
    bool dilationActive = false;
    if (g.velocityDilation && g.backend->SupportsVelocityDilation()) {
        // Phase E.18.2: autoSkip 模式下检测单消费者场景, 仅在 "仅 SSR Temporal + 无 MB" 时跳过
        //   SSR Temporal: 10 fetch (dilation) > 9 fetch (inline) → 跳过省 1 fetch ?
        //   MB only(N=8): 17 fetch (dilation) < 72 fetch (inline) → 不跳过
        //   SSR + MB:     18 fetch (dilation) < 81 fetch (inline) → 不跳过
        bool shouldRun = true;
        if (g.dilationAutoSkip) {
            const bool ssrTemporal = SSRRenderer::IsEnabled() && SSRRenderer::GetTemporalEnabled();
            const bool mbEnabled   = MotionBlurRenderer::IsEnabled();
            const bool ssrOnly     = ssrTemporal && !mbEnabled;
            shouldRun = !ssrOnly;
            // once-log: 仅在 active↔skip 状态转变时打一次
            if (g.lastDilationActiveLog && !shouldRun) {
                CC::Log(CC::LOG_INFO,
                        "HDRRenderer: Phase E.18.2 dilation pass auto-skipped (SSR-only, consumer fallback inline 9-tap)");
                g.lastDilationActiveLog = false;
            } else if (!g.lastDilationActiveLog && shouldRun) {
                CC::Log(CC::LOG_INFO,
                        "HDRRenderer: Phase E.18.2 dilation pass active (multi-consumer or non-SSR-only)");
                g.lastDilationActiveLog = true;
            }
        }

        if (shouldRun) {
            int dsw = 0, dsh = 0;
            ComputeDilationStorageSize(g.width, g.height, dsw, dsh);

            const uint32_t rawCombined = g.backend->GetHDRVelocityTex(g.fbo);
            if (rawCombined && g.dilatedVelocityFbo) {
                g.backend->DrawVelocityDilate(rawCombined, g.dilatedVelocityFbo, dsw, dsh);
                dilationActive = true;
            }
            // camera-only dilation 与 cameraVelocityTex 同条件 (MotionBlur mode=1/2 才用)
            const uint32_t rawCamera = g.backend->GetHDRCameraVelocityTex(g.fbo);
            if (rawCamera && g.dilatedCameraVelocityFbo) {
                g.backend->DrawVelocityDilate(rawCamera, g.dilatedCameraVelocityFbo, dsw, dsh);
                // dilationActive 已在 combined 时置 true; camera-only 单独失败不影响
            }
        }
    }
    // 通知 backend: dilation pass 当前帧是否激活 → 影响 SSRTemporal/MotionBlur uVelocityDilation 上传
    g.backend->SetDilationPassActive(dilationActive);
    // dilation pass 改了 FBO 绑定, 复位回 default fb (后续模块自己 bind 各自 fbo)
    g.backend->UnbindFBO();

    // Phase E.9 — SSR (屏幕空间反射, 加性写入 HDR; 在 SSAO 之后、Bloom 之前)
    // SSR 反射需要看到 SSAO 修正后的 HDR 调 (阴部在反射中仍为暗); Bloom 取反射 + AO HDR 提亮.
    // 内部自检 IsEnabled/IsSupported; 未启用时 no-op. 缺 G-buffer normal 时 silent skip + once warn.
    // Phase F.0.10.3: autoSSR=false 时跳过自动 SSR, 让用户手动 SSR.Process(rgn) (split-screen 必备)
    if (g.autoSSR) {
        SSRRenderer::Process(g.fbo, g.sceneTex);
    }

    // Phase E.7.2 — Lens Flare (内部自检 IsEnabled; 未启用 no-op)
    // 复用 Bloom bright/composite shader, 独立 ping-pong RT
    LensFlareRenderer::Process(g.fbo, g.sceneTex);

    // Phase E.15 — Motion Blur (LensFlare 之后, Tonemap 之前)
    // 读 sceneTex + velocityTex 写 ping-pong, 再 blit 覆盖回 sceneTex
    // Phase F.0.10.3: autoMotionBlur=false 时跳过自动 MotionBlur, 让用户手动 MB.Process(rgn) (split-screen 必备)
    if (g.autoMotionBlur) {
        MotionBlurRenderer::Process(g.fbo, g.sceneTex);
    }

    // Phase F.0 — TAA 主管线 (MotionBlur 之后, Tonemap 之前)
    //   读 cur sceneTex + history + dilated/raw velocity → 写 history递推、1  blit 回 sceneTex
    //   仅在用户 TAA::Enable(w,h) 主动启动 + supported 时才走 (默认 OFF, 零回归)
    // Phase F.0.10.2: autoTAA=false 时跳过自动 TAA, 让用户手动 TAA.Process / ProcessRegion 控时序 (split-screen 必备)
    if (g.autoTAA) {
        TAARenderer::Process(g.fbo, g.sceneTex);
    }

    // Tonemap exposure: AE 开时覆盖 manual; AE 关时回归 manual SetExposure
    float exposure = AutoExposureRenderer::IsEnabled()
                        ? AutoExposureRenderer::GetCurrentExposure()
                        : g.exposure;

    // Tonemap + sRGB encode → default fb (E.3.4 多 operator; 输入已含 bloom + lensDirt + streak + lensFlare 的 HDR RT)
    // Phase F.0.10.6: autoTonemap=false 时跳过自动 tonemap, 让用户手动 HDR.Tonemap(rgn) 控 split-screen
    // Phase F.0.10.8: 透传全局 LUT (lutTexId=0 或 strength=0 → shader 短路, 零回归)
    // Phase F.1 TAAU: GetSceneTexForOutput() 在 taauActive 时返 outputSceneTex (output-res), 否则返 sceneTex
    if (g.autoTonemap) {
        const uint32_t srcTex = GetSceneTexForOutput();
        g.backend->DrawTonemapFullscreen(srcTex, exposure, g.gamma, g.tonemap,
                                          g.lutTexId, g.lutStrength);
    }
    g.backend->CommitVelocityHistory();
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
uint32_t GetFBO()          { return g.fbo; }    // Phase E.8.x — SSAO 拿 normal tex 用
uint32_t GetVelocityTexture() {
    return (g.backend && g.fbo) ? g.backend->GetHDRVelocityTex(g.fbo) : 0;
}

/// Phase E.16 — 查询 HDR FBO 关联 camera-only velocity tex
uint32_t GetCameraVelocityTexture() {
    return (g.backend && g.fbo) ? g.backend->GetHDRCameraVelocityTex(g.fbo) : 0;
}

/// Phase E.18 — 查询 dilated combined velocity tex
/// 仅在 dilation pass 当前帧实际执行 (backend->GetDilationPassActive()=true) 时返非 0
/// 消费者据此决定绑 dilatedTex 还是 fallback raw velocityTex
uint32_t GetDilatedVelocityTexture() {
    if (!g.backend || !g.fbo) return 0;
    return g.backend->GetDilationPassActive() ? g.dilatedVelocityTex : 0;
}

/// Phase E.18 — 查询 dilated camera-only velocity tex (同上)
uint32_t GetDilatedCameraVelocityTexture() {
    if (!g.backend || !g.fbo) return 0;
    return g.backend->GetDilationPassActive() ? g.dilatedCameraVelocityTex : 0;
}

int      GetWidth()        { return g.width; }
int      GetHeight()       { return g.height; }

// ==================== Phase F.1 TAAU — 渲染分辨率与输出分辨率解耦 ====================

uint32_t GetSceneTexForOutput() {
    // taauActive 时 sharpen/tonemap 必须读 outputSceneTex (output-res, TAAU 输出已 blit 到此);
    // 否则 (F.0 路径) 读 sceneTex (output-res, F.0 行为零回归)
    return (g.taauActive && g.outputSceneTex) ? g.outputSceneTex : g.sceneTex;
}

uint32_t GetSceneFboForOutput() {
    // 与 GetSceneTexForOutput 配对: TAAU 模式返 outputSceneFbo (sharpen/TAA 写入此 FBO),
    // F.0 模式返 fbo (HDR FBO, sceneTex 是其 color attachment 0)
    return (g.taauActive && g.outputSceneFbo) ? g.outputSceneFbo : g.fbo;
}

bool OnTAAURenderScaleChanged(int renderW, int renderH, int outputW, int outputH) {
    if (!g.enabled || !g.backend) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::OnTAAURenderScaleChanged: HDR 未启用, 忽略 (renderW=%d outputW=%d)",
                renderW, outputW);
        return false;
    }
    if (renderW <= 0 || renderH <= 0 || outputW <= 0 || outputH <= 0) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::OnTAAURenderScaleChanged: 非法尺寸 (render=%dx%d, output=%dx%d)",
                renderW, renderH, outputW, outputH);
        return false;
    }

    // 已是同尺寸 + 同 active 状态 → no-op
    if (g.taauActive && g.width == renderW && g.height == renderH
                     && g.outputW == outputW && g.outputH == outputH) {
        return true;
    }

    // 释放旧 RT (含 outputSceneTex), 重建为 render-res
    ReleaseRT();
    if (!CreateRT(renderW, renderH)) {
        CC::Log(CC::LOG_ERROR,
                "HDRRenderer::OnTAAURenderScaleChanged: CreateHDRFBO(render=%dx%d) 失败",
                renderW, renderH);
        // 自动回退 F.0 路径: 重建为 output-res
        CreateRT(outputW, outputH);
        g.outputW = outputW;
        g.outputH = outputH;
        g.taauActive = false;
        return false;
    }

    // 分配 outputSceneTex (output-res)
    if (!g.backend->CreateOutputSceneTex(outputW, outputH,
                                          &g.outputSceneFbo, &g.outputSceneTex)) {
        CC::Log(CC::LOG_ERROR,
                "HDRRenderer::OnTAAURenderScaleChanged: CreateOutputSceneTex(%dx%d) 失败, 回退 F.0",
                outputW, outputH);
        ReleaseRT();
        CreateRT(outputW, outputH);
        g.outputW = outputW;
        g.outputH = outputH;
        g.taauActive = false;
        return false;
    }

    g.outputW    = outputW;
    g.outputH    = outputH;
    g.taauActive = true;

    // Phase F.2.0 — TAAU 切换通知下游后处理重建到 render-res
    // DESIGN F.1 §2.1: "Bloom/SSAO/SSR/MotionBlur 全部 @ render-res"
    // 缺此通知, 下游 RT 仍是 outputRes, 与 sceneTex(renderRes) 比例错位 + 性能浪费.
    // TAA 不在此处通知: history RT 必须保持 outputRes; applyTAAUChange_ 已清 hasHistory.
    BloomRenderer::OnHDRResized(renderW, renderH);
    AutoExposureRenderer::OnHDRResized(renderW, renderH);
    LensDirtRenderer::OnHDRResized(renderW, renderH);   // no-op (无 RT) 仍调以保接口一致
    StreakRenderer::OnHDRResized(renderW, renderH);
    LensFlareRenderer::OnHDRResized(renderW, renderH);
    SSAORenderer::OnHDRResized(renderW, renderH);
    SSRRenderer::OnHDRResized(renderW, renderH);
    MotionBlurRenderer::OnHDRResized(renderW, renderH);

    CC::Log(CC::LOG_INFO,
            "HDRRenderer::OnTAAURenderScaleChanged: TAAU 启用 render=%dx%d output=%dx%d (sceneTex=%u outputSceneTex=%u)",
            renderW, renderH, outputW, outputH, g.sceneTex, g.outputSceneTex);
    return true;
}

void OnTAAUDisabled() {
    if (!g.enabled || !g.backend) return;
    if (!g.taauActive) return;   // 已是 F.0 路径, no-op

    const int outputW = g.outputW;
    const int outputH = g.outputH;
    ReleaseRT();
    if (!CreateRT(outputW, outputH)) {
        CC::Log(CC::LOG_ERROR,
                "HDRRenderer::OnTAAUDisabled: CreateHDRFBO(output=%dx%d) 失败",
                outputW, outputH);
        g.enabled = false;
        return;
    }
    g.outputW    = outputW;
    g.outputH    = outputH;
    g.taauActive = false;

    // Phase F.2.0 — 切回 F.0 时同步下游 RT 回到 output-res
    BloomRenderer::OnHDRResized(outputW, outputH);
    AutoExposureRenderer::OnHDRResized(outputW, outputH);
    LensDirtRenderer::OnHDRResized(outputW, outputH);
    StreakRenderer::OnHDRResized(outputW, outputH);
    LensFlareRenderer::OnHDRResized(outputW, outputH);
    SSAORenderer::OnHDRResized(outputW, outputH);
    SSRRenderer::OnHDRResized(outputW, outputH);
    MotionBlurRenderer::OnHDRResized(outputW, outputH);

    CC::Log(CC::LOG_INFO,
            "HDRRenderer::OnTAAUDisabled: 切回 F.0 单尺寸 (%dx%d, sceneTex=%u)",
            outputW, outputH, g.sceneTex);
}

// ==================== Phase E.14 — Velocity dilation / format 切换 ====================

bool SetVelocityDilation(bool on) {
    g.velocityDilation = on;
    // backend 未初始化时仅更新 state，下次 Init 后 Enable 时会同步
    if (!g.backend) return false;
    g.backend->SetVelocityDilation(on);
    return true;
}

// Phase E.18.1 — dilation pass 半分辨率开关
// no-op 短路: 同值不重建
// 已 Enable: 立即 ReleaseDilationRT + RebuildDilationRT (双 RT 同步重建)
// 未 Enable: 仅更新 state、下次 Enable 时 CreateRT 走新 sw/sh
bool SetVelocityDilationHalfRes(bool on) {
    if (g.dilationHalfRes == on) return true;   // no-op (同值)
    g.dilationHalfRes = on;
    // 仅在已 Enable 且 backend 支持 dilation pass 时才需重建
    if (g.inited && g.backend && g.fbo && g.width > 0 && g.height > 0) {
        ReleaseDilationRT();
        RebuildDilationRT(g.width, g.height);
    }
    return true;
}
bool GetVelocityDilationHalfRes() { return g.dilationHalfRes; }

// Phase E.18.2 — dilation pass 自动跳过单消费者场景开关
// no-op 短路: 同值不重置
// 仅修改 state, 不重建 RT (decision 在 EndScene 每帧重新判)
// 切换时重置 once-log 状态，让下次状态转变能出日志
bool SetVelocityDilationAutoSkip(bool on) {
    if (g.dilationAutoSkip == on) return true;   // no-op (同值)
    g.dilationAutoSkip = on;
    g.lastDilationActiveLog = true;              // 重置为“active”, 下次转 skip 才会出日志
    return true;
}
bool GetVelocityDilationAutoSkip() { return g.dilationAutoSkip; }

// Phase F.0.10.2 — Auto-TAA 开关
// 默认 true = EndScene 内自动调 TAARenderer::Process(g.fbo, g.sceneTex) (零回归)
// 设 false 后用户负责手动调 TAA.Process / TAA.ProcessRegion 控制 TAA 时序 (split-screen 必备)
// 无状态依赖, 立即生效, 同值 no-op
bool SetAutoTAA(bool on) {
    if (g.autoTAA == on) return true;   // no-op (同值)
    g.autoTAA = on;
    return true;
}
bool GetAutoTAA() { return g.autoTAA; }

// Phase F.0.10.3 — Auto-Bloom/SSR/MotionBlur 开关 (与 SetAutoTAA 同模式)
// 默认 true = EndScene 内自动调对应 Renderer::Process(g.fbo, g.sceneTex) (零回归)
// 设 false 后用户负责手动调 .Process / Process(rgn) 控时序 (split-screen 必备)
// 无状态依赖, 立即生效, 同值 no-op
bool SetAutoBloom(bool on) {
    if (g.autoBloom == on) return true;
    g.autoBloom = on;
    return true;
}
bool GetAutoBloom() { return g.autoBloom; }

bool SetAutoSSR(bool on) {
    if (g.autoSSR == on) return true;
    g.autoSSR = on;
    return true;
}
bool GetAutoSSR() { return g.autoSSR; }

bool SetAutoMotionBlur(bool on) {
    if (g.autoMotionBlur == on) return true;
    g.autoMotionBlur = on;
    return true;
}
bool GetAutoMotionBlur() { return g.autoMotionBlur; }

// ==================== Phase F.0.10.6 — Auto-Tonemap + per-region Tonemap ====================

bool SetAutoTonemap(bool on) {
    if (g.autoTonemap == on) return true;   // no-op (同值)
    g.autoTonemap = on;
    return true;
}
bool GetAutoTonemap() { return g.autoTonemap; }

// Region 限定 tonemap pass — 复用全局 g.exposure (含 AE 叠加) / g.gamma / g.tonemap / LUT
// HDR 未启用 / sceneTex 无效时 silent skip
// Phase F.0.10.7 fix: 必须先 UnbindFBO 切到 default fb (因为调用方典型在 EndScene 之前,
//                    HDR fbo 仍绑着; 不 unbind 会把 tonemap 写回 HDR RT, 黑屏)
// Phase F.0.10.8: 透传全局 g.lutTexId / g.lutStrength
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH) {
    if (!g.enabled || !g.backend || !g.sceneTex) return;
    g.backend->UnbindFBO();   // F.0.10.7 fix: 切到 default fb
    // AE 叠加 (与 EndScene 同逻辑): AE 开时 AE current 覆盖 manual; AE 关时回归 g.exposure
    float exposure = AutoExposureRenderer::IsEnabled()
                        ? AutoExposureRenderer::GetCurrentExposure()
                        : g.exposure;
    // Phase F.1 TAAU: taauActive 时读 outputSceneTex (output-res); 否则读 sceneTex (F.0 行为)
    const uint32_t srcTex = GetSceneTexForOutput();
    g.backend->DrawTonemapRegion(srcTex, exposure, g.gamma, g.tonemap,
                                  rgnX, rgnY, rgnW, rgnH,
                                  g.lutTexId, g.lutStrength);
}

// Region 限定 tonemap pass (params 显式版) — 完全自定义, 不叠加 AE, 沿用全局 LUT
// 适合: split-screen 中每 region 独立 exposure (不希望 AE 干扰), LUT 共享
// Phase F.0.10.7 fix: 同上, 必须先 UnbindFBO 切到 default fb
// Phase F.0.10.8: 透传全局 g.lutTexId / g.lutStrength (兼容老 caller; 新 caller 用 6 参重载完全覆盖)
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH,
              float exposure, float gamma, int tonemapMode) {
    if (!g.enabled || !g.backend || !g.sceneTex) return;
    g.backend->UnbindFBO();   // F.0.10.7 fix: 切到 default fb
    // 防御性 clamp gamma (与 SetGamma 一致, 防 0/负数 崩溃)
    if (gamma < 0.0001f) gamma = 0.0001f;
    // Phase F.1 TAAU: 同上, taauActive 时读 outputSceneTex
    const uint32_t srcTex = GetSceneTexForOutput();
    g.backend->DrawTonemapRegion(srcTex, exposure, gamma, tonemapMode,
                                  rgnX, rgnY, rgnW, rgnH,
                                  g.lutTexId, g.lutStrength);
}

// Phase F.0.10.8 — Region 限定 tonemap pass (完全显式版: params + LUT 全自定义)
// 适合: split-screen 中每 region 独立 exposure + 独立 LUT (P1 黄昏暖调 LUT, P2 冷夜蓝调 LUT)
// lutTex=0 或 lutStrength<=0 → shader 跳过 LUT (uniform branch 短路)
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH,
              float exposure, float gamma, int tonemapMode,
              uint32_t lutTex, float lutStrength) {
    if (!g.enabled || !g.backend || !g.sceneTex) return;
    g.backend->UnbindFBO();   // F.0.10.7 fix: 切到 default fb
    if (gamma < 0.0001f) gamma = 0.0001f;
    // strength clamp [0, 1] (防数值漂移)
    if (lutStrength < 0.0f) lutStrength = 0.0f;
    else if (lutStrength > 1.0f) lutStrength = 1.0f;
    // Phase F.1 TAAU: 同上, taauActive 时读 outputSceneTex
    const uint32_t srcTex = GetSceneTexForOutput();
    g.backend->DrawTonemapRegion(srcTex, exposure, gamma, tonemapMode,
                                  rgnX, rgnY, rgnW, rgnH,
                                  lutTex, lutStrength);
}

// ==================== Phase F.0.10.8 — 3D LUT (Color Grading) ====================

uint32_t CreateLUT3D(int size, const uint8_t* data, size_t dataLen) {
    if (!g.backend) return 0u;          // backend 未初始化
    if (size < 4 || size > 64) return 0u;
    if (!data) return 0u;
    // 校验 data 长度 = size^3 * 3 bytes RGB
    const size_t expected = (size_t)size * (size_t)size * (size_t)size * 3u;
    if (dataLen != expected) return 0u;
    return g.backend->CreateLUT3D(size, data);
}

bool DeleteLUT3D(uint32_t lutTex) {
    if (!lutTex) return false;   // 0 id 防御
    // Phase F.0.10.9.x.1: state 清理 (跨 instance lutTexId 引用) 与 backend 无关, 必须先跑
    //   - 老实现 backend 检查在前导致 headless / 未 Init 时 Remap 不执行
    //   - 新顺序: 先 state cleanup (零依赖), 再 GL Delete (依赖 backend)
    RemapLUTIdAcrossInstances(lutTex, 0u);
    if (!g.backend) return false;
    return g.backend->DeleteLUT3D(lutTex);
}

// Phase F.0.10.8.6 — 探测 backend 是否支持 HDR LUT (透传)
bool SupportsHDRLUT() {
    if (!g.backend) return false;            // 未初始化
    return g.backend->SupportsLUT3DFloat();
}

bool SetGradingLUT(uint32_t lutTex, float strength) {
    if (strength < 0.0f) strength = 0.0f;
    else if (strength > 1.0f) strength = 1.0f;
    g.lutTexId    = lutTex;
    g.lutStrength = strength;
    return true;
}

uint32_t GetGradingLUTId()       { return g.lutTexId; }
float    GetGradingLUTStrength() { return g.lutStrength; }

// ==================== Phase F.0.10.8.1 — .cube LUT 文件解析 ====================

namespace {

// 安全的 outErr 写入 (caller 可能传 nullptr / errCap=0)
inline void writeErr_(char* outErr, size_t errCap, const char* fmt, ...) {
    if (!outErr || errCap == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(outErr, errCap, fmt, ap);
    va_end(ap);
}

// 关键字精确匹配 (避免 substring 误匹: "LUT_3D_SIZE" 不能匹 "LUT_3D_SIZE2")
// tok 必须是 lineEnd 内 trim 后的起始指针
// 匹配后必须紧跟 whitespace / 行尾 / EOF
inline bool matchKeyword_(const char* tok, const char* lineEnd, const char* kw) {
    const size_t kwLen = std::strlen(kw);
    if ((size_t)(lineEnd - tok) < kwLen) return false;
    if (std::strncmp(tok, kw, kwLen) != 0) return false;
    if (tok + kwLen >= lineEnd) return true;       // EOF / 行尾
    const char nextCh = tok[kwLen];
    return (nextCh == ' ' || nextCh == '\t');
}

// float [0,1] → byte [0,255] (含 clamp + 四舍五入)
inline uint8_t quantize_(float f) {
    if (f < 0.0f) return 0;
    if (f > 1.0f) return 255;
    return (uint8_t)(f * 255.0f + 0.5f);
}

}  // anonymous namespace

// Phase G.1 — Parser-only (无 GL 依赖), 供 worker thread 后台调用
// 与 LoadCubeLUTFromString 共享 parser 内核, 输出 size/isHDR/bytes/floats
bool ParseCubeLUTFromString(const char* text, size_t textLen,
                             int* outSize, bool* outIsHDR,
                             std::vector<uint8_t>* outBytes,
                             std::vector<float>* outFloats,
                             char* outErr, size_t errCap) {
    // 边界: 空指针 / 空文本立即报错
    if (!text || textLen == 0) {
        writeErr_(outErr, errCap, "ParseCubeLUTFromString: empty input");
        return false;
    }
    if (!outSize || !outIsHDR || !outBytes || !outFloats) {
        writeErr_(outErr, errCap, "ParseCubeLUTFromString: out params null");
        return false;
    }

    int  size       = 0;
    bool seenSize3D = false;
    int  lineNo     = 0;
    int  dataRow    = 0;
    std::vector<uint8_t>& bytes  = *outBytes;     // 写到 caller 缓冲
    std::vector<float>&   floats = *outFloats;
    bytes.clear();
    floats.clear();
    int  expectedRows = 0;          // size^3, 见到 LUT_3D_SIZE 后填

    // Phase F.0.10.8.5 — DOMAIN_MIN / DOMAIN_MAX (默认 [0,1] LDR)
    float domainMin[3] = {0.0f, 0.0f, 0.0f};
    float domainMax[3] = {1.0f, 1.0f, 1.0f};

    const char* p   = text;
    const char* end = text + textLen;

    while (p < end) {
        ++lineNo;

        // 1. 找行尾 (LF / CRLF / EOF)
        const char* lineStart = p;
        const char* lineEnd   = p;
        while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') ++lineEnd;

        // 2. 行首 trim whitespace (空格 + tab)
        const char* tok = lineStart;
        while (tok < lineEnd && (*tok == ' ' || *tok == '\t')) ++tok;

        // 3. skip 空行 / 注释 (行首 #)
        if (tok >= lineEnd || *tok == '#') goto next_line;

        // 4. 关键字识别 (按出现频率排序)
        if (matchKeyword_(tok, lineEnd, "LUT_3D_SIZE")) {
            tok += std::strlen("LUT_3D_SIZE");
            // strtol 自动 skip 前置 whitespace
            char* ep = nullptr;
            // 注意: strtol 需 null-terminated, 我们传 tok 但其后续是 lineEnd 后的字符
            // 因为 lineEnd 之外是 \n/\r 或 EOF 字节, strtol 会停在数字结束位置, 不会越界读
            long sz = std::strtol(tok, &ep, 10);
            if (ep == tok || sz < 4 || sz > 64) {
                writeErr_(outErr, errCap,
                          "line %d: LUT_3D_SIZE %ld out of range [4, 64]",
                          lineNo, sz);
                return false;
            }
            size          = (int)sz;
            seenSize3D    = true;
            expectedRows  = size * size * size;
            // 预分配 (size 64 时: bytes ~768KB, floats ~3MB; 一次到位避免重分配)
            bytes.reserve((size_t)expectedRows * 3u);
            floats.reserve((size_t)expectedRows * 3u);   // Phase F.0.10.8.5
            goto next_line;
        }
        if (matchKeyword_(tok, lineEnd, "LUT_1D_SIZE")) {
            writeErr_(outErr, errCap,
                      "line %d: 1D LUT not supported (use LUT_3D_SIZE)", lineNo);
            return false;
        }
        if (matchKeyword_(tok, lineEnd, "TITLE")) {
            goto next_line;   // TITLE 不存
        }
        // Phase F.0.10.8.5 — 解析 DOMAIN_MIN / DOMAIN_MAX 三 float (R G B)
        // 用于检测 HDR LUT (DOMAIN_MAX 任一分量 > 1.0 → 走 CreateLUT3DFloat)
        {
            const bool isMin = matchKeyword_(tok, lineEnd, "DOMAIN_MIN");
            const bool isMax = matchKeyword_(tok, lineEnd, "DOMAIN_MAX");
            if (isMin || isMax) {
                tok += std::strlen(isMin ? "DOMAIN_MIN" : "DOMAIN_MAX");
                float v[3] = {0.0f, 0.0f, 0.0f};
                for (int i = 0; i < 3; ++i) {
                    char* ep = nullptr;
                    const float f = std::strtof(tok, &ep);
                    if (ep == tok || ep > lineEnd) {
                        writeErr_(outErr, errCap,
                                  "line %d: %s expected 3 floats, parse failed at component %d",
                                  lineNo, isMin ? "DOMAIN_MIN" : "DOMAIN_MAX", i);
                        return false;
                    }
                    v[i] = f;
                    tok = ep;
                }
                if (isMin) {
                    domainMin[0] = v[0]; domainMin[1] = v[1]; domainMin[2] = v[2];
                } else {
                    domainMax[0] = v[0]; domainMax[1] = v[1]; domainMax[2] = v[2];
                }
                goto next_line;
            }
        }

        // 5. 数据行: 必须先见过 LUT_3D_SIZE
        if (!seenSize3D) {
            writeErr_(outErr, errCap,
                      "line %d: data row before LUT_3D_SIZE directive", lineNo);
            return false;
        }

        // 6. 解析 3 个 float (R G B)
        {
            char* ep = nullptr;
            float r = std::strtof(tok, &ep);
            if (ep == tok || ep > lineEnd) {
                writeErr_(outErr, errCap,
                          "line %d: expected 3 floats for data row, parse R failed", lineNo);
                return false;
            }
            tok = ep;
            float g_ = std::strtof(tok, &ep);
            if (ep == tok || ep > lineEnd) {
                writeErr_(outErr, errCap,
                          "line %d: expected 3 floats for data row, parse G failed", lineNo);
                return false;
            }
            tok = ep;
            float b = std::strtof(tok, &ep);
            if (ep == tok || ep > lineEnd) {
                writeErr_(outErr, errCap,
                          "line %d: expected 3 floats for data row, parse B failed", lineNo);
                return false;
            }
            // 防御: 数据行超过预期数量 (避免 vector OOM)
            if (dataRow >= expectedRows) {
                writeErr_(outErr, errCap,
                          "line %d: data row count exceeds size^3=%d", lineNo, expectedRows);
                return false;
            }
            bytes.push_back(quantize_(r));    // LDR 路径
            bytes.push_back(quantize_(g_));
            bytes.push_back(quantize_(b));
            floats.push_back(r);              // Phase F.0.10.8.5 — HDR 路径原始值
            floats.push_back(g_);
            floats.push_back(b);
            ++dataRow;
        }

      next_line:
        p = lineEnd;
        // 跨过 CRLF / LF / CR (Windows / Unix / 老 Mac 兼容)
        if (p < end && *p == '\r') ++p;
        if (p < end && *p == '\n') ++p;
    }

    // 7. 必须见过 LUT_3D_SIZE
    if (!seenSize3D) {
        writeErr_(outErr, errCap, "missing LUT_3D_SIZE directive");
        return false;
    }

    // 8. 数据行数必须匹配
    if (dataRow != expectedRows) {
        writeErr_(outErr, errCap,
                  "data row count %d mismatch (expected %d for size %d)",
                  dataRow, expectedRows, size);
        return false;
    }

    // 9. Phase F.0.10.8.5 — 检测 HDR LUT (DOMAIN_MAX 任一分量 > 1.0)
    //    Resolve / ACES workflow 输出的 LUT 通常 DOMAIN_MAX = (4, 4, 4) 或 (10, 10, 10)
    //    LDR LUT (Photoshop / DaVinci 默认) DOMAIN_MAX = (1, 1, 1) → 走传统 RGB8 路径
    *outSize  = size;
    *outIsHDR = (domainMax[0] > 1.0f || domainMax[1] > 1.0f || domainMax[2] > 1.0f);
    return true;
}

// Phase G.1 — 主线程上传 helper: 把 parse 结果上传 backend.
// 调用者: LoadCubeLUTFromString / LoadHaldLUTFile / AssetLoader::Tick
//
// 返 GL texture id (>0) 或 0 (失败, outErr 写入)
uint32_t UploadParsedLUT(int size, bool isHDR,
                          const std::vector<uint8_t>& bytes,
                          const std::vector<float>& floats,
                          char* outErr, size_t errCap) {
    if (!g.backend) {
        writeErr_(outErr, errCap,
                  "HDR backend not initialized (parse ok size=%d, isHDR=%d)",
                  size, isHDR ? 1 : 0);
        return 0u;
    }
    uint32_t id = 0u;
    if (isHDR) {
        if (!floats.empty()) {
            id = g.backend->CreateLUT3DFloat(size, floats.data());
        }
        if (id == 0u) {
            // backend 不支持 RGB16F / floats 空 → fallback 到 LDR 路径
            CC::Log(CC::LOG_WARN,
                    "HDRRenderer: HDR LUT (size=%d) CreateLUT3DFloat failed/floats empty, "
                    "fallback to RGB8 (high values clamped)", size);
            if (!bytes.empty()) id = g.backend->CreateLUT3D(size, bytes.data());
        }
    } else {
        if (!bytes.empty()) id = g.backend->CreateLUT3D(size, bytes.data());
    }
    if (id == 0u) {
        writeErr_(outErr, errCap,
                  "backend CreateLUT3D%s failed (size=%d, GL not ready or OOM)",
                  isHDR ? "Float" : "", size);
    }
    return id;
}

// Phase F.0.10.8.1 — 从内存字符串加载 .cube LUT (parse + GL 上传)
uint32_t LoadCubeLUTFromString(const char* text, size_t textLen,
                                char* outErr, size_t errCap) {
    int  size  = 0;
    bool isHDR = false;
    std::vector<uint8_t> bytes;
    std::vector<float>   floats;
    if (!ParseCubeLUTFromString(text, textLen, &size, &isHDR, &bytes, &floats, outErr, errCap)) {
        return 0u;
    }
    return UploadParsedLUT(size, isHDR, bytes, floats, outErr, errCap);
}

uint32_t LoadCubeLUTFile(const char* path, char* outErr, size_t errCap) {
    if (!path || !*path) {
        writeErr_(outErr, errCap, "LoadCubeLUTFile: empty path");
        return 0u;
    }
    size_t sz = 0;
    void* data = SDL_LoadFile(path, &sz);
    if (!data) {
        const char* sdlErr = SDL_GetError();
        writeErr_(outErr, errCap,
                  "file read failed: %s (path: %s)",
                  (sdlErr && *sdlErr) ? sdlErr : "unknown", path);
        return 0u;
    }
    const uint32_t id = LoadCubeLUTFromString((const char*)data, sz, outErr, errCap);
    SDL_free(data);
    return id;
}

// ==================== Phase F.0.10.8.2 — HALD CLUT 图像 LUT 加载 ====================

// Phase G.1 — Parser-only (无 GL 依赖) HALD CLUT 图像 解析
// 16-bit 路径: outFloats 填归一化 [0,1] (size^3 * 3); outBytes 填 quantize 备用 (RGB16F fallback)
// 8-bit  路径: outBytes  填 RGB byte (size^3 * 3); outFloats 留空; outIsHDR=false
bool ParseHaldLUTFile(const char* path,
                       int* outSize, bool* outIsHDR,
                       std::vector<uint8_t>* outBytes,
                       std::vector<float>* outFloats,
                       char* outErr, size_t errCap) {
    if (!path || !*path) {
        writeErr_(outErr, errCap, "ParseHaldLUTFile: empty path");
        return false;
    }
    if (!outSize || !outIsHDR || !outBytes || !outFloats) {
        writeErr_(outErr, errCap, "ParseHaldLUTFile: out params null");
        return false;
    }
    outBytes->clear();
    outFloats->clear();

    // Phase F.0.10.8.5 — 探测位深 (PNG 16-bit / 8-bit; 其他格式 stbi_is_16_bit 返 0)
    stbi_set_flip_vertically_on_load(0);
    const bool is16bit = (stbi_is_16_bit(path) != 0);

    int w = 0, h = 0, ch = 0;

    if (is16bit) {
        // ===== 16-bit 路径 (HDR-friendly) =====
        stbi_us* px16 = stbi_load_16(path, &w, &h, &ch, /*force RGBA*/ 4);
        if (!px16) {
            const char* sr = stbi_failure_reason();
            writeErr_(outErr, errCap,
                      "stbi_load_16 failed: %s (path: %s)",
                      (sr && *sr) ? sr : "unknown", path);
            return false;
        }
        if (w != h) {
            stbi_image_free(px16);
            writeErr_(outErr, errCap,
                      "HALD image not square: %dx%d (path: %s)", w, h, path);
            return false;
        }
        int N = 0;
        for (int n = 2; n <= 8; ++n) if (n * n * n == w) { N = n; break; }
        if (N == 0) {
            stbi_image_free(px16);
            writeErr_(outErr, errCap,
                      "HALD width %d is not N^3 for any N in [2,8] "
                      "(expected: 8/27/64/125/216/343/512)", w);
            return false;
        }
        const int size = N * N;
        if (size < 4 || size > 64) {
            stbi_image_free(px16);
            writeErr_(outErr, errCap,
                      "HALD level %d -> LUT size %d out of range [4, 64]", N, size);
            return false;
        }
        // RGBA(uint16) → RGB float 流 (归一化到 [0, 1]) + RGB byte fallback
        const size_t totalPx = (size_t)w * (size_t)h;
        outFloats->resize(totalPx * 3u);
        outBytes->resize(totalPx * 3u);
        const float inv = 1.0f / 65535.0f;
        for (size_t i = 0; i < totalPx; ++i) {
            const float r = (float)px16[i * 4u + 0u] * inv;
            const float gC = (float)px16[i * 4u + 1u] * inv;
            const float b = (float)px16[i * 4u + 2u] * inv;
            (*outFloats)[i * 3u + 0u] = r;
            (*outFloats)[i * 3u + 1u] = gC;
            (*outFloats)[i * 3u + 2u] = b;
            (*outBytes)[i * 3u + 0u] = quantize_(r);
            (*outBytes)[i * 3u + 1u] = quantize_(gC);
            (*outBytes)[i * 3u + 2u] = quantize_(b);
        }
        stbi_image_free(px16);
        *outSize  = size;
        *outIsHDR = true;
        return true;
    }

    // ===== 8-bit 路径 =====
    unsigned char* px = stbi_load(path, &w, &h, &ch, /*force RGBA*/ 4);
    if (!px) {
        const char* sr = stbi_failure_reason();
        writeErr_(outErr, errCap,
                  "stbi_load failed: %s (path: %s)",
                  (sr && *sr) ? sr : "unknown", path);
        return false;
    }
    if (w != h) {
        stbi_image_free(px);
        writeErr_(outErr, errCap,
                  "HALD image not square: %dx%d (path: %s)", w, h, path);
        return false;
    }
    int N = 0;
    for (int n = 2; n <= 8; ++n) if (n * n * n == w) { N = n; break; }
    if (N == 0) {
        stbi_image_free(px);
        writeErr_(outErr, errCap,
                  "HALD width %d is not N^3 for any N in [2,8] "
                  "(expected: 8/27/64/125/216/343/512)", w);
        return false;
    }
    const int size = N * N;
    if (size < 4 || size > 64) {
        stbi_image_free(px);
        writeErr_(outErr, errCap,
                  "HALD level %d -> LUT size %d out of range [4, 64]", N, size);
        return false;
    }
    const size_t totalPx = (size_t)w * (size_t)h;
    outBytes->resize(totalPx * 3u);
    for (size_t i = 0; i < totalPx; ++i) {
        (*outBytes)[i * 3u + 0u] = px[i * 4u + 0u];
        (*outBytes)[i * 3u + 1u] = px[i * 4u + 1u];
        (*outBytes)[i * 3u + 2u] = px[i * 4u + 2u];
    }
    stbi_image_free(px);
    *outSize  = size;
    *outIsHDR = false;
    return true;
}

// Phase F.0.10.8.2 — HALD LUT 文件加载 (parse + GL 上传, 现 G.1 wrapper)
uint32_t LoadHaldLUTFile(const char* path, char* outErr, size_t errCap) {
    int  size  = 0;
    bool isHDR = false;
    std::vector<uint8_t> bytes;
    std::vector<float>   floats;
    if (!ParseHaldLUTFile(path, &size, &isHDR, &bytes, &floats, outErr, errCap)) {
        return 0u;
    }
    return UploadParsedLUT(size, isHDR, bytes, floats, outErr, errCap);
}

// ==================== Phase F.0.10.8.3 — LUT 热重载 (mtime polling) ====================

namespace {

// 辅助: 判断 path 是否图像扩展名 (case-insensitive)
//   .png/.jpg/.jpeg/.bmp/.tga → true (走 HALD parser)
//   .cube 或其他 → false (走 .cube parser)
bool isImageExt_(const char* path) {
    if (!path) return false;
    const char* dot = strrchr(path, '.');
    if (!dot) return false;
    // Windows 用 _stricmp, POSIX 用 strcasecmp; 简化用手写 tolower 比较 (避免平台分支)
    char ext[8] = {0};
    for (int i = 0; i < 7 && dot[i]; ++i) {
        const char c = dot[i];
        ext[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    return strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0 ||
           strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".bmp") == 0 ||
           strcmp(ext, ".tga") == 0;
}

}  // anonymous namespace

uint32_t WatchLUT(const char* path, char* outErr, size_t errCap) {
    if (!path || !*path) {
        writeErr_(outErr, errCap, "WatchLUT: empty path");
        return 0u;
    }

    // 1. 判格式 → 委托对应 parser
    const bool hald = isImageExt_(path);
    const uint32_t id = hald
        ? LoadHaldLUTFile(path, outErr, errCap)
        : LoadCubeLUTFile(path, outErr, errCap);
    if (id == 0u) return 0u;   // 加载失败 (outErr 已被 parser 填)

    // 2. 取初始 mtime (失败容忍, mtime=0 也合法 — 下次 Poll 会用真值替换)
    SDL_PathInfo info{};
    SDL_GetPathInfo(path, &info);   // 失败时 info 全 0

    // 3. 同 path 重复注册: 清旧 entry + 旧 GL tex (避免泄漏)
    // Phase F.0.10.9.x.1: 旧 lutId 在 GL 删除前必须跨 instance 清引用 (防悬挂)
    for (auto it = g_global.lutWatchList.begin(); it != g_global.lutWatchList.end(); ++it) {
        if (it->path == path) {
            if (it->lutId && it->lutId != id && g.backend) {
                RemapLUTIdAcrossInstances(it->lutId, 0u);
                g.backend->DeleteLUT3D(it->lutId);
            }
            g_global.lutWatchList.erase(it);
            break;
        }
    }

    // 4. 注册新 entry
    WatchEntry e;
    e.path      = path;
    e.lastMtime = info.modify_time;
    e.lutId     = id;
    e.isHald    = hald;
    g_global.lutWatchList.push_back(std::move(e));
    return id;
}

bool UnwatchLUT(uint32_t lutTex) {
    if (lutTex == 0u) return false;
    for (auto it = g_global.lutWatchList.begin(); it != g_global.lutWatchList.end(); ++it) {
        if (it->lutId == lutTex) {
            // Phase F.0.10.9.x.1: 跨 instance 清引用必须在 GL 删除前 (顺序敏感)
            RemapLUTIdAcrossInstances(lutTex, 0u);
            if (g.backend) g.backend->DeleteLUT3D(lutTex);
            g_global.lutWatchList.erase(it);
            return true;
        }
    }
    return false;
}

uint32_t GetWatchedLUTId(const char* path) {
    if (!path || !*path) return 0u;
    for (auto& e : g_global.lutWatchList) {
        if (e.path == path) return e.lutId;
    }
    return 0u;
}

int PollLUTReloads() {
    // 短路: 关闭 / 列表空 → 直接返 (零 SDL 调用)
    if (!g_global.lutHotReload || g_global.lutWatchList.empty()) return 0;

    int reloaded = 0;
    char errBuf[256];
    for (auto& e : g_global.lutWatchList) {
        // 1. 取当前 mtime
        SDL_PathInfo info{};
        if (!SDL_GetPathInfo(e.path.c_str(), &info)) {
            continue;   // 文件被锁/移动, 保留 entry 下次再试
        }
        if (info.modify_time == e.lastMtime) continue;   // 无变化

        // 2. mtime 变化, 触发 reload (复用 LoadCubeLUTFile / LoadHaldLUTFile)
        errBuf[0] = '\0';
        const uint32_t newId = e.isHald
            ? LoadHaldLUTFile(e.path.c_str(), errBuf, sizeof(errBuf))
            : LoadCubeLUTFile(e.path.c_str(), errBuf, sizeof(errBuf));
        if (newId == 0u) {
            // reload 失败 (文件正被写, 内容不完整) → 保留 entry, 下次再试
            CC::Log(CC::LOG_WARN,
                    "HDRRenderer::PollLUTReloads: reload failed: %s -- %s",
                    e.path.c_str(), errBuf[0] ? errBuf : "unknown");
            continue;
        }

        // 3. 替换 id + mtime
        const uint32_t oldId = e.lutId;
        e.lutId     = newId;
        e.lastMtime = info.modify_time;

        // 4. 若 oldId 在任何 instance 当前 grading 用, 自动 remap 到 newId
        //    (用户视角: SetGradingLUT(id, ...) 设过之后, 美术改文件 → 所有 instance 自动看到新调色)
        // Phase F.0.10.9.x.1: 遍历所有 instance remap (老实现仅同步 active, 其他 instance 悬挂)
        RemapLUTIdAcrossInstances(oldId, newId);

        // 5. 删旧 GL tex (remap 之后, 所有 instance 的 lutTexId 已切到 newId)
        if (oldId && g.backend) g.backend->DeleteLUT3D(oldId);

        // 6. Phase F.0.10.8.4 — 触发 reload 回调 (在 oldId 释放后, 用户拿到的 newId 可立即使用)
        //    snapshot path 防回调内重入操作 lutWatchList 失效迭代器 (虽然约定不允许, 但防御性 copy)
        if (g_global.lutReloadCb) {
            const std::string pathSnap = e.path;
            g_global.lutReloadCb(pathSnap.c_str(), oldId, newId, g_global.lutReloadCbUser);
        }

        ++reloaded;
    }
    return reloaded;
}

void SetLUTHotReload(bool enabled) { g_global.lutHotReload = enabled; }
bool GetLUTHotReload()             { return g_global.lutHotReload; }

// ==================== Phase F.0.10.8.4 — LUT reload 回调 ====================

void SetLUTReloadCallback(LUTReloadCallback cb, void* userData) {
    g_global.lutReloadCb     = cb;
    g_global.lutReloadCbUser = userData;
}

bool HasLUTReloadCallback() { return g_global.lutReloadCb != nullptr; }

// ==================== Phase F.0.10.9 — Multi-Instance API ====================
//
// 设计说明:
//   - g_states[0] 是 default singleton, 永远占用; 老 100+ fn 默认作用于 [0]
//   - CreateInstance() 找空闲槽 [1, MAX_INSTANCES-1] 返 ID, 槽满返 0
//   - 新 instance 继承 default 的 backend/supported/inited (来自 Init() 时全量写入)
//   - SetActiveInstance(id) 切换 active, 后续 namespace fn 作用于 [id]
//   - DestroyInstance(id) 释放该槽 RT + 标空闲, 若 active 是该 id 自动切回 0
//   - 不能销毁 id=0 (default), 不能 SetActiveInstance(无效 id)

int CreateInstance() {
    // Phase F.0.10.9: 仅分配槽位 + 复位 state, 不要求 default Init() 完成
    //   (headless 环境 backend=nullptr 也允许分配槽, 后续 Enable 自然失败)
    // 找第一个空闲槽 (跳过 [0])
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 复位为干净 default state, 然后继承 default 的 backend / supported / inited
            g_states[i] = State{};
            g_states[i].backend   = g_states[0].backend;     // 可能 nullptr (headless)
            g_states[i].supported = g_states[0].supported;
            g_states[i].inited    = g_states[0].inited;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "HDRRenderer::CreateInstance: 创建 instance id=%d (count=%d, inited=%d)",
                    i, g_count, g_states[0].inited ? 1 : 0);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN,
            "HDRRenderer::CreateInstance: 槽位已满 (MAX_INSTANCES=%d)", MAX_INSTANCES);
    return 0;
}

bool DestroyInstance(int id) {
    if (id <= 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::DestroyInstance: 非法 id=%d (合法范围 [1, %d])",
                id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "HDRRenderer::DestroyInstance: id=%d 未分配", id);
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
    CC::Log(CC::LOG_INFO,
            "HDRRenderer::DestroyInstance: 销毁 instance id=%d (count=%d, active=%d)",
            id, g_count, g_active);
    return true;
}

bool SetActiveInstance(int id) {
    if (id < 0 || id >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::SetActiveInstance: 非法 id=%d (合法范围 [0, %d])",
                id, MAX_INSTANCES - 1);
        return false;
    }
    if (!g_slot_in_use[id]) {
        CC::Log(CC::LOG_WARN, "HDRRenderer::SetActiveInstance: id=%d 未分配", id);
        return false;
    }
    g_active = id;
    return true;
}

int GetActiveInstance() { return g_active; }
int GetInstanceCount()  { return g_count; }

// ==================== Phase F.0.10.9.x.3 — Clone (1-line setup) ====================

int CloneInstance(int srcId) {
    if (srcId < 0 || srcId >= MAX_INSTANCES) {
        CC::Log(CC::LOG_WARN,
                "HDRRenderer::CloneInstance: 非法 srcId=%d (合法范围 [0, %d])",
                srcId, MAX_INSTANCES - 1);
        return 0;
    }
    if (!g_slot_in_use[srcId]) {
        CC::Log(CC::LOG_WARN, "HDRRenderer::CloneInstance: srcId=%d 未分配", srcId);
        return 0;
    }
    for (int i = 1; i < MAX_INSTANCES; ++i) {
        if (!g_slot_in_use[i]) {
            // 全字段复制 (含 backend/exposure/tonemap/lutTexId/lutStrength/dilation/...)
            g_states[i] = g_states[srcId];
            // 复位 backend 创建的 RT (HDR FBO + dilation RT × 2; lutTexId 是用户传, 保留)
            g_states[i].fbo                       = 0;
            g_states[i].sceneTex                  = 0;
            g_states[i].dilatedVelocityFbo        = 0;
            g_states[i].dilatedVelocityTex        = 0;
            g_states[i].dilatedCameraVelocityFbo  = 0;
            g_states[i].dilatedCameraVelocityTex  = 0;
            g_states[i].width                     = 0;
            g_states[i].height                    = 0;
            // Phase F.1 TAAU: 复位双尺寸字段 (clone 不继承 active 状态; 新 instance 待自己 Enable + SetTAAUEnabled)
            g_states[i].outputW                   = 0;
            g_states[i].outputH                   = 0;
            g_states[i].taauActive                = false;
            g_states[i].outputSceneFbo            = 0;
            g_states[i].outputSceneTex            = 0;
            g_states[i].enabled                   = false;
            g_states[i].paused                    = false;
            g_slot_in_use[i] = true;
            ++g_count;
            CC::Log(CC::LOG_INFO,
                    "HDRRenderer::CloneInstance: srcId=%d -> id=%d (count=%d)",
                    srcId, i, g_count);
            return i;
        }
    }
    CC::Log(CC::LOG_WARN, "HDRRenderer::CloneInstance: 槽位已满");
    return 0;
}

bool GetVelocityDilation() { return g.velocityDilation; }

bool SetVelocityFormat(VelocityFormat fmt) {
    if (fmt == g.velocityFormat) return true;   // no-op
    g.velocityFormat = fmt;
    if (!g.enabled) return true;                 // 未 Enable，仅更新 state，下次 Enable 生效
    // Phase F.1 TAAU: 保存双尺寸状态以便重建后恢复 (ReleaseRT 会清 taauActive/outputW/H)
    const bool wasTaauActive = g.taauActive;
    const int  savedRenderW  = g.width;
    const int  savedRenderH  = g.height;
    const int  savedOutputW  = g.outputW;
    const int  savedOutputH  = g.outputH;
    // 重建 RT：ReleaseRT + CreateRT (与 Enable 同模式)
    ReleaseRT();
    if (wasTaauActive) {
        // Phase F.1 TAAU: 恢复双尺寸路径
        if (!CreateRT(savedRenderW, savedRenderH)) {
            CC::Log(CC::LOG_ERROR, "HDRRenderer::SetVelocityFormat: CreateRT(render) failed after format switch");
            g.enabled = false;
            return false;
        }
        if (!g.backend->CreateOutputSceneTex(savedOutputW, savedOutputH,
                                             &g.outputSceneFbo, &g.outputSceneTex)) {
            CC::Log(CC::LOG_ERROR, "HDRRenderer::SetVelocityFormat: CreateOutputSceneTex failed, 回退 F.0");
            ReleaseRT();
            CreateRT(savedOutputW, savedOutputH);
            g.outputW = savedOutputW;
            g.outputH = savedOutputH;
            g.taauActive = false;
            return false;
        }
        g.outputW    = savedOutputW;
        g.outputH    = savedOutputH;
        g.taauActive = true;
    } else {
        if (!CreateRT(savedOutputW, savedOutputH)) {
            CC::Log(CC::LOG_ERROR, "HDRRenderer::SetVelocityFormat: CreateRT failed after format switch");
            g.enabled = false;
            return false;
        }
        g.outputW = savedOutputW;
        g.outputH = savedOutputH;
    }
    // Velocity history 在 ReleaseRT 里已重置；这里不再重复
    return true;
}

VelocityFormat GetVelocityFormat() { return g.velocityFormat; }

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
