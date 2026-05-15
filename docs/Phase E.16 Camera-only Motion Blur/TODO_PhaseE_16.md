# Phase E.16 Camera-only Motion Blur — TODO（精简清单）

> 6A 工作流 · 完结后续项
> 用于直接寻找操作支持。已按必须 / 建议 / 候选三档排序。

---

## 1. 必须项（阻塞 Phase E.16 完成）

### 1.1 GitHub Actions CI 6/6 ✅ 完成

- **CI run**: [`25896826324`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25896826324)
- **Commit**: `46cd329`
- **状态**: ✅ 6/6 平台全 success
  - ✅ build-windows（含 runtime smoke phaseE16Smoke 21 PASS）
  - ✅ build-linux
  - ✅ build-macos
  - ✅ build-android
  - ✅ build-ios
  - ✅ build-web
- **Phase E.16 motion_blur.lua**: 21 PASS（16 原 + 5 mode 新）
- **现有 16 个 phase smoke**: 零回归确认

### 1.2 真机视觉验收（桌面 GL3.3 真窗口） 🧑‍💻

需要用户参与，非 CI 可覆盖。

- **场景 A — mode=1 camera_only 验证**：
  - 启动 demo_ssr，按 `H` 启 HDR、`M` 启 MotionBlur
  - 按 `;` 切换到 mode=1 (camera_only)
  - 让一个 mesh 静止在屏幕中央，相机旋转/平移
  - **预期**：mesh 出现拖尾（来自相机动）
  - 让相机静止，mesh 旋转/平移
  - **预期**：mesh 不出现拖尾（camera 不动）

- **场景 B — mode=2 object_only 验证**：
  - 切到 mode=2 (object_only)
  - mesh 旋转/平移，相机静止
  - **预期**：mesh 出现拖尾（来自物体动）
  - mesh 静止，相机平移
  - **预期**：拖尾消失或大幅减弱

- **场景 C — mode=0 combined 零回归**：
  - 切到 mode=0，与 Phase E.15 行为对比
  - **预期**：与 Phase E.15 视觉完全一致

- **场景 D — RG8 + camera_only 组合**：
  - 按 `L` 切到 RG8 velocity，再切 mode=1
  - **预期**：camera_only 视觉与 RG16F 相近（小幅截断可接受）

---

## 2. 建议项（提升质量）

### 2.1 性能基线测量（1080p）

需要真机 + GPU 性能 counter（NSight Graphics / RGP / PIX）。

| 模式 | 期望 Pass1 + Pass2 总耗时 | 备注 |
|------|--------------------------|------|
| mode=0 combined | ~0.7 ms | 应与 Phase E.15 等价 |
| mode=1 camera_only | ~0.7 ms | 同 dilation 9-tap × 1 套 |
| mode=2 object_only | ~0.77 ms (+10%) | 多 1 套 9-tap sampler |

VRAM：1080p +1 MB (RG8) / +4 MB (RG16F)，跟随 Phase E.14 format。

### 2.2 mode=2 性能优化（可选）

如果 §2.1 实测 mode=2 > +15%，考虑内联两个 sampler 在同一循环（省 1 次 9-tap 循环）：

```glsl
// 优化版（备选）：mode=2 时内联两次 sample
vec2 SampleObjectVelocityDilated(vec2 uv) {
    if (uVelocityDilation == 0) {
        return DecodeVelocity(texture(uVelocityTex, uv).rg)
             - DecodeVelocity(texture(uCameraVelocityTex, uv).rg);
    }
    vec2 bestV = vec2(0.0);
    float bestLen = -1.0;
    for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
        vec2 uvN = uv + vec2(float(dx), float(dy)) * uTexel;
        vec2 v = DecodeVelocity(texture(uVelocityTex, uvN).rg)
               - DecodeVelocity(texture(uCameraVelocityTex, uvN).rg);
        float l = dot(v, v);
        if (l > bestLen) { bestLen = l; bestV = v; }
    }
    return bestV;
}
```

仅在性能测得有需要时实施。

---

## 3. 后续 phase 候选

### 3.1 Phase E.17 — Half-res Motion Blur 优化

- 把 motion blur 输出 RT 降到 1/2 分辨率
- VRAM -50%，性能 +30%
- 视觉损失可接受（运动模糊本来就是低频信号）

### 3.2 Phase E.18 — Independent Velocity Dilation Pass

当前 dilation 是 inline 算法（每个消费者 9-tap）。如果未来 motion blur + SSR + TAA 三个消费者同时启用，**抽出独立 dilation pass**（写 dilatedVelocityTex），三方共享：
- 性能 -2× 9-tap 重复
- 复杂度小幅上升

### 3.3 Phase E.19 — SSR Temporal 选择性 camera-only velocity

物体反射在镜面上的稳定性：物体相对屏幕静止 + 相机平移时，**用 camera-only velocity reproject** 反射射线起点能减少高速运动模糊。需要：
- SSR API 新增 `SetVelocitySource(0/1)`
- SSR Temporal shader 接收 cameraVelocityTex

### 3.4 Phase F.x — Velocity TAA

利用 velocity buffer + history buffer 做 Temporal Anti-Aliasing。Phase E.13/E.14 velocity buffer 已经具备所有先决条件。

### 3.5 Phase F.y — MDFG 2012 Reconstruction Filter

经典 motion blur 复原滤波器（McGuire et al. 2012），相比线性多 sample 更接近真实快门效应。但实施复杂（per-tile max + per-pixel reconstruction）。

---

## 4. 用户操作引导

### 4.1 切换 mode 看效果

```bash
# 1. 启动 demo
./Light/light.exe samples/demo_ssr/main.lua

# 2. 在 demo 窗口中:
#    H — 启 HDR
#    M — 启 MotionBlur
#    ; — 循环切 mode (0 → 1 → 2 → 0)
#    控制台打印当前 mode 名
#    HUD 同时显示 "MotionBlur: ON | mode=N (name) | ..."
```

### 4.2 Lua 程序使用

```lua
local Gfx = require 'Light.Graphics'
local HDR = Gfx.HDR
local MB  = Gfx.MotionBlur

-- 1. HDR + MotionBlur 启用（mode 默认 0 = combined，与 Phase E.15 完全一致）
HDR.Enable(1280, 720)
if MB.IsSupported() then
    MB.Enable(1280, 720)

    -- 2. Phase E.16: 切到 camera_only mode（赛车 / FPS 推荐）
    MB.SetMode(1)

    -- 3. 调强度 / 采样数
    MB.SetStrength(1.5)
    MB.SetSampleCount(16)
end

-- 4. 主循环正常 Draw 3D 场景（mesh:Draw 接受 prevModel 才会写 velocity buffer）
-- ...

-- 5. 关闭（反向）
MB.Disable()
HDR.Disable()
```

### 4.3 调试 — 查看当前 mode

```lua
local m = MB.GetMode()
local names = { [0] = 'combined', [1] = 'camera_only', [2] = 'object_only' }
print('current motion blur mode:', m, names[m])
```

---

## 5. 缺少的配置 / 资源

无。Phase E.16 完全在引擎内部实现，不需要外部资源（图片 / glTF / shader 文件等）。

---

## 6. 总结

| 优先级 | 项目 | 阻塞 Phase 完成？ |
|--------|------|------------------|
| 🔴 必须 | CI 6/6 通过 | 是 |
| 🟡 必须 | 真机视觉验收（用户参与） | 否（推真机后） |
| 🟢 建议 | 性能基线测量 | 否 |
| 🔵 候选 | Phase E.17~E.19 + Phase F.x | 否（未来 phase） |
