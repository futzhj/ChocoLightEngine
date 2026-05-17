# Phase G.1 异步资源加载 — CONSENSUS (共识) 文档

> **阶段**：6A Workflow — 阶段 1.5 Consensus
> **创建日期**：2026-05-17
> **依赖**：[ALIGNMENT_PhaseG_1.md](ALIGNMENT_PhaseG_1.md)

---

## 1. 任务范围 (锁死)

| 子任务 | 包含 | 不包含 |
|--------|------|--------|
| AssetLoader 基础设施 | 单 worker thread + 双 lock-free 队列 + Future 框架 + Shared GL Context probe | 多 worker / 优先级队列 / 缓存 |
| Image 异步 | LoadImageAsync + Future + Callback | 内存解码 (LoadFromMemoryAsync 留 G.x) |
| Mesh/glTF 异步 | LoadGLTFAsync (复用 Image 解码 worker) | OBJ/FBX |
| LUT 异步 | LoadLUTAsync (.cube + .png HALD) | LUT hot reload 改造 (留 G.x) |
| Font 异步 | LoadFontAsync (TTF init + 异步预烘焙 ASCII) | 动态字形烘焙异步化 (复杂, 留 G.x) |
| Audio 异步 | LoadSoundAsync (miniaudio 内部已优化, 仅包装到统一 API) | streaming 大音频流式异步 |

## 2. 接口契约

### 2.1 C++ 内部 API

```cpp
namespace AssetLoader {
    // 启动 / 停止 worker (在 g_render Init 后调)
    bool Init(void* mainWindow, void* mainGLCtx);
    void Shutdown();

    // 是否 shared GL context 启用成功
    bool IsSharedGLContextEnabled();

    // 主线程每帧调用: 处理 worker 完成 + dispatch callback / Future 状态翻转
    void Tick();

    // 内部 Future (用 std::shared_ptr 管理 ref count)
    template<typename T>
    struct Future {
        bool IsReady() const;
        T* Get();             // 仅成功时返非 null
        const char* GetError() const;
        // ... worker 内部 Set / 主线程内部 Get
    };

    // 各资源类型的 LoadAsync API:
    auto LoadImageAsync(const char* path) -> std::shared_ptr<Future<Image>>;
    auto LoadGLTFAsync(const char* path) -> std::shared_ptr<Future<Mesh>>;
    auto LoadLUTAsync(const char* path)  -> std::shared_ptr<Future<LUT3D>>;
    auto LoadFontAsync(const char* path, int sz) -> std::shared_ptr<Future<Font>>;
    auto LoadSoundAsync(const char* path) -> std::shared_ptr<Future<Sound>>;
}
```

### 2.2 Lua API

```lua
-- Future 风格 (主动 poll)
local h = Light.Graphics.Image.LoadAsync("path/to/img.png")
-- ... 每帧 ...
if h:IsReady() then
    local img, err = h:Get()
    if img then ... end
end

-- Callback 风格 (被动 dispatch)
Light.Graphics.Image.LoadAsync("path/to/img.png", function(img, err)
    if img then ... end
end)

-- 同样 5 个资源都有: Image / Mesh / HDR.LUT / Font / Audio.Sound
```

## 3. 关键决策

### Q1: Future userdata 如何 GC?
**决定**: Future 内部用 `std::shared_ptr<FutureState>`. Lua userdata 持一份 ref, worker 队列条目持一份 ref. 双方释放才回收 (worker 不在意 Lua 端是否还想要结果, 总会跑完; Lua 端 GC 后 dispatch 时检测 ref count == 1 静默丢弃).

### Q2: 队列锁?
**决定**: 用 `std::mutex` + `std::condition_variable` + `std::deque`. Lock-free MPMC 队列 (moodycamel 等) 引入第三方依赖, 当前 worker 单线程, 性能瓶颈在解码不在排队.

### Q3: Shared GL Context 在 worker 线程的策略?
**决定**:
- AssetLoader::Init 时, 主线程当前 context current, 调 PlatformWindow::CreateGLContext 创建第二个 (SDL3 默认共享).
- 立即 unbind 主线程, MakeCurrent(worker_ctx) on worker thread.
- worker 线程一启动就 MakeCurrent worker_ctx, 直到 Shutdown.
- 主线程恢复 main_ctx current.
- 失败检测: CreateGLContext == nullptr OR worker thread 启动后第一个 GL 调用失败 → fallback flag, 主线程上传.

### Q4: GL upload sync 机制 (worker 上传后主线程能用)?
**决定**: glFenceSync + glClientWaitSync。Worker 上传完 → glFenceSync 创建 fence + push 到 result queue → 主线程 Tick 取出 → ClientWaitSync (timeout 0, 不等) → 若已完成, dispatch; 否则放回队列下帧再试.

### Q5: stb_image 全局状态 (vertical flip)?
**决定**: 用 `stbi_set_flip_vertically_on_load_thread(true)` (TLS 版本, stb_image 自带). 默认 worker 设为 true (与现有 stb 调用风格一致).

### Q6: 错误传播?
**决定**: Future 内部存 `std::string error`. Worker 失败时 set; 主线程 dispatch 时 callback 第二参数 = err msg, Future:Get 第二返回值 = err msg.

### Q7: 异步资源 vs Lua userdata 桥接?
**决定**:
- Worker 产出 raw decoded data (如 stbi 像素 buffer + w/h/channels).
- 主线程 dispatch 时调 backend->CreateTexture(...) 包成已存在的 Image userdata (复用 light_graphics_image.cpp Image 类型).
- 用户拿到的就是普通 Image, 与同步加载完全一致.

## 4. 验收口径细化

新增 smoke (`scripts/smoke/phase_g1_async_asset.lua`):
1. `Image.LoadAsync(path)` Future 风格: poll 直到 IsReady, Get 返 Image, GetTextureId() != 0.
2. `Image.LoadAsync(path, cb)` Callback: 等回调触发, cb 参数 1 是 Image, GetTextureId() != 0.
3. 错误路径: 不存在文件 → Future:Get() 返 nil, err 字符串非空; Callback cb(nil, err).
4. 5 个资源类型重复上述 3 用例.
5. 启动日志含 "AssetLoader: Shared GL Context enabled" 或 "fallback to main thread upload".

## 5. 不变量

- 主线程 OpenGL 调用永远只在主线程; worker 线程仅在 Shared Context 启用 + worker_ctx current 时调 GL.
- Lua callback 永远只在主线程 dispatch 中调 (Tick 内).
- Future 状态翻转 (pending → ready/error) 由 worker 写, 主线程读, 用 atomic.
