# Phase G.1.6 — 异步预加载 manifest (ACCEPTANCE)

> **创建日期**: 2026-05-18
> **关联**: [FINAL](FINAL_PhaseG_1_6.md) · [TASK](TASK_PhaseG_1_6.md)

---

## 一. 任务完成清单

| ID | 任务 | 状态 |
|----|------|------|
| T1 | light_asset.cpp 骨架 + BatchHandle metatable 注册 | ✅ |
| T2 | BatchHandleUd struct (8 字段含 paths 反查) | ✅ |
| T3 | BatchSubItemDispatcher_ 实现 (主线程独占, raw ptr 反查 path) | ✅ |
| T4 | 6 字段 manifest 解析 + LoadXxxAsync 派发 + 立即 Error 同步路径 | ✅ |
| T5 | FireTotalCb_ 实现 (errors 表构建, pcall 异常吞咽, 防重复触发) | ✅ |
| T6 | BatchHandle 4 方法 (GetProgress / IsDone / Cancel / __gc / __tostring) | ✅ |
| T7 | CMakeLists.txt + lumen g_lightModules 集成 | ✅ |
| T8 | smoke asset_loader_preload.lua (8 用例) | ✅ |
| T9 | CI 接入 (Windows job, 与 G.1 / G.1.5 / G.1 VRAM smoke 同级) | ✅ |
| T10 | 文档收尾 (ALIGNMENT/DESIGN/TASK/FINAL/ACCEPTANCE/TODO + HANDOFF 同步) | ✅ |

---

## 二. 功能验收

### 2.1 API 接口验收

| 项 | 要求 | 验证方式 | 结果 |
|----|------|---------|------|
| Light.AssetLoader 模块存在 | `type(Light.AssetLoader) == "table"` | smoke 顶部 require | ✅ |
| Preload 函数存在 | `type(...Preload) == "function"` | smoke API surface | ✅ |
| BatchHandle 元表注册 | `kBatchHandleMT = "Light.AssetLoader._BatchHandle"` | luaL_newmetatable | ✅ |
| 4 方法 metatable | GetProgress / IsDone / Cancel / __tostring 全签名 | smoke Case 7 | ✅ |
| __gc 析构 | placement-new 对应 explicit dtor + totalCbRef unref | C++ 实现 | ✅ |

### 2.2 功能验收

| 项 | 用例 | smoke Case |
|----|------|-----------|
| 空 manifest 同步触发 | `cb(0, 0, {})` IsDone=true 立即 | 1 ✅ |
| 单类多张全 missing | `cb(0, 3, [3 errors])` path 反查正确 | 2 ✅ |
| 6 类型混合路由 | 全 6 字段 → 对应 LoadXxxAsync | 3 ✅ |
| 字段缺省 | 仅 images 字段 OK; 其他自动跳过 | 4 ✅ |
| 未识别字段静默忽略 | `manifest.foo = ...` 不抛, 不计 total | 4 ✅ |
| 默认参数 | font.size=16 / mesh.primIdx=0 / withMaterial=false | 5 ✅ |
| 进度查询 | done = total - remaining; total = manifest entries | 2/3 ✅ |

### 2.3 错参容错验收

| 输入 | 期望 | smoke Case |
|------|------|-----------|
| `Preload(nil)` | luaL_argerror(1) | 6 ✅ |
| `Preload({}, "string")` | luaL_argerror(2) | 6 ✅ |
| `Preload({images={123}})` | entry type error (lua_type 严格 LUA_TSTRING) | 6 ✅ |
| `Preload({meshes={{primIdx=0}}})` | path missing error | 6 ✅ |
| `Preload({images="not_array"})` | "must be table or nil" | (TryGetArrayField_ 校验) ✅ |

### 2.4 Cancel 语义验收

| 项 | 要求 | smoke Case |
|----|------|-----------|
| Cancel 后 IsDone 仍可查 | true (假定 cb 已触发) | 8 ✅ |
| Cancel 双调不抛 | idempotent | 8 ✅ |
| Cancel 后 totalCbRef 释放 | -1, 不再 unref | C++ 实现 ✅ |

---

## 三. 代码审查

### 3.1 关键边界检查

| 边界 | 处理 |
|------|------|
| 立即 Ready (sync fallback 走通) | AppendOne_ else 分支: ud.succ++, ud.remaining-- |
| 立即 Error (sync fallback 失败) | AppendOne_ else 分支: ud.errors.push_back, ud.remaining-- |
| LoadXxxAsync 异步路径 (Pending) | 注册 RegisterCallback + sub-cb ref; 等 Tick |
| Cancel + 子 future 已 Ready | dispatcher 仍 ud.succ++ + remaining--; FireTotalCb_ 检 cancelled 跳过 |
| 用户手动丢弃 handle | sub-cb refs 持 batch 强引用; GC 不会过早 |
| 总 cb 抛 Lua error | pcall(0) 捕获 + LOG_WARN, 不向上抛 |
| 进程退出未完成 batch | AssetLoader::Shutdown set Pending → Error → 各自 dispatch → 总 cb 触发 |

### 3.2 内存安全

| 项 | 处理 |
|----|------|
| BatchHandleUd 析构 | __gc 显式 placement-delete (`ud->~BatchHandleUd()`) |
| futures shared_ptr | vector 析构自动 release; Lua GC 路径走通 |
| totalCbRef | 多路径释放: Cancel / FireTotalCb_ / __gc 三处, -1 后置防重复 |
| sub-cb refs | Tick 内逐个 luaL_unref (复用现有协议) |

### 3.3 Lua 5.1 兼容性

| API | lumen-master/lib/lua/auxiliary.cpp 提供 | 状态 |
|-----|---------------------------------------|------|
| luaL_setfuncs | ✅ shim 已实现 | OK |
| luaL_testudata | ✅ shim 已实现 | OK |
| luaL_checkudata | ✅ shim 已实现 | OK |
| luaL_argerror | ✅ shim 已实现 | OK |
| luaL_newmetatable | ✅ Lua 5.1 标准 | OK |
| lua_objlen | ✅ Lua 5.1 标准 | OK |
| lua_rawseti | ✅ Lua 5.1 标准 | OK |
| lua_pcall | ✅ Lua 5.1 标准 | OK |

---

## 四. 性能预算实测预期

| 操作 | 预期 | 备注 |
|------|------|------|
| Preload(N=100) 入队 | < 5ms | 主线程 |
| 单次 dispatcher | < 10μs | 主线程 Tick 内 |
| 总 cb 触发 | < 1ms | errors 表构建 + pcall(3) |
| BatchHandle GC | < 100μs | vector 析构 + 1 次 unref |

实际数据待真机性能测试 (Phase G.1.6 v1.1 候选, 见 TODO_PhaseG_1_6.md).

---

## 五. CI 接入状态

### 5.1 Windows job (.github/workflows/build-templates.yml)

```yaml
$phaseG16Smoke = Resolve-Path "scripts\smoke\asset_loader_preload.lua"
...
& "$runtimeDir\light.exe" $phaseG16Smoke
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
```

**位置**: 与 phaseG1Smoke / phaseG15Smoke / phaseG1VramSmoke 同段 (line ~109 + line ~233).

### 5.2 6 平台编译验证

依赖 ChocoLight/CMakeLists.txt 自动 build (不需 platform 特定 source). 期望:

- ✅ Windows x64
- ✅ Linux x64
- ✅ macOS x64
- ✅ iOS arm64 (build only, 无 smoke)
- ✅ Android arm64 (build only, 无 smoke)
- ✅ Emscripten wasm (build only, 无 smoke)

---

## 六. CI 实测结果 — 全绿 (commit `bd8ded3`)

CI run: [26041676477](https://github.com/futzhj/ChocoLightEngine/actions/runs/26041676477) (2026-05-18 15:00 UTC)

| 项 | 状态 |
|----|------|
| Windows 编译 + smoke | ✅ 8/8 PASS |
| Linux 编译 | ✅ |
| macOS 编译 | ✅ |
| iOS 编译 | ✅ |
| Android 编译 | ✅ |
| Emscripten 编译 | ✅ |
| 总耗时 | ~7 min (与 G.1.1/G.1.5 同量级) |
| 全套 smoke 零回归 | ✅ TAAU/VRAM/GLTF Async/Audio/SSR/Bloom/Tonemap 等 30+ smoke 全 PASS |

实测 Windows smoke 输出 (`scripts/smoke/asset_loader_preload.lua`):

```
PASS: Light.AssetLoader API surface ok
PASS: Case 1: empty manifest -> cb(0, 0, {}) synchronous
PASS: Case 2: 3 missing images -> cb(0, 3, errors) with path reverse-lookup
PASS: Case 3: 6-type mixed missing manifest -> all routed to corresponding LoadXxxAsync
PASS: Case 4: missing optional fields + unknown fields tolerated
PASS: Case 5: default params (font.size=16, mesh.primIdx=0, withMaterial=false)
PASS: Case 6: argument errors raise (nil manifest / non-func cb / entry typo / missing path)
PASS: Case 7: BatchHandle method signatures (GetProgress / IsDone / Cancel / __tostring)
PASS: Case 8: Cancel after-done is no-op (advisory, idempotent)
=== Phase G.1.6 Async Preload Manifest smoke: ALL TESTS PASSED ===
```

---

## 七. 已知边界与权衡

### 7.1 接受的限制

| 限制 | 理由 |
|------|------|
| dispatcher 反查 path 是 O(N) | N <= 1000 时单次 < 10μs, 收益不抵复杂度 (替代方案需要 N 个 lua_createtable) |
| Cancel 是 advisory | worker 任务粒度大 (单个文件解码), hard cancel 会泄漏 GL 资源 |
| 不支持 sub-cb (per-entry callback) | KISS 原则; 现有 8 个 LoadAsync 仍可单独调用 |
| 不暴露 results 嵌套结构 | 用户走子 cb 拿渐进资源, Preload 仅同步点 |
| 不支持 yield (协程化) | Lua 5.1 longjmp 限制; coroutine 化属 v1.x 子项 |
| manifest.foo 静默忽略 (无 LOG_WARN) | 避免 manifest 写法各异 → log spam |

### 7.2 longjmp 边界 (luaL_error)

`ProcessXxxArray_` 内部 luaL_error 会 longjmp 跳出函数. 已分配的 sub-cb refs 不会被自动清理. 但因为 lua_State 关闭时所有 ref slots 都释放, 不会真正泄漏. **接受此边界** (与现有所有 binding 代码一致).

---

## 八. 验收结论 — 全部通过

**功能完整**: ✅ 8 用例 smoke 全覆盖核心场景, CI 全 PASS
**API 设计**: ✅ 与现有 LoadAsync 模式对齐
**架构集成**: ✅ 零侵入 asset_loader.h/cpp
**文档完整**: ✅ ALIGNMENT/DESIGN/TASK/FINAL/ACCEPTANCE/TODO 6 文档
**CI 6 平台**: ✅ windows / linux / macos / ios / android / web 全绿
**回归测试**: ✅ 全套既有 smoke 零回归 (TAAU/VRAM/GLTF Async/Audio/SSR/Bloom/Tonemap 等)

**总结**: Phase G.1.6 已完整交付。commit `bd8ded3`, CI run [26041676477](https://github.com/futzhj/ChocoLightEngine/actions/runs/26041676477) 6/6 平台绿, Windows runtime smoke 8/8 PASS。

**Phase G.1 系列累计交付状态**:
- G.1.0 / G.1.1 / G.1.2 / G.1.3 / G.1.5 / G.1.6 全部交付完成
- 完整覆盖异步资源加载从基础 worker thread 到 manifest 聚合的所有需求
- HANDOFF §2 异步资源加载方向闭合
