# Phase G.1.6 — 异步预加载 manifest (ALIGNMENT)

> **创建日期**: 2026-05-18
> **依赖**: Phase G.1.0 (核心 worker), G.1.1 (shared GL ctx), G.1.2 (Mesh worker GL), G.1.5 (GLTF + Material)
> **状态**: Align 阶段

---

## 一. 项目上下文

### 1.1 现有异步资源加载能力

`AssetLoader` namespace (`@e:/jinyiNew/Light/ChocoLight/include/asset_loader.h`) 已交付完整 6 套 C++ 入口:

```cpp
LoadImageAsync(path)                              // Image (P0)
LoadCubeLUTAsync(path)                            // .cube LUT
LoadHaldLUTAsync(path)                            // HALD CLUT PNG
LoadFontAsync(path, size)                         // TTF Font
LoadSoundAsync(path)                              // miniaudio Sound
LoadGLTFAsync(path, primIdx, withMaterial=false)  // glTF Mesh + 可选 Material
```

每个入口返 `shared_ptr<FutureState>`, 状态翻转 `Pending → Ready/Error` 由主线程 `Tick()` 驱动.

### 1.2 现有 Lua API (8 个 LoadAsync)

```lua
Light.Graphics.Image.LoadAsync(path[, cb])          -> Future
Light.Graphics.Mesh.LoadGLTFAsync(...)              -> Future
Light.Audio.Sound.LoadAsync(path[, cb])             -> Future
Light.Graphics.Font.LoadAsync(path[, size][, cb])   -> Future
Light.Graphics.HDR.LoadCubeLUTAsync(path[, cb])     -> Future
Light.Graphics.HDR.LoadHaldLUTAsync(path[, cb])     -> Future
Light.IO.LoadAsync(path, cb)                        -> 直接 SDL3 raw bytes (与 AssetLoader 解耦)
```

每个 binding 走相同模式:
1. `LoadXxxAsync(path)` 拿 `shared_ptr<FutureState>`
2. `RegisterResultPusher` 设置 `Future:Get()` poll 路径的 push 函数
3. 用户提供 cb 时, `RegisterCallback(state, dispatcher, L, cbRef)` 注册主线程 dispatch
4. 立即 Error 路径主线程同步触发一次 dispatcher
5. `PushAsyncFuture` 把 shared_ptr 包成 Lua userdata 返回

### 1.3 主线程调度

`light_ui.cpp::l_UI_Resume` 每帧 `BeginFrame` 内调一次 `AssetLoader::Tick()`, 内部:
- drain `result_queue`
- 主线程 GL 上传 (fallback 路径) / fence 翻 status (G.1.1 worker 上传路径)
- 调 dispatcher 触发 Lua cb + `luaL_unref(cbLuaRef)`

---

## 二. 任务原始需求

### 2.1 用户描述

> 新增 `AssetLoader.Preload({path1, path2, ...})` 一次投递 N 个任务 + 全部完成回调. 估时 3-5h, 大场景启动加载常用模式.

### 2.2 边界确认

**做**:
- 提供统一入口聚合 6 个 LoadXxxAsync, 一次投递多类型 N 个任务
- 全部完成 (Ready 或 Error) 后触发用户提供的总 callback
- 提供 batch handle 供用户查询进度 / 取消

**不做**:
- 不引入 manifest 文件持久化解析 (用户用 Lua table 即可)
- 不实现 image cache / dedup (G.1.5 TODO 已跳过)
- 不实现 priority / depth-of-load / 优先级调度 (单 worker thread, FIFO 即可)
- 不暴露每个子 future 给 Lua (避免 N+1 问题; 用户拿到结果只通过总 cb)

---

## 三. 决策点 (智能决策, 部分需用户确认)

### D1 ｜ Manifest 形态: 类型分组 vs 平铺 entry?

**A. 类型分组 (推荐)**
```lua
Light.AssetLoader.Preload({
    images   = { "a.png", "b.png" },
    sounds   = { "s.wav" },
    cubeLUTs = { "lut.cube" },
    haldLUTs = { "hald.png" },
    fonts    = { { path = "f.ttf", size = 16 } },
    meshes   = { { path = "m.glb", primIdx = 0, withMaterial = true } },
}, function(succ, fail, errors) end)
```

**B. 平铺 entry**
```lua
Light.AssetLoader.Preload({
    {type="image", path="a.png"},
    {type="mesh",  path="m.glb", primIdx=0, withMaterial=true},
    ...
}, function(succ, fail, errors) end)
```

**决策: A**. 理由:
- 按类型分组直接对应 6 个 C++ 入口, binding 层无需运行时类型分发
- Lua 端写法直观, 不需重复 `type=` 字段
- 简单类型 (`images`/`sounds`/`cubeLUTs`/`haldLUTs`) 用 string array 即可, 减少嵌套
- 复合类型 (`fonts`/`meshes`) 用 table array 携带额外参数, 与同步 API 对齐

### D2 ｜ 总 cb 签名 ?

**候选**:
- A. `cb(succ, fail, errors)` — succ/fail 为 int, errors 为 `{{path=..., err=...}, ...}`
- B. `cb(results)` — results 为完整 `{images={[path]=Future, ...}, ...}` 嵌套结构
- C. `cb(succ, fail, errors, results)` — A + B 合并

**决策: A**. 理由:
- 用户拿到加载好的资源, 通常需要按 path 索引使用 (B 看似全, 但 path 又作了 key 增加复杂性)
- Future 在总 cb 触发时已 Ready, 暴露 Future 等于让用户多走一道 `:Get()`
- 简化设计: 用户在子 cb 路径里自己挂 Image / Mesh / Sound 实例;  Preload 仅负责 "全部完成" 同步点
- `errors` 列表足以让用户做错误诊断
- v2 可以加 `results=true` 选项扩展 (向后兼容)

### D3 ｜ 子 future 是否保留 Future:Get() / sub-cb 能力?

**问题**: 用户写 `images={"a.png", "b.png"}` 时, 是否应该有办法在 a.png 加载完时就先用?

**候选**:
- A. 完全黑箱, 用户只通过总 cb 拿结果 (本 Phase 选择)
- B. 每个 entry 可附 `{path=..., onLoad=function(asset) end}` 子 cb
- C. `handle:GetSubFutures()` 拿 future 数组

**决策: A** (v1). 理由:
- KISS, 实现量最小 (3-5h 预算)
- 现有 8 个 LoadAsync 仍可被用户单独调用满足 "边加载边用" 场景
- Preload 定位是 "一组资源全部 ready 才进场景" 模式 (loading screen, level start), 不是渐进流式
- v2 如有需要可加 sub-cb (向后兼容)

### D4 ｜ Cancel 语义 ?

**候选**:
- A. Hard cancel — worker 立即丢弃任务, 已派发的也强制中断
- B. Soft / advisory — 已派发的任务继续跑完, 但 batch 总 cb 不再触发
- C. 不支持 cancel (用户 GC 即可)

**决策: B** (advisory). 理由:
- worker 内部任务粒度大 (一整个文件解码 + 上传), 中途中断会泄漏 GL 资源
- 主用例: 用户切场景, 旧 batch 总 cb 不要再触发 (避免操作已销毁场景)
- 已派发的资源仍正常上传, 主线程 Tick 该走的流程都走
- C++ 层无需任何额外锁, 只在 dispatcher 入口检查 `cancelled` flag

### D5 ｜ Empty manifest (total=0) 行为 ?

**决策**: 立即同步触发总 cb(0, 0, {}). 主线程同步路径, 不入 Tick. 与单 LoadAsync 立即 Error 路径处理一致.

### D6 ｜ Manifest 字段类型错误如何处理 ?

**决策**: `luaL_argerror` 立即抛 (硬错). 错参立即崩比静默忽略更利于调试. 与现有 `luaL_check*` 模式一致.

具体:
- `images` 不是 table → arg error
- `images[i]` 不是 string → arg error
- `meshes[i]` 不是 table → arg error
- `meshes[i].path` 不是 string → arg error
- `meshes[i].primIdx` 缺省 → 默认 0
- `meshes[i].withMaterial` 缺省 → 默认 false
- `fonts[i].size` 缺省 → 默认 16

### D7 ｜ BatchHandle 方法集 ?

**决策**:
```lua
handle:GetProgress()  -> done(int), total(int), errors(int)
handle:IsDone()        -> boolean
handle:Cancel()        -> nil   (只是 advisory flag, 不抛)
handle:__tostring     -> "Light.AssetLoader.BatchHandle(done/total)"
handle:__gc           -> 清理 totalCb ref + 子 future 析构
```

不暴露 `:Wait()` (会卡主线程) / `:GetResults()` (D2 决策放弃).

---

## 四. 验收标准

### 4.1 功能
- ✅ 空 manifest 同步触发总 cb(0, 0, {})
- ✅ 单类型 manifest (5 张图) 异步全部 ready 后触发 cb(5, 0, {})
- ✅ 多类型 manifest (image+sound+font+mesh) 触发顺序无依赖
- ✅ 单个失败 (path 不存在) 不影响其他成功项, errors 列表正确
- ✅ Cancel 后总 cb 不再触发, 但子 future 仍正常完成
- ✅ handle:GetProgress() 反映实时 done / total / errors

### 4.2 错参容错
- ✅ Preload(nil) → arg error
- ✅ Preload({images={123}}) → arg error
- ✅ Preload({foo={"a.png"}}) → 未识别字段忽略 (warning 日志)
- ✅ Preload({}, "not_a_func") → arg error

### 4.3 平台
- ✅ Windows + NVIDIA GL 3.3
- ✅ Linux / macOS / iOS / Android / Emscripten 6 平台 CI 全绿
- ✅ 进程退出无 worker thread join 崩溃 (沿用 G.1.3 修复)

### 4.4 集成
- ✅ smoke `asset_loader_preload.lua` 接入 CI
- ✅ HANDOFF / FINAL / TODO 文档同步

---

## 五. 技术约束

### 5.1 不修改的内容
- `asset_loader.h/cpp` 不增加新公共 API (Preload 完全在 Lua binding 层实现)
- `light_ui.cpp::l_UI_Resume` 不增加新 Tick 钩子 (复用现有 `AssetLoader::Tick`)
- `FutureState` struct 不扩字段

### 5.2 新增内容
- 新文件 `ChocoLight/src/light_asset.cpp`
- `lumen-master/src/light/light.cpp` g_lightModules[] 加 1 行
- `ChocoLight/CMakeLists.txt` source list 加 1 行
- 新 smoke `scripts/smoke/asset_loader_preload.lua`
- `.github/workflows/build-templates.yml` 加 smoke 引用 + 调用
- 新文档目录 `docs/Phase G.1.6 Async Preload Manifest/`

### 5.3 性能预算
- Preload(N=100) 调用本身 (loop + push tasks) < 5ms 主线程
- 总 cb 触发开销 < 1ms (errors 表构建)
- 内存: O(N) FutureState shared_ptr + O(N) 临时 Lua refs

---

## 六. 估时分解

| 阶段 | 内容 | 估时 |
|------|------|------|
| Align | 本文档 + 决策 | 0.3h |
| Design | DESIGN_PhaseG_1_6.md 架构图 + 数据流 | 0.4h |
| Atomize | TASK 拆子任务 | 0.2h |
| 实现 | light_asset.cpp + cmake / module 集成 | 1.5h |
| smoke | asset_loader_preload.lua + CI 接入 | 0.5h |
| 文档 | FINAL + ACCEPTANCE + TODO + HANDOFF | 0.5h |
| **合计** | | **~3.4h** |

预算 3-5h 内.

---

## 七. 待澄清 (主动决策, 用户可后续推翻)

| 序号 | 问题 | 自动决策 |
|------|------|---------|
| Q1 | 是否需要 sub-cb (D3 B/C)? | **No** (v1 黑箱, 现有 LoadAsync 仍可单调) |
| Q2 | 是否需要 results 嵌套结构 (D2 B/C)? | **No** (v1 仅 succ/fail/errors, 用户走子 cb 拿资源) |
| Q3 | 是否需要 Wait / 阻塞 API? | **No** (违反主线程不阻塞原则) |
| Q4 | 是否暴露 manifest 文件解析 (.json/.toml)? | **No** (Lua table 即足够) |

如用户对以上决策有异议, 立即修正本文档并触发 v1.x 子版本.
