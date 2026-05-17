/**
 * @file lit_batch_renderer.cpp
 * @brief LitBatchRenderer 实现 (Phase E.2.3)
 *
 * 与 BatchRenderer 同节奏:
 *   - Init: 绑 backend + 预生成 quad 索引模板
 *   - SubmitQuad: 累积 4 个 RenderVertex2DLit + 推 6 个索引
 *   - 状态匹配: (baseColorTex, normalMapTex) 同步, 不同则 Flush
 *   - 容量满: Flush
 *   - Flush: 调 backend->DrawLit2DBatch (内部含 BeginLit2DDraw + UploadLighting2D)
 */

#include "lit_batch_renderer.h"
#include "light.h"
#include <vector>
#include <cstring>

namespace LitBatchRenderer {

// ==================== 内部状态 ====================

static RenderBackend* s_backend = nullptr;

/// 当前批: 累积的顶点
static std::vector<RenderVertex2DLit> s_vertices;

/// 当前批: 索引 (动态生成: [0,1,2, 0,2,3,  4,5,6, 4,6,7, ...])
static std::vector<uint32_t> s_indices;

/// 状态: 当前批的 baseColor / normalMap 纹理
static uint32_t s_currentBaseTex   = 0;
static uint32_t s_currentNormalTex = 0;

/// 当前批 quad 计数
static int s_currentQuadCount = 0;

/// 性能统计
static Stats s_stats = {};

/// 初始化标志
static bool s_inited = false;

// ==================== 内部辅助 ====================

/// 实际把当前批提交给 backend
static void DoFlush(const char* reason) {
    if (s_currentQuadCount == 0 || s_vertices.empty()) {
        // 空批, 重置状态即可
        s_vertices.clear();
        s_indices.clear();
        s_currentQuadCount = 0;
        s_currentBaseTex = 0;
        s_currentNormalTex = 0;
        return;
    }

    if (s_backend) {
        s_backend->DrawLit2DBatch(s_vertices.data(), (int)s_vertices.size(),
                                  s_indices.data(),  (int)s_indices.size(),
                                  s_currentBaseTex,  s_currentNormalTex);
        s_stats.drawCalls++;
        if (reason && reason[0] == 'F')      s_stats.batchesFull++;
        else if (reason && reason[0] == 'S') s_stats.batchesState++;
    }

    // 重置批状态
    s_vertices.clear();
    s_indices.clear();
    s_currentQuadCount = 0;
    s_currentBaseTex = 0;
    s_currentNormalTex = 0;
}

// ==================== 生命周期 ====================

bool Init(RenderBackend* backend) {
    if (s_inited) return true;
    if (!backend) {
        CC::Log(CC::LOG_ERROR, "LitBatchRenderer::Init: backend is null");
        return false;
    }
    s_backend = backend;
    s_vertices.reserve(MAX_VERTICES_PER_BATCH);
    s_indices.reserve(MAX_INDICES_PER_BATCH);

    s_inited = true;
    CC::Log(CC::LOG_INFO,
            "LitBatchRenderer: initialized (max %d quads / %d vertices per batch)",
            MAX_QUADS_PER_BATCH, MAX_VERTICES_PER_BATCH);
    return true;
}

void Shutdown() {
    if (!s_inited) return;
    s_vertices.clear();
    s_vertices.shrink_to_fit();
    s_indices.clear();
    s_indices.shrink_to_fit();
    s_backend = nullptr;
    s_currentQuadCount = 0;
    s_currentBaseTex = 0;
    s_currentNormalTex = 0;
    s_inited = false;
}

bool IsInited() { return s_inited; }

// ==================== 帧控制 ====================

void BeginFrame() {
    ResetStats();
    s_vertices.clear();
    s_indices.clear();
    s_currentQuadCount = 0;
    s_currentBaseTex = 0;
    s_currentNormalTex = 0;
}

void EndFrame() {
    DoFlush("E"); // EndFrame
}

// ==================== 提交 ====================

void SubmitQuad(const RenderVertex2DLit verts[4],
                uint32_t baseColorTex, uint32_t normalMapTex) {
    if (!s_inited || !verts) return;

    // 状态变化: 纹理对 (base, normal) 不匹配 -> Flush
    if (s_currentQuadCount > 0 &&
        (s_currentBaseTex != baseColorTex || s_currentNormalTex != normalMapTex)) {
        DoFlush("S"); // StateChange
    }

    // 容量检查: 顶点 + 4 超限或 quad 数超限 -> Flush
    if ((int)s_vertices.size() + 4 > MAX_VERTICES_PER_BATCH ||
        s_currentQuadCount + 1 > MAX_QUADS_PER_BATCH) {
        DoFlush("F"); // Full
    }

    // 首次累积确定纹理对
    if (s_currentQuadCount == 0) {
        s_currentBaseTex   = baseColorTex;
        s_currentNormalTex = normalMapTex;
    }

    // 累积顶点 + 索引 (4 verts + 6 idx 一组; idx 基于 baseVertex)
    const uint32_t baseVertex = (uint32_t)s_vertices.size();
    s_vertices.push_back(verts[0]);
    s_vertices.push_back(verts[1]);
    s_vertices.push_back(verts[2]);
    s_vertices.push_back(verts[3]);

    s_indices.push_back(baseVertex + 0);
    s_indices.push_back(baseVertex + 1);
    s_indices.push_back(baseVertex + 2);
    s_indices.push_back(baseVertex + 0);
    s_indices.push_back(baseVertex + 2);
    s_indices.push_back(baseVertex + 3);

    s_currentQuadCount++;
    s_stats.verticesSubmit += 4;
}

// ==================== Flush ====================

void Flush() { DoFlush("M"); /* Manual */ }

void NotifyStateChange() { DoFlush("S"); }

// ==================== 诊断 ====================

const Stats& GetStats() { return s_stats; }

void ResetStats() {
    s_stats.drawCalls = 0;
    s_stats.verticesSubmit = 0;
    s_stats.batchesFull = 0;
    s_stats.batchesState = 0;
}

// ==================== HDR 联动 (Phase F.2.1) ====================
// LitBatch 是 stateless w.r.t. HDR RT 尺寸; 三个 stub 仅为接口对齐.
void OnHDREnabled(int /*w*/, int /*h*/) {}
void OnHDRDisabled() {}
void OnHDRResized(int /*w*/, int /*h*/) {}

} // namespace LitBatchRenderer
