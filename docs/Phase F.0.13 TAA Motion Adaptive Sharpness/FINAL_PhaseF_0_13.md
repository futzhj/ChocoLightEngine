# Phase F.0.13 TAA Motion-Adaptive Sharpness — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告

---

## 1. 交付物

| 文件 | 行数 |
|------|------|
| `ChocoLight/include/render_backend.h` (virtual ComputeCameraMotionScalar) | +8 |
| `ChocoLight/src/render_gl33.cpp` (GL33 override Frobenius distance) | +14 |
| `ChocoLight/include/taa_renderer.h` (4 fn 声明 + doxygen) | +12 |
| `ChocoLight/src/taa_renderer.cpp` (state +2 + Process effSharpness + Set/Get +4) | +30 |
| `ChocoLight/src/light_graphics.cpp` (Lua API +4 + taa_funcs[]) | +50 |
| `scripts/smoke/taa.lua` (F.0.13 完整段 ~10 PASS) | +135 |
| `samples/demo_ssr/main.lua` (O 键 + Keys help) | +15 |
| `docs/api/Light_Graphics.md` (速查表 + 完整段) | +70 |
| `docs/Phase F.0.13 TAA Motion Adaptive Sharpness/` 4 文档 | ~430 |

**累计**: 代码 ~115 行 + 文档 ~580 行

---

## 2. 核心方案

引入 motion-adaptive sharpness：高速相机运动时自动 lerp sharpness 到 motionSharpness 减 reprojection trail；静止/慢速时保持原 sharpness 保锐度。与 Phase F.0.8 motion-adaptive variance γ 成对，使 motion-adaptive 设定一体化。

### 算法 (Frobenius distance + linear lerp)

```
backend->ComputeCameraMotionScalar():
    if !hasPrevViewProjForVelocity: return 0
    ssd = sum( (curViewProj[i] - prevViewProj[i])^2 ) for i in 0..15
    return sqrt(ssd)

TAARenderer Process (sharpness > 0 时):
    motion       = backend->ComputeCameraMotionScalar()
    factor       = clamp(motion * 0.5, 0, 1)
    effSharpness = sharpness + (motionSharpness - sharpness) * factor
    // 然后用 effSharpness 替代 g.sharpness 传给 backend
```

### 性能 (1080p)

| 场景 | motionAdaptiveSharpness=false | =true |
|------|-------------------------------|--------|
| ComputeCameraMotionScalar | 不调用 | ~0.001 ms (16 float SSD) |
| lerp | 不执行 | ~0.0001 ms |
| sharpen pass (unsharp/cas/rcas) | 原样 | 原样 |
| **累计 overhead** | **0** | **~0.001 ms** |

零额外 GPU 工作 (Frobenius distance 在 CPU)。

---

## 3. API surface (新增 4, TAA 31 → 35 fn)

```lua
TAA.SetMotionAdaptiveSharpness(true)      -- 开关 (default false)
TAA.GetMotionAdaptiveSharpness() → bool
TAA.SetMotionSharpness(0.1)               -- 高速目标 [0, 2] (default 0.1)
TAA.GetMotionSharpness() → number
```

---

## 4. Phase F.0 系列累计 (11 sub-phase)

| Phase | 功能 | Lua API |
|-------|------|---------|
| F.0 ~ F.0.12 | 主管线 + 10 优化 | 31 |
| **F.0.13** | **motion-adaptive sharpness** | **+4 (35)** |

**累计**: 35 fn / 5 shader / 4 backend pass / 5 backend 接口扩展 / 3 demos

---

## 5. CI 状态

| 项 | 值 |
|----|----|
| GitHub Run ID | [`25936869113`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113) |
| Result | **6/6 platforms success** ✅ |
| F.0.13 commit | `3923584` |
| Fix commit | `0a794d0` (typo GL33RenderBackend → GL33Backend) |
| Date | 2026-05-15 19:23 UTC |

**发现问题 + 修复**:
- F.0.13 原始 commit `3923584` 全 6 平台 build failure: render_gl33.cpp 使用了不存在的类名 `GL33RenderBackend` (实际是 `GL33Backend`)
- fix commit `0a794d0` 修正 typo + 去掉多余 const_cast (ComputeViewProj3D 已是 const)
- Windows 后续暴露 F.0.14 Lua 白名单漏加 'lanczos' (fix `c5264f2`, 含在同一 CI run)

---

## 6. 工程反思

### 做得好

1. **零 GPU overhead**: motion 检测在 CPU 端 (Frobenius distance 16 float SSD), 不增加 shader 复杂度
2. **零 shader 改动**: 在 TAARenderer 层 lerp 替代 g.sharpness, 与 unsharp/cas/rcas 三 mode 100% 兼容
3. **零回归**: motionAdaptiveSharpness=false 默认值, ComputeCameraMotionScalar 老 backend 默认 0
4. **决策矩阵 7/7 全自动**: 复用 F.0.8 模式 (4 fn = 2 对)
5. **与 F.0.8 协同**: 双重防 trail (sharpness lerp + γ lerp), 推荐 FPS/赛车场景

### 可改进

1. **Frobenius distance 不直接对应屏幕空间运动**: 经验系数 0.5 校准, 不同 FoV/zNear/zFar 下 motion 阈值不同
2. **未实现 per-pixel motion-adaptive sharpness**: 全屏统一 factor, 物体局部运动 (相机静止) 不能触发
3. **未在 demo HUD 显示当前 motion factor**: 调试时无法直观感知 motion 估计精度

---

## 7. Phase F.0.x 后续候选

- F.0.10 — TAARenderer 多实例化 + 真 split-screen demo
- F.0.11 — Demo 截图 / 录屏
- F.0.14 — Lanczos-2 25-tap 上采样
- F.0.15 — TAA-driven CAS strength scaling (sharpness 跟 history stability 自动调)
- F.1 — DLSS-like TAAU
