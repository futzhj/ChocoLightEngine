# Phase G.1 异步资源加载 — ALIGNMENT (对齐) 文档

> **阶段**：6A Workflow — 阶段 1 Align
> **创建日期**：2026-05-17
> **基线**：Phase F.2 完结后, 转入非渲染基础架构第一项

---

## 1. 任务背景

ChocoLight 引擎当前所有资源加载都是同步阻塞主线程：
- 大纹理 (PNG/JPG 4K+) 解码 50-200ms 卡顿
- glTF 模型解析 + 内嵌纹理串行加载, 大模型卡顿数秒
- LUT/.cube 文件解析 + Font TTF 加载也阻塞主线程

异步资源加载是非渲染基础架构 4 项任务中的第 1 项 (HANDOFF §2.1)。

## 2. 范围确认

### 2.1 用户确认的方案
- **架构**: Shared GL Context + auto-fallback (主线程降级路径)
- **API 风格**: Future/Promise (主动 IsReady/Get) + Callback (被动 dispatch) 两种都提供
- **首期资源**: Image (P0) + Mesh/glTF (P1) + LUT (P2) + Font/Audio (P3) 全部覆盖

### 2.2 不在本期范围
- 资源缓存 / 引用计数管理 (留 G.2 VRAM 追踪)
- 优先级调度 (FIFO 即可)
- 多 worker 线程池 (单 worker 起步, 性能不足再扩)
- Web/emscripten 上 Shared Context (该平台 fallback 到主线程)

## 3. 现状摘要 (来自 audit)

| 资源 | 加载库 | 同步阻塞性质 | 入口 |
|------|--------|-------------|------|
| Image | stb_image | 主线程 I/O + 解码 + GL 上传 | light_graphics_image.cpp:137 |
| Mesh | cgltf + stb_image | 主线程 I/O + 解析 + 内嵌纹理 + GL 上传 | light_graphics_mesh.cpp:521 |
| LUT (.cube) | SDL_LoadFile + 文本解析 | 主线程 I/O + 解析 + GL 上传 | hdr_renderer.cpp:1256 |
| LUT (.png HALD) | stbi_load_16 | 主线程 I/O + 解码 + GL 上传 | hdr_renderer.cpp:1293 |
| Font | fopen + stb_truetype | 主线程 I/O + Init + 烘焙 ASCII + GL 上传 | light_graphics_image.cpp:460 |
| Audio | miniaudio (内部解码) | 主线程 I/O + 解码 (无 GL) | light_audio_backend.cpp:107 |

## 4. 关键 Q&A 决议

### Q1: Shared GL Context 还是单 worker?
**决定**: Shared Context + auto-fallback。

**理由**: SDL3 默认在 main context 仍 current 时给后续 CreateContext 共享; NVIDIA/AMD 桌面驱动可靠; Intel iGPU/某些 GLES 驱动可能失败, 启动时 probe 检测, 失败 fallback 到主线程上传。

### Q2: Lua API 风格?
**决定**: 两种都提供。

**理由**:
- Future 版: `local h = Image.LoadAsync(path); -- ... game loop ... if h:IsReady() then local img = h:Get() end` — 与现有 game loop 风格契合
- Callback 版: `Image.LoadAsync(path, function(img, err) ... end)` — 适合 setup-time 链式加载

实施: 单一 C++ 入口 + 两种 Lua wrapper 表面。

### Q3: 资源类型的优先级?
**决定**: P0 Image → P1 Mesh → P2 LUT → P3 Font/Audio。

每个资源类型独立 Lua API, 互不依赖 (Mesh 内部 P1 复用 P0 Image 异步)。

### Q4: 首次失败/异常的处理?
**决定**: 主线程错误回调 (Future:Get() 返 nil + 第二返回值 err msg; Callback 版调 cb(nil, err))。Worker 线程 stbi_load 失败/cgltf 解析失败时, 把 err 字符串塞 Future, 主线程 dispatch 时传给 cb。

### Q5: 资源生命周期?
**决定**: Future 持有最终 Light 资源 userdata 的引用; Future 销毁不会释放资源 (Lua GC 决定)。

## 5. 验收口径

1. ✅ Build clean
2. ✅ 现有 smoke (image/mesh/lut/font/audio) 不回归
3. ✅ 新增 phase_g1_async_asset.lua: P0/P1/P2/P3 各资源 Future + Callback 路径都通过
4. ✅ Shared GL Context probe 启动时打印 INFO 日志
5. ✅ Auto-fallback 路径可手动测 (主线程上传)
6. ✅ 6A 文档 7 件套 (ALIGNMENT/CONSENSUS/DESIGN/TASK/ACCEPTANCE/FINAL/TODO)

## 6. 风险与备用方案

| 风险 | 备用 |
|------|------|
| Shared Context 在 Intel/移动 GLES 失败 | auto-fallback 到主线程上传, 仅工人线程负责解码 |
| Worker 线程 stbi 全局状态 (stbi__vertically_flip_on_load) 竞争 | 用 `stbi_set_flip_vertically_on_load_thread` (per-thread) |
| Lua callback 在 worker 直接调 → 多线程访问 lua_State | 严禁; callback 仅在主线程 dispatch 阶段调用 |
| Future 句柄被 GC 但 worker 还在跑 → 资源泄漏 | Future 用 ref-counted shared state (worker 持一份, Lua 持一份); 双方释放才回收 |

