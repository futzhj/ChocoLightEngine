# demo_tick_render — Phase H.0 Tick-Render 解耦演示

> **基线**: ChocoLight Engine Phase H.0 (Tick-Render decoupling)
> **目标**: 直观演示 fixed-step 逻辑 + variable-rate 渲染 + alpha 插值 + 物理 auto-step.

---

## 用途

固定 60Hz 逻辑帧 + VSync 渲染帧分离 (典型场景: 60Hz 物理 + 144Hz 显示器). 演示:

1. **alpha 插值的视觉差异** — 左右双方块在同一 fixed update 节奏下渲染:
   - 左方块: 直接渲染 `cube_curr.x` (无插值, 144Hz render 时看见 60Hz 跳动)
   - 右方块: `lerp(prev, curr, alpha)` (平滑, 144Hz render 仍流畅)
2. **Box2D auto-step** — 启用 `world:SetAutoStep(true)` 后, 引擎在每个 fixed step 自动调 `World:Step(fixedDt)`. Lua 不必再手动 Step.
3. **HUD 实时显示** — fixedHz / FPS / alpha / lastStepCount / accumulator.

---

## 按键

| 键 | 作用 |
|---|---|
| **1** | 切 fixedHz = 30  (低逻辑频率, 看 144Hz 显示器下 alpha 插值效果最明显) |
| **2** | 切 fixedHz = 60  (默认) |
| **3** | 切 fixedHz = 120 |
| **4** | 切 fixedHz = 144 (fixedHz == VSync 时 alpha 永远 0) |
| **A** | 切 alpha 插值开关 (off 时右方块也"卡顿") |
| **P** | 切 Box2D auto-step (off 时小球停; 重新 on 引擎继续 Step) |
| **R** | 复位 (fixedHz=60, alpha=ON, auto-step=ON) |
| **ESC** | 退出 |

---

## HUD 字段说明

```
Phase H.0 Tick-Render Decouple
fixedHz=60  fixedDt=0.0167s  maxStep=8
FPS=144  frameTime=6.94ms  steps/frame=0
alpha=0.42  accumulator=0.0070s
alpha lerp: ON   Box2D auto-step: ON
```

| 字段 | 含义 |
|------|------|
| `fixedHz` | Lua 逻辑频率 (Hz); `Light.Time.GetFixedTimestep()` |
| `fixedDt` | 单步逻辑时长 (s) = 1/fixedHz |
| `maxStep` | spiral guard 单帧最大 fixed step 数 (默认 8) |
| `FPS` | EMA 平均渲染帧率 |
| `frameTime` | 上一帧 wall-clock 时长 (已 clamp) |
| `steps/frame` | 上一帧实际 fixed step 数; **关键观察**: 144Hz 显示器 + fixedHz=60 → 平均 0.42, 个别帧 0/1 交替 |
| `alpha` | ∈ [0, 1), 距上次 fixed update 进度; 用于 `OnRender(alpha, dt)` 内的状态 lerp |
| `accumulator` | 累积器值 (s); 内部状态, 一般 < fixedDt |

---

## 代码要点

```lua
-- 1) 决定性逻辑写 OnFixedUpdate (dt = fixedDt, 永远固定 16.67ms @ 60Hz)
function Demo:OnFixedUpdate(dt)
    cube_prev.x = cube_curr.x         -- 保存上一帧位置 (lerp 用)
    cube_curr.x = cube_curr.x + 250 * dt
end

-- 2) 渲染时插值 (alpha = Light.Time.GetAlpha() 或参数)
function Demo:OnRender(alpha, dt)
    local x = cube_prev.x + (cube_curr.x - cube_prev.x) * alpha
    -- 用 x 绘制
end

-- 3) Box2D auto-step 启用
world:SetAutoStep(true)
-- 不再需要 world:Step(dt); 引擎自动调度
```

---

## 与老 sample 兼容性

`Window:Update(dt)` 和 `Window:Draw()` 仍正常工作 — 32 个老 sample (demo_taau / demo_ssr / 等) **零修改**, 全部继续运行.

`Window:OnFixedUpdate` / `Window:OnRender` 是**加性 API**: 不定义则不调用, 不影响旧路径.

---

## Phase 关系

- 基于 [Phase H.0 6A 7 件套](../../docs/Phase%20H.0%20Tick-Render%20Decouple/)
- 与 [Phase F.1.5 GPU Timer](../../docs/Phase%20F.1.5%20GPU%20Timer%20for%20DRS/) 正交: F.1.5 解决 DRS 决策精度, H.0 解决 Lua 逻辑频率漂移
- 与 [Phase G.0 Lua Hot Reload](../../docs/Phase%20G.0%20Lua%20Hot%20Reload/) 兼容: OnFixedUpdate / OnRender 走 Preserve 路径自动恢复
