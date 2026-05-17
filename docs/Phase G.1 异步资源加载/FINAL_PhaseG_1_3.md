# Phase G.1.3 — Window Shutdown Path Fix (FINAL)

> **完成日期**：2026-05-18
> **类型**：根因修复 (P2)
> **关联**：闭环 [TODO_PhaseG_1_1.md](TODO_PhaseG_1_1.md) NOTE-1 + NOTE-2

---

## 一. 背景

Phase G.1.x 收尾时，benchmark sample `perf_async_loader` 与 probe smoke 各暴露一个引擎遗留问题：

| 问题 | 现象 | 原始严重度 |
|----|----|----|
| NOTE-1 | probe 脚本 `self:Close()` 后窗口 hang | P3 |
| NOTE-2 | sample 末尾 `STATUS_STACK_BUFFER_OVERRUN` (exit `-1073740791`) | P2 |

实际定位发现：**两者根因相互独立但相互掩盖**，本期一次性修复。

---

## 二. 根因分析

### 根因 A ｜ Lua 回调命名约定不一致

引擎 `Window:__call` 实际触发的回调是 `Update(dt)` + `Draw()`（见 `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp:833-849`），但 G.1.x 期间新增的 `asset_loader_async_probe.lua` 和 `perf_async_loader/main.lua` 都误用了 `OnFrame()`。

后果：
- `OnFrame` 永远不被引擎触发
- 其中的 `self:Close()` 永远不执行
- 窗口必须用户手动关闭 (NOTE-1 表现)
- benchmark 收集不到帧时数据 (TODO-3 早期表现)

### 根因 B ｜ 窗口清理路径只在 `UI.Resume` 内实现

所有 21 个 sample 都用通用模板 `while UI.Loop() do UI.Resume() end`：

```
时序                          UI.Loop  UI.Resume  ShouldClose
1. Update 调 self:Close()        T        T (运行中)    false→true
2. 当前 UI.Resume 完成           T        T (正常一帧分支)  true
3. 下一次 UI.Loop()              F        -            true
4. while 退出                    -        ❌ 不再调用       true
```

清理代码（`AssetLoader::Shutdown` / 13 个 renderer shutdown / GL ctx destroy / SDL3 shutdown）原本只挂在 `l_UI_Resume` 的 `ShouldClose=true` 分支内。模板写法导致**所有 sample 都从未走过该分支**。

为什么 G.1.x 之前没有人发现：
- 21 个 sample 大多不创 worker thread；进程退出时静态对象析构链路上没有 joinable `std::thread`
- GL ctx / SDL 窗口在进程退出时由 OS 强制回收，看不到表面崩溃
- 只有 G.1.x 引入 AssetLoader worker 后，`std::thread g_worker` 静态析构发现 joinable 触发 `std::terminate()`，Windows 上表现为 `STATUS_STACK_BUFFER_OVERRUN`

---

## 三. 修复方案

### 修复 A ｜ Lua 脚本回调命名规范化

- `@e:/jinyiNew/Light/scripts/smoke/asset_loader_async_probe.lua`：`OnFrame` → `Update(dt)` + 空 `Draw()`
- `@e:/jinyiNew/Light/samples/perf_async_loader/main.lua`：同上（已在 TODO-3 期间修复）

### 修复 B ｜ 抽 `PerformWindowShutdown_` 共享 helper

`@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` 修改：

```cpp
static void PerformWindowShutdown_(lua_State* L) {
    if (!g_mainWindow) return;   // 幂等 guard
    // ... 完整 13 个子系统 shutdown 序列 + GL ctx 销毁 + SDL3 shutdown ...
}

static int l_UI_Loop(lua_State* L) {
    if (g_mainWindow && PlatformWindow::ShouldClose(g_mainWindow)) {
        PerformWindowShutdown_(L);   // 任一路径首次检测都触发
    }
    lua_pushboolean(L, g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow));
    return 1;
}

static int l_UI_Resume(lua_State* L) {
    // ...
    if (g_mainWindow && PlatformWindow::ShouldClose(g_mainWindow)) {
        PerformWindowShutdown_(L);   // 与 l_UI_Loop 共用 (幂等)
        // ...
    }
}
```

核心要点：
- **幂等 guard** = `g_mainWindow != nullptr`：清理后 g_mainWindow 置 null，重复调用安全
- **覆盖两条路径**：`UI.Loop` 与 `UI.Resume` 任一路径首次检测到 `ShouldClose` 时都触发
- **保留原顺序**：13 个子系统 shutdown 顺序按渲染管线依赖逆序，与 G.1.x 之前完全一致

---

## 四. 验证矩阵

| 场景 | 修复前 | 修复后 |
|----|----|----|
| `perf_async_loader` sample (5 PNG) | exit=-1073740791 崩溃 | exit=0 ✅, worker thread exit + shutdown complete 日志见 stdout |
| 3 次连续运行 perf sample | 100% 崩 | 100% 干净退出 ✅ |
| `asset_loader_async_probe.lua` | OnFrame 命名错 + 退出崩溃, hang | exit=0 ✅, 2 秒内完成 |
| `asset_loader_async.lua` (无窗口) | OK | OK ✅ (不受影响) |
| `ssao.lua` smoke | OK | OK ✅ (清理路径行为不变) |
| `bloom.lua` smoke | OK | OK ✅ (清理路径行为不变) |
| `hdr.lua` smoke | exit=1 (pre-existing FAIL) | exit=1 (同 pre-existing FAIL, 与本修复无关) |

实测数据 (NVIDIA RTX, GL 3.3 NVIDIA 560.94, Windows 10)：

```
[perf_async_loader] frames=7  avg=2.04ms  P50=1.28ms  P95=5.07ms  P99=5.07ms  Max=5.07ms
[perf_async_loader] loaded 5/5 textures (errors=0)
[perf_async_loader] PASS: P95 < 16.7ms (60fps budget held)
[I] AssetLoader: worker thread exit
[I] AssetLoader: shutdown complete
perf_async_loader sample done
exit_code=0
```

---

## 五. 副作用与影响范围

### 正向

- **所有 21 个 sample** 现在都会执行真正的引擎清理路径（之前全部跳过）
- 消除潜在资源泄漏：GL ctx / SDL 窗口 / Audio backend / Network / 13 个 renderer 都会被正确 shutdown
- 后续如果新加 worker thread / 异步系统，引擎层已经具备幂等的关停入口

### 中性

- 用户层 API 完全不变（`UI.Loop` 与 `UI.Resume` 签名、行为外观一致）
- 所有现存 Lua 代码无需修改

### 风险

- 修改前依赖 OS 强制回收的资源（GL ctx / SDL window）现在会显式 shutdown。如果某个 renderer 的 `Shutdown()` 实现有 bug（例如 nullptr 解引用），现在可能暴露出来。但本期 4 个 smoke + 3 次 sample 验证未发现新问题。

---

## 六. 改动清单

| 文件 | 改动 | 行数 |
|----|----|----|
| `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` | 抽 `PerformWindowShutdown_` helper + `l_UI_Loop` 触发清理 | +87 -39 |
| `@e:/jinyiNew/Light/scripts/smoke/asset_loader_async_probe.lua` | `OnFrame` → `Update(dt)` + 空 `Draw()` | +5 -2 |
| `@e:/jinyiNew/Light/samples/perf_async_loader/README.md` | 移除"已知崩溃问题"段, 改为"Phase G.1.3 已修复" | +5 -7 |
| `@e:/jinyiNew/Light/docs/Phase G.1 异步资源加载/TODO_PhaseG_1_1.md` | NOTE-1 + NOTE-2 标记已修复 | +29 -39 |

---

## 七. 经验沉淀

1. **回调命名一致性**：新增 sample / smoke 时必须 grep 现有 sample 看回调约定。本期发现 `OnFrame` 与 `Update + Draw` 不一致的根因是凭直觉命名而非看现有代码。

2. **退出路径必须幂等**：引擎层清理代码不能只挂在 "一次性" 分支，必须在所有可能到达终态的路径都触发。`g_mainWindow != nullptr` 是天然的幂等 guard。

3. **`std::thread` 静态析构的陷阱**：任何引擎全局 `std::thread` 必须在进程退出前 join 或 detach。这是 C++ 标准强制要求，与 OS 资源回收无关。

4. **崩溃可能被掩盖**：21 个 sample 都有同样隐患，但只有 G.1.x 引入 worker thread 后才暴露。需要在引擎层主动加入自检机制（例如 atexit 钩子断言 worker 已 join）。

---

## 八. 后续加固 — 已完成 (2026-05-18)

三件套加固均已交付，防退化机制就位：

### 8.1 ✅ Lua 回调命名规范化文档 — `docs/API_REFERENCE.md` 加 `Light.UI` 章节

新增 `## Light.UI` 节（行 1954+），包含：

- **概览** + **主循环模式**（标准 `while UI.Loop() do UI.Resume() end`）
- **执行顺序流程图**（DispatchEvents → ShouldClose 判断 → __call → Draw + Update + Swap）
- **Window 回调约定表**（14 个回调名 + 签名 + 触发位置 + 行号引用）
- **不存在的回调清单**（`OnFrame` / `OnClose` / `OnResize` / `onUpdate` 等常见误用名）
- **Window 实例方法 + 静态 API**
- **退出与清理章节**（13 个子系统 shutdown 顺序 + 幂等保证 + 两种主循环写法等价性）

防止后续开发者再踩 `OnFrame` / 大小写错误 等坑。

### 8.2 ✅ 进程退出审计钩子 — `light_ui.cpp` `WindowLifecycleAuditor` (atexit)

`@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp:761-803` 新增匿名 namespace 的 `WindowLifecycleAuditor` 静态对象，在构造时 `atexit(&WindowLifecycleAudit_)` 注册退出钩子，进程正常退出（main 返回）时检查 6 项资源状态：

| 检查项 | 触发条件 |
|----|----|
| `g_mainWindow not destroyed` | 窗口未销毁 |
| `g_glContext not destroyed` | GL ctx 未销毁 |
| `g_render not deleted` | 渲染后端泄漏 |
| `PlatformWindow::Shutdown not called` | SDL3 仍 inited |
| `g_windowRef still held in Lua registry` | Lua 引用未释放 |
| `AssetLoader worker thread still running` | worker 仍在跑 |

任一非 nullptr / true → 输出 `[ChocoLight] WARNING` 到 stderr + 修复指引（指向 `docs/API_REFERENCE.md (Light.UI section)`）。

正常路径（PerformWindowShutdown_ 已被走）：6 项全为 nullptr/false → 静默返回。

### 8.3 ✅ Worker Thread RAII 兜底 — `asset_loader.cpp` `WorkerThreadGuard`

`@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp:147-179` 新增 RAII guard 解决 atexit 钩子在 fast-fail 场景失效的问题：

**为什么需要**：
- 进程退出时若 `g_worker` 仍 joinable → `std::thread::~thread()` 调用 `std::terminate()` → Windows 上是 fast-fail (`__fastfail`) → 跳过 atexit handlers → audit 钩子无法运行
- 测试验证：直接 `Demo:Open()` 后不进 main loop → 旧版本 exit=-1073740791 (`STATUS_STACK_BUFFER_OVERRUN`)，audit warning 数量=0

**实现**：
```cpp
namespace {
struct WorkerThreadGuard {
    ~WorkerThreadGuard() {
        if (!g_worker.joinable()) return;   // 正常路径: 静默
        // 异常路径: 主动唤醒 worker + detach (不能 join 否则可能死锁)
        g_shouldStop.store(true);
        g_taskCv.notify_all();
        g_worker.detach();
        g_running.store(false);
        fputs("[AssetLoader] WARNING: worker thread not joined ... detached as last resort.\n", stderr);
        fflush(stderr);
    }
};
static WorkerThreadGuard g_workerGuard;   // 必须在 g_worker 之后声明 (LIFO 析构序)
} // namespace
```

**关键设计点**：
- 静态对象同 TU 内 LIFO 析构 → guard 后声明确保先析构 → 在 `g_worker.~thread()` 之前主动 detach
- 不调 join：worker 可能阻塞在 `cv.wait`，主线程已无法 notify → 死锁。detach 让 OS 在进程退出时回收
- 与 audit 钩子配合：guard 先 detach (在静态析构期) → audit 在 atexit 期检查时 `IsRunning()=false` → 不重复警告 worker，但其他 5 项资源未释放仍会列出

### 8.4 ✅ 生命周期 smoke — `scripts/smoke/window_lifecycle.lua`

新增专门验证清理路径的 smoke：

- 开窗口 + 触发 `Image.LoadAsync`（确保 worker thread 启动）
- 第 3 帧 `self:Close()`
- 期望 stdout 出现 `AssetLoader: worker thread exit` + `AssetLoader: shutdown complete`
- 期望 stderr 完全干净（无 `[ChocoLight] WARNING`、无 `[AssetLoader] WARNING`）
- 期望 exit_code = 0

实测：✅ 全部满足。

---

## 九. 加固验证矩阵

### 正常路径 (8/8 全部 audit 沉默, exit=0 except hdr pre-existing)

| Smoke / Sample | exit | audit warnings | 备注 |
|----|----|----|----|
| `window_lifecycle.lua` | 0 | 0 | 新增, 主验证目标 |
| `asset_loader_async_probe.lua` | 0 | 0 | G.1.1 probe |
| `asset_loader_async.lua` | 0 | 0 | 无窗口分支 |
| `perf_async_loader/main.lua` | 0 | 0 | benchmark sample |
| `ssao.lua` | 0 | 0 | 普通图形 |
| `mesh_3d.lua` | 0 | 0 | G.1.2 mesh 路径 |
| `bloom.lua` | 0 | 0 | 普通图形 |
| `hdr.lua` | 1 | 0 | pre-existing FAIL, 与本期无关 |

### 异常路径 (负面测试: 故意跳过清理)

通过临时脚本 `Demo:Open(...)` 后直接 `return`（不进 `while UI.Loop()` 循环）：

| 行为 | 修复前 | 修复后 |
|----|----|----|
| exit code | `-1073740791` (`STATUS_STACK_BUFFER_OVERRUN`) | `0` ✅ |
| `[AssetLoader] WARNING: worker thread not joined ... detached` | 无 (fast-fail) | 1 行 ✅ |
| `[ChocoLight] WARNING: window lifecycle audit failed` | 无 (atexit 跳过) | 完整列表 ✅ |
| 其余 5 项资源 (g_mainWindow / g_glContext / g_render / g_platformInited / g_windowRef) | 静默泄漏 | 列出 + 修复指引 ✅ |

### 修改清单 (本期加固增量)

| 文件 | 改动 | 行数 |
|----|----|----|
| `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` | +`WindowLifecycleAuditor` (atexit hook) + `<cstdio>` | +43 |
| `@e:/jinyiNew/Light/ChocoLight/src/asset_loader.cpp` | +`WorkerThreadGuard` (RAII) | +33 |
| `@e:/jinyiNew/Light/docs/API_REFERENCE.md` | +`## Light.UI` 章节 + 目录 | +112 |
| `@e:/jinyiNew/Light/scripts/smoke/window_lifecycle.lua` | 新建生命周期 smoke | +66 |

---

## 十. 经验沉淀 (补充)

5. **Atexit ≠ Static dtor**: atexit 调用顺序在静态析构之前，但**不同 TU 静态对象构造 / 析构顺序未定义**。Windows fast-fail (`__fastfail` / `STATUS_STACK_BUFFER_OVERRUN`) 跳过 atexit + 静态析构两条链。结论：永远不要把进程清理逻辑只挂在 atexit；必须用 RAII guard 确保资源在 fast-fail 之前已被处理。

6. **`std::thread` 全局变量必须配 RAII guard**: 任何全局 `std::thread` 都应用 RAII wrapper 保护，dtor 内主动 detach 或 set stop flag + notify。这是 C++ 标准库设计的硬性要求 (参考 `std::jthread`，C++20)。

7. **detach 优于 join 作为兜底策略**: 进程退出阶段 join 可能死锁（主线程已无法满足 worker 的 cv.wait 条件）。detach 让 OS 回收，是"两害相权取其轻"。但 detach 后 worker 可能正在写共享资源 — 必须确保所有共享资源在 detach 前已 set 为 stop 状态。

8. **诊断输出必须用 fputs/fprintf 而非 logger**: 进程退出阶段，自定义 logger / Lua state / SDL log 的全局可能已析构。直接 `fputs(..., stderr)` 是唯一安全方式（CRT 的 stdio 在 atexit 之后才关）。
