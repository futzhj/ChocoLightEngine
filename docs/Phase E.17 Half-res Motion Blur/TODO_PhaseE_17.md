# Phase E.17 Half-res Motion Blur — TODO（精简清单）

> 6A 工作流 · 完结后续项

---

## 1. 必须项

### 1.1 GitHub Actions CI 6/6 ✅ 完成

- **CI run**: [`25897849619`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25897849619)
- **Commit**: `6769bba`
- **状态**: ✅ 6/6 平台全 success
  - ✅ build-windows（含 runtime smoke phaseE17Smoke 24 PASS）
  - ✅ build-linux
  - ✅ build-macos
  - ✅ build-android
  - ✅ build-ios
  - ✅ build-web
- **Phase E.17 motion_blur.lua**: 24 PASS（21 原 + 3 halfRes 新）
- **现有 16 个 phase smoke**: 零回归确认

### 1.2 真机视觉验收（桌面 GL3.3 真窗口） 🧑‍💻

- **场景 A — full-res vs half-res 视觉对比**：
  - 启动 demo_ssr，按 `H` `M` 启 motion blur
  - 让 mesh 旋转 + 相机平移产生明显运动模糊
  - 按 `[` 切换 halfRes ON/OFF
  - **预期**：视觉差异 ≤ 5%（肉眼几乎不可见）

- **场景 B — half-res × mode=2 (object_only) 组合**：
  - mode=2 是减法 (combined - camera)，halfRes 下精度损失最大
  - **预期**：拖尾边缘略模糊，但运动方向正确

- **场景 C — 1080p 帧率提升验证**：
  - 性能 monitor 测量帧率：halfRes ON vs OFF
  - **预期**：motion blur Pass 节省 ~0.35 ms（占帧 16.7 ms 的 ~2%）

- **场景 D — 极小尺寸边界**：
  - 360p 分辨率 → halfRes 后 180p
  - **预期**：用户可手动判断启停

### 1.3 移动端 / VR 性能基线 🧑‍💻

- 用 NSight Graphics / RGP / PIX 测量 GPU profiler
- 期望 mobile/VR halfRes ON 帧率提升 ≥ 10%

---

## 2. 建议项

### 2.1 Quarter-res motion blur 候选（Phase E.17.1）

- 1/4 分辨率（(w+3)/4, (h+3)/4），VRAM −94%、性能 ~16×
- 视觉损失更明显（~10~15%），仅适合 VR / mobile
- 工作量：~30 行（仅扩展 ComputeStorageSize 一个分支）

### 2.2 自适应分辨率（Phase E.17.2）

- 根据屏幕分辨率自动切 full / half / quarter
- 例：< 720p → full；720p~1440p → half；> 1440p → quarter
- 需要新增 `MotionBlur.SetAutoResolution(bool)` API

### 2.3 视觉差异测试（CI 集成）

- 渲染参考帧 → SSIM 比较 full-res vs half-res
- 阈值 SSIM ≥ 0.95 通过
- 需要 PNG 比较框架 + reference 图（重活，留候选）

---

## 3. 后续 phase 候选

### 3.1 Phase E.18 — Independent Velocity Dilation Pass — ✅ 已完成

- ✅ Phase E.18 已完成（CI run [25900086693](https://github.com/futzhj/ChocoLightEngine/actions/runs/25900086693) 6/6 green，commit `e894834`）
- 实施回顾：抽出 `FS_VELOCITY_DILATE` shader (GLES3 + GL33)；HDR EndScene 内在 SSR/MotionBlur 之前执行 dilation pass，dilatedVelocityTex (RG16F) + 可选 dilatedCameraVelocityTex；consumer 优先取 dilated、fallback raw
- 性能：SSR + Motion Blur 同开时 velocity fetch 节省 ~50%（81 → 18 fetch / pixel @ N=8）
- API：复用 `Light.Graphics.HDR.SetVelocityDilation(bool)`，零 API 变动 + 三层 silent fallback 保证业务零感知
- 详见 `@e:/jinyiNew/Light/docs/Phase E.18 Independent Velocity Dilation Pass/FINAL_PhaseE_18.md`

### 3.2 Phase E.19 — SSR Temporal 选择性 camera-only velocity

- 镜面反射在物体相对屏幕静止 + 相机平移时用 camera-only velocity reproject
- 减少高速运动模糊伪影
- 复用 Phase E.16 cameraVelocityTex

### 3.3 Phase F.x — Velocity TAA / MDFG 2012

- TAA：velocity buffer + history buffer
- MDFG：经典 motion blur 复原滤波器，per-tile max + per-pixel reconstruction

---

## 4. 用户操作引导

### 4.1 切换 halfRes 看效果

```bash
./Light/light.exe samples/demo_ssr/main.lua

# demo 窗口:
#   H — 启 HDR
#   M — 启 MotionBlur
#   [ — 切 halfRes ON/OFF
# 控制台 + HUD 同步显示状态
```

### 4.2 Lua 程序使用

```lua
local Gfx = require 'Light.Graphics'
local HDR = Gfx.HDR
local MB  = Gfx.MotionBlur

HDR.Enable(1280, 720)
if MB.IsSupported() then
    MB.Enable(1280, 720)

    -- 推荐：移动端 / 高分屏（≥ 1440p）始终 ON
    MB.SetHalfRes(true)

    -- 可选：与 Phase E.16 mode 组合
    MB.SetMode(1)               -- camera_only + halfRes 性价比最高
    MB.SetStrength(1.5)
    MB.SetSampleCount(8)        -- halfRes 下 8 已经足够
end

-- 查询状态
local stats = string.format(
    'MotionBlur: %s | mode=%d | halfRes=%s | strength=%.2f',
    MB.IsEnabled() and 'ON' or 'OFF',
    MB.GetMode(),
    MB.GetHalfRes() and 'ON' or 'OFF',
    MB.GetStrength()
)
print(stats)
```

### 4.3 适用场景速查表

| 设备 / 分辨率 | halfRes 推荐 |
|--------------|-------------|
| Mobile / VR  | ✅ 始终 ON |
| 4K (2160p)   | ✅ ON |
| 1440p QHD    | ✅ ON |
| 1080p FHD    | ⚖️ 看性能预算 |
| 720p HD      | ❌ OFF |
| < 720p       | ❌ OFF |

---

## 5. 缺少的配置 / 资源

无。Phase E.17 完全在引擎内部实现，不需要外部资源（图片 / glTF / shader 文件等）。

---

## 6. 总结

| 优先级 | 项目 | 阻塞 Phase 完成？ |
|--------|------|------------------|
| 🔴 必须 | CI 6/6 通过 | 是 |
| 🟡 必须 | 真机视觉验收（用户参与） | 否（推真机后） |
| 🟢 建议 | Quarter-res / 自适应分辨率 | 否（候选 phase） |
| 🔵 候选 | Phase E.18~E.19 / Phase F.x | 否（未来 phase） |
