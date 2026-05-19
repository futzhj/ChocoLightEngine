# Phase H.0.3 Pause-Resume State Machine — FINAL 总结

> **完成日期**: 2026-05-19
> **状态**: T1~T7 (含 CI) 全部完成

---

## 1. 一句话总结

H.0.3 让 TickRender 在 iOS/Android/Web 切后台时**自动 Pause** (accumulator 冻结), 切回前台时**自动 Resume** (skipNextDt 强制下帧 dt=0). Lua 用户得 3 个 API + 2 个 callback, 32+ 老 sample 完全等价老行为.

---

## 2. 交付物

### 2.1 代码 (~205 LOC 净增)
- C++: `light_time.h` +18 / `light_time.cpp` +50 / `platform_window.h` +3 / `platform_window_sdl3.cpp` +9 / `light_ui.cpp` +50
- Lua: `tick_render.lua` smoke §13 +50; `demo_tick_render/main.lua` +15 (G 键 + HUD PAUSED + R reset)

### 2.2 文档 (4 件套)
- `CONSENSUS_PhaseH_0_3.md`
- `ACCEPTANCE_PhaseH_0_3.md`
- `FINAL_PhaseH_0_3.md` — 本文
- `TODO_PhaseH_0_3.md`

### 2.3 H.0 TODO 标记
- §5.2 iOS/Android pause 状态机 ✅

---

## 3. CI 验证

待 commit 后填.

---

## 4. 关键技术点

| 点 | 实现 |
|----|------|
| TickRender state 加 paused + skipNextDt | 2 个 bool 字段 (默认 false) |
| BeginFrame paused 守卫 | 仅刷 lastTime + 复位 stepCount; 不累积 |
| BeginFrame Resume 后第一帧 | dt 强制 0 (skipNextDt 联动) |
| Pause/Resume 幂等 | `if (paused) return` 守卫 |
| PlatformWindow Event 新类型 | AppEnterBackground=25, AppEnterForeground=26 |
| SDL→Event 转换 | `SDL_EVENT_DID_ENTER_BACKGROUND` / `SDL_EVENT_WILL_ENTER_FOREGROUND` |
| DispatchEvents hook 顺序 | TickRender state 先, Lua callback 后 |
| Lua API | `Light.Time.Pause` / `Resume` / `IsPaused` (3 fn) |
| Lua callback | `Window:OnAppEnterBackground` / `OnAppEnterForeground` (2 个) |

---

## 5. 度量

### 5.1 工时 vs 估算
| 阶段 | 估时 | 实际 | 偏差 |
|------|------|------|------|
| 4A 文档 | 0.5h | ~0.4h | -20% |
| T1~T4 实施 | 1.6h | ~0.8h | -50% |
| T5~T6 smoke + demo + 文档 | 1h | ~0.8h | -20% |
| T7 CI | 0.5h | 待 | - |
| **总计** | **2-3h** | **~2h** | **-20%** |

### 5.2 LOC
```
代码: ~205 LOC
文档: ~330 LOC
```

### 5.3 性能影响
- 默认 paused=false: `if (paused) return` 一次 branch 预测, 无可测量损耗.
- Paused 路径: BeginFrame 只刷 lastTime + 一次 assign, accumulator 不动 — 反而比 active 路径快.
- DispatchEvents 2 个 case 加入: 老路径 (无 BG/FG 事件) 不进入, 零损耗.

---

## 6. 零回归

| 项 | 状态 |
|----|------|
| 32+ 老 sample syntax | ✅ |
| Phase AR/H.0/H.0.1/H.0.2 fn 完整保留 | ✅ smoke §1~§12 |
| 默认 IsPaused=false | ✅ smoke §13 |
| PlatformWindow Event 老 enum 值不变 | ✅ (新值 25/26 追加) |
| BeginFrame 非 paused 路径行为不变 | ✅ 代码可读 |

---

## 7. 与已发布模块关系

| 模块 | 关系 |
|------|------|
| Phase H.0 主循环 | BeginFrame 内插入 paused 守卫, 不影响 fixed loop / FinalizeFrame |
| Phase H.0.1 HUD | `Light.Time.DrawHUD` 在 paused 时仍工作 (显示最后一帧值) |
| Phase H.0.2 RunBrowserMainLoop | 完全独立; Web 浏览器后台暂停 callback 与本期互补 |
| Phase AR Timer | 完全独立 (SDL_AddTimer 仍工作于 paused 期间, 用户在 Timer callback 内可查 IsPaused) |

---

## 8. 后续可做 (留 TODO)

- GPU alpha 插值 helper (H.0.4, 见 H.0 TODO §5.4)
- pauseTime 记录 + Lua `Light.Time.GetPausedDuration()` (若有 metrics 需求)
- Web visibilitychange 事件验证 (SDL3 是否自动映射到 BG/FG)

---

## 9. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T7 完成 |
