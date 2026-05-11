# Phase E.4 — Bloom Demo

演示 ChocoLight `Light.Graphics.Bloom` 后处理管线: 亮度提取 + 多尺度高斯模糊金字塔 + 加性合成.

Bloom 依赖 HDR 离屏 RT (RGBA16F), 默认 `Bloom.GetAutoEnable() == true`, `HDR.Enable` 时自动拉起.

## 运行

从工程根:

```
./Light samples/demo_bloom
```

或在 IDE 集成的 demo 切换器中选 `demo_bloom`.

仅 GL33 后端 (Windows/Linux/macOS desktop / Android GLES3) 支持; Legacy / WebGL 后端 `Bloom.IsSupported()` 返回 `false`, demo 会做 API surface 探测后退出.

## 控制

| 按键 | 行为 |
|------|------|
| `B` | 切换 Bloom ON / OFF (HDR 必须已开启) |
| `1` / `2` | Threshold `-` / `+` (步长 0.1; clamp `[0, +∞)`) |
| `3` / `4` | Intensity `-` / `+` (步长 0.1; clamp `[0, +∞)`) |
| `5` / `6` | Radius `-` / `+` (步长 0.05; clamp `[0, 1]`) |
| `7` / `8` | Levels `-` / `+` (clamp `[2, 8]`; 重启 Bloom 后生效) |
| `R` | 重置默认 (Thr=1.0, Int=0.8, Rad=0.7, Lv=5) |
| `ESC` | 退出 |

## 参数说明

| 参数 | 含义 | 推荐范围 |
|------|------|----------|
| **Threshold** | 亮度阈值, 高于此值的像素被提取进 bloom (soft knee 软阈值) | `0.5 ~ 2.0` |
| **Intensity** | 加性合成强度 (最终对原图贡献比例) | `0.3 ~ 1.5` |
| **Radius** | 上采样滤波半径 (0 = 紧贴, 1 = 大范围扩散) | `0.4 ~ 0.9` |
| **Levels** | 金字塔层数 (越多越软, 但开销略增) | `4 ~ 6` |

## 预期视觉

| 场景 | OFF | ON |
|------|-----|-----|
| 亮点阵列 (色块亮度 1.5 ~ 3.8) | 硬边色块, 背景纯黑 | 色块四周扩散柔和辉光, 颜色与亮度匹配 |
| 暗背景网格 (亮度 0.04) | 仅低亮度网格 | 完全不参与 bloom (低于 Threshold) |

## 联动行为

- `HDR.Disable()` 自动 `Bloom.Disable()` (Bloom 依赖 HDR RT)
- `HDR.Resize(w, h)` 自动 `Bloom.Resize(w, h)` (idempotent, 同尺寸 no-op)
- `Bloom.SetAutoEnable(false)` 后 `HDR.Enable` 不再自动启 Bloom
- `Bloom.SetLevels(n)` 在 enabled 时不立刻生效, 需 `Disable + Enable` 或 `Resize` 重建 pyramid

## API 入口

```lua
local Bloom = require('Light.Graphics').Bloom

Bloom.Enable(w, h)        -- 通常不用 (autoEnable 自动)
Bloom.Disable()
Bloom.IsEnabled()
Bloom.IsSupported()
Bloom.Resize(w, h)

Bloom.SetAutoEnable(true|false)
Bloom.GetAutoEnable()

Bloom.SetThreshold(v)
Bloom.GetThreshold()
Bloom.SetIntensity(v)
Bloom.GetIntensity()
Bloom.SetRadius(v)
Bloom.GetRadius()
Bloom.SetLevels(n)
Bloom.GetLevels()
```

详见 `docs/api/Light_Graphics_Bloom.md` (Phase E.4 交付).
