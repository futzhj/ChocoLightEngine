# Phase E.5 — Auto Exposure (Eye Adaptation) Demo

演示 ChocoLight Auto Exposure 模块，通过 HDR RT 测量场景平均亮度并平滑调整 exposure，模拟人眼适应行为。

## 运行

```bash
light.exe samples/demo_auto_exposure/main.lua
```

## 操作

| 按键 | 功能 |
|------|------|
| `A` | 切换 AE on/off（off 时回归 manual `HDR.SetExposure`） |
| `D` | 切换场景：split (左暗右亮) / darkOnly / brightOnly |
| `1` / `2` | SpeedUp -/+ 0.5 EV/s（暗→亮 适应速度） |
| `3` / `4` | SpeedDown -/+ 0.5 EV/s（亮→暗 适应速度） |
| `5` / `6` | TargetEV -/+ 0.5（用户偏移；正值更亮） |
| `R` | 重置默认 (SpeedUp=3 / SpeedDown=1 / TargetEV=0) |
| `ESC` | 退出 |

## 默认参数

| 参数 | 默认值 | 范围 / 含义 |
|------|--------|------------|
| `TargetEV` | 0.0 | 用户偏移；加在测量 EV 上 |
| `SpeedUp` | 3.0 EV/s | 暗→亮 适应速度（人眼快） |
| `SpeedDown` | 1.0 EV/s | 亮→暗 适应速度（人眼慢） |
| `MinEV` | -8.0 | 曝光下限（防夜场过曝） |
| `MaxEV` | +8.0 | 曝光上限（防强光欠曝） |
| `AutoEnable` | **false** | 默认 manual exposure（与 Bloom 默认 true 区别） |

## 场景设计

| 模式 | 内容 | 用途 |
|------|------|------|
| **split** | 左半屏暗 (bg=0.02) + 右半屏亮 (bg=1.0)，各带色块 | 空间对比；AE 取整画面平均，EV 应稳定在中间 |
| **darkOnly** | 全屏暗 (bg=0.02) + 中亮色块 (0.5) | AE ON 时 EV 上升 (~+3-5)，色块逐渐变亮显现 |
| **brightOnly** | 全屏亮 (bg=1.0) + HDR 色块 (5.0) | AE ON 时 EV 下降 (~-2)，过曝高亮区域恢复细节 |

## 预期视觉

- **AE OFF**：manual exposure=1.0，亮场景过曝、暗场景欠曝；切换场景立即"硬切"
- **AE ON**：
  - **暗→亮**：~0.5-1s 平滑收缩 exposure，过曝刺眼瞬间消失
  - **亮→暗**：~3-5s 慢慢放大 exposure，暗区细节渐显
  - **OSD `CurrentEV`** 实时下滑/上升

## 模块联动

| AE 状态 | HDR.Tonemap exposure 来源 |
|---------|--------------------------|
| `Disabled` | `HDR.SetExposure` (manual) |
| `Enabled` | `AE.GetCurrentExposure()` 覆盖 manual |

`AE.SetAutoEnable(true)` 后，下次 `HDR.Enable()` 自动启 AE（同 Bloom 模式）；默认 false 是为了不破坏 demo_hdr 等 manual exposure 测试。

## API 入口（18 函数）

```lua
local AE = require('Light.Graphics').AutoExposure

-- 生命周期
AE.Enable(w, h) / AE.Disable() / AE.IsEnabled() / AE.IsSupported() / AE.Resize(w, h)

-- 联动开关 (默认 false, 与 Bloom 默认 true 区别)
AE.SetAutoEnable(flag) / AE.GetAutoEnable()

-- EV-based 参数
AE.SetTargetEV(v)   / AE.GetTargetEV()
AE.SetSpeedUp(v)    / AE.GetSpeedUp()      -- clamp [0.1, 20]
AE.SetSpeedDown(v)  / AE.GetSpeedDown()    -- clamp [0.1, 20]
AE.SetMinEV(v)      / AE.GetMinEV()        -- 强制 min <= max
AE.SetMaxEV(v)      / AE.GetMaxEV()

-- 调试 / OSD
AE.GetCurrentEV()           -- 平滑后当前 EV
AE.GetCurrentExposure()     -- = 2^GetCurrentEV()
AE.GetMeasuredLuminance()   -- 上一帧测得 log luma
```

## 已知限制

1. **CPU 同步 readback**：v1 用 `glReadPixels` 读 1×1，约 10us stall。后续可用 PBO ping-pong 异步化（TODO_PhaseE_5）。
2. **依赖 HDR**：AE 测量基于 HDR RT；`HDR.Disable` 时自动 `AE.Disable`。
3. **R16F 半精度**：log luma 范围 clamp [-12, 12]（覆盖亮度 6e-6 ~ 162754，远超任何真实场景）。
4. **Legacy 后端不支持**：`SupportsAutoExposure()=false`，所有 API no-op。
