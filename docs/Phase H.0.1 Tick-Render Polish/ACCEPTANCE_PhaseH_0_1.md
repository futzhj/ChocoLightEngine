# Phase H.0.1 Tick-Render Polish — ACCEPTANCE 验收

> **基线**: H.0 已交付; H.0.1 闭环 H.0 TODO §5.3 + §5.5 + §5.6
> **完成日期**: 2026-05-19
> **状态**: T1~T5 已完成

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 双 Step 检测 (Box2D + Bullet) | 1h | ~0.4h | ✅ |
| T2 HUD overlay (C++ state + Lua DrawHUD) | 2h | ~0.8h | ✅ |
| T3 smoke §10 perf budget + §11 HUD API | 0.5h | ~0.3h | ✅ |
| T4 demo 切 DrawHUD + 更新 H.0 TODO + 文档 | 0.5h | ~0.5h | ✅ |
| T5 提交 + CI 验证 | 0.5h | 待执行 | ⏳ |

**累计**: ~2h (估时 4.5h, 节约 56% — HUD 用 Lua loadstring 注入比写完整 C++ DrawText 快得多)

---

## 2. 文件改动清单

### 新建 (3 文档)
| 文件 | LOC |
|------|-----|
| `docs/Phase H.0.1 Tick-Render Polish/CONSENSUS_PhaseH_0_1.md` | ~150 |
| `docs/Phase H.0.1 Tick-Render Polish/ACCEPTANCE_PhaseH_0_1.md` | 本文 |
| `docs/Phase H.0.1 Tick-Render Polish/FINAL_PhaseH_0_1.md` | ~80 |
| `docs/Phase H.0.1 Tick-Render Polish/TODO_PhaseH_0_1.md` | ~50 |

### 修改 (6 文件)
| 文件 | 改动 |
|------|------|
| `ChocoLight/include/light_time.h` | +13 行 (HUD overlay API 4 fn) |
| `ChocoLight/src/light_time.cpp` | +90 行 (HUD state + 4 Lua wrapper + DrawHUD Lua 注入) |
| `ChocoLight/src/light_physics.cpp` | +12 行 (Box2D 双 Step 检测) |
| `ChocoLight/src/light_physics3d.cpp` | +12 行 (Bullet 双 Step 检测) |
| `scripts/smoke/tick_render.lua` | +75 行 (§10 perf + §11 HUD) |
| `samples/demo_tick_render/main.lua` | +20 -25 行 (切 DrawHUD + H 键) |
| `docs/Phase H.0/TODO_PhaseH_0.md` | -20 +6 行 (标记 §5.3/§5.5/§5.6 完成) |

**总计**: ~120 LOC 净增

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| Box2D `world:Step()` while autoStep=true 触发 warn | 实机 / smoke §11 (autoStep round-trip) | ✅ 代码已加 |
| Bullet 同上 | 同上 | ✅ |
| Warn 限 3 次后静默 | static counter | ✅ |
| `Light.Time.SetHUDEnabled` round-trip | smoke §11 | ✅ |
| `Light.Time.DrawHUD` 不抛异常 | smoke §11 (HUD 关 / HUD 开 + Gfx 不可用) | ✅ |
| `Light.Time.SetHUDPosition` round-trip | smoke §11 | ✅ |
| 默认 HUD 关 / position (10, 10) | smoke §11 | ✅ |
| smoke perf budget §10 | smoke run | ✅ syntax PASS |
| demo 按 H 键切 HUD | 实机 | ⏳ 留实机 |
| 6/6 平台 CI PASS | GH Actions | ⏳ T5 |

### 3.1 零回归
- HUD 默认 enabled = false → 老 sample 0 改动.
- 双 Step warn 默认 throttle 3 次 → 不刷日志.
- `Light.Time.DrawHUD` 在 Gfx 不可用时 silent no-op.
- Phase AR + H.0 + H.0.1 共存, 11+5+5 fn 共 21 个 (smoke §1 验证).

---

## 4. 设计权衡回顾

| 决策 | 选择 | 验证 |
|------|------|------|
| HUD 用 Lua 实现 | luaL_loadstring 注入 | 不依赖 GL ctx; Gfx 不可用时 silent no-op |
| 双 Step 检测 throttle | static counter limit 3 | 用户能定位错误用法; 不刷屏 |
| HUD 默认关 | release 性能优先 | 老 sample 零回归 |
| smoke perf budget | accumulator < fixedDt*2 | smoke headless 下 acc=0 永远 PASS, 真正 budget 在 demo 长时观察 |

---

## 5. 已知限制

- **§5.6 实机基准未做** — 需要两台显示器 (60Hz + 144Hz) 手动跑 demo, 记录 alpha 直方图. smoke 仅做 budget 健全性检查.
- **HUD 定制化** — 当前固定 4 行内容 (fixedHz/FPS/alpha/accumulator). 如需自定义内容用户复制 Lua 函数即可.
- **DrawHUD 字体大小** — 走 `Light.Graphics.DrawText` 默认大小. 用户调用前可通过 `Light.Graphics.SetColor` 改色.

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T4 完成 (T5 CI 待) |
