/**
 * @file lit_batch_renderer.h
 * @brief Phase E.2.3 — 2D Lit Sprite 批渲染器 (forward 多光照)
 *
 * 设计原则 (与 BatchRenderer 同风格, 独立命名空间):
 *   - 后端无关: 通过 RenderBackend::DrawLit2DBatch(verts, vcount, idx, icount,
 *     baseTex, normalTex) 提交, 不直接调 GL.
 *   - 状态匹配累积: (baseColorTex, normalMapTex) 不变时持续累积顶点.
 *   - 状态变化自动 Flush: 纹理对任一变化 / 容量满 / 手动 Flush.
 *   - 顶点格式: RenderVertex2DLit (64 字节; 含 normal + tangent, shader 侧做法线变换).
 *
 * 与 BatchRenderer 的关系:
 *   - 独立实例, 共享 RenderBackend 句柄, 但批内状态互不感知.
 *   - 调用方 (light_graphics.cpp::l_Draw / l_DrawLit) 切换绘制路径时, 必须互相 Flush
 *     以保证 GPU 顺序与 CPU 提交顺序一致 (画家算法不破).
 *
 * 与 E.2.1 dirty bit 的关系:
 *   - 单次 DrawLit2DBatch 内, backend 只调一次 UploadLighting2D; 由 dirty 再护航:
 *     state.version 不变时跳过 8 次 glUniform*v, 多连续 batch 仍受益.
 *
 * CPU 烘焙 transform:
 *   - 普通 Draw 走 gfx.Push/Translate/...; 批渲染 N quad 共享 modelview, 每 quad 不同
 *     transform 必须在 CPU 端烘到 vertex.pos 里. light_graphics.cpp 负责烘焙.
 */

#pragma once

#include "render_backend.h"
#include <cstdint>

namespace LitBatchRenderer {

// ==================== 容量常量 ====================

/// 单批最大 quad 数 (RenderVertex2DLit 是 RenderVertex 的 2 倍大, 容量减半)
constexpr int MAX_QUADS_PER_BATCH = 8192;

/// 单批最大顶点数
constexpr int MAX_VERTICES_PER_BATCH = MAX_QUADS_PER_BATCH * 4;

/// 单批最大索引数
constexpr int MAX_INDICES_PER_BATCH = MAX_QUADS_PER_BATCH * 6;

// ==================== 性能统计 ====================

struct Stats {
    int drawCalls;       ///< 实际触发 GPU draw call 次数
    int verticesSubmit;  ///< 当前帧累积顶点总数
    int batchesFull;     ///< 因容量满触发的 Flush 次数
    int batchesState;    ///< 因纹理状态变化触发的 Flush 次数
};

// ==================== 生命周期 ====================

/// 初始化批处理器, 绑定到给定 RenderBackend
bool Init(RenderBackend* backend);

/// 关闭并释放资源
void Shutdown();

/// 是否已初始化
bool IsInited();

// ==================== 帧控制 ====================

/// 帧开始 (调用方在 BatchRenderer::BeginFrame 之后或之前均可, 独立)
void BeginFrame();

/// 帧结束, Flush 残余批
void EndFrame();

// ==================== 提交 ====================

/// 提交一个 lit quad (4 顶点, 顺序 TL-TR-BR-BL, CPU 端已烘焙 transform)
/// @param verts 4 个 RenderVertex2DLit (pos / uv / color / normal / tangent)
/// @param baseColorTex baseColor 纹理 (0 = 纯顶点色, 内部仍走 programLit2D)
/// @param normalMapTex normal map 纹理 (0 = 无, shader 内 uHasNormalMap=0)
void SubmitQuad(const RenderVertex2DLit verts[4],
                uint32_t baseColorTex, uint32_t normalMapTex);

// ==================== Flush 控制 ====================

/// 强制 Flush 当前批
/// 调用方在切换到普通 Draw / 状态切换 (blend / scissor / shader) 前必须 Flush.
void Flush();

/// 通知状态变化, 触发 Flush (与 Flush 等价, 命名更明确)
void NotifyStateChange();

// ==================== 诊断 ====================

const Stats& GetStats();
void ResetStats();

} // namespace LitBatchRenderer
