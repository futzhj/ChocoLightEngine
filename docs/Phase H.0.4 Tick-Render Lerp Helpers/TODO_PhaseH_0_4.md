# Phase H.0.4 Tick-Render Lerp Helpers — TODO

> **完成日期**: 2026-05-19
> **状态**: T1~T6 完成

---

## 1. 用户配置

无新增配置. 默认行为:
- `Light.Time.Lerp/LerpVec2/LerpVec3/LerpAngle` 4 fn 直接可用.
- `t` 参数缺省 → 自动用 `LT::TickRender::GetAlpha()`.
- 不 clamp t (允许 extrapolation: t < 0 反向, t > 1 过冲).
- LerpAngle 输入是 **radians** (与 math.sin/cos 一致).

---

## 2. 推荐验证

### 2.1 smoke
```powershell
.\build\Release\Light.exe scripts\smoke\tick_render.lua
# 期待: §1~§14 全 PASS, 包括 11 个 §14 检查
```

### 2.2 demo 视觉
```powershell
.\build\Release\Light.exe samples\demo_tick_render\main.lua
# 看右方块 (绿色) 平滑移动 - 与 H.0.3 之前视觉完全一致 (内部用 LerpVec2 helper 替代手写)
# 按 A 键切关 alpha → 右方块跟左方块一样跳动
```

---

## 3. 已知限制

### 3.1 不支持 Vec4 / Color RGBA
**当前**: 仅 1D/2D/3D + Angle.
**升级** (~0.2h):
```cpp
int l_Time_LerpVec4(lua_State* L) { ... 8 个参数 + push 4 个 ... }
```

### 3.2 不支持 Quaternion slerp
**当前**: LerpAngle 仅 1D 角度 (轴-角无 axis). 真正 3D 旋转 lerp 需 quat slerp.
**前置**: 引擎需先引入 quat lib (math/quat.h).
**估时**: 1-2h.

### 3.3 不支持矩阵插值
**当前**: 4x4 矩阵 lerp 一般不正确 (旋转部分必须 slerp).
**升级路径**:
1. 先做 quat slerp.
2. 再做 mat4 split (translate + rotate + scale) + slerp 旋转 + lerp 平移/缩放 + 重组.
3. 估时 2-3h, 留 H.1 Transform component 范畴.

### 3.4 不暴露独立 Light.Math 命名空间
**当前**: 4 fn 都在 Light.Time 表下 (符合 GetAlpha 同表).
**未来**: 若引入更大 math 库, 可在 Light.Math 重导出 (`Light.Math.Lerp = Light.Time.Lerp`).

### 3.5 CPU 端实现, 不替代 shader mix()
**当前**: H.0.4 仅 CPU Lua helper. GPU shader 端 alpha 插值仍需 fragment 自己写:
```glsl
out_color = mix(prev_color, curr_color, u_alpha);
```
**升级**: H.1 Transform component 时考虑自动注入 `u_alpha` uniform.

---

## 4. 增强候选 (按 ROI)

### 4.1 LerpVec4 / LerpColor ⭐⭐
**估时**: 0.3h
**收益**: UI 元素颜色淡入淡出 / HDR 数据更顺手.

### 4.2 LerpArray(a_array, b_array [, t]) ⭐
**估时**: 0.5h
**收益**: 用户可一次性 lerp 整个 vertex array / control points.
**实现**: 遍历 array, 数值字段 lerp, 其他字段保留 a 值.

### 4.3 Smoothstep / Smootherstep ⭐⭐
**估时**: 0.2h
**收益**: ease-in/out 经典曲线; 用户少写 `t = t*t*(3-2*t)`.
```cpp
int l_Time_Smoothstep(lua_State* L);   // t -> t*t*(3-2t)
int l_Time_Smootherstep(lua_State* L); // t -> t*t*t*(t*(6t-15)+10)  Perlin
```

### 4.4 Bezier / Hermite curve ⭐
**估时**: 1h
**收益**: 路径动画.

### 4.5 Quaternion slerp + 3D 旋转 lerp ⭐⭐⭐
**估时**: 1-2h (需先引入 quat lib)
**前置**: math/quat.h
**收益**: 真正正确的 3D 旋转 lerp.

---

## 5. 文档状态

| 文档 | 状态 |
|------|------|
| CONSENSUS | ✅ |
| ACCEPTANCE | ✅ |
| FINAL | ✅ |
| TODO | ✅ 本文 |

注: H.0.4 是 4A 轻量, 无 ALIGNMENT/DESIGN/TASK 单独文件 (合并入 CONSENSUS).

---

## 6. 版本

| 版本 | 日期 | 修订 |
|------|------|------|
| v1.0 | 2026-05-19 | 初稿 |
