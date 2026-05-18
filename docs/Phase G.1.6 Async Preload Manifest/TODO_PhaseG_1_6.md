# Phase G.1.6 — 待办事项 (TODO)

> **创建日期**: 2026-05-18
> **状态**: 主体交付完成. 本文记录后续可继续优化方向 + 用户配置需求.

---

## 一. 已完成 (Phase G.1.6 主体)

- ✅ `Light.AssetLoader.Preload(manifest, totalCb)` 入口
- ✅ 6 字段聚合 (images / sounds / cubeLUTs / haldLUTs / fonts / meshes)
- ✅ 灵活参数 (默认值 / 嵌套 entry table / 字段缺省 / 未识别字段静默忽略)
- ✅ BatchHandle 4 方法 (GetProgress / IsDone / Cancel / __tostring)
- ✅ 立即 Error 同步路径 + 异步 dispatcher 路径
- ✅ FireTotalCb_ 防重复触发 + pcall 异常吞咽
- ✅ Cancel advisory 语义 (子 future 仍跑完, 总 cb 不触发)
- ✅ 路径反查 (失败 entry 在 errors 中带原始 manifest path)
- ✅ 错参 luaL_argerror 严格抛 (4 类: nil manifest / non-func cb / entry typo / missing path)
- ✅ smoke 8 用例 + CI 接入
- ✅ 文档 6 篇 (ALIGNMENT / DESIGN / TASK / FINAL / ACCEPTANCE / TODO)

---

## 二. 可选优化 (后续候选)

### 2.1 功能扩展

#### T1. Per-entry sub-callback (Phase G.1.6.1 候选)

- **现状**: 用户拿不到单个 entry 的 ready 时机 (只能等总 cb)
- **场景**: 渐进式加载, e.g. `images` 第一张先显示在 splash screen, 不等其他完成
- **方案**: manifest 每个 entry 可附 `onLoad` 子 cb
  ```lua
  AssetLoader.Preload({
      images = {
          { path = "splash.png", onLoad = function(img, err) ... end },
          { path = "ui.png" },
      },
  }, function(succ, fail, errors) ... end)
  ```
- **位置**: `light_asset.cpp::ProcessStringArray_` / `ProcessFontsArray_` / etc; 每个 entry 转 table 形态后增加 onLoad 字段解析
- **优先级**: 中 (有真实需求时再做)
- **预估**: 3-4h

#### T2. Results 嵌套结构 (Phase G.1.6.2 候选)

- **现状**: 总 cb 仅返 succ/fail/errors, 用户拿到资源需走子 cb 路径
- **场景**: 用户写法 `local res = await Preload(...)` 后期望 `res.images["a.png"]:Draw(...)`
- **方案**: 总 cb 加 results 参数
  ```lua
  cb(succ, fail, errors, results)
  -- results = {
  --     images = { ["a.png"] = imageUserdata, ["b.png"] = imageUserdata },
  --     sounds = { ["s.wav"] = soundUserdata },
  --     ...
  -- }
  ```
- **位置**: `light_asset.cpp::FireTotalCb_` 增加第 4 参数构建; 需要 BatchHandleUd 增加 type-specific 资源映射
- **优先级**: 中
- **预估**: 4-5h (含资源 push 函数复用)

#### T3. 协程化 AwaitPreload (Phase G.1.6.3 候选)

- **现状**: 仅 cb 风格 + 轮询风格
- **场景**: Lua 写同步代码享异步性能
  ```lua
  local succ, fail, errs = AssetLoader.AwaitPreload(manifest)
  ```
- **方案**: `AssetLoader.AwaitPreload(manifest)` 内部:
  1. 创建 BatchHandle 不带 cb
  2. coroutine.yield() 让出主循环
  3. 主线程 Tick 后, 检查 IsDone; true 则 coroutine.resume 返
- **限制**: Lua 5.1 主 coroutine 不能 yield (lua_yield 错误); 需用户在普通 coroutine 中调
- **位置**: `light_asset.cpp` 加 `l_AssetLoader_AwaitPreload`; 注意 yield 与 lua_State 协议
- **优先级**: 中 (减负开发者)
- **预估**: 2-3h

#### T4. manifest .json 加载器 (Phase G.1.6.4 候选)

- **现状**: 用户必须 inline Lua table; 不便复用 / 可视化编辑
- **场景**: 关卡设计师维护 .json 资源清单 (DCC 工具友好)
  ```lua
  AssetLoader.PreloadFromFile("scenes/level1.json", cb)
  ```
- **方案**: 复用 cJSON / Light.Data 解析 .json → Lua table → 转给 Preload
- **位置**: 新增 `l_AssetLoader_PreloadFromFile`
- **优先级**: 低 (用户可在 Lua 层自行解析后调 Preload)
- **预估**: 4h (含错误处理)

#### T5. Dependency Graph (Phase G.1.6.5 候选, 大块)

- **现状**: 6 字段并行加载, 无依赖关系语义
- **场景**: "load A 必须等 B 先 done" (e.g. material depends on texture)
- **方案**: manifest 引入 DAG, e.g.
  ```lua
  {
      groups = {
          textures = { images = { ... } },
          materials = { meshes = { ... }, depends = { "textures" } },
      },
  }
  ```
- **位置**: 引入 group + topological sort + 多阶段触发
- **优先级**: 低 (复杂度高, 用户可分多次 Preload 实现)
- **预估**: 12h+

---

### 2.2 性能优化

#### T6. perf_async_preload Sample (推荐先做)

- **现状**: FINAL 文档给定的性能预算是基于代码结构估算, 未真机验证
- **方案**: 仿 `samples/perf_async_loader/` 写一个 Preload N=100 / 500 / 1000 真机 benchmark
  - 主线程帧时 P50 / P95 / P99 / Max
  - Preload 调用 wall-clock 耗时
  - 总 cb 触发延迟 (从最后 dispatch 到 cb 入口)
- **位置**: `samples/perf_async_preload/main.lua` + README
- **优先级**: 中 (验证设计假设)
- **预估**: 2-3h

#### T7. dispatcher 反查 path 优化 (低优先级)

- **现状**: `BatchSubItemDispatcher_` 用 raw ptr 线性扫描 ud->futures 反查 path. N <= 1000 单次 < 10μs
- **场景**: N > 1000 / N > 10000 大 batch
- **方案 A**: 用 `std::unordered_map<FutureState*, size_t>` 索引 (额外内存 ~50B/entry, O(1) 查询)
- **方案 B**: 把 path 存到独立 sub-context Lua table, dispatcher pop 后读取
- **优先级**: 低 (绝大多数用例 N <= 100)
- **预估**: 1-2h (方案 A) / 2-3h (方案 B)

---

### 2.3 文档 / Demo

#### T8. 演示 Sample (demo_preload_manifest)

- **现状**: 仅 smoke 测试; 用户没有可视化参考
- **方案**: `samples/demo_preload_manifest/main.lua` 演示:
  - Loading 屏 (进度条 / 百分比 / 失败列表 hover)
  - 切场景时 Cancel 旧 batch
  - 切到主场景渲染加载好的资源
- **位置**: `samples/demo_preload_manifest/{main.lua,README.md}`
- **优先级**: 中 (用户友好 / 文档配套)
- **预估**: 3-4h

#### T9. API 文档 docs/api/Light_AssetLoader.md

- **现状**: 仅 6A 工作流文档, 无独立 API 速查
- **方案**: 新增 `docs/api/Light_AssetLoader.md` 含:
  - Preload 完整签名 + 参数表
  - 6 字段语义
  - BatchHandle 方法
  - 错误处理表
  - 完整示例 (Loading 屏)
- **位置**: 与 docs/api/Light_Graphics.md 同级
- **优先级**: 中
- **预估**: 1h

---

## 三. 配置 / 环境

| 项 | 是否需要 | 说明 |
|----|----|----|
| 新依赖 | ❌ 无 | 复用现有 AssetLoader 框架 |
| 新平台支持 | ❌ 无 | 6 平台原生兼容 |
| CI 自托管 GPU runner | ❌ 无 | smoke 全 headless (立即 Error 同步路径) |
| 资源 fixture | ❌ 无 | smoke 用 `__missing_*` 路径 |

---

## 四. 风险与监控

| 风险 | 当前状态 | 监控方式 |
|------|---------|---------|
| 6 平台编译失败 | ⚠️ 待 CI 验证 | GitHub Actions 6 job 全绿 |
| 未识别字段静默忽略 → 用户 typo 难发现 | 🟡 设计选择 | 后续可加 `strict=true` 选项 |
| dispatcher 反查 O(N) | 🟢 接受 (N <= 1000 时单次 <10μs) | T7 监控真实场景 N |
| 用户 totalCb 内 yield → pcall 报错 | 🟡 文档说明 | T9 API 文档明确禁忌 |

---

## 五. 推荐处理顺序 (剩余)

| 优先级 | 任务 | 预估 | 收益 |
|----|----|----|----|
| **P0** | (无) 主体已完成 | — | — |
| P1 | T6 perf_async_preload sample | 2-3h | 验证性能预算 |
| P1 | T8 demo_preload_manifest sample | 3-4h | 用户友好 + 文档配套 |
| P1 | T9 API 文档 Light_AssetLoader.md | 1h | API 速查 |
| P2 | T1 Per-entry sub-cb | 3-4h | 渐进式加载场景 |
| P2 | T2 Results 嵌套结构 | 4-5h | 同步语义友好 |
| P3 | T3 协程化 AwaitPreload | 2-3h | 减负开发者 |
| P3 | T4 .json manifest 加载器 | 4h | DCC 工具集成 |
| P4 | T5 Dependency Graph | 12h+ | 大复杂度 |
| P4 | T7 dispatcher O(1) 优化 | 1-2h | 当前用例无需 |

---

## 六. 联系方式 / 反馈

任何 Phase G.1.6 实测问题 / 用户反馈 / 迭代建议, 直接更新本文档第 5 节优先级表.

主交付物索引:
- 入口实现: `@e:/jinyiNew/Light/ChocoLight/src/light_asset.cpp`
- 编译接入: `@e:/jinyiNew/Light/ChocoLight/CMakeLists.txt:227`
- 模块注册: `@e:/jinyiNew/Light/lumen-master/src/light/light.cpp:541`
- smoke 测试: `@e:/jinyiNew/Light/scripts/smoke/asset_loader_preload.lua`
- 6A 文档: `@e:/jinyiNew/Light/docs/Phase G.1.6 Async Preload Manifest/`
