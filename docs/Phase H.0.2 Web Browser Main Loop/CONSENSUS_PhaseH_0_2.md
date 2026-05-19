# Phase H.0.2 Web Browser Main Loop — 6A 轻量合并文档

> **基线**: H.0 (commit `8b7837d`) + H.0.1 (commit `63f75ba`) 已交付, CI 6/6 PASS
> **类型**: H.0 TODO §5.1 落地 — emscripten_set_main_loop_arg 真集成
> **估时**: 4-6h
> **风险**: 涉及 Web 主循环结构, 需保持 32+ 老 sample 零回归

---

## 1. Align — 项目对齐

### 1.1 来源
H.0 TODO §5.1 — Web 后台标签页能耗 / CPU 优化. 从 ASYNCIFY 路径 (whilelooping + emscripten_sleep) 切换到原生 `emscripten_set_main_loop_arg` 主循环.

### 1.2 现状
- **设施已有**: `PlatformWindow::RunMainLoop` 已声明 + 实现 (`emscripten_set_main_loop_arg(frame, ud, 0, 1)`), 但**0 调用方**.
- **当前 Web 主循环**: 走 ASYNCIFY (lua `while UI.Loop() do UI.Resume() end` + C 内部 `emscripten_sleep`).
- **32+ 老 sample**: 全部用 `while UI.Loop() do UI.Resume() end` 写法.

### 1.3 用户决策 (轻量自决)
| 问 | 决策 | 理由 |
|---|---|---|
| 改默认主循环还是加新 API? | **加新 API `Light.UI.RunBrowserMainLoop`** | 32+ sample 零回归; 用户主动 opt-in |
| Native 平台行为? | 阻塞跑等价循环 | 让用户写法可统一: 一行 `Light.UI.RunBrowserMainLoop()` 跨平台 |
| simulate_infinite_loop 取值? | **1 (web 永不返回)** | lua VM 持续 alive; 浏览器异步驱动 |
| Lua side 退出机制? | `Window:Close()` → frame 内 `CancelMainLoop()` + Shutdown | 已有的 ShouldClose 检测复用 |

### 1.4 边界
- **IN**: 加新 API + 抽 RunSingleFrame_ helper + smoke API surface + 文档.
- **OUT**: 改默认 ASYNCIFY 路径 / 改 32+ 老 sample / iOS-Android pause (留 H.0.3) / 真实机 Web 性能基准 (留浏览器手动跑).

---

## 2. Architect — 设计

### 2.1 接口

**Lua API (新)**:
```lua
Light.UI.RunBrowserMainLoop()  -- 阻塞 (native) 或 emscripten 接管 (web)
```

**用法对比**:
```lua
-- 老写法 (32+ sample, 仍工作; 默认 ASYNCIFY 路径)
Light(MyApp):Open(W, H, "Title")
while Light.UI.Loop() do Light.UI.Resume() end

-- 新写法 (H.0.2, 跨平台单行)
Light(MyApp):Open(W, H, "Title")
Light.UI.RunBrowserMainLoop()
-- web 下永不返回; native 下窗口关闭后返回
```

### 2.2 实现要点

**抽 helper**:
```cpp
// 单帧逻辑: 拉事件 + Window:__call. 复用方: l_UI_Resume / BrowserMainLoopFrame_ / l_UI_RunBrowserMainLoop native 循环.
static void RunSingleFrame_(lua_State* L);
```

**Web 路径**:
```cpp
#ifdef __EMSCRIPTEN__
static void BrowserMainLoopFrame_(void* arg) {
    lua_State* L = (lua_State*)arg;
    if (!g_mainWindow || PlatformWindow::ShouldClose(g_mainWindow)) {
        PlatformWindow::CancelMainLoop();
        PerformWindowShutdown_(L);
        return;
    }
    RunSingleFrame_(L);
}
#endif

static int l_UI_RunBrowserMainLoop(lua_State* L) {
#ifdef __EMSCRIPTEN__
    PlatformWindow::RunMainLoop(BrowserMainLoopFrame_, L);
    // simulate_infinite_loop=1 → 永不返回 (浏览器接管)
    return 0;
#else
    // Native: 阻塞循环 (等价 while UI.Loop() do UI.Resume() end)
    while (g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow)) {
        RunSingleFrame_(L);
    }
    PerformWindowShutdown_(L);
    return 0;
#endif
}
```

### 2.3 风险与缓解

| 风险 | 缓解 |
|------|------|
| `simulate_infinite_loop=1` 触发 longjmp 经过 lua_pcall setjmp | 已有 emscripten 内部用 EXIT_RUNTIME=0; 不会 unwind 用户 stack. 但首次 CI 验证 |
| 老 sample 调 RunBrowserMainLoop 不期望 | 加性 API; 老 sample 不调 → 零回归 |
| Native 阻塞循环阻挡 lua VM | 与老写法等价 (lua 在 RunSingleFrame 内 cooperatively 让出) |
| 浏览器后台标签页 callback 暂停 | 这正是预期收益 (能耗下降); accumulator 可能积累 → 复用 frameTimeClamp |

### 2.4 API 表面增量
| 项 | 数量 |
|----|------|
| 新增 Lua fn | 1 (`Light.UI.RunBrowserMainLoop`) |
| 新增 C++ static fn | 1 (`RunSingleFrame_`) + 1 (web only `BrowserMainLoopFrame_`) |

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输入 | 输出 |
|------|------|------|------|
| **T1** 抽 RunSingleFrame_ + 加 RunBrowserMainLoop API | 1.5h | 现有 l_UI_Resume | helper + new API + 注册 |
| **T2** demo 路径示意 (留注释; 老写法仍工作) | 0.3h | demo_tick_render/main.lua | 加注释说可切新 API |
| **T3** smoke API surface | 0.5h | smoke tick_render.lua | §12 验证 RunBrowserMainLoop fn 存在 |
| **T4** ACCEPTANCE/FINAL/TODO | 0.5h | - | 4 件套 |
| **T5** CI 验证 | 0.5h | git push + GH Actions | 6/6 PASS |

**总计**: ~3.3h (估时 4-6h, 节约 因复用 PlatformWindow::RunMainLoop 已有设施)

---

## 4. 验收标准

| 项 | 验证 |
|----|------|
| ✅ `Light.UI.RunBrowserMainLoop` 函数存在 | smoke §12 |
| ✅ Native 平台调用 = 阻塞循环, 窗口关闭后返回 | 实机 |
| ✅ Web 平台调用 = emscripten_set_main_loop_arg + 永不返回 | 实机 (浏览器) |
| ✅ 老 sample `while UI.Loop() do UI.Resume() end` 仍工作 | smoke + CI |
| ✅ 6/6 平台 CI PASS | GH Actions |

### 4.1 零回归
- 老 sample 不调 RunBrowserMainLoop → 路径完全不变.
- Native 路径行为与老 while 等价 (复用 RunSingleFrame_ helper).
- Web 路径行为优化 (主循环不依赖 ASYNCIFY suspend, 浏览器后台暂停时 callback 自然不被调).

---

## 5. 6A 7 件套精简映射

H.0.2 是 H.0 TODO §5.1 的实施, **本文 = ALIGNMENT + CONSENSUS + DESIGN + TASK 合并**, 后续:
- ACCEPTANCE_PhaseH_0_2.md — T1~T5 完成验收
- FINAL_PhaseH_0_2.md — 总结
- TODO_PhaseH_0_2.md — 留观察项

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — H.0.2 轻量 6A 启动 |
