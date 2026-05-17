/**
 * @file   asset_loader.cpp
 * @brief  Phase G.1 — 异步资源加载器实现
 *
 * 详见 asset_loader.h 架构说明.
 *
 * 实现:
 *   - 单 worker thread, 单一 task_queue + result_queue, mutex+cv 保护.
 *   - Phase G.1.1: probe Shared GL Context, 成功则 worker 直接 GL 上传 (Image/LUT/Font),
 *                 主线程 Tick 用 glClientWaitSync 翻 status; 失败透明回落.
 *   - 失败 fallback: AssetLoader 未 Init / worker 没启动 → LoadXxxAsync 立即同步加载.
 *
 * 后期 TODO:
 *   - GLTF Mesh worker 上传 (G.1.2 候选)
 *
 * 作者: ChocoLight Engine
 * 版本: Phase G.1.1
 */

#include "asset_loader.h"
#include "render_backend.h"
#include "platform_window.h"        // Phase G.1.1 — CreateSharedGLContext / MakeCurrent
#include "light.h"                 // CC::Log + lua headers
#include "lauxlib.h"               // luaL_ref / luaL_unref
#include "stb_image.h"
#include "hdr_renderer.h"          // Phase G.1 — ParseCube/Hald + UploadParsedLUT
#include "light_audio_backend.h"   // Phase G.1 — Sound 异步: AudioBackend::LoadFile / Free

// Phase G.1.1 — worker 直接 GL 上传需的函数指针 (仅桌面平台)
// 移动 / Web 平台不走共享 ctx 路径, 不需要 glad
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
#include <glad/gl.h>
#endif

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <utility>
#include <vector>

// 第三方 (worker thread CPU 解码)
#include "stb_truetype.h"   // Phase G.1 — Font 异步: stbtt_InitFont 验证
extern "C" {
#include "cgltf.h"          // Phase G.1 — GLTF Mesh 异步: cgltf_parse_file
}

extern RenderBackend* g_render;
extern lua_State*     g_callbackL;     // light_ui.cpp 内的全局 Lua state (主线程)

namespace AssetLoader {

FutureState::~FutureState() {
    // Phase G.1.1 — 兜底清理未释放的 fence (正常路径在 Tick / Shutdown 已清)
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
    if (glFence) {
        glDeleteSync((GLsync)glFence);
        glFence = nullptr;
    }
    // Phase G.1.2 — 兜底清理未注册的 mesh GL handles
    // (worker 已 glGen 但 Tick fence Ready 路径未调到 RegisterUploadedMesh)
    if (glMeshVao) { GLuint v = glMeshVao; glDeleteVertexArrays(1, &v); glMeshVao = 0; }
    if (glMeshVbo) { GLuint v = glMeshVbo; glDeleteBuffers(1, &v);      glMeshVbo = 0; }
    if (glMeshEbo) { GLuint v = glMeshEbo; glDeleteBuffers(1, &v);      glMeshEbo = 0; }
    // Phase G.1.5 — 兜底清理 material textures: worker 上传成功但未写入 MaterialDesc slot
    // (例如 fence 失败 / WriteSlots 前异常退出)
    for (auto& job : gltfMaterialImages) {
        if (job.glTexId) { GLuint t = job.glTexId; glDeleteTextures(1, &t); job.glTexId = 0; }
    }
#endif
    // Phase G.1.5 — 兜底清理 material image pixels (fallback 路径未走到主线程上传时)
    for (auto& job : gltfMaterialImages) {
        if (job.pixels) { stbi_image_free(job.pixels); job.pixels = nullptr; }
    }
    // Image
    if (imgPixels) {
        stbi_image_free(imgPixels);
        imgPixels = nullptr;
    }
    // Font: 主线程 dispatch 完成后转移到 FontContext, 这里仅清理 worker 已分配但未交接的 buffer
    //       (主线程 Tick 上传成功后会把 fontTTFBuffer 置空, 表示 ownership 转移)
    if (fontTTFBuffer) {
        free(fontTTFBuffer);
        fontTTFBuffer = nullptr;
    }
    // GLTF: 主线程 Tick 完成后会清理 (gltfData / gltfVerts / gltfIndices 全置空); 这里兜底
    if (gltfData) {
        cgltf_free((cgltf_data*)gltfData);
        gltfData = nullptr;
    }
    if (gltfVerts) {
        free(gltfVerts);
        gltfVerts = nullptr;
    }
    if (gltfIndices) {
        free(gltfIndices);
        gltfIndices = nullptr;
    }
    // Sound: 主线程 dispatch 完成后转移 ownership 给 SoundUserdata; 这里兜底释放未交接的句柄
    if (resSoundHandle) {
        AudioBackend::Free((AudioHandle*)resSoundHandle);
        resSoundHandle = nullptr;
    }
    // 释放 Lua callback ref (主线程 GC 路径; worker 不持 shared_ptr 不会触发此析构)
    if (cbLuaState && cbLuaRef >= 0) {
        luaL_unref((lua_State*)cbLuaState, LUA_REGISTRYINDEX, cbLuaRef);
        cbLuaRef = -1;
        cbLuaState = nullptr;
    }
}

// ==================== 内部状态 ====================
namespace {

enum class TaskType : int {
    Image     = 1,
    LUT_Cube  = 2,    // .cube 文件
    LUT_Hald  = 3,    // PNG/JPG/etc HALD CLUT
    Font      = 4,    // TTF
    Sound     = 5,    // miniaudio 各种格式
    GLTF      = 6,    // .gltf / .glb (单 primitive, 无 material)
};

struct Task {
    TaskType                       type;
    std::string                    path;
    std::shared_ptr<FutureState>   state;
    // 主线程 dispatch 的 Lua callback (本期不在此层处理; 留给 Lua binding 自管)
};

static std::thread             g_worker;
static std::mutex              g_taskMutex;
static std::condition_variable g_taskCv;
static std::deque<Task>        g_taskQueue;

static std::mutex              g_resultMutex;
static std::deque<Task>        g_resultQueue;

static std::atomic<bool>       g_running{false};
static std::atomic<bool>       g_shouldStop{false};

// Phase G.1.1 — Shared GL Context probe 状态
static void*                   g_mainWin     = nullptr;   // 主窗口 (light_ui.cpp 传入)
static void*                   g_mainCtx     = nullptr;   // 主 GL ctx (调用时已 current)
static void*                   g_workerCtx   = nullptr;   // worker 共享的额外 ctx, probe 失败为 nullptr
static std::atomic<bool>       g_sharedCtxOk{false};      // probe 结果, worker 以此判断是否走 GL 上传

// fence 超时阈值 — 60 帧 ≈60fps 带 1s, 30fps 带 2s, 足够应付驱动上传延迟
static constexpr int kFenceMaxWaitFrames = 60;

// ==================== Phase G.1.3 — Worker Thread RAII Guard ====================
// 进程退出时 (atexit / 静态析构) 的最后一道防线:
// 若 AssetLoader::Shutdown 未被调用 (sample 跳过 PerformWindowShutdown_),
// std::thread::~thread() joinable 会触发 std::terminate → 进程崩溃
// (Windows: STATUS_STACK_BUFFER_OVERRUN, 0xC0000409, fast-fail 跳过 atexit/析构链)。
//
// guard 在 g_worker.~thread() 之前执行 (同 TU 内静态对象 LIFO 析构序;
// 此 guard 在 g_worker 之后声明所以先析构):
//   - 不调用 join (worker 可能阻塞在 cv.wait, 主线程已无法 notify → 死锁)
//   - 主动 set g_shouldStop + notify_all 唤醒 worker
//   - detach 让 OS 在进程退出时回收 thread, 避免 std::terminate
//
// 正常路径 (Shutdown 已 join): joinable() == false → 静默返回。
namespace {
struct WorkerThreadGuard {
    ~WorkerThreadGuard() {
        if (!g_worker.joinable()) return;   // 正常路径: Shutdown 已完成 join

        // 异常路径: sample 跳过引擎清理路径 → worker 仍在跑
        g_shouldStop.store(true);
        g_taskCv.notify_all();
        g_worker.detach();
        g_running.store(false);

        // 不能用 CC::Log (logger 全局可能已析构), 直接 stderr
        fputs("[AssetLoader] WARNING: worker thread not joined before process exit; detached as last resort.\n", stderr);
        fputs("[AssetLoader]          See docs/API_REFERENCE.md (Light.UI section) for the proper main loop pattern.\n", stderr);
        fflush(stderr);
    }
};
// 必须在 g_worker 之后声明 (line 127), 这样静态析构 LIFO 序保证 guard 先析构。
static WorkerThreadGuard g_workerGuard;
} // namespace

// ==================== Worker 解码 helper (worker thread only) ====================

// Image: stbi_load (force RGBA), 失败写 errorMsg
static void DecodeImage_(Task& task) {
    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load(task.path.c_str(), &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        task.state->errorMsg = std::string("stbi_load failed: ") +
                               (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        return;
    }
    task.state->imgW        = w;
    task.state->imgH        = h;
    task.state->imgChannels = 4;
    task.state->imgPixels   = pixels;
}

// LUT .cube: 读文件 → ParseCubeLUTFromString → 填 lutBytes/lutFloats/lutSize/lutIsHDR
static void DecodeCubeLUT_(Task& task) {
    FILE* fp = fopen(task.path.c_str(), "rb");
    if (!fp) {
        task.state->errorMsg = std::string("fopen failed: ") + task.path;
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        task.state->errorMsg = std::string("empty file: ") + task.path;
        return;
    }
    std::vector<char> buf((size_t)sz);
    size_t rd = fread(buf.data(), 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        task.state->errorMsg = std::string("fread truncated: ") + task.path;
        return;
    }

    char errBuf[256] = {0};
    int  size  = 0;
    bool isHDR = false;
    if (!HDRRenderer::ParseCubeLUTFromString(buf.data(), (size_t)sz,
                                              &size, &isHDR,
                                              &task.state->lutBytes,
                                              &task.state->lutFloats,
                                              nullptr,   // worker 不需要 domainMax (Phase G.1.4)
                                              errBuf, sizeof(errBuf))) {
        task.state->errorMsg = errBuf[0] ? errBuf : "ParseCubeLUTFromString unknown err";
        return;
    }
    task.state->lutSize  = size;
    task.state->lutIsHDR = isHDR;
}

// LUT HALD: ParseHaldLUTFile (内部含 stbi_load + 维度验证)
static void DecodeHaldLUT_(Task& task) {
    char errBuf[256] = {0};
    int  size  = 0;
    bool isHDR = false;
    if (!HDRRenderer::ParseHaldLUTFile(task.path.c_str(),
                                        &size, &isHDR,
                                        &task.state->lutBytes,
                                        &task.state->lutFloats,
                                        errBuf, sizeof(errBuf))) {
        task.state->errorMsg = errBuf[0] ? errBuf : "ParseHaldLUTFile unknown err";
        return;
    }
    task.state->lutSize  = size;
    task.state->lutIsHDR = isHDR;
}

// Font: fread TTF + stbtt_InitFont 验证 (主线程后续把 buffer 转交 FontContext)
static void DecodeFont_(Task& task) {
    FILE* fp = fopen(task.path.c_str(), "rb");
    if (!fp) {
        task.state->errorMsg = std::string("fopen failed: ") + task.path;
        return;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) {
        fclose(fp);
        task.state->errorMsg = std::string("empty TTF file: ") + task.path;
        return;
    }
    // malloc (FontContext::__gc 调 free, 与同步路径行为一致)
    unsigned char* ttfBuf = (unsigned char*)malloc((size_t)sz);
    if (!ttfBuf) {
        fclose(fp);
        task.state->errorMsg = "TTF malloc failed";
        return;
    }
    size_t rd = fread(ttfBuf, 1, (size_t)sz, fp);
    fclose(fp);
    if (rd != (size_t)sz) {
        free(ttfBuf);
        task.state->errorMsg = std::string("fread truncated: ") + task.path;
        return;
    }
    // 验证 TTF 头 (offset=0)
    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttfBuf, 0)) {
        free(ttfBuf);
        task.state->errorMsg = std::string("stbtt_InitFont failed (invalid TTF): ") + task.path;
        return;
    }
    // worker 阶段完成: 转交 buffer 给主线程 (Tick 内创建 FontContext)
    task.state->fontTTFBuffer = ttfBuf;
    task.state->fontTTFSize   = (int)sz;
    // fontSize / fontAtlasW / fontAtlasH 已在 LoadFontAsync 时填入
}

// Sound: ma_sound_init_from_file 在 worker thread (miniaudio 内部线程安全)
static void DecodeSound_(Task& task) {
    AudioHandle* h = AudioBackend::LoadFile(task.path.c_str());
    if (!h) {
        task.state->errorMsg = std::string("ma_sound_init_from_file failed (format/path): ") + task.path;
        return;
    }
    task.state->resSoundHandle = h;
}

// ==================== Phase G.1.5 — GLTF Material + Embedded Image (Worker) ====================

// 取 .gltf 文件所在目录 (含尾部分隔符), 用于解析相对纹理 URI.
// 与 light_graphics_mesh.cpp::GetGLTFDirectory 同语义, 简化版 (worker 内独立, 避免链 binding)
static std::string GetGLTFDirectory_(const char* gltfPath) {
    if (!gltfPath) return "";
    std::string p = gltfPath;
    size_t pos = p.find_last_of("/\\");
    if (pos == std::string::npos) return "";
    return p.substr(0, pos + 1);
}

// 从 cgltf_image 提取 raw 图像 bytes (3 来源, 与同步 LoadGLTFImage 等价).
// outBytes 保存解码到 stbi 之前的原始 PNG/JPG 字节流.
// 失败返 false, 不写 errBuf (worker 仅 log warn).
static bool ReadImageBytes_(const cgltf_image* img,
                              const std::string& gltfDir,
                              std::vector<uint8_t>& outBytes) {
    if (!img) return false;

    // case 1: GLB embedded buffer_view
    if (img->buffer_view) {
        const uint8_t* bvData = (const uint8_t*)cgltf_buffer_view_data(img->buffer_view);
        if (!bvData) return false;
        size_t sz = img->buffer_view->size;
        if (sz == 0) return false;
        outBytes.assign(bvData, bvData + sz);
        return true;
    }
    if (!img->uri) return false;

    // case 2: data URI (data:image/png;base64,XXX)
    if (strncmp(img->uri, "data:", 5) == 0) {
        const char* commaPos = strchr(img->uri, ',');
        if (!commaPos) return false;
        const char* b64    = commaPos + 1;
        size_t      b64Len = strlen(b64);
        // 估算解码后大小 (与同步路径完全一致)
        size_t decodedSize = (b64Len / 4) * 3;
        if (b64Len >= 2 && b64[b64Len - 1] == '=') {
            decodedSize--;
            if (b64[b64Len - 2] == '=') decodedSize--;
        }
        if (decodedSize == 0) return false;
        cgltf_options opts = {};
        void*         dec  = nullptr;
        cgltf_result  cr   = cgltf_load_buffer_base64(&opts, decodedSize, b64, &dec);
        if (cr != cgltf_result_success || !dec) return false;
        outBytes.assign((uint8_t*)dec, (uint8_t*)dec + decodedSize);
        free(dec);  // cgltf 默认 malloc
        return true;
    }

    // case 3: 相对文件路径 (worker 直接 fopen 读取)
    std::string fullPath = gltfDir + img->uri;
    FILE* fp = fopen(fullPath.c_str(), "rb");
    if (!fp) return false;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0) { fclose(fp); return false; }
    outBytes.resize((size_t)sz);
    size_t rd = fread(outBytes.data(), 1, (size_t)sz, fp);
    fclose(fp);
    return rd == (size_t)sz;
}

// 解码 cgltf_texture_view 引用的 image 到 RGBA8 pixels, 写入 FutureState.gltfMaterialImages.
// 任一步失败仅 log warn 跳过, 不破坏 mesh 主流程.
static void DecodeMaterialImage_(const cgltf_texture_view* view, int slotIdx,
                                  FutureState& st, const std::string& gltfDir) {
    if (!view || !view->texture || !view->texture->image) return;
    const cgltf_image* img = view->texture->image;

    std::vector<uint8_t> imgBytes;
    if (!ReadImageBytes_(img, gltfDir, imgBytes)) {
        CC::Log(CC::LOG_WARN,
                "AssetLoader: GLTF material slot %d image source unreachable (skipped)",
                slotIdx);
        return;
    }

    int w = 0, h = 0, ch = 0;
    unsigned char* pixels = stbi_load_from_memory(imgBytes.data(), (int)imgBytes.size(),
                                                    &w, &h, &ch, 4);
    if (!pixels || w <= 0 || h <= 0) {
        CC::Log(CC::LOG_WARN,
                "AssetLoader: GLTF material slot %d stbi decode failed: %s",
                slotIdx,
                stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        if (pixels) stbi_image_free(pixels);
        return;
    }

    MaterialImageJob job;
    job.slotIdx = slotIdx;
    job.w       = w;
    job.h       = h;
    job.pixels  = pixels;
    job.glTexId = 0;
    st.gltfMaterialImages.push_back(std::move(job));
}

// 提取 MaterialDesc 全数值字段 (texture id slot 留 0).
// 与 light_graphics_mesh.cpp::ExtractMaterial 数值部分等价, 但不调 LoadGLTFImage
// (worker thread 不持锁 backend, GL 调用走 WorkerUploadMesh_).
static void ExtractMaterial_NoTexture_(MaterialDesc& d, const cgltf_material* mat) {
    // 默认 PBR + 白底 (与同步路径一致)
    memset(&d, 0, sizeof(d));
    d.mode              = 1;            // PBR
    d.color[0]          = 1.0f;
    d.color[1]          = 1.0f;
    d.color[2]          = 1.0f;
    d.color[3]          = 1.0f;
    d.metallic          = 0.0f;
    d.roughness         = 1.0f;
    d.normalScale       = 1.0f;
    d.occlusionStrength = 1.0f;
    d.alphaMode         = 0;
    d.alphaCutoff       = 0.5f;
    d.doubleSided       = 0;

    if (!mat) return;

    d.mode = mat->unlit ? 0 : 1;

    if (mat->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness& pbr = mat->pbr_metallic_roughness;
        d.color[0]  = pbr.base_color_factor[0];
        d.color[1]  = pbr.base_color_factor[1];
        d.color[2]  = pbr.base_color_factor[2];
        d.color[3]  = pbr.base_color_factor[3];
        d.metallic  = pbr.metallic_factor;
        d.roughness = pbr.roughness_factor;
    }

    // normal_texture.scale (与同步路径一致, 是 normalScale)
    if (mat->normal_texture.texture) {
        d.normalScale = mat->normal_texture.scale;
    }

    // occlusion_texture.scale (cgltf 共用 cgltf_texture_view, scale 实为 strength)
    if (mat->occlusion_texture.texture) {
        d.occlusionStrength = mat->occlusion_texture.scale;
    }

    d.emissive[0] = mat->emissive_factor[0];
    d.emissive[1] = mat->emissive_factor[1];
    d.emissive[2] = mat->emissive_factor[2];

    switch (mat->alpha_mode) {
        case cgltf_alpha_mode_mask:  d.alphaMode = 2; break;
        case cgltf_alpha_mode_blend: d.alphaMode = 1; break;
        case cgltf_alpha_mode_opaque:
        default:                     d.alphaMode = 0; break;
    }
    d.alphaCutoff = mat->alpha_cutoff;
    d.doubleSided = mat->double_sided ? 1 : 0;
}

// GLTF: cgltf_parse + cgltf_load_buffers + 提取 primitive 顶点/索引到 malloc 数组
// Phase G.1.5: withMaterial=true 时额外提取 MaterialDesc + 5 类 PBR texture
static void DecodeGLTF_(Task& task) {
    cgltf_options opts = {};
    cgltf_data*   data = nullptr;
    cgltf_result  r    = cgltf_parse_file(&opts, task.path.c_str(), &data);
    if (r != cgltf_result_success) {
        task.state->errorMsg = std::string("cgltf_parse_file err ") +
                               std::to_string((int)r) + ": " + task.path;
        return;
    }
    r = cgltf_load_buffers(&opts, data, task.path.c_str());
    if (r != cgltf_result_success) {
        cgltf_free(data);
        task.state->errorMsg = std::string("cgltf_load_buffers err ") +
                               std::to_string((int)r) + ": " + task.path;
        return;
    }

    // 计算总 primitive 数
    size_t total = 0;
    for (size_t i = 0; i < data->meshes_count; ++i) total += data->meshes[i].primitives_count;
    if ((size_t)task.state->gltfPrimIdx >= total) {
        cgltf_free(data);
        task.state->errorMsg = "primitive index out of range (have " + std::to_string(total) + ")";
        return;
    }

    // 找到第 N primitive
    const cgltf_primitive* prim = nullptr;
    {
        size_t cursor = 0;
        for (size_t i = 0; i < data->meshes_count && !prim; ++i) {
            const cgltf_mesh& m = data->meshes[i];
            if ((size_t)task.state->gltfPrimIdx < cursor + m.primitives_count) {
                prim = &m.primitives[task.state->gltfPrimIdx - cursor];
            }
            cursor += m.primitives_count;
        }
    }
    if (!prim) {
        cgltf_free(data);
        task.state->errorMsg = "primitive lookup unexpectedly failed";
        return;
    }

    // 提取 attributes (POSITION 必须)
    const cgltf_accessor* accPos   = nullptr;
    const cgltf_accessor* accNorm  = nullptr;
    const cgltf_accessor* accUV    = nullptr;
    const cgltf_accessor* accColor = nullptr;
    for (size_t a = 0; a < prim->attributes_count; ++a) {
        const cgltf_attribute& at = prim->attributes[a];
        if (at.index != 0) continue;
        switch (at.type) {
            case cgltf_attribute_type_position: accPos   = at.data; break;
            case cgltf_attribute_type_normal:   accNorm  = at.data; break;
            case cgltf_attribute_type_texcoord: accUV    = at.data; break;
            case cgltf_attribute_type_color:    accColor = at.data; break;
            default: break;
        }
    }
    if (!accPos || accPos->count == 0) {
        cgltf_free(data);
        task.state->errorMsg = "primitive has no POSITION";
        return;
    }
    const size_t vCount = accPos->count;
    if (vCount > 1000000) {
        cgltf_free(data);
        task.state->errorMsg = "primitive vertex count > 1M soft limit";
        return;
    }

    // unpack 到 malloc 数组 (RenderVertex3D 12 floats/v)
    RenderVertex3D* verts = (RenderVertex3D*)malloc(sizeof(RenderVertex3D) * vCount);
    if (!verts) {
        cgltf_free(data);
        task.state->errorMsg = "vertex malloc failed";
        return;
    }
    std::vector<float> posData(vCount * 3);
    cgltf_accessor_unpack_floats(accPos, posData.data(), vCount * 3);

    std::vector<float> normData;
    if (accNorm && accNorm->count == vCount && cgltf_num_components(accNorm->type) == 3) {
        normData.resize(vCount * 3);
        cgltf_accessor_unpack_floats(accNorm, normData.data(), vCount * 3);
    }
    std::vector<float> uvData;
    if (accUV && accUV->count == vCount && cgltf_num_components(accUV->type) == 2) {
        uvData.resize(vCount * 2);
        cgltf_accessor_unpack_floats(accUV, uvData.data(), vCount * 2);
    }
    std::vector<float> colorData;
    int colorComp = 0;
    if (accColor && accColor->count == vCount) {
        size_t comp = cgltf_num_components(accColor->type);
        if (comp == 3 || comp == 4) {
            colorComp = (int)comp;
            colorData.resize(vCount * comp);
            cgltf_accessor_unpack_floats(accColor, colorData.data(), vCount * comp);
        }
    }

    for (size_t i = 0; i < vCount; ++i) {
        RenderVertex3D& v = verts[i];
        v.x = posData[i * 3 + 0];
        v.y = posData[i * 3 + 1];
        v.z = posData[i * 3 + 2];
        if (!normData.empty()) {
            v.nx = normData[i * 3 + 0]; v.ny = normData[i * 3 + 1]; v.nz = normData[i * 3 + 2];
        } else {
            v.nx = 0.0f; v.ny = 1.0f; v.nz = 0.0f;
        }
        if (!uvData.empty()) {
            v.u = uvData[i * 2 + 0]; v.v = uvData[i * 2 + 1];
        } else {
            v.u = 0.0f; v.v = 0.0f;
        }
        if (!colorData.empty()) {
            v.r = colorData[i * colorComp + 0];
            v.g = colorData[i * colorComp + 1];
            v.b = colorData[i * colorComp + 2];
            v.a = (colorComp == 4) ? colorData[i * 4 + 3] : 1.0f;
        } else {
            v.r = v.g = v.b = v.a = 1.0f;
        }
    }

    // 索引: 有则提取, 无则自动生成
    uint32_t* indices = nullptr;
    int       idxCount = 0;
    if (prim->indices) {
        const size_t iCount = prim->indices->count;
        if (iCount > 3000000) {
            free(verts);
            cgltf_free(data);
            task.state->errorMsg = "index count > 3M soft limit";
            return;
        }
        indices = (uint32_t*)malloc(sizeof(uint32_t) * iCount);
        if (!indices) {
            free(verts);
            cgltf_free(data);
            task.state->errorMsg = "index malloc failed";
            return;
        }
        cgltf_accessor_unpack_indices(prim->indices, indices, sizeof(uint32_t), iCount);
        idxCount = (int)iCount;
    } else {
        indices = (uint32_t*)malloc(sizeof(uint32_t) * vCount);
        if (!indices) {
            free(verts);
            cgltf_free(data);
            task.state->errorMsg = "index (auto) malloc failed";
            return;
        }
        for (size_t i = 0; i < vCount; ++i) indices[i] = (uint32_t)i;
        idxCount = (int)vCount;
    }

    // worker 阶段完成. cgltf_data 在主线程 Tick 时 free (释 prim 指针之后)
    task.state->gltfData      = data;
    task.state->gltfVerts     = verts;
    task.state->gltfVertCount = (int)vCount;
    task.state->gltfIndices   = indices;
    task.state->gltfIdxCount  = idxCount;

    // Phase G.1.5 — withMaterial: 提取 MaterialDesc 数值字段 + 解码 5 类 PBR texture
    // cgltf_data 还活着 (主线程 Tick 时 free), prim 指针有效, image data 引用 buffer_view 可读
    if (task.state->gltfWithMaterial) {
        static_assert(sizeof(MaterialDesc) <= 128,
                      "MaterialDesc exceeds gltfMaterialDesc[128] buffer; bump capacity in asset_loader.h");

        MaterialDesc md{};
        ExtractMaterial_NoTexture_(md, prim->material);
        memcpy(task.state->gltfMaterialDesc, &md, sizeof(MaterialDesc));

        if (prim->material) {
            std::string gltfDir = GetGLTFDirectory_(task.path.c_str());
            const cgltf_material* mat = prim->material;
            // 5 个 PBR texture slot (与 MaterialDesc::tex* 字段顺序一致):
            //   0=baseColor / 1=metallicRoughness / 2=normal / 3=emissive / 4=occlusion
            if (mat->has_pbr_metallic_roughness) {
                DecodeMaterialImage_(&mat->pbr_metallic_roughness.base_color_texture,
                                      0, *task.state, gltfDir);
                DecodeMaterialImage_(&mat->pbr_metallic_roughness.metallic_roughness_texture,
                                      1, *task.state, gltfDir);
            }
            DecodeMaterialImage_(&mat->normal_texture,    2, *task.state, gltfDir);
            DecodeMaterialImage_(&mat->emissive_texture,  3, *task.state, gltfDir);
            DecodeMaterialImage_(&mat->occlusion_texture, 4, *task.state, gltfDir);
        }
    }
}

// ==================== Phase G.1.1 — Worker 直接 GL 上传 ====================
// 仅桌面平台编译; 移动 / Web 平台 g_sharedCtxOk 永远 false, 永远不会调到这些函数.
// 函数职责: 把 worker 解码完的 raw buffer 直接 glTexImage* 上传, 写入 state 的资源 id
//          + glFenceSync. 主线程 Tick 用 glClientWaitSync 翻 status.

#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)

// Worker 上传 2D 纹理 (Image). 与 GL33Core::CreateTexture(channels=4) 路径等价.
// 成功: 设 state->resTexId + state->glFence; 调用方释放 imgPixels.
// 失败: 写 state->errorMsg.
static void WorkerUploadImage_(Task& task) {
    auto& st = *task.state;
    if (!st.imgPixels || st.imgW <= 0 || st.imgH <= 0) {
        st.errorMsg = "WorkerUploadImage: invalid pixels";
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        st.errorMsg = "WorkerUploadImage: glGenTextures failed";
        return;
    }
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // worker DecodeImage_ 用 stbi_load(.., 4) force RGBA; channels 字段记录原始数, 上传按 RGBA8
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                 st.imgW, st.imgH, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, st.imgPixels);
    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_2D, 0);
    if (err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WorkerUploadImage: glTexImage2D err=0x%x", err);
        st.errorMsg = buf;
        return;
    }
    // 释放 raw pixels (主线程 UploadImage_ 路径在上传完也做这一步, 这里复用)
    stbi_image_free(st.imgPixels);
    st.imgPixels = nullptr;
    st.resTexId  = (uint32_t)tex;
    glFlush();   // 确保命令进入 GPU 队列, 让 fence 在主线程 ClientWaitSync 时可见
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    st.glFence   = (void*)fence;
}

// Worker 上传 3D mesh (GLTF). 与 GL33Core::CreateMesh 的 GL 部分等价;
// C++ 共享状态 (nextMeshId++ + meshes[id]=m) 留给主线程 Tick 调 RegisterUploadedMesh.
// 成功: 设 state->glMeshVao/Vbo/Ebo/IdxCount + state->glFence; resMeshId 主线程写.
// 失败: glDelete 已 gen 的 handles + 写 errorMsg.
static void WorkerUploadMesh_(Task& task) {
    auto& st = *task.state;
    if (!st.gltfVerts || !st.gltfIndices || st.gltfVertCount <= 0 || st.gltfIdxCount <= 0) {
        st.errorMsg = "WorkerUploadMesh: gltf arrays missing";
        return;
    }
    GLuint vao = 0, vbo = 0, ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    if (!vao || !vbo || !ebo) {
        if (vao) glDeleteVertexArrays(1, &vao);
        if (vbo) glDeleteBuffers(1, &vbo);
        if (ebo) glDeleteBuffers(1, &ebo);
        st.errorMsg = "WorkerUploadMesh: glGen* failed";
        return;
    }
    glBindVertexArray(vao);

    // VBO: 上传顶点 (与 CreateMesh 等价的 RenderVertex3D 布局, sizeof=48)
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)st.gltfVertCount * (GLsizeiptr)sizeof(RenderVertex3D),
                 st.gltfVerts, GL_STATIC_DRAW);

    // EBO: 上传索引 (uint32, 允许顶点 > 65536)
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)st.gltfIdxCount * (GLsizeiptr)sizeof(uint32_t),
                 st.gltfIndices, GL_STATIC_DRAW);

    // 顶点属性 layout: pos(0), normal(1), uv(2), color(3) — 与 CreateMesh 严格一致
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                          (void*)offsetof(RenderVertex3D, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                          (void*)offsetof(RenderVertex3D, nx));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                          (void*)offsetof(RenderVertex3D, u));
    glEnableVertexAttribArray(3);
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(RenderVertex3D),
                          (void*)offsetof(RenderVertex3D, r));

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        glDeleteVertexArrays(1, &vao);
        glDeleteBuffers(1, &vbo);
        glDeleteBuffers(1, &ebo);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WorkerUploadMesh: gl err=0x%x", err);
        st.errorMsg = buf;
        return;
    }

    // 释放顶点/索引中间数据 (与主线程 UploadGLTF_ 路径一致) +  cgltf_data
    free(st.gltfVerts);   st.gltfVerts   = nullptr;
    free(st.gltfIndices); st.gltfIndices = nullptr;
    if (st.gltfData) {
        cgltf_free((cgltf_data*)st.gltfData);
        st.gltfData = nullptr;
    }

    st.glMeshVao      = (uint32_t)vao;
    st.glMeshVbo      = (uint32_t)vbo;
    st.glMeshEbo      = (uint32_t)ebo;
    st.glMeshIdxCount = st.gltfIdxCount;

    // Phase G.1.5 — withMaterial: 串行上传 5 类 PBR texture (与 CreateTexture(channels=4) 等价).
    // 任一 texture 失败仅 log warn, 不破坏 mesh; 释放本 job pixels 后继续下一个.
    // single fence 在最后 glFlush 共用 — GL 命令队列保证顺序完成, 主线程 ClientWaitSync 能感知.
    if (st.gltfWithMaterial) {
        for (auto& job : st.gltfMaterialImages) {
            if (!job.pixels || job.w <= 0 || job.h <= 0) continue;
            GLuint tex = 0;
            glGenTextures(1, &tex);
            if (!tex) {
                CC::Log(CC::LOG_WARN,
                        "WorkerUploadMesh: glGenTextures failed for material slot %d (skipped)",
                        job.slotIdx);
                stbi_image_free(job.pixels); job.pixels = nullptr;
                continue;
            }
            glBindTexture(GL_TEXTURE_2D, tex);
            // Phase G.1.5 收尾 T2 — PBR material texture 启用 mipmap (LINEAR_MIPMAP_LINEAR 三线性过滤).
            // 对比直接 LINEAR: 远距离/低分辨率采样抗锯齿明显, 是 PBR 渲染标配.
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
                         job.w, job.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, job.pixels);
            GLenum texErr = glGetError();
            if (texErr != GL_NO_ERROR) {
                glBindTexture(GL_TEXTURE_2D, 0);
                glDeleteTextures(1, &tex);
                CC::Log(CC::LOG_WARN,
                        "WorkerUploadMesh: material slot %d glTexImage2D err=0x%x (skipped)",
                        job.slotIdx, (unsigned)texErr);
                stbi_image_free(job.pixels); job.pixels = nullptr;
                continue;
            }
            // Phase G.1.5 收尾 T2 — 生成 mipmap 链 (失败仅 log warn, 不破坏 mesh; 退化为 base level 采样).
            glGenerateMipmap(GL_TEXTURE_2D);
            GLenum mipErr = glGetError();
            if (mipErr != GL_NO_ERROR) {
                // mipmap 失败时 fallback 到 LINEAR (避免采样到未初始化的 mip level)
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                CC::Log(CC::LOG_WARN,
                        "WorkerUploadMesh: material slot %d glGenerateMipmap err=0x%x (fallback to LINEAR)",
                        job.slotIdx, (unsigned)mipErr);
            }
            glBindTexture(GL_TEXTURE_2D, 0);
            stbi_image_free(job.pixels); job.pixels = nullptr;
            job.glTexId = (uint32_t)tex;
        }
    }

    glFlush();   // 让 mesh + 5 textures 命令进入 GPU 队列
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    st.glFence    = (void*)fence;
}

// Worker 上传 3D LUT 纹理 (Cube / HALD 共享). 与 GL33Core::CreateLUT3D[Float] 等价.
static void WorkerUploadLUT_(Task& task) {
    auto& st = *task.state;
    if (st.lutSize < 4 || st.lutSize > 64) {
        st.errorMsg = "WorkerUploadLUT: size out of range";
        return;
    }
    const bool isHDR = st.lutIsHDR;
    if (isHDR && st.lutFloats.empty()) {
        st.errorMsg = "WorkerUploadLUT: HDR LUT but lutFloats empty";
        return;
    }
    if (!isHDR && st.lutBytes.empty()) {
        st.errorMsg = "WorkerUploadLUT: LDR LUT but lutBytes empty";
        return;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        st.errorMsg = "WorkerUploadLUT: glGenTextures failed";
        return;
    }
    glBindTexture(GL_TEXTURE_3D, tex);
    if (isHDR) {
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB16F,
                     st.lutSize, st.lutSize, st.lutSize,
                     0, GL_RGB, GL_FLOAT, st.lutFloats.data());
    } else {
        glTexImage3D(GL_TEXTURE_3D, 0, GL_RGB8,
                     st.lutSize, st.lutSize, st.lutSize,
                     0, GL_RGB, GL_UNSIGNED_BYTE, st.lutBytes.data());
    }
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    GLenum err = glGetError();
    glBindTexture(GL_TEXTURE_3D, 0);
    if (err != GL_NO_ERROR) {
        glDeleteTextures(1, &tex);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "WorkerUploadLUT: glTexImage3D err=0x%x", err);
        st.errorMsg = buf;
        return;
    }
    // 释放中间数据
    st.lutBytes.clear();  st.lutBytes.shrink_to_fit();
    st.lutFloats.clear(); st.lutFloats.shrink_to_fit();
    st.resLUTId = (uint32_t)tex;
    glFlush();
    GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    st.glFence  = (void*)fence;
}

#endif // !移动 / !Web

// ==================== Worker 主循环 ====================

static void WorkerMain() {
    // Phase G.1.1 — 若 probe 成功, worker 拥有自己的 GL ctx
    // (light_ui.cpp 内 Init 调用前主 ctx 已 current; CreateSharedGLContext 后新 ctx 是 current,
    //  Init 立刻拉回主 ctx; 此处 worker 线程把共享 ctx MakeCurrent 到本线程)
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
    if (g_sharedCtxOk.load() && g_workerCtx && g_mainWin) {
        PlatformWindow::MakeCurrent(g_mainWin, g_workerCtx);
    }
#endif

    // 用 thread-local flip flag (与现有 stb 行为一致: 0 = 不翻转)
    stbi_set_flip_vertically_on_load_thread(0);

    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(g_taskMutex);
            g_taskCv.wait(lk, []{ return g_shouldStop.load() || !g_taskQueue.empty(); });
            if (g_shouldStop.load() && g_taskQueue.empty()) break;
            task = std::move(g_taskQueue.front());
            g_taskQueue.pop_front();
        }

        // Step 1: dispatch 到对应解码器. 解码失败时填 errorMsg, 不翻 status (主线程 Tick 统一翻).
        switch (task.type) {
            case TaskType::Image:    DecodeImage_(task);   break;
            case TaskType::LUT_Cube: DecodeCubeLUT_(task); break;
            case TaskType::LUT_Hald: DecodeHaldLUT_(task); break;
            case TaskType::Font:     DecodeFont_(task);    break;
            case TaskType::Sound:    DecodeSound_(task);   break;
            case TaskType::GLTF:     DecodeGLTF_(task);    break;
        }

        // Step 2: Phase G.1.1 — 若 shared ctx OK 且解码成功, worker 直接 GL 上传 + fence
        //         (Image / LUT 两类; Mesh 留 G.1.2; Font/Sound 主线程上传无 GL 操作)
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
        if (g_sharedCtxOk.load() && task.state && task.state->errorMsg.empty()) {
            switch (task.type) {
                case TaskType::Image:    WorkerUploadImage_(task); break;
                case TaskType::LUT_Cube:
                case TaskType::LUT_Hald: WorkerUploadLUT_(task);   break;
                case TaskType::GLTF:
                    // Phase G.1.5 fix: WorkerUploadMesh_ 需 GL 3.0+ (glGenVertexArrays/glVertexAttribPointer).
                    // Legacy GL 2.x backend (CI software runner / 老硬件) 上这些函数指针为 NULL → crash.
                    // 跳过 worker 上传 → 主线程 Tick UploadGLTF_ fallback (内部已有 Supports3D check 转 Error).
                    if (g_render && g_render->Supports3D()) {
                        WorkerUploadMesh_(task);
                    }
                    break;
                default: break;   // Font / Sound 走主线程 Tick (无 GL 操作)
            }
        }
#endif

        // Step 3: push 到 result_queue (无论成功/失败)
        {
            std::lock_guard<std::mutex> lk(g_resultMutex);
            g_resultQueue.push_back(std::move(task));
        }
    }

    CC::Log(CC::LOG_INFO, "AssetLoader: worker thread exit");
}

} // anonymous namespace

// ==================== 生命周期 ====================

bool Init(void* mainWin, void* mainCtx) {
    if (g_running.load()) {
        CC::Log(CC::LOG_WARN, "AssetLoader::Init: already running");
        return true;
    }
    g_mainWin = mainWin;
    g_mainCtx = mainCtx;
    g_workerCtx = nullptr;
    g_sharedCtxOk.store(false);
    g_shouldStop.store(false);

    // Phase G.1.1 — probe Shared GL Context
    // 调用前提: 主 ctx 已 MakeCurrent 到本 (主) 线程 (由 light_ui.cpp::Window:Open 保证).
    // CreateSharedGLContext 会让新 ctx 成为 current, 这里立即拉回主 ctx, 交由 worker 线程 MakeCurrent.
    if (mainWin && mainCtx) {
        void* sharedCtx = PlatformWindow::CreateSharedGLContext(mainWin);
        if (sharedCtx) {
            // SDL_GL_CreateContext 后新 ctx is current; 拉回主 ctx 保障主线程后续 GL 调用
            PlatformWindow::MakeCurrent(mainWin, mainCtx);
            g_workerCtx = sharedCtx;
            g_sharedCtxOk.store(true);
        }
    }

    try {
        g_worker = std::thread(WorkerMain);
    } catch (const std::exception& e) {
        CC::Log(CC::LOG_ERROR, "AssetLoader::Init: thread spawn failed: %s", e.what());
        // 清理 probe 成果
        if (g_workerCtx) {
            PlatformWindow::DestroyGLContext(g_workerCtx);
            g_workerCtx = nullptr;
        }
        g_sharedCtxOk.store(false);
        return false;
    }
    g_running.store(true);

    CC::Log(CC::LOG_INFO,
            g_sharedCtxOk.load()
                ? "AssetLoader: Shared GL Context enabled (worker direct upload + fence)"
                : "AssetLoader: fallback to main-thread upload (probe failed or unsupported)");
    return true;
}

void Shutdown() {
    if (!g_running.load()) return;
    g_shouldStop.store(true);
    g_taskCv.notify_all();
    if (g_worker.joinable()) g_worker.join();
    g_running.store(false);

    // 把 pending task 也标 Error, 让 Lua 端 Get 不会无限等
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        for (auto& t : g_taskQueue) {
            if (t.state) {
                t.state->errorMsg = "AssetLoader shutdown";
                t.state->status.store((int)FutureStatus::Error);
            }
        }
        g_taskQueue.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        // result_queue 里没 dispatch 的也直接清掉 (上传未做), 标 Error
        // Phase G.1.1 — 如果带 fence (worker 已上传但主线程未翻 status), 清理 fence
        for (auto& t : g_resultQueue) {
            if (!t.state) continue;
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
            if (t.state->glFence) {
                glDeleteSync((GLsync)t.state->glFence);
                t.state->glFence = nullptr;
            }
            // Phase G.1.2 — 未注册的 mesh handles 兜底 glDelete
            if (t.state->glMeshVao) { GLuint v = t.state->glMeshVao; glDeleteVertexArrays(1, &v); t.state->glMeshVao = 0; }
            if (t.state->glMeshVbo) { GLuint v = t.state->glMeshVbo; glDeleteBuffers(1, &v);      t.state->glMeshVbo = 0; }
            if (t.state->glMeshEbo) { GLuint v = t.state->glMeshEbo; glDeleteBuffers(1, &v);      t.state->glMeshEbo = 0; }
#endif
            if (t.state->status.load() == (int)FutureStatus::Pending) {
                t.state->errorMsg = "AssetLoader shutdown before main-thread dispatch";
                t.state->status.store((int)FutureStatus::Error);
            }
        }
        g_resultQueue.clear();
    }

    // Phase G.1.1 — 销毁 worker 共享 ctx (worker join 后主 ctx 仍 current, 安全删除)
    if (g_workerCtx) {
        PlatformWindow::DestroyGLContext(g_workerCtx);
        g_workerCtx = nullptr;
    }
    g_sharedCtxOk.store(false);
    g_mainWin = nullptr;
    g_mainCtx = nullptr;

    CC::Log(CC::LOG_INFO, "AssetLoader: shutdown complete");
}

bool IsRunning() { return g_running.load(); }

// ==================== Tick ====================
// 主线程每帧调一次. 6 类资源 dispatch + GL 上传/资源注册 + Lua callback 唤起.

namespace {

// Image: 主线程 backend->CreateTexture (worker 已 stbi_load)
static void UploadImage_(FutureState& st, const std::string& path) {
    if (!st.imgPixels || st.imgW <= 0 || st.imgH <= 0) {
        st.errorMsg = "internal: pixels missing in result";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    uint32_t texId = 0;
    if (g_render) {
        texId = g_render->CreateTexture(st.imgW, st.imgH, st.imgChannels,
                                        (const unsigned char*)st.imgPixels);
    }
    if (!texId) {
        st.errorMsg = "g_render->CreateTexture failed (no backend or GL error)";
        st.status.store((int)FutureStatus::Error);
        return;   // pixels 在 dtor 中 stbi_image_free
    }
    st.resTexId = texId;
    stbi_image_free(st.imgPixels);
    st.imgPixels = nullptr;
    CC::Log(CC::LOG_INFO,
            "AssetLoader: Image async upload ok (%dx%d, texId=%u, path=%s)",
            st.imgW, st.imgH, texId, path.c_str());
    st.status.store((int)FutureStatus::Ready);
}

// LUT (cube + hald 共享): 主线程 backend->CreateLUT3D[Float]
static void UploadLUT_(FutureState& st, const std::string& path) {
    if (st.lutSize < 4 || st.lutSize > 64) {
        st.errorMsg = "internal: LUT size out of range";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    char errBuf[256] = {0};
    uint32_t id = HDRRenderer::UploadParsedLUT(st.lutSize, st.lutIsHDR,
                                                st.lutBytes, st.lutFloats,
                                                errBuf, sizeof(errBuf));
    if (!id) {
        st.errorMsg = errBuf[0] ? errBuf : "UploadParsedLUT failed";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    st.resLUTId = id;
    // 上传后释放数据, 不再占内存
    st.lutBytes.clear();  st.lutBytes.shrink_to_fit();
    st.lutFloats.clear(); st.lutFloats.shrink_to_fit();
    CC::Log(CC::LOG_INFO,
            "AssetLoader: LUT async upload ok (size=%d, isHDR=%d, lutId=%u, path=%s)",
            st.lutSize, st.lutIsHDR ? 1 : 0, id, path.c_str());
    st.status.store((int)FutureStatus::Ready);
}

// Font: 主线程不创 GL (lazy bake), 仅打 log + status=Ready, Lua dispatcher 创 FontContext userdata
static void UploadFont_(FutureState& st, const std::string& path) {
    if (!st.fontTTFBuffer || st.fontTTFSize <= 0) {
        st.errorMsg = "internal: TTF buffer missing in result";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    // ownership 仍在 FutureState, dispatcher 创 FontContext 时转移
    CC::Log(CC::LOG_INFO,
            "AssetLoader: Font async ready (size=%.1f, ttfBytes=%d, path=%s)",
            st.fontSize, st.fontTTFSize, path.c_str());
    st.status.store((int)FutureStatus::Ready);
}

// Sound: 主线程不创 GL, 句柄已在 worker 创建; 仅标 Ready, Lua dispatcher 包 SoundUserdata
static void UploadSound_(FutureState& st, const std::string& path) {
    if (!st.resSoundHandle) {
        st.errorMsg = "internal: AudioHandle missing in result";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    CC::Log(CC::LOG_INFO, "AssetLoader: Sound async ready (path=%s)", path.c_str());
    st.status.store((int)FutureStatus::Ready);
}

// Phase G.1.5 — 主线程 helper: 把 5 类 PBR texture id 写入 MaterialDesc 5 slots.
// 反序列化 MaterialDesc 字节 → 按 slotIdx 写 texXxx → 重新序列化回 char[128].
// 等价于 light_graphics_material.cpp::TexSlotPtr 的指针映射 (worker 内独立 helper).
static void WriteMaterialTextureSlots_(FutureState& st) {
    if (!st.gltfWithMaterial) return;
    MaterialDesc d{};
    memcpy(&d, st.gltfMaterialDesc, sizeof(MaterialDesc));
    int written = 0;
    for (auto& job : st.gltfMaterialImages) {
        if (job.glTexId == 0) continue;
        switch (job.slotIdx) {
            case 0: d.texBaseColor         = job.glTexId; ++written; break;
            case 1: d.texMetallicRoughness = job.glTexId; ++written; break;
            case 2: d.texNormal            = job.glTexId; ++written; break;
            case 3: d.texEmissive          = job.glTexId; ++written; break;
            case 4: d.texOcclusion         = job.glTexId; ++written; break;
            default: break;   // 未知 slot 跳过
        }
    }
    memcpy(st.gltfMaterialDesc, &d, sizeof(MaterialDesc));
    if (written > 0) {
        CC::Log(CC::LOG_INFO,
                "AssetLoader: GLTF material textures upload ok (slots=%d)", written);
    }
}

// GLTF: 主线程 backend->CreateMesh (vertex/index 数组已在 worker malloc)
// Phase G.1.5: withMaterial=true 时额外串行 5 textures CreateTexture + WriteSlots
static void UploadGLTF_(FutureState& st, const std::string& path) {
    if (!st.gltfVerts || !st.gltfIndices || st.gltfVertCount <= 0 || st.gltfIdxCount <= 0) {
        st.errorMsg = "internal: GLTF arrays missing in result";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    if (!g_render || !g_render->Supports3D()) {
        st.errorMsg = "render backend does not support 3D mesh";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    uint32_t meshId = g_render->CreateMesh(
        (const RenderVertex3D*)st.gltfVerts, st.gltfVertCount,
        (const uint32_t*)st.gltfIndices,     st.gltfIdxCount);
    if (!meshId) {
        st.errorMsg = "g_render->CreateMesh failed (GL OOM or invalid data)";
        st.status.store((int)FutureStatus::Error);
        return;
    }
    st.resMeshId = meshId;
    // 释放中间数据
    free(st.gltfVerts);   st.gltfVerts   = nullptr;
    free(st.gltfIndices); st.gltfIndices = nullptr;
    if (st.gltfData) {
        cgltf_free((cgltf_data*)st.gltfData);
        st.gltfData = nullptr;
    }
    CC::Log(CC::LOG_INFO,
            "AssetLoader: GLTF async upload ok (verts=%d, idx=%d, meshId=%u, path=%s)",
            st.gltfVertCount, st.gltfIdxCount, meshId, path.c_str());

    // Phase G.1.5 — withMaterial fallback 路径: 串行 CreateTexture × N + WriteSlots
    if (st.gltfWithMaterial) {
        for (auto& job : st.gltfMaterialImages) {
            if (!job.pixels || job.w <= 0 || job.h <= 0) continue;
            uint32_t texId = g_render->CreateTexture(job.w, job.h, 4, job.pixels);
            if (texId) {
                // Phase G.1.5 收尾 T2 — 启用 mipmap (LINEAR_MIPMAP_LINEAR 三线性过滤).
                // 与 worker 路径一致, 失败时 backend 内部已 no-op (Legacy) 或 fallback (GL33).
                g_render->GenerateMipmap2D(texId);
                job.glTexId = texId;
            } else {
                CC::Log(CC::LOG_WARN,
                        "UploadGLTF: material slot %d CreateTexture failed (slot 0)",
                        job.slotIdx);
            }
            stbi_image_free(job.pixels);
            job.pixels = nullptr;
        }
        WriteMaterialTextureSlots_(st);
    }

    st.status.store((int)FutureStatus::Ready);
}

}  // anonymous namespace

// Phase G.1.1 — fence 结果判断
// Returns: 0=Ready, 1=未完成需重试, 2=失败转Error
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
static int CheckFenceState_(FutureState& st) {
    if (!st.glFence) return 0;   // 无 fence 路径 → 当作已 Ready
    GLsync fence = (GLsync)st.glFence;
    GLenum r = glClientWaitSync(fence, 0, 0);
    if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
        glDeleteSync(fence);
        st.glFence = nullptr;
        return 0;
    }
    if (r == GL_TIMEOUT_EXPIRED) {
        if (++st.fenceWaitFrames >= kFenceMaxWaitFrames) {
            glDeleteSync(fence);
            st.glFence = nullptr;
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "fence wait exceeded %d frames", kFenceMaxWaitFrames);
            st.errorMsg = buf;
            return 2;
        }
        return 1;
    }
    // GL_WAIT_FAILED 或其他异常
    glDeleteSync(fence);
    st.glFence = nullptr;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "glClientWaitSync failed (r=0x%x)", r);
    st.errorMsg = buf;
    return 2;
}
#endif

void Tick() {
    if (!g_running.load()) return;

    // 把 result_queue 里的全部 task 一次性 drain (避免持锁太久)
    std::deque<Task> local;
    {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        if (g_resultQueue.empty()) return;
        local.swap(g_resultQueue);
    }

    // Phase G.1.1 — fence 未完成的 task 走这里, 循环末批量放回 g_resultQueue 下帧重试
    std::deque<Task> retry;

    for (auto& task : local) {
        auto& st = task.state;
        if (!st) continue;

        // 已被 Shutdown 标过 Error, 跳过
        if (st->status.load() == (int)FutureStatus::Error) continue;

        // Worker 解码失败时 errorMsg 非空, 翻 Error
        if (!st->errorMsg.empty()) {
            st->status.store((int)FutureStatus::Error);
        }
#if !defined(__EMSCRIPTEN__) && !defined(__ANDROID__) && !defined(CHOCO_PLATFORM_IOS)
        // Phase G.1.1 — worker 已上传 + fence 路径: 查 fence, 未完成放回重试
        else if (st->glFence) {
            int r = CheckFenceState_(*st);
            if (r == 1) {
                retry.push_back(std::move(task));
                continue;   // 未完成, 不走 dispatch
            }
            if (r == 2) {
                // fence 失败: glDelete worker 已创建的 mesh handles 兜底 (其他资源类型无 handle 留下)
                if (st->glMeshVao) { GLuint v = st->glMeshVao; glDeleteVertexArrays(1, &v); st->glMeshVao = 0; }
                if (st->glMeshVbo) { GLuint v = st->glMeshVbo; glDeleteBuffers(1, &v);      st->glMeshVbo = 0; }
                if (st->glMeshEbo) { GLuint v = st->glMeshEbo; glDeleteBuffers(1, &v);      st->glMeshEbo = 0; }
                // Phase G.1.5 — fence 失败兜底: worker 已上传的 material textures 释放
                for (auto& job : st->gltfMaterialImages) {
                    if (job.glTexId) { GLuint t = job.glTexId; glDeleteTextures(1, &t); job.glTexId = 0; }
                }
                st->status.store((int)FutureStatus::Error);
            } else {
                // r==0: fence Ready
                // Image / LUT: resTexId / resLUTId 已由 worker 写好, 直接翻 Ready
                // Mesh:  worker 写了 glMesh* handles, 主线程调 RegisterUploadedMesh 完成 backend 注册
                if (task.type == TaskType::GLTF) {
                    uint32_t meshId = 0;
                    if (g_render && st->glMeshVao && st->glMeshVbo && st->glMeshEbo) {
                        meshId = g_render->RegisterUploadedMesh(
                            st->glMeshVao, st->glMeshVbo, st->glMeshEbo, st->glMeshIdxCount);
                    }
                    if (!meshId) {
                        // backend 拒绝注册 → glDelete 兜底 + 转 Error
                        if (st->glMeshVao) { GLuint v = st->glMeshVao; glDeleteVertexArrays(1, &v); }
                        if (st->glMeshVbo) { GLuint v = st->glMeshVbo; glDeleteBuffers(1, &v); }
                        if (st->glMeshEbo) { GLuint v = st->glMeshEbo; glDeleteBuffers(1, &v); }
                        // Phase G.1.5 — backend 注册失败也兜底 textures
                        for (auto& job : st->gltfMaterialImages) {
                            if (job.glTexId) { GLuint t = job.glTexId; glDeleteTextures(1, &t); job.glTexId = 0; }
                        }
                        st->errorMsg = "RegisterUploadedMesh failed (backend Supports3D=false?)";
                        st->status.store((int)FutureStatus::Error);
                    } else {
                        // backend 接管了 handles, 清空 state 以避免 dtor 兜底派发 glDelete
                        st->glMeshVao = 0; st->glMeshVbo = 0; st->glMeshEbo = 0;
                        st->resMeshId = meshId;
                        // Phase G.1.5 — withMaterial: 把 worker 已上传的 5 GL texId 写入 MaterialDesc
                        // 写完后 textures 由 g_render 全局生命周期管理, dtor 不再 glDelete
                        WriteMaterialTextureSlots_(*st);
                        for (auto& job : st->gltfMaterialImages) {
                            // 表明 textures 已交接 (写入 MaterialDesc), dtor 不再 glDelete
                            job.glTexId = 0;
                        }
                        st->status.store((int)FutureStatus::Ready);
                        CC::Log(CC::LOG_INFO,
                                "AssetLoader: worker mesh upload ok (verts=%d, idx=%d, meshId=%u, path=%s)",
                                st->gltfVertCount, st->glMeshIdxCount, meshId, task.path.c_str());
                    }
                } else {
                    st->status.store((int)FutureStatus::Ready);
                    CC::Log(CC::LOG_INFO,
                            "AssetLoader: worker upload ok (type=%d, path=%s)",
                            task.type, task.path.c_str());
                }
            }
        }
#endif
        else {
            // 走主线程上传/注册分发 (G.1.0 路径)
            switch (task.type) {
                case TaskType::Image:    UploadImage_(*st, task.path); break;
                case TaskType::LUT_Cube:
                case TaskType::LUT_Hald: UploadLUT_(*st, task.path);   break;
                case TaskType::Font:     UploadFont_(*st, task.path);  break;
                case TaskType::Sound:    UploadSound_(*st, task.path); break;
                case TaskType::GLTF:     UploadGLTF_(*st, task.path);  break;
            }
        }

        // ---- 完成后 dispatch Lua callback (如有) ----
        // status 此时已是 Ready 或 Error; dispatcher 由 Lua binding 注册.
        if (st->dispatcher && st->cbLuaState && st->cbLuaRef >= 0) {
            st->dispatcher(st->cbLuaState, st.get(), st->cbLuaRef);
            // 调过一次后释放 ref + 清, 避免重复触发
            luaL_unref((lua_State*)st->cbLuaState, LUA_REGISTRYINDEX, st->cbLuaRef);
            st->cbLuaRef   = -1;
            st->cbLuaState = nullptr;
            st->dispatcher = nullptr;
        }
    }

    // Phase G.1.1 — fence 未完成的 task 连 fence 一起放回队列 (push_front 让下帧优先处理)
    if (!retry.empty()) {
        std::lock_guard<std::mutex> lk(g_resultMutex);
        for (auto it = retry.rbegin(); it != retry.rend(); ++it) {
            g_resultQueue.push_front(std::move(*it));
        }
    }
}

// ==================== RegisterCallback ====================

void RegisterCallback(const std::shared_ptr<FutureState>& state,
                       FutureState::Dispatcher dispatcher,
                       void* L,
                       int luaRef) {
    if (!state) return;
    state->dispatcher  = dispatcher;
    state->cbLuaState  = L;
    state->cbLuaRef    = luaRef;
}

void RegisterResultPusher(const std::shared_ptr<FutureState>& state,
                           FutureState::ResultPusher pusher) {
    if (!state) return;
    state->resultPusher = pusher;
}

namespace {

static FutureUserdata* CheckFuture_(lua_State* L, int idx) {
    return (FutureUserdata*)luaL_checkudata(L, idx, kFutureMetaName);
}

static int l_Future_IsReady_(lua_State* L) {
    FutureUserdata* ud = CheckFuture_(L, 1);
    lua_pushboolean(L, ud && ud->state &&
                       ud->state->status.load() == (int)FutureStatus::Ready ? 1 : 0);
    return 1;
}

static int l_Future_IsError_(lua_State* L) {
    FutureUserdata* ud = CheckFuture_(L, 1);
    lua_pushboolean(L, ud && ud->state &&
                       ud->state->status.load() == (int)FutureStatus::Error ? 1 : 0);
    return 1;
}

static int l_Future_GetError_(lua_State* L) {
    FutureUserdata* ud = CheckFuture_(L, 1);
    if (ud && ud->state) lua_pushstring(L, ud->state->errorMsg.c_str());
    else                 lua_pushstring(L, "");
    return 1;
}

static int l_Future_Get_(lua_State* L) {
    FutureUserdata* ud = CheckFuture_(L, 1);
    if (!ud || !ud->state) {
        lua_pushnil(L);
        lua_pushstring(L, "invalid future");
        return 2;
    }

    int s = ud->state->status.load();
    if (s == (int)FutureStatus::Pending) {
        lua_pushnil(L);
        lua_pushstring(L, "pending");
        return 2;
    }
    if (s == (int)FutureStatus::Error) {
        lua_pushnil(L);
        lua_pushstring(L, ud->state->errorMsg.empty() ? "unknown" : ud->state->errorMsg.c_str());
        return 2;
    }

    // Phase G.1.5 — pusher 返 push 数量 (默认 1, GLTF withMaterial=2)
    int n = 1;
    if (ud->state->resultPusher) {
        n = ud->state->resultPusher(L, ud->state.get());
        if (n < 1) n = 1;   // 防御
    } else {
        lua_pushnil(L);
    }
    lua_pushnil(L);   // err = nil
    return n + 1;
}

static int l_Future_Gc_(lua_State* L) {
    FutureUserdata* ud = (FutureUserdata*)lua_touserdata(L, 1);
    if (ud) ud->~FutureUserdata();
    return 0;
}

static int l_Future_Tostring_(lua_State* L) {
    lua_pushstring(L, kFutureMetaName);
    return 1;
}

static const luaL_Reg kFutureMethods_[] = {
    {"IsReady",    l_Future_IsReady_},
    {"IsError",    l_Future_IsError_},
    {"Get",        l_Future_Get_},
    {"GetError",   l_Future_GetError_},
    {"__gc",       l_Future_Gc_},
    {"__tostring", l_Future_Tostring_},
    {NULL, NULL}
};

static void EnsureFutureMetatable_(lua_State* L) {
    if (luaL_newmetatable(L, kFutureMetaName)) {
        lua_pushvalue(L, -1);
        lua_setfield(L, -2, "__index");
        luaL_setfuncs(L, kFutureMethods_, 0);
    }
    lua_pop(L, 1);
}

}  // anonymous namespace

int PushAsyncFuture(lua_State* L, std::shared_ptr<FutureState> state) {
    if (!L) return 0;
    EnsureFutureMetatable_(L);
    void* udmem = lua_newuserdata(L, sizeof(FutureUserdata));
    new (udmem) FutureUserdata{std::move(state)};
    luaL_getmetatable(L, kFutureMetaName);
    lua_setmetatable(L, -2);
    return 1;
}

// ==================== LoadImageAsync ====================

std::shared_ptr<FutureState> LoadImageAsync(const char* path) {
    auto state = std::make_shared<FutureState>();
    state->type = (int)TaskType::Image;

    if (!path || !*path) {
        state->errorMsg = "LoadImageAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    // worker 未 Init: fallback 同步加载
    if (!g_running.load()) {
        int w = 0, h = 0, ch = 0;
        unsigned char* pixels = stbi_load(path, &w, &h, &ch, 4);
        if (!pixels || w <= 0 || h <= 0) {
            state->errorMsg = std::string("sync stbi_load failed: ") +
                              (stbi_failure_reason() ? stbi_failure_reason() : "unknown");
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        uint32_t texId = g_render ? g_render->CreateTexture(w, h, 4, pixels) : 0;
        stbi_image_free(pixels);
        if (!texId) {
            state->errorMsg = "sync CreateTexture failed";
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        state->imgW = w;
        state->imgH = h;
        state->imgChannels = 4;
        state->resTexId = texId;
        state->status.store((int)FutureStatus::Ready);
        return state;
    }

    // 异步路径: push 到 task_queue
    Task task;
    task.type  = TaskType::Image;
    task.path  = path;
    task.state = state;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_taskQueue.push_back(std::move(task));
    }
    g_taskCv.notify_one();
    return state;
}

// ==================== 内部: push 任务 helper ====================
namespace {

// 把 task 推到 worker queue + 唤醒. (调用前 state 已写好 type / 业务字段)
static void PushTask_(TaskType type, const char* path,
                       const std::shared_ptr<FutureState>& state) {
    Task task;
    task.type  = type;
    task.path  = path ? path : "";
    task.state = state;
    {
        std::lock_guard<std::mutex> lk(g_taskMutex);
        g_taskQueue.push_back(std::move(task));
    }
    g_taskCv.notify_one();
}

}  // anonymous namespace

// ==================== LoadCubeLUTAsync ====================

std::shared_ptr<FutureState> LoadCubeLUTAsync(const char* path) {
    auto state = std::make_shared<FutureState>();
    state->type = (int)TaskType::LUT_Cube;

    if (!path || !*path) {
        state->errorMsg = "LoadCubeLUTAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    // worker 未启动: 同步 fallback (主线程 parse + GL 上传)
    if (!g_running.load()) {
        char errBuf[256] = {0};
        uint32_t id = HDRRenderer::LoadCubeLUTFile(path, errBuf, sizeof(errBuf));
        if (!id) {
            state->errorMsg = errBuf[0] ? errBuf : "sync LoadCubeLUTFile failed";
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        state->resLUTId = id;
        state->status.store((int)FutureStatus::Ready);
        return state;
    }

    PushTask_(TaskType::LUT_Cube, path, state);
    return state;
}

// ==================== LoadHaldLUTAsync ====================

std::shared_ptr<FutureState> LoadHaldLUTAsync(const char* path) {
    auto state = std::make_shared<FutureState>();
    state->type = (int)TaskType::LUT_Hald;

    if (!path || !*path) {
        state->errorMsg = "LoadHaldLUTAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    if (!g_running.load()) {
        char errBuf[256] = {0};
        uint32_t id = HDRRenderer::LoadHaldLUTFile(path, errBuf, sizeof(errBuf));
        if (!id) {
            state->errorMsg = errBuf[0] ? errBuf : "sync LoadHaldLUTFile failed";
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        state->resLUTId = id;
        state->status.store((int)FutureStatus::Ready);
        return state;
    }

    PushTask_(TaskType::LUT_Hald, path, state);
    return state;
}

// ==================== LoadFontAsync ====================

std::shared_ptr<FutureState> LoadFontAsync(const char* path, float size) {
    auto state = std::make_shared<FutureState>();
    state->type      = (int)TaskType::Font;
    state->fontSize  = size > 0.0f ? size : 16.0f;   // 默认 16px
    state->fontAtlasW = 1024;                         // 与同步 Font 一致
    state->fontAtlasH = 1024;

    if (!path || !*path) {
        state->errorMsg = "LoadFontAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    // worker 未启动: 主线程同步加载 TTF buffer (与 worker 路径数据一致, 让 dispatcher 创 FontContext)
    if (!g_running.load()) {
        FILE* fp = fopen(path, "rb");
        if (!fp) {
            state->errorMsg = std::string("sync fopen failed: ") + path;
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz <= 0) { fclose(fp); state->errorMsg = "empty TTF"; state->status.store((int)FutureStatus::Error); return state; }
        unsigned char* ttf = (unsigned char*)malloc((size_t)sz);
        if (!ttf) { fclose(fp); state->errorMsg = "malloc failed"; state->status.store((int)FutureStatus::Error); return state; }
        size_t rd = fread(ttf, 1, (size_t)sz, fp);
        fclose(fp);
        if (rd != (size_t)sz) { free(ttf); state->errorMsg = "fread truncated"; state->status.store((int)FutureStatus::Error); return state; }
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, ttf, 0)) {
            free(ttf);
            state->errorMsg = "stbtt_InitFont failed";
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        state->fontTTFBuffer = ttf;
        state->fontTTFSize   = (int)sz;
        state->status.store((int)FutureStatus::Ready);
        return state;
    }

    PushTask_(TaskType::Font, path, state);
    return state;
}

// ==================== LoadSoundAsync ====================

std::shared_ptr<FutureState> LoadSoundAsync(const char* path) {
    auto state = std::make_shared<FutureState>();
    state->type = (int)TaskType::Sound;

    if (!path || !*path) {
        state->errorMsg = "LoadSoundAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    // 确保 AudioBackend 已 Init (主线程调, 线程安全; worker 内调 LoadFile 假设已 Init)
    if (!AudioBackend::Init()) {
        state->errorMsg = "AudioBackend init failed (no audio device?)";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    if (!g_running.load()) {
        // 同步 fallback
        AudioHandle* h = AudioBackend::LoadFile(path);
        if (!h) {
            state->errorMsg = std::string("sync ma_sound_init_from_file failed: ") + path;
            state->status.store((int)FutureStatus::Error);
            return state;
        }
        state->resSoundHandle = h;
        state->status.store((int)FutureStatus::Ready);
        return state;
    }

    PushTask_(TaskType::Sound, path, state);
    return state;
}

// ==================== LoadGLTFAsync ====================

std::shared_ptr<FutureState> LoadGLTFAsync(const char* path, int primIdx, bool withMaterial) {
    auto state = std::make_shared<FutureState>();
    state->type             = (int)TaskType::GLTF;
    state->gltfPrimIdx      = primIdx >= 0 ? primIdx : 0;
    state->gltfWithMaterial = withMaterial;   // Phase G.1.5 — 决定 worker 是否提取 material + image

    if (!path || !*path) {
        state->errorMsg = "LoadGLTFAsync: path is null/empty";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    // worker 未启动: 同步 fallback (创个 Task 在主线程跑 DecodeGLTF_ + UploadGLTF_)
    // 注: DecodeGLTF_ 是 static, 不能直接调用; 退而用主线程 cgltf 直接走 — 复杂, 简化为
    //     报错让用户用同步 Mesh.LoadGLTF (与 G.1 文档约束一致)
    if (!g_running.load()) {
        state->errorMsg = "AssetLoader worker not running; use Mesh.LoadGLTF (sync) instead";
        state->status.store((int)FutureStatus::Error);
        return state;
    }

    PushTask_(TaskType::GLTF, path, state);
    return state;
}

} // namespace AssetLoader
