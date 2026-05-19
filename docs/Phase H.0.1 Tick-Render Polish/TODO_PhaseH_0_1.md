# Phase H.0.1 Tick-Render Polish — TODO

> **完成日期**: 2026-05-19
> **范围**: H.0.1 已完成的 3 项无待办; 列出后续可做项

---

## 1. 用户配置

无新增配置. 默认行为:
- 双 Step 检测自动开 (throttle 3 次后停)
- HUD 默认关 (`Light.Time.SetHUDEnabled(true)` 启用)
- HUD 默认位置 (10, 10) — `Light.Time.SetHUDPosition(x, y)` 自定义

---

## 2. 推荐验证

### 2.1 双 Step 检测
```lua
local Physics = require "Light.Physics"
local world = ...    -- 创建 World
world:SetAutoStep(true)
world:Step(1/60)     -- 应 log: "World:Step called while autoStep=true ... (warn 1/3)"
world:Step(1/60)     -- warn 2/3
world:Step(1/60)     -- warn 3/3
world:Step(1/60)     -- 静默 (throttle 已满)
```

### 2.2 HUD overlay
```lua
Light.Time.SetHUDEnabled(true)
Light.Time.SetHUDPosition(20, 20)
-- demo:Draw / OnRender 内调
Light.Time.DrawHUD()    -- 4 行: Phase 标题 / fixedHz / FPS+frameTime / alpha+accumulator
```

### 2.3 smoke run
```powershell
.\build\Release\Light.exe scripts\smoke\tick_render.lua
# 期待: §1~§11 全 PASS
```

### 2.4 demo
```powershell
.\build\Release\Light.exe samples\demo_tick_render\main.lua
# H 键切 HUD; 1/2/3/4 切 fixedHz; A 切 alpha lerp; P 切 Box2D auto-step
```

---

## 3. 已知限制

### 3.1 双 Step warn throttle 全局
**当前**: static counter 是函数级, 全部 World 共享 3 次配额.
**影响**: 多 World 场景下可能某个 World 的错误用法触发 1 次, 其他 World 错用全静默.
**升级**: 改 World 级 counter (~10 行改动, low priority).

### 3.2 HUD 内容固定
**当前**: 4 行: Phase 标题 / fixedHz / FPS+frameTime / alpha+accumulator.
**影响**: 用户想加自定义字段需复制 Lua 函数自写.
**升级**: 提供 `Light.Time.SetHUDFormatFunc(fn)` 钩子让用户自定义 (~15 行, low priority).

### 3.3 smoke perf budget 在 headless 永远 PASS
**当前**: smoke 不真跑主循环, accumulator 永远 = 0.
**影响**: §10 是 sanity check 不是 real budget.
**升级**: demo 长时跑 + JSON HUD 输出 + CI 解析 (~2h, 见 §6.1).

---

## 4. 增强候选 (按 ROI)

### 4.1 实机性能直方图 ⭐⭐
**估时**: 2-3h
**收益**: 真实性能数据替代估算; 跨平台对比.
**实现**:
- `Light.Time.GetFrameTimeHistogram()` 返回 100 桶直方图 (1ms~30ms).
- demo 长时跑 60s, 输出 JSON.
- CI 设阈值 (e.g. p99 < 20ms) 防回归.

### 4.2 World 级 warn counter ⭐
**估时**: 0.5h
**收益**: 多 World 场景下 warn 不被吃掉.
**实现**: `PhysicsRegistry::Entry` 加 `int stepWarnCount` 字段; 函数内 cast 到 entry 查/改.

### 4.3 HUD 自定义钩子 ⭐
**估时**: 0.5h
**收益**: 用户加自定义字段不需复制函数.
**实现**:
```lua
Light.Time.SetHUDFormatFunc(function(state)
    return {
        "Phase H.0 HUD",
        string.format("fixedHz=%d", state.fixedHz),
        string.format("Custom: %s", myAppData),
    }
end)
```

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

注: H.0.1 是 4A 轻量, 无 ALIGNMENT/DESIGN/TASK 单独文件 (合并入 CONSENSUS).

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
