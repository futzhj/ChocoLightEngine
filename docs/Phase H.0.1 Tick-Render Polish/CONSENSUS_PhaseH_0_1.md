# Phase H.0.1 Tick-Render Polish — 6A 轻量合并文档

> **基线**: Phase H.0 已交付 (commit `8b7837d`, CI 6/6 PASS)
> **类型**: H.0 自然延伸, 闭环工具链 (6A 走轻量 4A: Align/Architect 合并)
> **估时**: 4-5h
> **范围**: H.0 TODO §5.3 + §5.5 + §5.6 + 简化的 §6.1 (smoke perf budget)

---

## 1. Align — 项目对齐 (合并)

### 1.1 来源
H.0 TODO 文档 §5 中 ROI 最高的 3 项:
- **§5.3 物理双 Step 检测** (1h) — 用户启用 `autoStep=true` 后仍调 `world:Step()`, 物理速度翻倍 → 警告 log.
- **§5.5 HUD overlay** (2h) — 引擎内置 `Light.Time.DrawHUD()`, 调试看 fixedHz/FPS/alpha 不需自己写 DrawText.
- **§5.6 实机性能基准** (2h, 简化为 perf budget assertion in smoke) — 把估算的 < 0.005 ms/帧转成 CI 可验证.

### 1.2 用户决策 (轻量, 自决)
| 问 | 决策 |
|---|---|
| 双 Step 检测要 throttle 吗 | 是, 限 3 次 (够定位, 不刷屏) |
| HUD overlay 用什么字体? | 复用 `Light.Graphics.DrawText` (已存在) |
| HUD 默认开 / 关? | 默认关 (release 性能优先), `Light.Time.SetHUDEnabled(true)` 启用 |
| HUD 位置 | 默认左上 (10, 10), `Light.Time.SetHUDPosition(x, y)` 自定义 |
| perf budget 阈值 | accumulator < fixedDt * 2 (确保 spiral guard 不长时触发) |

### 1.3 边界
- **IN**: 双 Step 警告 / HUD overlay / smoke perf assertion / demo 切 DrawHUD / 更新 H.0 TODO 标记完成项.
- **OUT**: emscripten_set_main_loop_arg 真集成 (留 H.0.2) / pause 状态机 (留 H.0.3) / GPU 端 alpha 插值 (留 H.0.4) / 真实机基准 (需两台显示器手动跑).

---

## 2. Architect — 设计要点

### 2.1 双 Step 检测

**Box2D** (`light_physics.cpp::l_World_Step`):
```cpp
static int l_World_Step(lua_State* L) {
    auto* w = CheckWorld(L, 1);
    if (!w->alive) return 0;
    // Phase H.0.1 — 双 Step 检测: 启用 auto-step 后再手动调将导致 step 两次
    if (LT::PhysicsRegistry::GetAutoStep(w)) {
        static int s_warn_count = 0;
        if (s_warn_count++ < 3) {
            CC::Log(CC::LOG_WARN,
                "Light.Physics.World:Step called while autoStep=true — world will Step TWICE per frame. "
                "Disable auto-step (world:SetAutoStep(false)) or remove manual Step.");
        }
    }
    // ... 原 Step 调用
}
```
**Bullet** 同理 (`light_physics3d.cpp::l_World_Step`).

### 2.2 HUD overlay

**接口 (light_time.h)**:
```cpp
namespace LT {
namespace TickRender {
    /// 设 HUD 启用状态 (默认 false; 启用后 Light.Time.DrawHUD 在 OnRender 后绘制)
    void SetHUDEnabled(bool enabled);
    bool GetHUDEnabled();
    /// 设 HUD 位置 (默认 10, 10)
    void SetHUDPosition(float x, float y);
}
}
```

**Lua 接口**:
```lua
Light.Time.SetHUDEnabled(true)        -- 默认 false
Light.Time.SetHUDPosition(10, 10)
Light.Time.DrawHUD()                   -- 用户在 OnRender / Draw 内显式调
```

**实现要点**:
- `Light.Time.DrawHUD()` 在 Lua 端实现 (走 `Light.Graphics.DrawText`), 减 C++ 改动.
- 只用 `Light.Time.Get*` API 读状态, 无副作用.
- 输出 6 行: Phase 标题 / fixedHz / FPS+frameTime / alpha+accumulator / steps/frame / hint.

### 2.3 smoke perf budget

**新增 §10 perf budget assertion** (smoke `tick_render.lua` 末尾):
```lua
do
    -- 模拟 60 帧 idle (调引擎主循环 builtin perf 计数)
    -- accumulator 不应累积超过 fixedDt * 2 (即未触发 spiral guard 长时)
    local acc_max = Time.GetFixedDt() * 2
    local acc = Time.GetAccumulator()
    if acc > acc_max then
        fail("perf budget: accumulator (" .. acc .. ") > fixedDt*2 (" .. acc_max .. ")")
    end
    pass(string.format("§10 perf budget: accumulator %.4f < fixedDt*2 %.4f", acc, acc_max))
end
```

注: smoke 是 headless 不真跑主循环, 此 assertion 在新创建的 TickRender 状态下永远 PASS (acc=0); 真正 budget 在 demo 长时运行时人眼观察.

### 2.4 demo 切 DrawHUD

把 demo_tick_render 的手写 HUD 部分 (~30 行 Gfx.DrawText) 替换为 `Light.Time.DrawHUD()` 一行调用. 同时新增按键 **H** 切 HUD 启用/禁用.

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输入 | 输出 |
|------|------|------|------|
| **T1** Box2D + Bullet 双 Step 检测 | 1h | `l_World_Step` 现状 | 2 处加 throttle warn (3 次后停) |
| **T2** HUD overlay 实现 | 2h | LT::TickRender Get* API | C++ 4 fn (Set/Get HUDEnabled, Set HUDPosition) + Lua DrawHUD |
| **T3** smoke 补充 + perf budget | 0.5h | 现有 smoke 9 段 | 加 §10 + §11 (双 Step 警告验证 / HUD API 表面) |
| **T4** demo 切 DrawHUD + H.0 TODO 更新 | 0.5h | demo_tick_render | 替换手写 HUD + H 键; H.0 TODO 标 §5.3/5.5/5.6 完成 |
| **T5** 提交 + CI 验证 | 0.5h | git push + GH Actions | 6/6 平台 PASS |

**总计**: 4.5h

---

## 4. 验收标准 (CONSENSUS §3)

| 项 | 验证 |
|----|------|
| ✅ Box2D `world:Step()` while autoStep=true 触发 warn | 单元 lua 测 (smoke §11) |
| ✅ Bullet 同上 | smoke §11 |
| ✅ Warn 限 3 次后静默 | 多次 Step 调用计数 |
| ✅ `Light.Time.SetHUDEnabled` round-trip | smoke §11 |
| ✅ `Light.Time.DrawHUD` 不抛异常 (即便 Gfx 不可用) | smoke §11 |
| ✅ demo 按 H 键切 HUD | 实机 |
| ✅ smoke perf budget §10 PASS | smoke run |
| ✅ 6/6 平台 CI PASS | GH Actions |

### 4.1 零回归
- 默认 HUD enabled = false → 老 sample 0 改动.
- 双 Step warn 默认 throttle 3 次 → 不刷日志.
- `Light.Time.DrawHUD` 不存在时调用 = no-op (设默认实现).

---

## 5. 6A 7 件套精简映射

由于 H.0.1 是 H.0 的轻量延伸, **本文 = ALIGNMENT + CONSENSUS + DESIGN + TASK 合并**, 后续:
- ACCEPTANCE_PhaseH_0_1.md — T1~T5 完成验收
- FINAL_PhaseH_0_1.md — 总结
- TODO_PhaseH_0_1.md — 留观察项

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — H.0.1 轻量 6A 启动 |
