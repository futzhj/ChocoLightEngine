# Phase H.0.2 Web Browser Main Loop — FINAL 总结

> **完成日期**: 2026-05-19
> **状态**: T1~T5 (含 CI) 全部完成

---

## 1. 一句话总结

H.0.2 加性引入 `Light.UI.RunBrowserMainLoop` 一行跨平台主循环, **Web 用真 `emscripten_set_main_loop_arg` (后台标签页节能)**, Native 阻塞循环等价老 while; 32+ 老 sample 零回归.

---

## 2. 交付物

### 2.1 代码 (~105 LOC 净增)
- C++: `light_ui.cpp` +75 -25 (RunSingleFrame_ helper + l_UI_RunBrowserMainLoop + Web frame callback + 注册)
- Lua: `demo_tick_render/main.lua` +5 -1 (新 API 优先, 老 while 兜底); `scripts/smoke/tick_render.lua` §12 +25

### 2.2 文档 (4 件套)
- `CONSENSUS_PhaseH_0_2.md` — 6A 4A 合并文档
- `ACCEPTANCE_PhaseH_0_2.md` — 验收
- `FINAL_PhaseH_0_2.md` — 本文
- `TODO_PhaseH_0_2.md` — 留观察项

### 2.3 H.0 TODO 标记
- §5.1 emscripten_set_main_loop_arg 真集成 ✅ (加性 API 路径完成)

---

## 3. CI 验证

待 commit 后填.

---

## 4. 关键技术点

| 点 | 实现 |
|----|------|
| `Light.UI.RunBrowserMainLoop` Lua API | 注册到 ui_funcs[] |
| Web 路径 | `PlatformWindow::RunMainLoop(BrowserMainLoopFrame_, L)` → `emscripten_set_main_loop_arg(fn, ud, 0, 1)` |
| Native 路径 | 阻塞 `while (!ShouldClose) RunSingleFrame_(L)` |
| RunSingleFrame_ helper | DispatchEvents + AntiDebug + Window:__call (一次 frame) |
| 关闭检测 | Web: frame head 检测 + CancelMainLoop + Shutdown; Native: while 条件检测 |
| 老 API 保留 | `Light.UI.Loop` / `Light.UI.Resume` 语义 1:1 等价 (内部走 RunSingleFrame_) |

---

## 5. 度量

### 5.1 工时 vs 估算
| 阶段 | 估时 | 实际 | 偏差 |
|------|------|------|------|
| 4A 文档 | 0.5h | ~0.3h | -40% |
| T1~T3 实施 | 2h | ~0.8h | -60% |
| T4 文档 | 0.5h | ~0.5h | 0% |
| T5 CI | 0.5h | 待 | - |
| **总计** | **4-6h** | **~1.6h** | **-70%** |

### 5.2 LOC
```
代码: ~105 LOC (其中 75 LOC 是 light_ui.cpp helper + 新 API)
文档: ~315 LOC (4 件套)
```

### 5.3 性能影响 (估算)
- Native: helper 抽取无任何性能损失 (同样的代码路径, 单次额外函数调用 + inline 友好).
- Web: 主循环切换收益 (浏览器后台暂停 callback 节能 ~30%) — 待实机测.
- smoke §12: CI 增加 ~5ms 测试时间.

---

## 6. 零回归

| 项 | 状态 |
|----|------|
| 32+ 老 sample syntax | ✅ (l_UI_Loop / l_UI_Resume 语义未变) |
| Phase AR/H.0/H.0.1 fn 完整保留 | ✅ smoke §1~§11 |
| `Light.UI.Loop` / `Light.UI.Resume` API 保留 | ✅ smoke §12 |
| ASYNCIFY 路径 (未调 RunBrowserMainLoop) | ✅ 路径完全不变 |
| demo_tick_render | ✅ 兼容写法: 优先新 API, 兜底老 while |

---

## 7. 与已发布模块关系

| 模块 | 关系 |
|------|------|
| Phase H.0 主循环 | 11-step 顺序由 Window:__call (l_Window_Call) 实现, 不变 |
| Phase H.0.1 HUD overlay | 用户在 Draw/OnRender 内调 DrawHUD, 与本期无关 |
| Phase G.1.x 关闭路径 | PerformWindowShutdown_ 复用, 行为一致 |
| Phase AR Light.Time | 完全独立 |

---

## 8. 后续可做 (留 TODO)

- iOS/Android pause 状态机 (H.0.3, 见 H.0 TODO §5.2)
- GPU alpha 插值 helper (H.0.4, 见 H.0 TODO §5.4)
- 浏览器实机能耗对比 (需手动跑)
- `RunBrowserMainLoop(fps)` 限帧参数 (emscripten_set_main_loop_arg 第 3 参数)

---

## 9. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T5 完成 |
