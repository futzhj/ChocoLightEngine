# Phase H.0.2 Web Browser Main Loop — TODO

> **完成日期**: 2026-05-19
> **状态**: T1~T5 完成

---

## 1. 用户配置

无新增配置. 默认行为:
- `Light.UI.RunBrowserMainLoop()` 调用时:
  - Web: 调 `emscripten_set_main_loop_arg(fn, ud, 0, 1)` — fps=0 (浏览器决定刷新率), simulate_infinite_loop=1.
  - Native: 阻塞循环直到 `Window:Close()`.

---

## 2. 推荐验证

### 2.1 Native (推荐先验)
```powershell
.\build\Release\Light.exe samples\demo_tick_render\main.lua
# 期待: 跟原 `while UI.Loop() do UI.Resume() end` 完全等价
# HUD/物理/键盘控制都正常; 按 ESC 退出
```

### 2.2 Web (浏览器手动)
```bash
# 1. 构建 web 版
cd build_emcc && make demo_tick_render

# 2. 启 server
python -m http.server 8000

# 3. 浏览器开 localhost:8000
# 期待: 60Hz 主循环正常, 切到后台标签页时 callback 暂停 (节能)
# 切回前台时: 累积器可能 = frameTimeClamp (0.25s) → spiral guard log 一次, 然后恢复
```

### 2.3 老 sample (零回归)
```powershell
# 任选一个老 sample (不调 RunBrowserMainLoop)
.\build\Release\Light.exe samples\demo_lighting\main.lua
# 期待: 路径完全不变, 走 ASYNCIFY (web) 或 SDL 主循环 (native)
```

---

## 3. 已知限制

### 3.1 simulate_infinite_loop=1 与 lua_pcall setjmp 帧
**当前**: emscripten 在 simulate_infinite_loop=1 时 throw 一个 JS 异常退出 C++ stack.
**风险**: 若 lua_pcall 已建立 setjmp 帧, longjmp/throw 可能 unwind 错误.
**实际**: emscripten EXIT_RUNTIME=0 (默认) 时不 unwind 用户栈; 应该 OK. 待 CI 与实机验证.
**升级**: 若发现问题, 改用 simulate_infinite_loop=0 + host main 配合 (~3h).

### 3.2 RunBrowserMainLoop 不可重入
**当前**: 调一次就永不返回 (web) 或阻塞 (native). 不支持中途 stop + restart.
**升级**: 加 `Light.UI.StopMainLoop()` (调 PlatformWindow::CancelMainLoop) + 让 native while 检测 stopFlag.

### 3.3 帧率参数缺失
**当前**: emscripten_set_main_loop_arg 第 3 参 fps=0 (浏览器决定).
**升级**: 加 `Light.UI.RunBrowserMainLoop(fps)` 参数, 透传到 emscripten + 用 SDL_Delay 节流 native 循环.

### 3.4 后台标签页累积器
**当前**: 浏览器切后台 → callback 暂停; 切回前台时单帧 dt 可能 = clamp (0.25s) → spiral guard log 一次.
**影响**: 不 crash, 仅日志一行.
**升级**: 配合 H.0.3 (iOS/Android pause 状态机) 一起做 — `Pause()` / `Resume()` API 主动清零累积器.

---

## 4. 增强候选 (按 ROI)

### 4.1 RunBrowserMainLoop(fps) 限帧 ⭐⭐
**估时**: 1h
**收益**: 用户主动限 30/60/144 fps; 移动端省电.
**实现**:
```cpp
static int l_UI_RunBrowserMainLoop(lua_State* L) {
    int fps = (int)luaL_optinteger(L, 1, 0);  // 0 = 自适应
#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop_arg(BrowserMainLoopFrame_, L, fps, 1);
    return 0;
#else
    Uint32 minFrameMs = fps > 0 ? (Uint32)(1000.0 / fps) : 0;
    while (g_mainWindow && !PlatformWindow::ShouldClose(g_mainWindow)) {
        Uint32 start = SDL_GetTicks();
        RunSingleFrame_(L);
        if (minFrameMs > 0) {
            Uint32 elapsed = SDL_GetTicks() - start;
            if (elapsed < minFrameMs) SDL_Delay(minFrameMs - elapsed);
        }
    }
    PerformWindowShutdown_(L);
    return 0;
#endif
}
```

### 4.2 Light.UI.StopMainLoop() ⭐
**估时**: 0.5h
**收益**: 高级用户可在 callback 中调 Stop 主动退出主循环.
**实现**: 设 g_stopMainLoop 标志, 帧 callback 检测.

### 4.3 浏览器实机能耗基准 ⭐⭐
**估时**: 2-3h (手动)
**收益**: 量化 ASYNCIFY vs main_loop 的差异.
**手段**:
- Chrome DevTools Performance 录制 60s 跑 demo, 对比前后 CPU%.
- 后台标签页 60s 测试, 对比 callback 暂停效果.

### 4.4 暴露 RunMainLoop 中途参数 (e.g. fps_per_sec_high_freq) ⭐
**估时**: 0.5h
**收益**: 编辑器场景下可低频跑.

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

注: H.0.2 是 4A 轻量, 无 ALIGNMENT/DESIGN/TASK 单独文件 (合并入 CONSENSUS).

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
