# Phase E.6 — Lens Dirt + Streak Demo

演示 ChocoLight 镜头后处理链路：Bloom 之上加 **Lens Dirt**（脏镜头）+ **Streak**（anamorphic flare）。

## 运行

```bash
light.exe samples/demo_lens_fx/main.lua
```

## 控制

| 按键 | 功能 |
|------|------|
| `L` | 切换 LensDirt on/off |
| `K` | 切换 Streak on/off |
| `1` / `2` | LensDirt Intensity -/+ (步长 0.1) |
| `3` / `4` | Streak Intensity -/+ (步长 0.05) |
| `5` / `6` | Streak Length -/+ (步长 0.005；范围 [0, 0.1]) |
| `7` / `8` | Streak Iterations -/+ (范围 [1, 8]) |
| `H` / `V` / `G` | Streak Direction 水平 / 垂直 / 对角 (0.707, 0.707) |
| `R` | reset 所有参数到默认 |
| `ESC` | 退出 |

## 默认参数

| 参数 | 默认值 | 范围 |
|------|--------|------|
| **LensDirt** Intensity | 0.4 | [0, +∞) |
| **LensDirt** DirtTexture | 0（fallback 1×1 白） | 传 Image table / 纹理 id / nil |
| **Streak** Threshold | 1.0 | [0, +∞) |
| **Streak** Intensity | 0.3 | [0, +∞) |
| **Streak** Length | 0.02 | [0, 0.1] |
| **Streak** Direction | (1, 0) 水平 | (0,0) 保留旧值 |
| **Streak** Iterations | 5 | [1, 8]（倍距扩展，i 步 × 2^i） |

## 管线流程

```
HDR.EndScene:
  UnbindFBO
  Bloom.Process(hdrFbo, hdrTex)      -- bloom 加性写回 HDR RT
  UnbindFBO
  AE.Process(hdrTex, dt)              -- 更新 exposure
  LensDirt.Process(hdrFbo,
                   Bloom.GetPyramidTopTex(),
                   w, h)              -- bloom × dirt × intensity 加到 HDR RT
  Streak.Process(hdrFbo, hdrTex)      -- bright + N iter blur + composite
  exposure = AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
  Tonemap(hdrTex, exposure, gamma)
```

## 预期视觉

- **LensDirt OFF / Streak OFF**：仅 bloom 柔光（基线）
- **LensDirt ON**：亮点周围辉光处有"斑点/划痕"叠加（使用默认 1×1 白时变成 bloom × intensity）
- **Streak ON**：每个高亮处生成水平（或 H/V/G 切换方向）长条光带，扩散很远
- **LD + ST 同开**：脏镜头 + 横向星芒组合，电影感后处理

## 自定义 Dirt 纹理

```lua
-- 方式 1: 传 Image table
local img = Light(Gfx.Image):New('assets/lens_dirt.png')
Gfx.LensDirt.SetDirtTexture(img)

-- 方式 2: 传原生 GL tex id
Gfx.LensDirt.SetDirtTexture(img:GetTextureId())

-- 方式 3: 重置为内置 1×1 白 fallback
Gfx.LensDirt.SetDirtTexture(nil)   -- 或 0
```

## API 入口汇总

**LensDirt (10 fn)**:
```
Enable / Disable / IsEnabled / IsSupported
SetAutoEnable / GetAutoEnable
SetDirtTexture(img_or_id_or_nil) / GetDirtTextureId
SetIntensity / GetIntensity
```

**Streak (13 fn)**:
```
Enable(w, h) / Disable / IsEnabled / IsSupported / Resize(w, h)
SetAutoEnable / GetAutoEnable
SetThreshold / GetThreshold
SetIntensity / GetIntensity
SetLength / GetLength
SetDirection(x, y) / GetDirection() -> x, y
SetIterations / GetIterations
```

## 已知限制

1. **LensDirt 需 Bloom 启用**：`bloomTex=0` 时 Process 静默 no-op
2. **Streak Bright Pass 复用 Bloom shader**：Bloom 不支持时 streak bright 降级
3. **RGBA16F 硬件要求**：Legacy 后端或无 FP16 能力设备 `SupportsStreak()=false`
4. **Direction (0, 0) 拒绝**：shader `normalize` 会 NaN，保留旧值
