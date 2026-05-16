# demo_multi_hdr_pip — Multi-HDR-Instance (Main + PIP)

> Phase F.0.10.9.2 演示

真 GL 环境下演示 F.0.10.9 multi-HDR-instance 核心能力: **同帧两个真不同分辨率 HDR fbo 独立调色**.

## 视觉效果

- **主屏 (Main)** — 1600×900 HDR fbo (instance 0)
  - 全屏渲染, **暖红 LUT (warm_red.cube)**
  - exposure=1.2 (亮 20%), ACES tonemap, 高远 overview 相机
- **PIP** — 480×270 HDR fbo (instance 1, **真低分辨率**)
  - 右上角 320×180 显示区, **冷蓝 LUT (cool_blue.cube)**
  - exposure=0.6 (暗 40%), Uncharted2 tonemap, 低近绕场旋转相机

两个 fbo 真不同分辨率, 不是 region 切分模拟. 每帧各自 BeginScene/Draw/EndScene/Tonemap.

## 运行

```bash
# Windows
.\lumen-master\build\src\light\Release\light.exe samples\demo_multi_hdr_pip\main.lua
```

## 控制

- **L** — 切换 per-instance LUT 启用 / 关闭 (验证 LUT 立即生效)
- **E** — 切换主屏 exposure (1.2 ↔ 0.4, 极暗对比演示)
- **R** — 切换 PIP 相机绕场旋转
- **ESC** — 退出

## 验证什么

| Phase | 验证项 |
|-------|--------|
| F.0.10.9 | Multi-instance HDR fbo 同帧独立工作 |
| F.0.10.9.1 | Per-instance state (exposure/gamma/tonemap/LUT) 真隔离 |
| F.0.10.9.x.1 | LUT id 跨 instance 同步 (热重载 LUT 文件时所有 instance 看到新调色) |
| F.0.10.9.2 | 手动 HDR.BeginScene/EndScene Lua API |

## 关键 API 用法 (新增 BeginScene/EndScene)

```lua
-- Init
HDR.Enable(MAIN_W, MAIN_H)              -- instance 0
local pipId = HDR.CreateInstance()
HDR.SetActiveInstance(pipId)
HDR.Enable(PIP_W, PIP_H)                -- instance pipId, 真不同分辨率
HDR.SetAutoTonemap(false)
HDR.SetActiveInstance(0); HDR.SetAutoTonemap(false)

-- Per-instance state
HDR.SetActiveInstance(0)
HDR.SetExposure(1.2); HDR.SetTonemapper('aces')
HDR.SetGradingLUT(warm_lut, 0.85)

HDR.SetActiveInstance(pipId)
HDR.SetExposure(0.6); HDR.SetTonemapper('uncharted2')
HDR.SetGradingLUT(cool_lut, 0.85)

-- Frame (in Draw callback, instance 0 已 BeginScene by Window:__call)
function Demo:Draw()
    -- Main
    Gfx.SetViewport(0, 0, MAIN_W, MAIN_H)
    drawScene()
    HDR.EndScene()                      -- 手动结束 instance 0
    HDR.Tonemap(0, 0, WIN_W, WIN_H)     -- → backbuffer 全屏 (warm)

    -- PIP
    HDR.SetActiveInstance(pipId)
    HDR.BeginScene()                    -- 手动开始 instance pipId
    Gfx.SetViewport(0, 0, PIP_W, PIP_H)
    drawScene()
    HDR.EndScene()
    HDR.Tonemap(PIP_X, PIP_Y, PIP_W_DISP, PIP_H_DISP)  -- → backbuffer 角落 (cool)

    HDR.SetActiveInstance(0)            -- 切回防 EndFrame 重复调
end
```

## 已知限制

- `HDR.BeginScene/EndScene` 调用次数与 Window:__call 自动调不匹配会导致 SSAO/AE/LensFx 重复跑 (但 demo 未启用这些, 实际 no-op)
- PIP 显示尺寸 (320×180) 是 default backbuffer 上的"截图区", 实际渲染是 480×270 — GL bilinear 自动 scale

## headless 模式 (CI 兼容)

无 GL ctx 时跑 API probe (5 子项), 验证:
1. 默认 count=1 active=0
2. CreateInstance + SetActive round-trip
3. Per-instance state 隔离 (exposure/gamma/tonemap)
4. Per-instance LUT id 隔离 (mock id)
5. Cleanup (count 回到 1)
