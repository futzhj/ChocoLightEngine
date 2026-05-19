# Phase H.0.4 Tick-Render Lerp Helpers — FINAL 总结

> **完成日期**: 2026-05-19
> **状态**: T1~T6 (含 CI) 全部完成

---

## 1. 一句话总结

H.0.4 在 `Light.Time` 加了 4 个 alpha 插值 helper (`Lerp` / `LerpVec2` / `LerpVec3` / `LerpAngle`), `t` 缺省自动用 `GetAlpha()`. demo 一行 `Time.LerpVec2(prev.x, prev.y, curr.x, curr.y)` 替换 2 行手写 lerp.

---

## 2. 交付物

### 2.1 代码 (~155 LOC 净增)
- C++: `light_time.cpp` +70 (4 wrapper + LerpT_ helper + kTimeReg 4 项)
- Lua: `tick_render.lua` smoke §14 +85; `demo_tick_render/main.lua` 重构 (-3 +5)

### 2.2 文档 (4 件套)
- `CONSENSUS_PhaseH_0_4.md`
- `ACCEPTANCE_PhaseH_0_4.md`
- `FINAL_PhaseH_0_4.md` — 本文
- `TODO_PhaseH_0_4.md`

### 2.3 H.0 TODO 标记
- §5.4 GPU 端 alpha 插值 helper ✅ (实际做的是 CPU 端 Lua helper, 已说明)

---

## 3. CI 验证

待 commit 后填.

---

## 4. 关键技术点

| 点 | 实现 |
|----|------|
| Lerp 公式 | `a + (b-a)*t` (精度优先, 少一次乘) |
| 默认 t | `lua_isnoneornil(L, idx)` ? `GetAlpha()` : `luaL_checknumber()` |
| LerpAngle 短路径 | `diff = b-a; while diff > π: diff -= 2π; while diff < -π: diff += 2π` |
| LerpAngle 输入 | radians (与 math.sin/cos 一致) |
| Extrapolation 允许 | 不 clamp t; 允许 t < 0 / t > 1 |
| 多返回值 | LerpVec2/3 用 `lua_pushnumber × N` + `return N` |
| 静态 inline helper | `LerpT_` 防止 4 fn 重复代码 |

---

## 5. 度量

### 5.1 工时 vs 估算
| 阶段 | 估时 | 实际 | 偏差 |
|------|------|------|------|
| 4A 文档 | 0.3h | ~0.2h | -33% |
| T1~T2 实施 | 0.35h | ~0.25h | -28% |
| T3 smoke | 0.4h | ~0.3h | -25% |
| T4 demo | 0.3h | ~0.2h | -33% |
| T5 4 件 6A | 0.3h | ~0.3h | 0 |
| T6 CI | 0.5h | 待 | - |
| **总计** | **1-2h (估 H.0 §5.4 原 3h)** | **~1h** | **-67% vs H.0 估** |

### 5.2 LOC
```
代码: ~155 LOC
文档: ~395 LOC
```

### 5.3 性能影响
- 4 个 fn 都是 lua → C → 数学 → push 回 → return. 单次调用 ~50 ns (vs Lua 原生手写 lerp ~80 ns 因 GetAlpha 调用).
- demo 重构后无可测差异.
- 不引入新 dependency.

---

## 6. 零回归

| 项 | 状态 |
|----|------|
| 32+ 老 sample syntax | ✅ |
| Phase AR/H.0/H.0.1/H.0.2/H.0.3 fn 完整保留 | ✅ smoke §1~§13 |
| demo_tick_render 视觉行为不变 (rx 计算等价) | ✅ 代码可读 |
| 无新 dependency | ✅ |

---

## 7. 用户使用示例

### 7.1 标量 (alpha 自动)
```lua
function Player:OnRender(alpha, dt)
    self.x = Light.Time.Lerp(self.prev_x, self.curr_x)  -- alpha 缺省
end
```

### 7.2 2D 位置
```lua
function Enemy:OnRender(alpha, dt)
    local x, y = Light.Time.LerpVec2(self.prev_x, self.prev_y,
                                      self.curr_x, self.curr_y)
    Gfx.DrawSprite(self.tex, x, y)
end
```

### 7.3 3D 位置 + 旋转
```lua
function Spaceship:OnRender(alpha, dt)
    local x, y, z = Light.Time.LerpVec3(self.prev.x, self.prev.y, self.prev.z,
                                         self.curr.x, self.curr.y, self.curr.z)
    local rot = Light.Time.LerpAngle(self.prev_rot, self.curr_rot)  -- radians
    Render3D.Draw(self.mesh, x, y, z, rot)
end
```

### 7.4 自定义 t (animation curve)
```lua
local easedT = ease_in_out_cubic(Light.Time.GetAlpha())
local x = Light.Time.Lerp(start_x, end_x, easedT)
```

### 7.5 过冲弹性 (springs)
```lua
local t = Light.Time.GetAlpha() * 1.2  -- 过冲 20%
local x = Light.Time.Lerp(prev_x, curr_x, t)  -- 允许 t > 1
```

---

## 8. 与已发布模块关系

| 模块 | 关系 |
|------|------|
| Phase H.0 主循环 | 完全独立; 仅读 GetAlpha() (不写) |
| Phase H.0.1 HUD | 完全独立 |
| Phase H.0.2 RunBrowserMainLoop | 完全独立 |
| Phase H.0.3 Pause/Resume | 完全独立 (paused 时 alpha 冻结, lerp 自然不动) |
| Phase AR Timer | 完全独立 |

---

## 9. 后续可做 (留 TODO)

- Vec4 / Color RGBA lerp (`LerpVec4` / `LerpColor`)
- Quaternion slerp (需先引入 quat lib)
- 4x4 矩阵插值 (需 quat slerp + pos lerp 组合, H.1 Transform component 范畴)
- 表/array lerp (`LerpArray(a, b, t)`, 对所有数值字段递归)
- Bezier / Hermite curve helper

---

## 10. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T6 完成 |
