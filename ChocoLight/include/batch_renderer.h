/**
 * @file batch_renderer.h
 * @brief 后端无关的 2D Sprite 批渲染器
 * @note Phase A4 新增, 解决每 sprite/粒子/字符单 DrawArrays 的性能问题
 *
 * 设计原则:
 *   - 后端无关: 通过 RenderBackend::DrawIndexed 接口提交, 不直接调 GL/SDL_GPU
 *   - 状态匹配累积: 纹理/blend/scissor/shader 不变时持续累积顶点
 *   - 状态变化自动 Flush: 由调用方主动 NotifyStateChange + Flush
 *   - EBO 静态共享: 启动时一次生成 65536 顶点的 quad 索引, 永久不变
 *
 * 工作流程:
 *   BeginFrame()
 *       SubmitQuad(verts, texId)      // 累积到 CPU 顶点缓冲
 *       SubmitQuad(verts, texId)      // 同纹理继续累积
 *       SubmitQuad(verts, otherTex)   // 切纹理 -> 自动 Flush 之前的, 开新批
 *       NotifyStateChange()           // blend/scissor 变化时调用
 *       SubmitTriangles(...)
 *   EndFrame()                        // 自动 Flush 残余批
 */

#pragma once

#include "render_backend.h"
#include <cstdint>

namespace BatchRenderer {

// ==================== 容量常量 ====================

/// 单批最大 quad 数 (uint16 索引上限 65536 / 4 顶点)
constexpr int MAX_QUADS_PER_BATCH = 16384;

/// 单批最大顶点数
constexpr int MAX_VERTICES_PER_BATCH = MAX_QUADS_PER_BATCH * 4;

/// 单批最大索引数 (每 quad 6 索引)
constexpr int MAX_INDICES_PER_BATCH = MAX_QUADS_PER_BATCH * 6;

// ==================== 性能统计 ====================

struct Stats {
    int drawCalls;       ///< 当前帧 Flush 次数 (实际触发 GPU draw call 数)
    int verticesSubmit;  ///< 当前帧累积顶点总数
    int batchesFull;     ///< 因 batch 满触发的 Flush 次数
    int batchesState;    ///< 因状态变化触发的 Flush 次数
};

// ==================== 生命周期 ====================

/// 初始化批处理器, 绑定到给定 RenderBackend
/// @param backend 已就绪的渲染后端 (g_render)
/// @return true 成功, false 失败 (内存分配失败)
bool Init(RenderBackend* backend);

/// 关闭并释放资源
void Shutdown();

/// 是否已初始化
bool IsInited();

// ==================== 帧控制 ====================

/// 帧开始, 重置统计 + 当前批
void BeginFrame();

/// 帧结束, Flush 残余批
void EndFrame();

// ==================== 提交 ====================

/// 提交一个 quad (4 顶点, 顺序 TL-TR-BR-BL 或调用者约定)
/// @param verts 4 个 RenderVertex
/// @param textureId 当前 quad 绑定的纹理 (0 = 纯色绘制)
void SubmitQuad(const RenderVertex verts[4], uint32_t textureId);

/// 提交三角形列表 (count 必须是 3 的倍数)
/// @param verts 顶点数组
/// @param count 顶点数
/// @param textureId 纹理 (0 = 纯色)
void SubmitTriangles(const RenderVertex* verts, int count, uint32_t textureId);

/// 提交线段列表 (count 必须是 2 的倍数)
/// @note 线段不批渲染, 直接走 backend->DrawArrays(Lines, ...) 立即提交
/// @note 内部会先 Flush 当前批以保证渲染顺序
void SubmitLines(const RenderVertex* verts, int count);

/// 提交 LineLoop / LineStrip / TriangleFan 等不规则拓扑
/// @note 不批渲染, 内部 Flush 后立即走 backend->DrawArrays(mode, ...)
void SubmitImmediate(DrawMode mode, const RenderVertex* verts, int count, uint32_t textureId);

// ==================== Flush 控制 ====================

/// 强制 Flush 当前批 (将累积顶点提交 GPU)
/// 状态切换 (blend/scissor/shader/矩阵) 之前必须调用
void Flush();

/// 通知状态变化, 触发 Flush
/// 等价于调用 Flush(), 语义更明确
void NotifyStateChange();

// ==================== 诊断 ====================

/// 获取当前帧统计
const Stats& GetStats();

/// 重置统计 (BeginFrame 自动调用)
void ResetStats();

} // namespace BatchRenderer
