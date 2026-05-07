/**
 * @file batch_renderer.cpp
 * @brief BatchRenderer 实现 (Phase A4)
 */

#include "batch_renderer.h"
#include "light.h"
#include <vector>
#include <cstring>

namespace BatchRenderer {

// ==================== 内部状态 ====================

static RenderBackend* s_backend = nullptr;

/// 当前批: 累积的顶点
static std::vector<RenderVertex> s_vertices;

/// 静态共享 quad 索引 (一次生成, 永久不变)
/// 索引模式: [0,1,2, 0,2,3,  4,5,6, 4,6,7,  ...]
static std::vector<uint16_t> s_quadIndices;

/// 当前批: 实际使用的索引数 (= quadCount * 6 + 提交的 triangle 索引数)
static int s_currentIndexCount = 0;

/// 当前批的纹理状态 (匹配则继续累积, 不匹配则 Flush)
static uint32_t s_currentTexture = 0;

/// 当前批中已提交的 quad 计数 (用于推进静态索引指针)
static int s_currentQuadCount = 0;

/// 当前批中是否含三角形 (混合模式: quad 索引 + tri 索引)
/// 由于 quad 索引已预生成, triangle 索引需要动态构造在 quad 之后
static std::vector<uint16_t> s_dynamicIndices;

/// 性能统计
static Stats s_stats = {};

/// 初始化标志
static bool s_inited = false;

// ==================== 内部辅助 ====================

/// 启动时一次性生成 quad 索引: [0,1,2, 0,2,3,  4,5,6, 4,6,7,  ...]
static void GenerateQuadIndices() {
    s_quadIndices.resize(MAX_INDICES_PER_BATCH);
    for (int i = 0; i < MAX_QUADS_PER_BATCH; ++i) {
        int base = i * 4;       // 顶点起始
        int idx  = i * 6;       // 索引起始
        s_quadIndices[idx + 0] = (uint16_t)(base + 0);
        s_quadIndices[idx + 1] = (uint16_t)(base + 1);
        s_quadIndices[idx + 2] = (uint16_t)(base + 2);
        s_quadIndices[idx + 3] = (uint16_t)(base + 0);
        s_quadIndices[idx + 4] = (uint16_t)(base + 2);
        s_quadIndices[idx + 5] = (uint16_t)(base + 3);
    }
}

/// 真正提交当前批到后端
static void DoFlush(const char* reason) {
    if (s_currentIndexCount == 0 || s_vertices.empty()) {
        // 空批, 重置状态即可
        s_vertices.clear();
        s_dynamicIndices.clear();
        s_currentIndexCount = 0;
        s_currentQuadCount = 0;
        s_currentTexture = 0;
        return;
    }

    if (!s_backend) {
        s_vertices.clear();
        s_dynamicIndices.clear();
        s_currentIndexCount = 0;
        s_currentQuadCount = 0;
        s_currentTexture = 0;
        return;
    }

    // 选择索引源:
    //   - 纯 quad 批: 用静态预生成索引 s_quadIndices.data()
    //   - 含 triangle 批: 用 s_dynamicIndices (其前部分是 quad 索引拷贝, 后部分是 tri 索引)
    const uint16_t* indices;
    int indexCount;
    if (s_dynamicIndices.empty()) {
        indices = s_quadIndices.data();
        indexCount = s_currentQuadCount * 6;
    } else {
        indices = s_dynamicIndices.data();
        indexCount = (int)s_dynamicIndices.size();
    }

    s_backend->DrawIndexed(s_vertices.data(), (int)s_vertices.size(),
                           indices, indexCount,
                           s_currentTexture);

    s_stats.drawCalls++;
    if (reason && reason[0] == 'F') s_stats.batchesFull++;
    else if (reason && reason[0] == 'S') s_stats.batchesState++;

    // 重置批状态
    s_vertices.clear();
    s_dynamicIndices.clear();
    s_currentIndexCount = 0;
    s_currentQuadCount = 0;
    s_currentTexture = 0;
}

// ==================== 生命周期 ====================

bool Init(RenderBackend* backend) {
    if (s_inited) return true;
    if (!backend) {
        CC::Log(CC::LOG_ERROR, "BatchRenderer::Init: backend is null");
        return false;
    }
    s_backend = backend;

    GenerateQuadIndices();
    s_vertices.reserve(MAX_VERTICES_PER_BATCH);
    s_dynamicIndices.reserve(MAX_INDICES_PER_BATCH);

    s_inited = true;
    CC::Log(CC::LOG_INFO,
            "BatchRenderer: initialized (max %d quads / %d vertices / %d indices per batch)",
            MAX_QUADS_PER_BATCH, MAX_VERTICES_PER_BATCH, MAX_INDICES_PER_BATCH);
    return true;
}

void Shutdown() {
    if (!s_inited) return;
    s_vertices.clear();
    s_vertices.shrink_to_fit();
    s_dynamicIndices.clear();
    s_dynamicIndices.shrink_to_fit();
    s_quadIndices.clear();
    s_quadIndices.shrink_to_fit();
    s_backend = nullptr;
    s_currentTexture = 0;
    s_currentIndexCount = 0;
    s_currentQuadCount = 0;
    s_inited = false;
}

bool IsInited() { return s_inited; }

// ==================== 帧控制 ====================

void BeginFrame() {
    ResetStats();
    s_vertices.clear();
    s_dynamicIndices.clear();
    s_currentIndexCount = 0;
    s_currentQuadCount = 0;
    s_currentTexture = 0;
}

void EndFrame() {
    DoFlush("E"); // EndFrame
}

// ==================== 提交 ====================

void SubmitQuad(const RenderVertex verts[4], uint32_t textureId) {
    if (!s_inited || !verts) return;

    // 状态变化: 纹理不匹配 -> Flush
    // 注: 首批 currentTexture==0 + textureId==0 视为匹配 (纯色批)
    //     首批 currentTexture==0 + textureId!=0 也视为匹配 (取首次 textureId)
    if (s_currentQuadCount > 0 && s_currentTexture != textureId) {
        DoFlush("S"); // StateChange
    }

    // 容量检查: 顶点 + 4 超限或 quad 数超限 -> Flush
    if ((int)s_vertices.size() + 4 > MAX_VERTICES_PER_BATCH ||
        s_currentQuadCount + 1 > MAX_QUADS_PER_BATCH) {
        DoFlush("F"); // Full
    }

    // 累积顶点
    if (s_currentQuadCount == 0) {
        s_currentTexture = textureId;
    }
    s_vertices.push_back(verts[0]);
    s_vertices.push_back(verts[1]);
    s_vertices.push_back(verts[2]);
    s_vertices.push_back(verts[3]);
    s_currentQuadCount++;
    s_currentIndexCount = s_currentQuadCount * 6;
    s_stats.verticesSubmit += 4;
}

void SubmitTriangles(const RenderVertex* verts, int count, uint32_t textureId) {
    if (!s_inited || !verts || count <= 0 || (count % 3) != 0) return;

    // 状态变化检查
    if (s_currentQuadCount > 0 && s_currentTexture != textureId) {
        DoFlush("S");
    }

    // 容量检查
    if ((int)s_vertices.size() + count > MAX_VERTICES_PER_BATCH) {
        DoFlush("F");
    }

    // 首次提交确定纹理
    if (s_vertices.empty() && s_currentQuadCount == 0) {
        s_currentTexture = textureId;
    } else if (s_currentTexture != textureId) {
        // 不同纹理, Flush 后再提交
        DoFlush("S");
        s_currentTexture = textureId;
    }

    // 准备动态索引 (若之前是纯 quad 批, 先把已有 quad 索引拷贝过来)
    if (s_dynamicIndices.empty() && s_currentQuadCount > 0) {
        s_dynamicIndices.assign(s_quadIndices.begin(),
                                s_quadIndices.begin() + s_currentQuadCount * 6);
    }

    // 累积三角形顶点 + 索引
    int baseVertex = (int)s_vertices.size();
    for (int i = 0; i < count; ++i) {
        s_vertices.push_back(verts[i]);
    }
    if (!s_dynamicIndices.empty() || s_currentQuadCount > 0) {
        // 已有动态索引, 直接 append
        for (int i = 0; i < count; ++i) {
            s_dynamicIndices.push_back((uint16_t)(baseVertex + i));
        }
    } else {
        // 纯三角形批, 用顺序索引: 0,1,2, 3,4,5, ...
        for (int i = 0; i < count; ++i) {
            s_dynamicIndices.push_back((uint16_t)(baseVertex + i));
        }
    }
    s_currentIndexCount = (int)s_dynamicIndices.size();
    s_stats.verticesSubmit += count;
}

void SubmitLines(const RenderVertex* verts, int count) {
    if (!s_inited || !verts || count <= 0 || (count % 2) != 0) return;
    // Lines 不批渲染: 先 Flush 当前批, 然后立即提交
    DoFlush("S");
    if (s_backend) {
        s_backend->DrawArrays(DrawMode::Lines, verts, count);
        s_stats.drawCalls++;
        s_stats.verticesSubmit += count;
    }
}

void SubmitImmediate(DrawMode mode, const RenderVertex* verts, int count, uint32_t textureId) {
    if (!s_inited || !verts || count <= 0) return;
    // 不规则拓扑 (LineLoop/LineStrip/TriangleFan): 先 Flush, 立即提交
    DoFlush("S");
    if (s_backend) {
        if (textureId) s_backend->BindTexture(textureId);
        s_backend->DrawArrays(mode, verts, count);
        if (textureId) s_backend->UnbindTexture();
        s_stats.drawCalls++;
        s_stats.verticesSubmit += count;
    }
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

} // namespace BatchRenderer
