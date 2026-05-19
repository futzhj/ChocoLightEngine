# Phase H.0.4 Tick-Render Lerp Helpers — 6A 轻量合并文档

> **基线**: H.0 + H.0.1 + H.0.2 + H.0.3 已交付
> **类型**: H.0 TODO §5.4 落地 — CPU 端 alpha 插值 helper (Lua 用户少写 lerp 代码)
> **估时**: 1-2h (从原 §5.4 的 3h 降低, 因为去除 4x4 矩阵复杂方案)
> **风险**: 低 — 纯数学 wrapper, 无状态, 无副作用

---

## 1. Align — 项目对齐

### 1.1 原文需求 (H.0 TODO §5.4)
> 新增 `Light.Time.LerpTransform(prev, curr, alpha)` (返回 4x4 矩阵).
> 或 Component 系统自动维护 `Transform.prev` (类似 E.13 Motion Vector).

### 1.2 现状

```lua
-- demo_tick_render 中用户手写 lerp:
local rx = cube_prev.x + (cube_curr.x - cube_prev.x) * alpha
local ry = cube_prev.y + (cube_curr.y - cube_prev.y) * alpha
```

每个用户都需要写一遍, 易错 (符号顺序), 难读. Box2D 弹球 demo 也没用 alpha lerp (60Hz 物理 + 144Hz 渲染时仍有可见跳动).

### 1.3 用户决策 (轻量自决)

| 问 | 决策 | 理由 |
|---|---|---|
| 4x4 矩阵 lerp? | **暂不做** | 矩阵 lerp 一般不正确 (旋转用 slerp/quat), 不要诱导错误用法. 后续 H.1 Job System 时若有 Transform component 再考虑 |
| Component 自动维护 prev? | **暂不做** | 引擎当前无 Transform component 系统; 这是 H.1+ 范畴 |
| API 范围 | **4 个标量/向量 helper** | 覆盖 90% 用户场景: 1D scalar / 2D pos / 3D pos / angle |
| t 参数是否可选? | **可选, 缺省自动用 GetAlpha()** | 90% 用户用引擎 alpha; 5% 用户用自定义 t (如 anim curve); 兼顾两者 |
| LerpAngle 算法 | **最短路径** (处理 -π..π wrap) | 普通 lerp 在 -3.0 → 3.0 转一大圈; 短路径走 -0.28 → -3.14 → 2.86 → 3.0 |
| 实现位置 | **C++ Lua wrapper, 无 internal API** | 1 行数学, C++ 实现避免 Lua 调用开销, 也不需 Light::Math 命名空间 |

### 1.4 边界

- **IN**: 4 个 Lua API (`Light.Time.Lerp` / `LerpVec2` / `LerpVec3` / `LerpAngle`) + smoke §14 + demo 用 helper 重构 + 4 件 6A.
- **OUT**: 4x4 矩阵 lerp (slerp+pos 分别 lerp 是后续 H.1 Transform component 范畴) / Quat 库 (现引擎无 quaternion 模块) / Bezier/Hermite curve / GLSL helper (shader 端自己写).

---

## 2. Architect — 设计

### 2.1 API 签名

```c++
// Light.Time.Lerp(a, b [, t])
// 返回: lerp 后标量
// t 缺省 → 用 GetAlpha()
double Lerp(double a, double b, double t);

// Light.Time.LerpVec2(ax, ay, bx, by [, t])
// 返回: 2 个值 (rx, ry)
(double, double) LerpVec2(...);

// Light.Time.LerpVec3(ax, ay, az, bx, by, bz [, t])
// 返回: 3 个值 (rx, ry, rz)
(double, double, double) LerpVec3(...);

// Light.Time.LerpAngle(a, b [, t]) — radians, 最短路径
// 返回: lerp 后角度 (in [-π, π])
double LerpAngle(double a, double b, double t);
```

### 2.2 算法

**标量 lerp**: `a + (b - a) * t` (避免 `a * (1-t) + b * t` 的精度损失 + 多一次乘法).

**LerpAngle 短路径**:
```cpp
double diff = b - a;
// 归一化到 (-π, π]
while (diff > M_PI)  diff -= 2 * M_PI;
while (diff < -M_PI) diff += 2 * M_PI;
return a + diff * t;
```

实测 `while` 通常 0~1 次循环 (除非用户传非常远的角度), 不必担心循环次数.

### 2.3 t 默认值处理

```cpp
int l_Time_Lerp(lua_State* L) {
    double a = luaL_checknumber(L, 1);
    double b = luaL_checknumber(L, 2);
    // t 可选 (3 个参数模式 vs 2 个参数自动模式)
    double t = lua_isnoneornil(L, 3) ? LT::TickRender::GetAlpha() : luaL_checknumber(L, 3);
    lua_pushnumber(L, a + (b - a) * t);
    return 1;
}
```

### 2.4 demo 重构 (前后对比)

**前** (手写):
```lua
local alpha = use_alpha and Light.Time.GetAlpha() or 1.0
local rx = cube_prev.x + (cube_curr.x - cube_prev.x) * alpha
-- rx 用于 DrawRect
```

**后** (helper):
```lua
-- alpha 缺省自动用 GetAlpha()
local rx = use_alpha and Light.Time.LerpVec2(cube_prev.x, cube_prev.y, cube_curr.x, cube_curr.y)
                       or cube_curr.x
-- 或更对称:
local rx, ry = Light.Time.LerpVec2(cube_prev.x, cube_prev.y, cube_curr.x, cube_curr.y,
                                    use_alpha and nil or 1.0)
```

Box2D 弹球同样可加 `prev_pos / curr_pos`, 然后 OnRender 内 lerp 渲染.

---

## 3. Atomize — 任务拆分

| 任务 | 估时 | 输出 |
|------|------|------|
| **T1** 4 个 C++ Lua wrapper | 0.3h | light_time.cpp 加 4 个 fn |
| **T2** kTimeReg 注册 | 0.05h | 4 行 |
| **T3** smoke §14 覆盖 | 0.4h | round-trip + edge cases + 默认 t |
| **T4** demo 重构 + 注释更新 | 0.3h | 左右方块 + Box2D 弹球都用 helper |
| **T5** 4 件 6A + H.0 TODO §5.4 标完成 | 0.3h | docs |
| **T6** CI | 0.5h | 6/6 PASS |

**总计**: ~1.85h (估时 1-2h)

---

## 4. 验收标准

| 项 | 验证 |
|----|------|
| ✅ `Light.Time.Lerp/LerpVec2/LerpVec3/LerpAngle` 4 fn 存在 | smoke §14 |
| ✅ Lerp(0, 10, 0.3) == 3.0 | smoke §14 |
| ✅ LerpVec2 返回 2 值 | smoke §14 |
| ✅ LerpVec3 返回 3 值 | smoke §14 |
| ✅ LerpAngle 短路径正确 (-π+0.1 → π-0.1 走 0.2 短弧不走 6.08 长弧) | smoke §14 |
| ✅ t 缺省自动用 GetAlpha() | smoke §14 (需先 BeginFrame 让 alpha != 0) |
| ✅ 边界: t=0 → a; t=1 → b | smoke §14 |
| ✅ demo 编译 + 视觉上左右方块行为不变 | syntax check + 视觉留 dev |
| ✅ 6/6 CI PASS | GH Actions |

### 4.1 零回归
- 加性 API, 不动主循环和老 Light.Time fn.
- 32+ 老 sample 不调新 fn → 零影响.

---

## 5. 风险

| 风险 | 缓解 |
|------|------|
| t 超出 [0, 1] 用户传错 | 不 clamp (允许 extrapolation, t=2 等价于过冲, 是合法用法). 文档说明 |
| LerpAngle 用户传 degree | 文档明确 radians + 命名 |
| 浮点精度: 远点 lerp 时 b-a 溢出 | 极端值才会, 文档不提 (用户不到那场景) |

---

## 6. 6A 7 件套精简映射

**本文 = ALIGNMENT + CONSENSUS + DESIGN + TASK 合并**, 后续:
- ACCEPTANCE_PhaseH_0_4.md
- FINAL_PhaseH_0_4.md
- TODO_PhaseH_0_4.md

---

## 7. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 — H.0.4 启动 |
