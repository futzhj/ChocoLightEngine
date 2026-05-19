# Phase H.0.4 Tick-Render Lerp Helpers — ACCEPTANCE 验收

> **基线**: H.0 + H.0.1 + H.0.2 + H.0.3 已交付
> **范围**: H.0 TODO §5.4 落地 — CPU 端 alpha 插值 helper
> **完成日期**: 2026-05-19
> **状态**: T1~T5 完成, T6 (CI) 待

---

## 1. 任务完成情况

| 任务 | 估时 | 实际 | 状态 |
|------|------|------|------|
| T1 4 个 C++ Lua wrapper (Lerp/LerpVec2/LerpVec3/LerpAngle) | 0.3h | ~0.2h | ✅ |
| T2 kTimeReg 注册 | 0.05h | ~0.05h | ✅ |
| T3 smoke §14 (~80 LOC, 11 PASS) | 0.4h | ~0.3h | ✅ |
| T4 demo 重构 (LerpVec2 替换手写 + 注释更新) | 0.3h | ~0.2h | ✅ |
| T5 4 件 6A + H.0 TODO §5.4 标完成 | 0.3h | ~0.3h | ✅ |
| T6 CI 6/6 PASS | 0.5h | 待 | ⏳ |

**总计**: ~1.05h (估时 1-2h, 实际 -50%)

---

## 2. 文件改动清单

### 新建 (4 文档)
| 文件 | LOC |
|------|-----|
| `docs/Phase H.0.4 Tick-Render Lerp Helpers/CONSENSUS_PhaseH_0_4.md` | ~165 |
| `docs/Phase H.0.4 Tick-Render Lerp Helpers/ACCEPTANCE_PhaseH_0_4.md` | 本文 |
| `docs/Phase H.0.4 Tick-Render Lerp Helpers/FINAL_PhaseH_0_4.md` | ~95 |
| `docs/Phase H.0.4 Tick-Render Lerp Helpers/TODO_PhaseH_0_4.md` | ~70 |

### 修改 (4 文件)
| 文件 | 改动 |
|------|------|
| `ChocoLight/src/light_time.cpp` | +70 行 (4 wrapper + LerpT_ helper + kTimeReg 4 项) |
| `scripts/smoke/tick_render.lua` | +85 行 (§14 + summary 更新到 H.0.4) |
| `samples/demo_tick_render/main.lua` | -3 +5 行 (LerpVec2 替换手写 + 头部注释) |
| `docs/Phase H.0/TODO_PhaseH_0.md` | -7 +7 行 (§5.4 标完成) |

**总计**: ~155 LOC 净增

---

## 3. 验收标准核对

| 项 | 验证 | 结果 |
|----|------|------|
| `Light.Time.Lerp/LerpVec2/LerpVec3/LerpAngle` 4 fn 存在 | smoke §14 | ✅ |
| Lerp(0, 10, 0.3) == 3.0 | smoke §14 | ✅ |
| 边界 t=0 → a, t=1 → b | smoke §14 | ✅ |
| Extrapolation 允许 (t < 0, t > 1) | smoke §14 | ✅ |
| LerpVec2 返回 2 值 (rx, ry) | smoke §14 | ✅ |
| LerpVec3 返回 3 值 (rx, ry, rz) | smoke §14 | ✅ |
| LerpAngle 短路径 (-π+0.1 → π-0.1 经 -π) | smoke §14 | ✅ |
| t 缺省自动用 GetAlpha() | smoke §14 (Lerp / LerpVec2 两种) | ✅ |
| demo 重构后视觉行为不变 | syntax check + 视觉留 dev | ✅ syntax |
| 6/6 平台 CI PASS | GH Actions | ⏳ T6 |

### 3.1 零回归

| 项 | 保证 |
|----|------|
| 加性 API (不动主循环 / 老 fn) | ✅ |
| 32+ 老 sample 不调新 fn | ✅ 完全等价老行为 |
| H.0/H.0.1/H.0.2/H.0.3 API 完整保留 | ✅ smoke §1~§13 |

---

## 4. 设计权衡回顾

| 决策 | 选择 | 验证 |
|------|------|------|
| 4x4 矩阵 lerp? | **不做** | 矩阵 lerp 旋转不正确; 留 H.1 Transform component |
| Component 自动维护 prev? | **不做** | 当前无 Transform component 系统; H.1+ 范畴 |
| API 范围 | **4 fn (scalar/vec2/vec3/angle)** | 覆盖 90% 用户场景 |
| t 可选 | **是, 缺省自动 GetAlpha()** | 90% 用户用引擎 alpha; 5% 用户用自定义 t |
| LerpAngle 算法 | **最短路径** (`while diff > π`) | 防 lerp(-π+0.1, π-0.1) 走 6.08 长弧 |
| t clamp? | **不 clamp** | 允许 extrapolation 是合法用法 (过冲动画 / springs) |
| 实现位置 | **C++ Lua wrapper** | 1 行数学; 避免 Lua 调用 GetAlpha() 开销 |

---

## 5. 风险评估

| 风险 | 等级 | 缓解 |
|------|------|------|
| t 超出 [0, 1] 用户传错 | 低 | 文档明确允许 extrapolation; 不 clamp 是有意设计 |
| 用户传 degree 给 LerpAngle | 低 | 文档 + 命名 (Angle 而非 Degree) 明确 radians |
| 浮点远点精度 (b - a 溢出) | 极低 | 仅极端值才会, 不在 demo 场景 |
| 引入新 dependency | 无 | 仅依赖 luaL_checknumber + LT::TickRender::GetAlpha (已有) |

---

## 6. 已知限制

- **不暴露 Light.Math 命名空间**: 4 fn 都在 Light.Time 表下 (符合 GetAlpha 同表). 未来若有更大 math 库, 可重导出.
- **Vec4 / 4x4 矩阵未做**: 待 H.1 Transform component + 真正 quat lib 引入后再考虑.
- **Quaternion slerp 未做**: 当前 demo 用不到; 留 TODO.
- **CPU 端实现**: shader 端 alpha 插值仍需 fragment 自行 mix(prev, curr, alpha), 这是 H.1+ 的 GPU 全自动方案.

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | T1~T5 完成 (T6 CI 待) |
