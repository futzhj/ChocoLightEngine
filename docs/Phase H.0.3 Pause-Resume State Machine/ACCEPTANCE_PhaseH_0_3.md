# Phase H.0.3 Pause-Resume State Machine — ACCEPTANCE 验收

> **基线**: H.0 + H.0.1 + H.0.2 已交付
> **范围**: H.0 TODO §5.2 落地 — iOS/Android pause 状态机 (Web 后台亦受益)
> **完成日期**: 2026-05-19
> **状态**: T1~T6 完成, T7 (CI) 待

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 TickRender Pause/Resume/IsPaused + BeginFrame skip | 0.5h | ~0.3h | ✅ |
| T2 PlatformWindow Event 2 类型 + PollEvent SDL 转换 | 0.3h | ~0.1h | ✅ |
| T3 light_ui DispatchEvents hook + 2 个 DispatchOn* | 0.5h | ~0.2h | ✅ |
| T4 Lua wrappers (Pause/Resume/IsPaused) + kTimeReg 注册 | 0.3h | ~0.2h | ✅ |
| T5 smoke §13 (API + round-trip + 幂等 + 多次切换) | 0.5h | ~0.3h | ✅ |
| T6 demo G 键 + HUD 状态行 + 4 件 6A + H.0 TODO 更新 | 0.5h | ~0.5h | ✅ |
| T7 提交 + CI 6/6 验证 | 0.5h | 待 | ⏳ |

**总计**: ~1.6h (估时 2-3h, 节约 ~30%)

---

## 2. 文件改动清单

### 新建 (3 文档)
| 文件 | LOC |
|------|-----|
| `docs/Phase H.0.3 Pause-Resume State Machine/CONSENSUS_PhaseH_0_3.md` | ~190 |
| `docs/Phase H.0.3 Pause-Resume State Machine/ACCEPTANCE_PhaseH_0_3.md` | 本文 |
| `docs/Phase H.0.3 Pause-Resume State Machine/FINAL_PhaseH_0_3.md` | ~85 |
| `docs/Phase H.0.3 Pause-Resume State Machine/TODO_PhaseH_0_3.md` | ~55 |

### 修改 (8 文件)
| 文件 | 改动 |
|------|------|
| `ChocoLight/include/light_time.h` | +18 行 (Pause/Resume/IsPaused 声明 + 注释) |
| `ChocoLight/src/light_time.cpp` | +50 行 (State 字段 + BeginFrame 守卫 + 3 fn 实现 + 3 Lua wrapper + kTimeReg) |
| `ChocoLight/include/platform_window.h` | +3 行 (Event::AppEnterBackground/Foreground enum) |
| `ChocoLight/src/platform_window_sdl3.cpp` | +9 行 (SDL→Event 2 case) |
| `ChocoLight/src/light_ui.cpp` | +50 行 (2 个 DispatchOnApp* + DispatchEvents 2 case + LT::TickRender 调用) |
| `scripts/smoke/tick_render.lua` | +50 行 (§13 + summary 行) |
| `samples/demo_tick_render/main.lua` | +15 行 (G 键 + HUD 行加 PAUSED + R 加 Resume) |
| `docs/Phase H.0/TODO_PhaseH_0.md` | -8 +8 行 (§5.2 标记完成) |

**总计**: ~205 LOC 净增

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| `Light.Time.Pause/Resume/IsPaused` 函数存在 | smoke §13 | ✅ |
| Pause → IsPaused=true; Resume → IsPaused=false | smoke §13 round-trip | ✅ |
| Pause/Resume 幂等 (重复调用不抛, 状态不变) | smoke §13 | ✅ |
| 4 次切换状态机正确 | smoke §13 | ✅ |
| 默认 paused=false (零回归) | smoke §13 | ✅ |
| `Window:OnAppEnterBackground/Foreground` 可绑 | DispatchOn* 已实现, demo 可绑 | ✅ 代码可读 |
| BeginFrame paused 路径仅刷 lastTime, 不累积 | 代码可读 + skipNextDt 联动 | ✅ |
| SDL3 BG/FG 事件自动驱动 Pause/Resume | DispatchEvents 已加 hook | ✅ 代码可读 |
| 6/6 平台 CI PASS | GH Actions | ⏳ T7 |
| 实机 iOS/Android 切后台验证 | 留实机 | ⏳ 留 dev |

### 3.1 零回归

| 项 | 保证 |
|----|------|
| 默认 paused=false → BeginFrame 走原逻辑 | smoke §13 |
| 32+ 老 sample (不调 Pause/Resume) | 完全等价老行为 |
| PlatformWindow Event 老 enum 值不变 | AppEnterBackground/Foreground 是 25/26 新追加 |
| PollEvent 未识别老事件仍走 default → Event::None | switch fallthrough 不变 |
| H.0/H.0.1/H.0.2 API 完整保留 | smoke §1~§12 |

---

## 4. 设计权衡回顾

| 决策 | 选择 | 验证 |
|------|------|------|
| Lua API 是否暴露 Pause/Resume? | **是** (3 fn) | smoke 单元测试需要 + 用户手动控制 |
| BeginFrame paused 路径 | **仅刷 lastTime, 累积器冻结** | 防 Resume 后 dt 长跳 |
| Resume 后第一帧 dt | **skipNextDt=true → dt=0** | 防 background 期间 wall-clock 长跳 |
| pauseTime 记录 | **暂不** (无消费方) | 留 TODO |
| Lua callback 形参 | **无参** (callback 自查 Light.Time.IsPaused) | 接口简单 |
| 事件分发顺序 | **TickRender state 先, Lua callback 后** | 用户在 callback 内可查到正确状态 |

---

## 5. 风险评估

| 风险 | 等级 | 缓解 |
|------|------|------|
| Pause 后第一次 Resume dt 长跳 | 低 | skipNextDt + g_lastTime 在 paused 期间每帧刷新 双保险 |
| 多次 Pause/Resume 嵌套 | 低 | 状态机用 if (paused) return 守卫, 不计数 |
| iOS Background 时仍调主循环 | 低 | Pause 后 BeginFrame 仅刷 lastTime, accumulator 不增量 |
| Web 浏览器后台 callback 暂停 | 低 | H.0.2 已覆盖; 切回前台靠 visibilitychange/focus 事件 (SDL3 自映射) |

---

## 6. 已知限制

- **实机未测**: iOS/Android 模拟器实测留 dev 手动跑 (CI 仅 build).
- **pauseTime 未记录**: 若后续需 "暂停时长 metrics" 功能再加.
- **Web visibilitychange**: SDL3 是否自动映射到 SDL_EVENT_DID_ENTER_BACKGROUND? 留实机验证, 不确定时用户可手动 Light.Time.Pause/Resume 兜底.

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T6 完成 (T7 CI 待) |
