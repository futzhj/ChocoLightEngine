# Phase F.0.10.9.2 — Multi-HDR Demo (Main + PIP) PLAN

> 6A · ALIGN + DESIGN + TASK 合并
> 工作量 ~2h, demo 类型 phase (无 C++ 改动)

---

## 1. ALIGN

### 1.1 目标

写一个 demo `samples/demo_multi_hdr_pip` 真 GL 环境演示 F.0.10.9 multi-HDR-instance 核心能力:

- **主屏 1600×900 HDR fbo** (instance 0): warm LUT, exposure=1.2, ACES tonemap
- **PIP 480×270 HDR fbo** (instance pipId): cool LUT, exposure=0.6, Uncharted2 tonemap

两个 fbo **真不同分辨率** (无法用 region 模拟), 每帧独立 Begin/Draw/End/Tonemap, 验证 F.0.10.9 multi-instance + F.0.10.9.1 state 隔离 + F.0.10.9.x.1 LUT id 跨 instance 同步 (真 GL 下 hot reload 可观察).

### 1.2 边界

**In**:
- `samples/demo_multi_hdr_pip/main.lua` (~400 行)
- 复用 `demo_taa_split2/luts/warm_red.cube + cool_blue.cube` (复制到本 demo)
- `README.md`
- headless API probe (CI 兼容)

**Out**:
- C++ 代码改动 (multi-instance 已 ready)
- 新 Lua API (HDR.GetState / Clone 留可选增强)
- 复杂 scene (用 demo_taa_split2 的 cube + bar + plane mesh 复用)

### 1.3 关键决策

| # | 决策 | 选择 | 理由 |
|---|------|------|------|
| 1 | LUT 文件来源 | 复制 demo_taa_split2 的 warm/cool | 已验证可用, demo 自包含 |
| 2 | PIP 显示位置 | 右下角 ~280×158 (16:9 ratio scale) | mini-map 典型布局, 避开 HUD |
| 3 | scene 内容 | 复用 cube + 8 bar + plane (demo_taa_split2 模板) | 减重复实现 |
| 4 | 相机视角差异 | 主屏: 高远 overview / PIP: 低近 ground-level | 视觉明显差异 |
| 5 | EndScene 重复跑 | 接受 (SSAO/AE/LensFx 未启用时 no-op) | Lua 层无 Pause API |
| 6 | Hot reload demo | 不主动演示 (用户可手测复制 LUT 文件触发) | 减复杂度 |

### 1.4 风险

- **中**: BeginFrame/EndFrame 自动 BeginScene/EndScene 与手动切 instance 冲突 → 已确认 EndFrame 自动 EndScene 调一次 active instance, 用 HDR.SetActiveInstance(0) 终止帧前重置
- **低**: PIP 渲染时 viewport 错位 → 在 PIP fbo 内用全尺寸 (0, 0, PIP_W, PIP_H) viewport, fbo 自动 scale
- **低**: HDR.Tonemap 不调 active instance 的 sceneTex → 已 grep 确认 Tonemap 用 g.sceneTex (per-instance)

---

## 2. ARCHITECT

### 2.1 帧流程

```
[active=0]
win:BeginFrame()                                  -- 自动 HDR.BeginScene(0): bind 主屏 fbo + clear

-- ===== 主屏 (instance 0) =====
Gfx.SetViewport(0, 0, MAIN_W, MAIN_H)
Gfx.SetCamera(main_camera)
drawScene(time)                                   -- 渲染到主屏 fbo
HDR.EndScene()                                    -- 手动 EndScene(0): unbind + SSAO/AE/LensFx
HDR.Tonemap(0, 0, WIN_W, WIN_H)                   -- 主屏 fbo → default backbuffer 全屏 (warm LUT)

-- ===== PIP (instance pipId) =====
HDR.SetActiveInstance(pipId)
HDR.BeginScene()                                  -- 手动 BeginScene(pipId): bind PIP fbo + clear
Gfx.SetViewport(0, 0, PIP_W, PIP_H)               -- PIP fbo 全尺寸 viewport
Gfx.SetCamera(pip_camera)
drawScene(time)                                   -- 渲染到 PIP fbo (低分辨率)
HDR.EndScene()                                    -- 手动 EndScene(pipId)
HDR.Tonemap(PIP_X, PIP_Y, PIP_DISP_W, PIP_DISP_H) -- PIP fbo → 角落 280×158 (cool LUT + auto scale)

-- ===== 终止 =====
HDR.SetActiveInstance(0)                          -- 切回防 EndFrame 自动 EndScene(pipId) 二次
win:EndFrame()                                    -- 自动 HDR.EndScene(0): 二次跑 SSAO/AE/LensFx
                                                  -- (SSAO/AE/LensFx 未启用时全 no-op, 零开销)
```

### 2.2 Init 流程

```
win = Window.Open(1600, 900)
HDR.Enable(MAIN_W=1600, MAIN_H=900)                -- instance 0
HDR.SetAutoTonemap(false)                          -- 手动 tonemap per-instance

pipId = HDR.CreateInstance()
HDR.SetActiveInstance(pipId)
HDR.Enable(PIP_W=480, PIP_H=270)                   -- PIP, 真不同分辨率
HDR.SetAutoTonemap(false)
HDR.SetActiveInstance(0)

-- LUT 加载 (LoadCubeLUT 后 lutId 全 global, 任何 instance 都可引用)
warm_id = HDR.LoadCubeLUT('warm_red.cube')
cool_id = HDR.LoadCubeLUT('cool_blue.cube')

-- 主屏 (instance 0) per-instance state: warm 暖调电影感
HDR.SetActiveInstance(0)
HDR.SetExposure(1.2)
HDR.SetGamma(2.2)
HDR.SetTonemapper('aces')
HDR.SetGradingLUT(warm_id, 0.8)

-- PIP (instance pipId) per-instance state: cool 冷夜监控感
HDR.SetActiveInstance(pipId)
HDR.SetExposure(0.6)
HDR.SetGamma(2.4)
HDR.SetTonemapper('uncharted2')
HDR.SetGradingLUT(cool_id, 0.8)

HDR.SetActiveInstance(0)
```

### 2.3 Cleanup 流程

```
HDR.SetActiveInstance(pipId); HDR.Disable()
HDR.SetActiveInstance(0); HDR.Disable()
HDR.DestroyInstance(pipId)
HDR.SetAutoTonemap(true)
HDR.DeleteLUT3D(warm_id)
HDR.DeleteLUT3D(cool_id)
mesh:Delete() ...
```

---

## 3. ATOMIZE

| Step | 内容 | 工作量 |
|------|------|------|
| S1 | 看 demo_taa_split2 模板 + 确认 API 边界 | 完成 |
| S2 | 写 PLAN (本文) | 完成 |
| S3 | 复制 LUT 文件到本 demo | 5min |
| S4 | 写 main.lua headless API probe + UI init | 30min |
| S5 | 写 main.lua scene/draw/frame loop | 30min |
| S6 | 写 main.lua cleanup + HUD + README | 20min |
| S7 | headless 跑 + smoke 零回归 | 10min |
| S8 | FINAL + TODO + commit + CI | 15min |
| **合计** | | **~2h** |

---

## 4. 验收

| 类型 | 标准 |
|------|------|
| headless 模式 | API probe 跑通, 输出 PASS, exit 0 |
| 真 GL 模式 | 用户跑 IDE 看到主屏 warm 调色 + 右下角 PIP cool 调色, 真两套调色显著差异 |
| 8 相关 smoke 零回归 | hdr/bloom/ssr/auto_exposure/lens_fx/motion_blur/taa/lighting2d 全 PASS |
| CI | 6/6 绿 (demo headless 兼容) |
