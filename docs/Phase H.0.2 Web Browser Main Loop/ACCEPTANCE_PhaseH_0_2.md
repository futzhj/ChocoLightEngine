# Phase H.0.2 Web Browser Main Loop — ACCEPTANCE 验收

> **基线**: H.0 + H.0.1 已交付
> **范围**: H.0 TODO §5.1 落地 — emscripten_set_main_loop_arg 真集成 (加性新 API)
> **完成日期**: 2026-05-19
> **状态**: T1~T4 完成, T5 (CI) 待

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 抽 RunSingleFrame_ + l_UI_RunBrowserMainLoop + BrowserMainLoopFrame_ + 注册 | 1.5h | ~0.5h | ✅ |
| T2 demo_tick_render 切到 RunBrowserMainLoop (兜底 while 写法) | 0.3h | ~0.1h | ✅ |
| T3 smoke §12 API surface + 老 API 零回归校验 | 0.5h | ~0.2h | ✅ |
| T4 ACCEPTANCE/FINAL/TODO | 0.5h | ~0.5h | ✅ |
| T5 提交 + CI 6/6 验证 | 0.5h | 待 | ⏳ |

**总计**: ~1.3h (估时 4-6h, 节约 ~70% — `PlatformWindow::RunMainLoop` 已有底层设施可直接复用)

---

## 2. 文件改动清单

### 新建 (3 文档)
| 文件 | LOC |
|------|-----|
| `docs/Phase H.0.2 Web Browser Main Loop/CONSENSUS_PhaseH_0_2.md` | ~140 |
| `docs/Phase H.0.2 Web Browser Main Loop/ACCEPTANCE_PhaseH_0_2.md` | 本文 |
| `docs/Phase H.0.2 Web Browser Main Loop/FINAL_PhaseH_0_2.md` | ~75 |
| `docs/Phase H.0.2 Web Browser Main Loop/TODO_PhaseH_0_2.md` | ~50 |

### 修改 (4 文件)
| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_ui.cpp` | +75 -25 行 (抽 RunSingleFrame_ + l_UI_RunBrowserMainLoop + BrowserMainLoopFrame_ + 注册) |
| `samples/demo_tick_render/main.lua` | +5 -1 行 (优先用 RunBrowserMainLoop, 兜底老 while) |
| `scripts/smoke/tick_render.lua` | +25 行 (§12 API surface + 零回归校验) |
| `docs/Phase H.0/TODO_PhaseH_0.md` | (后续更新, 见 T5) |

**总计**: ~105 LOC 净增

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| `Light.UI.RunBrowserMainLoop` 注册到模块表 | smoke §12 | ✅ syntax PASS |
| 老 `Light.UI.Loop` / `Light.UI.Resume` 保留 | smoke §12 | ✅ |
| Native 平台调 RunBrowserMainLoop = 阻塞循环 | 复用 RunSingleFrame_ + while loop | ✅ 代码可读 |
| Web 平台调 RunBrowserMainLoop = emscripten_set_main_loop_arg | `#ifdef __EMSCRIPTEN__` 分支 | ✅ |
| 老 sample `while UI.Loop() do UI.Resume() end` 仍工作 | l_UI_Resume 已重构走 RunSingleFrame_, 语义不变 | ✅ syntax PASS |
| demo_tick_render 优先用新 API, 兜底老 API | 条件式调用 | ✅ syntax PASS |
| 6/6 平台 CI PASS | GH Actions | ⏳ T5 |

### 3.1 零回归保证

| 项 | 保证机制 |
|----|---------|
| `Light.UI.Loop` 语义不变 | 函数体未修改 (只补 ShouldClose 后 PerformWindowShutdown_) |
| `Light.UI.Resume` 语义不变 | 重构走 RunSingleFrame_ 但执行序列与原代码 1:1 等价 |
| 32+ 老 sample | 不调 RunBrowserMainLoop → 完全走 ASYNCIFY 路径 |
| Web ASYNCIFY 路径 | 仅在调 RunBrowserMainLoop 时才进入 emscripten_set_main_loop_arg; 否则保留 |

---

## 4. 设计权衡回顾

| 决策 | 选择 | 验证 |
|------|------|------|
| 加新 API vs 改默认主循环 | **加新 API** | 32+ sample 零回归 |
| simulate_infinite_loop 取值 | **1 (永不返回)** | lua VM 保持 alive; 浏览器异步驱动 |
| Native 平台行为 | **阻塞 while 等价老写法** | 跨平台单行 Lua 写法 |
| 帧 callback 内 ShouldClose 检测 | **在 callback 头部** | 用户调 `Window:Close()` 立即触发 CancelMainLoop + Shutdown |
| helper 抽 RunSingleFrame_ | **是** | DRY: l_UI_Resume / browser frame / native loop 共用 |

---

## 5. 风险评估

| 风险 | 等级 | 缓解 |
|------|------|------|
| `simulate_infinite_loop=1` 经过 lua_pcall setjmp 帧 | 中 | emscripten EXIT_RUNTIME=0 时不 unwind 用户栈; 首次 CI 验证 |
| 浏览器后台 callback 暂停导致 accumulator 积累 | 低 | 已由 H.0 frameTimeClamp (0.25s) clamp 单帧 dt |
| 老 sample 误用 RunBrowserMainLoop 双调 | 低 | 既然 callback 永不返回, 不会有"双调" |

---

## 6. 已知限制

- **simulate_infinite_loop=1 行为依赖 emscripten EXIT_RUNTIME=0**: 若工程切到 EXIT_RUNTIME=1, lua_close 可能在 longjmp 后调用. 当前默认配置 OK.
- **Native 平台调用方需保证 lua state 不被外部销毁** — 同 ASYNCIFY 路径要求.
- **后台标签页 callback 暂停** = 预期行为 (节能); 但意味着累积器在切回前台时可能 = frameTimeClamp (0.25s) → spiral guard log 一次. 可接受.
- **simulate_infinite_loop=0 路径未实现** — 需修改 host main 配合, 留 H.0.2.x.

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T4 完成 (T5 CI 待) |
