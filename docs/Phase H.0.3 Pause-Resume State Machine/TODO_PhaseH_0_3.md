# Phase H.0.3 Pause-Resume State Machine — TODO

> **完成日期**: 2026-05-19
> **状态**: T1~T7 完成

---

## 1. 用户配置

无新增配置. 默认行为:
- 默认 `paused=false` (与 H.0 行为一致).
- SDL_EVENT_DID_ENTER_BACKGROUND 自动触发 `Light.Time.Pause()`.
- SDL_EVENT_WILL_ENTER_FOREGROUND 自动触发 `Light.Time.Resume()`.
- Resume 后第一帧 `dt` 强制为 0 (避免长跳触发 spiral guard).

---

## 2. 推荐验证

### 2.1 Native 手动验证 (G 键)
```powershell
.\build\Release\Light.exe samples\demo_tick_render\main.lua
# 按 G 键: HUD "PAUSED: YES", accumulator/FPS 在 HUD 上保持上一帧值.
# 再按 G 键: PAUSED: no, 立即恢复正常.
# 控制台 log: "TickRender::Pause()" / "TickRender::Resume() (next dt forced to 0)"
```

### 2.2 iOS 实机 (需 Xcode build)
```
1. 跑 demo_tick_render 在 iOS 设备
2. 按 Home 键回桌面 → log "TickRender::Pause() (accumulator frozen)"
3. 回到应用 → log "TickRender::Resume() (next dt forced to 0)"
4. 验证: 应用恢复后不卡顿, 不刷 spiral guard log
```

### 2.3 Android 实机
```
1. 跑 demo_tick_render 在 Android 设备
2. 按 Home 键回桌面 → 同上
3. 切回应用 → 同上
```

### 2.4 Web 浏览器
```
1. 跑 web 版 demo_tick_render
2. 切到其他标签页 → 浏览器自然暂停 callback (H.0.2 路径)
3. 切回 → 取决于 SDL3 是否把 visibilitychange 映射到 SDL_EVENT_DID_ENTER_BACKGROUND
   (实机验证留 dev; 若不自动, 用户可绑 Window:OnVisibilityChange 手动调 Light.Time.Pause/Resume)
```

### 2.5 smoke
```powershell
.\build\Release\Light.exe scripts\smoke\tick_render.lua
# 期待: §1~§13 全 PASS, 包括 6 个 §13 检查
```

---

## 3. 已知限制

### 3.1 实机未测
**当前**: smoke + syntax check + CI build 全 PASS, 但 iOS/Android 模拟器 BG/FG 事件流未实测.
**升级**: dev 在真机跑一遍 demo, 按 Home 键观察 log.

### 3.2 pauseTime 未记录
**当前**: Pause/Resume 不记录时间戳, 无法回答 "暂停了多久".
**升级** (~0.3h):
```cpp
struct State {
    ...
    double pauseStartTime = 0.0;
    double totalPausedDuration = 0.0;
};
// Pause(): pauseStartTime = NowSeconds();
// Resume(): totalPausedDuration += NowSeconds() - pauseStartTime;
// 新增 GetTotalPausedDuration() / GetCurrentPauseDuration()
```

### 3.3 Lua callback 形参为空
**当前**: `Window:OnAppEnterBackground(self)` / `OnAppEnterForeground(self)` 无 timestamp 参数.
**升级**: 若加 pauseTime, 可顺带传入 `Window:OnAppEnterForeground(self, pausedSeconds)`.

### 3.4 Web visibilitychange 映射
**当前**: SDL3 在 web 平台是否自动 emit SDL_EVENT_DID_ENTER_BACKGROUND 取决于 emscripten 实现; 不确定.
**兜底**: 用户可手动绑 visibilitychange (通过 emscripten EM_ASM) → 调 `Light.Time.Pause/Resume`.
**升级**: 在 platform_window_sdl3.cpp 加 web specific hook, 用 emscripten_set_visibilitychange_callback.

---

## 4. 增强候选 (按 ROI)

### 4.1 pauseTime + 总暂停时长统计 ⭐
**估时**: 0.5h
**收益**: 应用层可知"用户离开了多久" → 决定是否需要刷新 token / re-fetch 数据.

### 4.2 Web visibilitychange explicit hook ⭐⭐
**估时**: 1h
**收益**: Web 后台切回时 100% 触发 Pause/Resume (不依赖 SDL3 自动映射).
**实现**: `emscripten_set_visibilitychange_callback` 注入到 `PlatformWindow::Init`.

### 4.3 Pause callback 形参增强 ⭐
**估时**: 0.3h
**收益**: 用户在 callback 内可直接拿到时间戳/原因.
**实现**: `Window:OnAppEnterForeground(self, pausedSeconds)`.

### 4.4 Audio/Music auto-pause 联动 ⭐⭐⭐
**估时**: 2-3h (跨模块)
**收益**: 用户不用手写 OnAppEnterBackground 来停止音乐, 引擎自动调.
**实现**: 在 light_audio_backend.h 加 Suspend/Resume + 在 H.0.3 BG/FG hook 内联动.

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

注: H.0.3 是 4A 轻量, 无 ALIGNMENT/DESIGN/TASK 单独文件 (合并入 CONSENSUS).

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
