/**
 * @file   asset_loader.h
 * @brief  Phase G.1 — 异步资源加载器 (5 套对称: Image / GLTF Mesh / LUT / Font / Sound)
 *
 * 设计目标:
 *   - 后台 worker thread 负责 I/O + CPU 解码.
 *   - Phase G.1.1: probe Shared GL Context, 成功则 worker 直接 glTexImage2D + glFenceSync
 *                 (Image / LUT / Font 三类); 失败则透明回落主线程上传.
 *   - Mesh / Sound 仍走主线程 (前者依赖 backend Mesh 抽象, 后者无 GL).
 *   - 双缓冲 task_queue / result_queue (mutex+cv 保护).
 *   - Future / Callback 双风格 API.
 *
 * 后期演进 (留 TODO):
 *   - GLTF Mesh worker 上传 (G.1.2)
 *   - GLTF with_material 异步路径 (内嵌纹理 N 张, 当前仅基础 mesh)
 *
 * 用法 (Lua 端):
 *   local h = Light.Graphics.Image.LoadAsync("foo.png")
 *   if h:IsReady() then local img, err = h:Get() end
 *   -- 或
 *   Light.Graphics.Image.LoadAsync("foo.png", function(img, err) ... end)
 *
 * 主线程钩子:
 *   - AssetLoader::Init()    在 g_render 创建后调
 *   - AssetLoader::Tick()    每帧 BeginFrame 内调
 *   - AssetLoader::Shutdown()  退出前调
 *
 * 不变量:
 *   - 主线程 GL 操作仅主线程 (本期 worker 不调 GL)
 *   - Lua callback 仅在主线程 Tick 内调用
 *   - Future state 翻转 pending → ready/error 由主线程 Tick 翻 (worker 仅写中间数据)
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct lua_State;

namespace AssetLoader {

// ==================== Future / FutureState ====================

enum class FutureStatus : int {
    Pending = 0,
    Ready   = 1,
    Error   = 2,
};

// ==================== Phase G.1.5 — GLTF Material Image Job ====================
// withMaterial=true 时, worker 解码 5 类 PBR texture (任一 cgltf material 实际指定的)
// 到 RGBA8 pixels, shared_ctx 路径 worker 自己 glTexImage2D + glFenceSync;
// fallback 路径 pixels 留到主线程 Tick 调 g_render->CreateTexture 上传.
//
// slotIdx 枚举对应 MaterialDesc 5 个 texture slot (与 light_graphics_material.cpp::TexSlotPtr 一致):
//   0 = baseColor / 1 = metallicRoughness / 2 = normal / 3 = emissive / 4 = occlusion
struct MaterialImageJob {
    int      slotIdx = -1;
    int      w       = 0;
    int      h       = 0;
    uint8_t* pixels  = nullptr;   // worker stbi_load_from_memory; 上传完释放; 析构兜底
    uint32_t glTexId = 0;         // 上传完成后的 GL texture id (0=失败 / 未上传)

    // Phase G.1.5 T3 — cgltf sampler 透传 (raw GL enum, 0 = 未指定 → 用 glTF 2.0 默认)
    // glTF 默认: mag=LINEAR / min=LINEAR_MIPMAP_LINEAR / wrap_s=wrap_t=REPEAT
    // 来自 cgltf_texture->sampler->{mag_filter, min_filter, wrap_s, wrap_t}
    int      samplerMagFilter = 0;
    int      samplerMinFilter = 0;
    int      samplerWrapS     = 0;
    int      samplerWrapT     = 0;
};

/**
 * Future 共享状态: worker 写 (decode 完), 主线程读 + 完成上传后翻 status.
 *
 * Lua userdata 持一份 shared_ptr; result_queue 持一份 shared_ptr.
 * 双方释放才回收.
 *
 * 多资源类型设计: 平铺各资源字段 (KISS) — 互斥占用, type 字段判断.
 * vector / string 默认空, 不占额外字节.
 */
struct FutureState {
    std::atomic<int> status{0};       // FutureStatus
    std::string      errorMsg;        // 仅 Error 时有效
    int              type = 0;        // TaskType (内部用)

    // ---- Image 资源 ----
    int      imgW = 0;
    int      imgH = 0;
    int      imgChannels = 0;
    void*    imgPixels = nullptr;     // worker 用 stbi_load 分配, 主线程上传后 stbi_image_free
    uint32_t resTexId   = 0;          // 上传完成后的 GL texture id

    // ---- Phase G.1.1 Shared GL Context 路径 ----
    // worker 直接 GL 上传 + glFenceSync 时填; Tick 用 glClientWaitSync 翻 status.
    // void* 是为避免头文件依赖 <glad/gl.h>; 内部强转 GLsync.
    void*    glFence         = nullptr;
    int      fenceWaitFrames = 0;     // Tick 已尝试等待帧数 (满 60 帧转 Error)

    // ---- LUT (.cube + HALD PNG) ----
    int                  lutSize    = 0;       // [4, 64]
    bool                 lutIsHDR   = false;   // .cube DOMAIN_MAX > 1.0 或 16-bit HALD
    std::vector<uint8_t> lutBytes;             // LDR 路径: size^3 * 3 bytes
    std::vector<float>   lutFloats;            // HDR 路径: size^3 * 3 floats
    uint32_t             resLUTId   = 0;       // 上传完成后 GL texture id

    // ---- Font (TTF) ----
    void*    fontTTFBuffer = nullptr;     // worker malloc, 主线程取走后归 FontContext 管理
    int      fontTTFSize   = 0;           // TTF 二进制字节数
    float    fontSize      = 0.0f;        // 像素大小
    int      fontAtlasW    = 0;           // 默认 1024
    int      fontAtlasH    = 0;
    void*    resFontUd     = nullptr;     // 主线程创建的 FontContext userdata 地址 (Lua GC 接管)

    // ---- Sound (miniaudio) ----
    void*    resSoundHandle = nullptr;    // AudioHandle* — miniaudio 内部线程安全, worker 直接 init

    // ---- GLTF Mesh (基础版本, 不含 material) ----
    void*               gltfData      = nullptr;  // cgltf_data*; worker 解析, 主线程 free
    int                 gltfPrimIdx   = 0;        // 用户指定的 primitive index
    void*               gltfVerts     = nullptr;  // RenderVertex3D* (worker malloc)
    int                 gltfVertCount = 0;
    void*               gltfIndices   = nullptr;  // uint32_t* (worker malloc)
    int                 gltfIdxCount  = 0;
    uint32_t            resMeshId     = 0;        // 上传完成后 GL mesh id

    // ---- Phase G.1.2 — Worker GL upload Mesh 路径 ----
    // worker WorkerUploadMesh_ 写; 主线程 Tick fence Ready 后调
    // backend->RegisterUploadedMesh 拿 meshId, 写入 resMeshId.
    // 与 G.1.0 主线程 UploadGLTF_ 路径互斥 (g_sharedCtxOk 决定走哪条)
    uint32_t glMeshVao      = 0;
    uint32_t glMeshVbo      = 0;
    uint32_t glMeshEbo      = 0;
    int      glMeshIdxCount = 0;

    // ---- Phase G.1.5 — GLTF Material + Embedded Texture ----
    // withMaterial=true 时由 worker 填充. char[128] POD 序列化 MaterialDesc
    // (避免 asset_loader.h 依赖 light_graphics_material.h 引入循环依赖).
    // binding 层 (light_graphics_mesh.cpp) 反序列化为 MaterialDesc + 创建 Material userdata.
    bool                          gltfWithMaterial = false;
    char                          gltfMaterialDesc[128] = {0};   // POD MaterialDesc (sizeof ~80, 留余量)
    std::vector<MaterialImageJob> gltfMaterialImages;            // 0..5 个, 按 cgltf material 实际指定

    // ---- Callback (Lua, 用于 LoadAsync(path, cb) 风格) ----
    // dispatcher: 由 Lua binding 在 LoadXxxAsync 时设置. Tick 完成上传后 (status=Ready/Error)
    // 调用它. dispatcher 负责: push 资源 userdata + lua_rawgeti(cbRef) + lua_pcall(...).
    // 签名: void dispatcher(lua_State* L, FutureState* state, int cbLuaRef)
    using Dispatcher = void(*)(void* L, FutureState* state, int cbLuaRef);
    Dispatcher dispatcher = nullptr;
    void*      cbLuaState = nullptr;    // lua_State*; nullptr = 无 callback (用户走 Future poll)
    int        cbLuaRef   = -1;         // luaL_ref into LUA_REGISTRYINDEX

    // ---- Result pusher (Lua, 用于 Future:Get() poll 风格) ----
    // 由 Lua binding 在 LoadXxxAsync 时设置. Future:Get() Ready 状态下调用,
    // 它负责 push 一个或多个 type-specific 资源 userdata 到栈顶, 并返回 push 数量.
    //
    // 签名: int pusher(lua_State* L, FutureState* state) — Phase G.1.5: 返 push 数量
    // 约定:
    //   - 只在 status==Ready 时被调
    //   - 必须 push 至少 1 个值到栈顶 (失败时 push nil 返 1)
    //   - GLTF withMaterial 路径返 2 (mesh + material); 其他资源返 1
    //   - 不应抛 Lua error (用户 poll 路径需要稳定语义)
    using ResultPusher = int(*)(void* L, FutureState* state);
    ResultPusher resultPusher = nullptr;

    virtual ~FutureState();
};

inline constexpr const char* kFutureMetaName = "Light.Graphics._AsyncFuture";

struct FutureUserdata {
    std::shared_ptr<FutureState> state;
};

// ==================== 生命周期 ====================

/**
 * 启动 worker + probe Shared GL Context (Phase G.1.1).
 *
 * @param mainWin   PlatformWindow 主窗口句柄 (light_ui.cpp 持有)
 * @param mainCtx   主 GL context 句柄 (调用前必须已 MakeCurrent 主线程)
 * @return true = worker 启动成功 (即使 shared ctx probe 失败仍返 true,
 *               走 G.1.0 主线程上传 fallback)
 *         false = worker thread 自身启动失败 (致命)
 *
 * 副作用:
 *   - probe 成功: 持有 worker GL ctx, 启动日志 "Shared GL Context enabled"
 *   - probe 失败: 不持有 ctx, 启动日志 "fallback to main-thread upload"
 */
bool Init(void* mainWin, void* mainCtx);

/**
 * 停止 worker, 清空队列.
 * Pending future 被 Set 为 Error("AssetLoader shutdown").
 */
void Shutdown();

bool IsRunning();

// ==================== 主循环 hook ====================

/**
 * 主线程每帧调一次 (BeginFrame 之内).
 * 行为:
 *   - drain result_queue
 *   - 调 backend->CreateXxx(...) 上传 GL 资源, set resXxxId, status = Ready
 *   - dispatch Lua callback
 */
void Tick();

// ==================== 异步加载 API (5 套对称) ====================

/**
 * 异步加载图像 (P0).
 *
 * @param path UTF-8 文件路径
 * @return Future state shared_ptr (Lua userdata 持一份)
 *
 * 失败路径 (path=NULL / 文件不存在 / 解码失败): 立即返一个已 Error 的 FutureState.
 */
std::shared_ptr<FutureState> LoadImageAsync(const char* path);

/**
 * 异步加载 .cube LUT (Adobe Cube LUT 1.0).
 * worker 阶段: 读文件 → 调 HDRRenderer::ParseCubeLUT 解析 → 填 lutBytes/lutFloats.
 * 主线程阶段: 调 backend->CreateLUT3D[Float] → resLUTId.
 */
std::shared_ptr<FutureState> LoadCubeLUTAsync(const char* path);

/**
 * 异步加载 HALD CLUT 图像 (PNG/JPG/BMP/TGA, 8-bit 或 16-bit).
 * worker 阶段: stbi_load(_16) + 验证 N^3 维度 → 填 lutBytes/lutFloats.
 * 主线程阶段: 调 backend->CreateLUT3D[Float] → resLUTId.
 */
std::shared_ptr<FutureState> LoadHaldLUTAsync(const char* path);

/**
 * 异步加载 TTF Font (lazy-bake atlas, 与同步路径一致).
 * worker 阶段: fread + stbtt_InitFont 验证 → 缓存 ttfBuffer + 字节数.
 * 主线程阶段: 创建 FontContext userdata (atlas 仍 lazy-bake 在首次 GetGlyph 时).
 *
 * @param path UTF-8 字体文件路径
 * @param size 像素大小 (默认 16)
 */
std::shared_ptr<FutureState> LoadFontAsync(const char* path, float size);

/**
 * 异步加载音频 (miniaudio 各种格式).
 * worker 阶段: ma_sound_init_from_file (miniaudio 自身线程安全).
 * 主线程阶段: 注册到 SoundUserdata.
 */
std::shared_ptr<FutureState> LoadSoundAsync(const char* path);

/**
 * 异步加载 glTF Mesh 单 primitive (Phase G.1.5: 可选 with material).
 * worker 阶段: cgltf_parse + cgltf_load_buffers + 提取顶点/索引到 malloc array;
 *              withMaterial=true 时额外提取 MaterialDesc + 解码 5 类 PBR texture (stbi_load_from_memory).
 * 主线程阶段: backend->CreateMesh → resMeshId; withMaterial=true 时 5 textures 上传 + 写 MaterialDesc slots.
 *
 * @param path          UTF-8 .gltf 或 .glb 路径
 * @param primIdx       primitive index (跨所有 mesh 0-indexed)
 * @param withMaterial  Phase G.1.5: true 时同步 LoadGLTF 完整功能对齐 (返 mesh + material)
 */
std::shared_ptr<FutureState> LoadGLTFAsync(const char* path, int primIdx, bool withMaterial = false);

// ==================== Callback dispatch (Lua) ====================

/**
 * 让 Future 关联一个 Lua callback. 调过此函数后, Tick 在 Future 完成时会调
 * dispatcher(L, state, luaRef). dispatcher 由 Lua binding 在调用前传入.
 *
 * 用法约定:
 *   - L 必须是创建 future 的同一 lua_State
 *   - luaRef 是 luaL_ref 出来的 cb 引用; Future 析构时会 luaL_unref 释放
 *   - dispatcher 必须在主线程调用安全 (Tick 主线程内调)
 */
void RegisterCallback(const std::shared_ptr<FutureState>& state,
                       FutureState::Dispatcher dispatcher,
                       void* L,
                       int luaRef);

/**
 * 让 Future 关联一个 Lua result pusher (Future:Get() poll 风格).
 * pusher 在 Future:Get() Ready 状态下被调, 负责 push type-specific userdata.
 */
void RegisterResultPusher(const std::shared_ptr<FutureState>& state,
                           FutureState::ResultPusher pusher);

/**
 * 在 Lua 栈顶 push 一个 Future userdata (包裹 shared_ptr<FutureState>).
 * 复用 Light.Graphics.Image 模块注册的 metatable "Light.Graphics._AsyncFuture".
 * 调用方不需要管理 userdata 内部布局 / metatable 名 / 析构.
 *
 * @return 1 (push 了 1 个 stack slot)
 *
 * 前置: luaopen_Light_Graphics_Image 已被调过 (Light.Graphics.Image 已加载).
 *       ChocoLight 模块加载顺序保证此前提.
 */
int PushAsyncFuture(lua_State* L, std::shared_ptr<FutureState> state);

} // namespace AssetLoader

