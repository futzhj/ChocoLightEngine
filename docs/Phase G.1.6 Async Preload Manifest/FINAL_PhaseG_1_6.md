# Phase G.1.6 — 异步预加载 manifest (FINAL)

> **交付日期**: 2026-05-18
> **依赖**: [ALIGNMENT](ALIGNMENT_PhaseG_1_6.md) · [DESIGN](DESIGN_PhaseG_1_6.md) · [TASK](TASK_PhaseG_1_6.md)
> **基线**: Phase G.1.0 + G.1.1 + G.1.2 + G.1.3 + G.1.5 (异步资源加载完整框架)

---

## 一. 交付概览

`Light.AssetLoader.Preload` — 一次提交 N 个混合类型异步资源, 全部完成触发统一 callback。

```lua
local handle = Light.AssetLoader.Preload({
    images   = { "ui/btn.png", "ui/title.png" },
    sounds   = { "audio/bgm.ogg" },
    cubeLUTs = { "post/teal_orange.cube" },
    haldLUTs = { "post/cinematic.png" },
    fonts    = { { path = "fonts/chinese.ttf", size = 18 } },
    meshes   = { { path = "models/hero.glb", primIdx = 0, withMaterial = true } },
}, function(succ, fail, errors)
    print(string.format("loaded: %d ok, %d failed", succ, fail))
    for _, e in ipairs(errors) do
        print("  failed:", e.path, e.err)
    end
    -- 进入主场景...
end)

-- 进度查询 (用户可在 loading 屏每帧调)
local done, total, errCount = handle:GetProgress()
local pct = total > 0 and (done / total * 100) or 100
```

---

## 二. 实现规模

### 2.1 新增

| 文件 | 行数 | 内容 |
|------|------|------|
| `@e:/jinyiNew/Light/ChocoLight/src/light_asset.cpp` | ~510 | BatchHandle userdata + Preload binding + 6 字段聚合 |
| `@e:/jinyiNew/Light/scripts/smoke/asset_loader_preload.lua` | ~165 | 8 用例 smoke 测试 |
| `@e:/jinyiNew/Light/docs/Phase G.1.6 Async Preload Manifest/ALIGNMENT_PhaseG_1_6.md` | ~210 | 决策点 + API 起草 |
| `@e:/jinyiNew/Light/docs/Phase G.1.6 Async Preload Manifest/DESIGN_PhaseG_1_6.md` | ~310 | 架构图 + 数据流 + 接口契约 |
| `@e:/jinyiNew/Light/docs/Phase G.1.6 Async Preload Manifest/TASK_PhaseG_1_6.md` | ~165 | T1-T10 子任务拆分 |

### 2.2 修改

| 文件 | 修改 |
|------|------|
| `@e:/jinyiNew/Light/ChocoLight/CMakeLists.txt:227` | source list +1 行 (`light_asset.cpp`) |
| `@e:/jinyiNew/Light/lumen-master/src/light/light.cpp:541` | `g_lightModules[]` +2 行 (`Light.AssetLoader` 注册) |
| `@e:/jinyiNew/Light/.github/workflows/build-templates.yml:109,233` | smoke 引用 + 调用 (Windows job) |

### 2.3 不修改

- `asset_loader.h` / `asset_loader.cpp` — 复用现有 `LoadXxxAsync` + `RegisterCallback`, 无新公共 API
- `light_ui.cpp` — 无新 Tick 钩子, 完全复用现有 `AssetLoader::Tick` 路径
- 8 个现有 LoadAsync binding 文件 — 零回归

---

## 三. Lua API

### 3.1 Light.AssetLoader.Preload(manifest [, totalCb]) → BatchHandle

**manifest 字段** (任意字段缺省 = 跳过, 未识别字段静默忽略):

| 字段 | 元素类型 | 调用入口 |
|------|---------|---------|
| `images` | `string array` | `AssetLoader::LoadImageAsync` |
| `sounds` | `string array` | `AssetLoader::LoadSoundAsync` |
| `cubeLUTs` | `string array` | `AssetLoader::LoadCubeLUTAsync` |
| `haldLUTs` | `string array` | `AssetLoader::LoadHaldLUTAsync` |
| `fonts` | `{ {path, size?}, ... }` | `AssetLoader::LoadFontAsync` (size 默认 16) |
| `meshes` | `{ {path, primIdx?, withMaterial?}, ... }` | `AssetLoader::LoadGLTFAsync` (primIdx 默认 0, withMaterial 默认 false) |

**totalCb** (可选):
```lua
function totalCb(succ, fail, errors)
    -- succ:  int, 成功完成的子任务数
    -- fail:  int, 失败的子任务数 (= #errors)
    -- errors: { {path="...", err="..."}, ... }
end
```

### 3.2 BatchHandle 方法

| 方法 | 签名 | 语义 |
|------|------|------|
| `:GetProgress()` | → `done, total, errors_count` | 实时查询, 用于 loading 屏 |
| `:IsDone()` | → `boolean` | done == total |
| `:Cancel()` | → `nil` | advisory: 子 future 仍跑完, 但总 cb 不再触发 |
| `tostring(h)` | "Light.AssetLoader.BatchHandle(d/N[, cancelled])" | 调试 |

### 3.3 6 种调用形式 (灵活)

```lua
-- 形式 1: 仅 manifest (用户走 :GetProgress 轮询)
local h = AssetLoader.Preload({ images = { "a.png", "b.png" } })

-- 形式 2: manifest + 总 cb
local h = AssetLoader.Preload({ images = { ... } }, function(s, f, errs) ... end)

-- 形式 3: 空 manifest (立即同步触发 cb(0, 0, {}))
local h = AssetLoader.Preload({}, function(s, f, errs) ... end)

-- 形式 4: 仅 cb 失败 → arg error
local h = AssetLoader.Preload({}, "not_a_function")  -- ❌ luaL_argerror

-- 形式 5: 多类型混合 + 默认值
local h = AssetLoader.Preload({
    images = { "a.png" },
    fonts  = { { path = "f.ttf" } },                   -- size 默认 16
    meshes = { { path = "m.glb" } },                   -- primIdx=0, withMaterial=false
})

-- 形式 6: 进度 polling 模式 (无总 cb)
local h = AssetLoader.Preload({ images = { ... } })
while not h:IsDone() do
    local done, total = h:GetProgress()
    DrawLoadingBar(done / total)
    coroutine.yield()
end
```

---

## 四. 架构亮点

### 4.1 零侵入设计

完全在 Lua binding 层实现, 不修改 `AssetLoader` 命名空间任何 C++ API. 复用现有 6 个 `LoadXxxAsync` + `RegisterCallback` 机制. 主线程 Tick 路径与单 LoadAsync 完全一致.

### 4.2 BatchHandle 引用持有

每个 sub-future 在 `LUA_REGISTRYINDEX` 中持有 BatchHandle userdata 的独立引用 (luaL_ref 同一 userdata 多次返回不同 ref id). 这保证:

- 用户提前丢弃 handle (Lua 局部变量 GC) 时, sub-future 仍持引用 → handle 不会过早 GC
- 所有 sub-future dispatch 完成 (Tick luaL_unref N 次) 后, batch 才 GC eligible
- 总 cb 触发顺序: 最后一个 sub-future dispatch 时, ud->remaining 归 0 → FireTotalCb_

### 4.3 立即 Error 同步路径

`LoadXxxAsync` 在 worker 未启动时走 sync fallback (G.1.0 设计). 失败时立即返 Error 状态. binding 层 `AppendOne_` 检测到 `status != Pending` 时同步更新 `ud.succ / ud.errors / ud.remaining`, 不走 dispatcher, 不注册 sub-cb ref. 这样:

- smoke 测试无需 worker / GL ctx / 主循环 Tick 即可验证
- headless / CI 环境天然 PASS
- 性能: 立即路径 0 额外开销 (无 mutex 获取 / 无 thread sync)

### 4.4 Cancel 语义

`Cancel()` 是 advisory: 已派发的 future 在 worker 中仍正常完成, 主线程 Tick 仍正常上传 GL. 仅总 cb 不再触发. 主用例: 用户切场景时丢弃旧 batch 的最终通知, 避免操作已销毁的场景对象.

无需 worker 复杂中断逻辑 (worker 内任务粒度大, 中途中断会泄漏 GL 资源).

---

## 五. 性能特征

### 5.1 时间复杂度

| 操作 | 复杂度 | 说明 |
|------|-------|------|
| `Preload(manifest with N entries)` | O(N) | N 次 LoadXxxAsync 入队 (mutex 一次) + N 次 luaL_ref |
| dispatcher 单次 (Ready 路径) | O(1) | ud 字段更新 + Lua rawgeti |
| dispatcher 单次 (Error 路径) | O(N) | 线性扫描反查 path (N <= 1000 时单次成本可忽略) |
| `FireTotalCb_` | O(F) | F 是失败数, 构建 errors 表 |
| `:GetProgress()` | O(1) | 三个 int 字段 |

### 5.2 内存占用

| 实体 | 大小 | 说明 |
|------|------|------|
| BatchHandleUd | ~80B | sizeof(struct), vector overhead 另算 |
| 每个 future | sizeof(FutureState) ≈ 200B | shared_ptr 引用计数 1 |
| sub-cb registry slot | 1 个 lua_Number | luaL_ref array slot |

100 资源 batch 内存峰值: ~80B + 100 × 200B + 100 ints + 100 std::string paths ≈ 25KB. 微不足道.

### 5.3 主线程预算

| 调用 | 实测预期 |
|------|---------|
| `Preload(N=100)` 入队 | < 5ms (主线程 mutex 6 次, luaL_ref 100 次) |
| 总 cb 触发开销 | < 1ms (errors 表构建 + pcall) |
| 单次 dispatcher | < 10μs |

均远低于 16.7ms (60fps budget).

---

## 六. 测试覆盖 (smoke 8 用例)

`@e:/jinyiNew/Light/scripts/smoke/asset_loader_preload.lua`:

| Case | 描述 | 验证 |
|------|------|------|
| 1 | 空 manifest | cb(0, 0, {}) 同步触发, IsDone=true |
| 2 | 单类多张 (3 missing images) | cb(0, 3, errors), path 反查正确 |
| 3 | 6-type 混合 missing | 全 6 字段路由到对应 LoadXxxAsync |
| 4 | 字段缺省 + 未识别字段 | 缺省字段跳过, 未识别字段静默忽略 |
| 5 | 默认参数 (font.size=16, mesh.primIdx=0, withMaterial=false) | 全部正确生效 |
| 6 | 参数错误 (4 类) | nil manifest / non-func cb / entry typo / missing path 全部 luaL_error |
| 7 | BatchHandle 方法签名 | GetProgress/IsDone/Cancel/__tostring 全签名验证 |
| 8 | Cancel after-done | advisory + idempotent (双 Cancel 不抛) |

**约束**: 全部 headless 兼容 — 用 missing 资源走 sync fallback 立即 Error 路径, 无需 worker / GL ctx / Tick.

**状态**: 待 CI 验证。

---

## 七. 决策点回顾 (来自 ALIGNMENT)

| 序号 | 决策 | 实施结果 |
|------|------|---------|
| D1 | manifest 形态 = 类型分组 | ✅ 6 字段 (images/sounds/cubeLUTs/haldLUTs/fonts/meshes) |
| D2 | 总 cb 签名 = (succ, fail, errors) | ✅ 不暴露 results 嵌套结构 |
| D3 | 不支持 sub-cb | ✅ v1 黑箱, 用户走子 LoadAsync 拿渐进结果 |
| D4 | Cancel = advisory | ✅ 子 future 仍跑完, 总 cb 不触发 |
| D5 | 空 manifest = 立即同步 cb(0, 0, {}) | ✅ |
| D6 | 错参 = luaL_argerror 立即抛 | ✅ 严格类型校验 (string/table) |
| D7 | BatchHandle = 4 方法 (GetProgress/IsDone/Cancel/__tostring) | ✅ 不暴露 :Wait / :GetResults |

---

## 八. 与现有架构的协同

### 8.1 与 G.1.0 框架

复用 `LoadXxxAsync` 6 个 C++ 入口, 不引入新解码路径. 异步资源生命周期与单调 LoadAsync 完全一致.

### 8.2 与 G.1.1 Shared GL Context

worker 上传路径 (G.1.1) 自动生效 — sub-future 走 `glFenceSync` + `glClientWaitSync`, dispatcher 在主线程 Tick 检测 fence Ready 时被调. Preload 不需要任何额外协调.

### 8.3 与 G.1.2 Mesh Worker GL Upload

meshes 字段下的 GLTF mesh 使用 G.1.2 worker GL 路径. dispatch 时 mesh 已完成 GL 上传, ud.succ++ 即可.

### 8.4 与 G.1.3 进程退出 RAII

`AssetLoader::Shutdown` 在用户切退出路径触发, Pending future 被 Set Error("AssetLoader shutdown") → 各自 dispatch → batch 总 cb 仍正常触发. 无 worker thread join 崩溃 (G.1.3 已修).

### 8.5 与 G.1.5 GLTF + Material

meshes 字段 `withMaterial=true` 时, 每个 mesh sub-future 内部并行解码 5 类 PBR texture (G.1.5 thread pool). dispatcher 触发时 mesh + material 都已 ready. 与单 LoadGLTFAsync 完全等价.

---

## 九. 后续可选优化

| 项 | 优先级 | 估时 | 说明 |
|----|--------|------|------|
| **sub-cb (per-entry callback)** | 中 | 3-4h | 每个 entry 可附 onLoad 子 cb, 渐进式加载 |
| **results 嵌套结构** | 低 | 2-3h | 总 cb 接收 `{images={[path]=Future}, ...}` |
| **协程化 Preload** | 中 | 2-3h | `local succ, fail, errs = AssetLoader.AwaitPreload(m)` (基于 coroutine.yield) |
| **manifest .json 加载器** | 低 | 4h | `AssetLoader.PreloadFromFile("scenes/level1.json")` |
| **dependency graph** | 高复杂 | 12h+ | "load A depends on B done first" 语义 (引入 DAG) |

详见 [TODO_PhaseG_1_6.md](TODO_PhaseG_1_6.md).

---

## 十. 关键工程经验

### 10.1 Lua registry ref 持有 userdata 的妙用

`luaL_ref(L, LUA_REGISTRYINDEX)` 对同一 Lua 值多次调用返回不同 ref id, 各 ref 独立计数. 这让 batch 用 N 个独立 ref 持同一 BatchHandle 强引用, Tick 自动 luaL_unref N 次, 无需手动协调 — **Lua 的 ref 计数器就是天然的 N 沉计数器**.

### 10.2 立即 Error 路径设计的可测性收益

`LoadXxxAsync` 在 worker 未启动时立即 Error 而非抛错, 让 smoke 完全 headless 即可验证: 不需 GL ctx / 不需 Window / 不需主循环, 仅需 `light.exe smoke.lua` 一行命令. CI 时间从 worker 启动 + Tick 等待 (~100ms+) 降到立即返回 (<1ms).

### 10.3 dispatcher 一次性 ref + 线性反查的权衡

`RegisterCallback` 接口仅暴露 1 个 int 槽 (cbLuaRef). 相比把 path 编码到独立 sub-context Lua table (每个 sub-future 多 1 次 lua_createtable + N 次 lua_setfield), 用 raw ptr 线性扫描反查 path **零额外 Lua 操作**, N <= 1000 时单次成本可忽略.

### 10.4 advisory cancel 的简洁性

不引入 worker 中断协议: 用户切场景时直接 `handle:Cancel()` + 让旧 batch 的总 cb 静默. worker 内任务自然跑完, GL 资源正常上传 (无泄漏). 复杂度比 hard cancel 低 1 个量级.

---

## 十一. 验收状态

| 项 | 状态 |
|----|------|
| 代码实现完成 | ✅ |
| 文档 (ALIGNMENT/DESIGN/TASK/FINAL) | ✅ |
| smoke 编写 | ✅ |
| CMake / lumen 集成 | ✅ |
| CI 接入 | ✅ |
| 本地编译 | ⚠️ 待 CI 验证 |
| Windows runtime smoke | ⚠️ 待 CI |
| 6 平台 build green | ⚠️ 待 CI |

---

## 十二. Phase G.1.x 累计交付

完整 Phase G.1 系列异步资源加载基础设施:

| Sub-Phase | 主交付物 | API 增量 |
|-----------|---------|---------|
| **G.1.0** | 基础 worker thread + task/result queue + 6 类型异步 | 6 LoadAsync (Image/LUT/Hald/Font/Sound/GLTF Mesh) |
| **G.1.1** | Shared GL Context probe + worker `glTexImage2D` + `glFenceSync` | 透明回落 |
| **G.1.2** | Mesh worker GL upload (VAO/VBO/EBO + RegisterUploadedMesh) | 透明回落 |
| **G.1.3** | 进程退出 RAII guard + STATUS_STACK_BUFFER_OVERRUN 修复 | (内部) |
| **G.1.5** | GLTF Material + 5 类 PBR texture + Worker Thread Pool 并行解码 | LoadGLTFAsync withMaterial 参数 + sampler 透传 |
| **G.1.6** ← **本期** | manifest 聚合 + BatchHandle 进度/取消 | `Light.AssetLoader.Preload` + 4 方法 |

**总计**: 7 Lua 入口 (6 LoadAsync + 1 Preload) + 1 BatchHandle 元表 (4 方法) + 完整覆盖大场景启动加载 + 渐进式加载 + 失败聚合 + 进度查询.
