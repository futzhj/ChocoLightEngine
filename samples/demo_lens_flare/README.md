# Phase E.7 — Lens Flare Demo

演示 **Lens Flare**（Ghost + Halo + Chromatic Aberration）—— HDR 链路第 6 剑客，电影感屏幕空间镜头光晕。

## 运行

```bash
light.exe samples/demo_lens_flare/main.lua
```

## 控制

| 按键 | 功能 |
|------|------|
| `F` | 切换 LensFlare on/off |
| `1` / `2` | GhostCount −/+ （步长 1, 范围 [0, 8]） |
| `3` / `4` | GhostDispersal −/+ （步长 0.05, 范围 [0, 2.0]） |
| `5` / `6` | HaloWidth −/+ （步长 0.05, 范围 [0, 1.0]） |
| `7` / `8` | ChromaticAberration −/+ （步长 0.001, 范围 [0, 0.02]） |
| `9` / `0` | Intensity −/+ （步长 0.05） |
| `D` | 切换 DistortionEnabled（色差开关） |
| `R` | reset 所有参数到默认 |
| `ESC` | 退出 |

## 默认参数

| 参数 | 默认 | 范围 |
|------|------|------|
| **Threshold** | 1.0 | [0, +∞) |
| **Intensity** | 0.4 | [0, +∞) |
| **GhostCount** | 4 | [0, 8] 整数（0 = 关 ghost） |
| **GhostDispersal** | 0.4 | [0, 2.0] 径向缩放 |
| **HaloWidth** | 0.5 | [0, 1.0] UV（0 = 关 halo） |
| **ChromaticAberration** | 0.005 | [0, 0.02] 径向偏移 |
| **DistortionEnabled** | true | bool（false = 关色差节省 3× 带宽） |

## 管线流程

```
HDR.EndScene:
  Bloom.Process              -- bloom additive → HDR RT
  AE.Process                  -- 更新 exposure
  LensDirt.Process            -- bloom × dirt → HDR RT
  Streak.Process              -- anamorphic flare → HDR RT
  LensFlare.Process:                                       [本 demo]
    1. Bloom.DrawBloomBrightPass(hdrTex → lfRT[0], threshold)  -- 复用
    2. DrawLensFlareGhost(lfRT[0] → lfRT[1],
         ghostCount, dispersal, haloWidth, ca, distortion)     -- 新 shader
    3. Bloom.DrawBloomComposite(lfRT[1] → hdrFbo, intensity)    -- 复用
  Tonemap → backbuffer
```

## 预期视觉

- **默认（F=ON）**：每个 HDR 亮点周围可见
  - **Ghost**：朝画面中心反投的多个光圈，颜色因 chromatic aberration 略带红蓝边
  - **Halo**：围绕画面中心的环形光晕
- **GhostCount=0**：仅剩 halo
- **HaloWidth=0**：仅剩 ghost
- **DistortionEnabled=OFF**：色差消失，纯白光圈

## 算法核心（fragment shader）

```glsl
vec2 flippedUV = 1.0 - vUV;
vec2 centerVec = vec2(0.5) - vUV;
vec3 result = vec3(0.0);

// Ghost: 反向采样 N 次, 倍距径向收缩
if (uGhostCount > 0) {
    vec2 ghostVec = (vec2(0.5) - flippedUV) * uGhostDispersal;
    for (int i = 0; i < 8; ++i) {
        if (i >= uGhostCount) break;
        vec2 sampleUV = flippedUV + ghostVec * float(i);
        float distFromCenter = length(vec2(0.5) - sampleUV);
        float w = pow(max(0.0, 1.0 - 2.0 * distFromCenter), 4.0);
        result += sampleChroma(uBrightTex, sampleUV, caDir) * w;
    }
}

// Halo: 单环径向采样
if (uHaloWidth > 0.0) {
    vec2 haloVec = normalize(centerVec) * uHaloWidth;
    float distFromRing = abs(length(centerVec) - uHaloWidth);
    float w = smoothstep(0.5, 0.0, distFromRing);
    result += sampleChroma(uBrightTex, vUV + haloVec, caDir) * w;
}
```

## API 入口汇总

**LensFlare (21 fn)**:
```
Enable(w, h) / Disable / IsEnabled / IsSupported / Resize(w, h)
SetAutoEnable / GetAutoEnable
SetThreshold / GetThreshold
SetIntensity / GetIntensity
SetGhostCount / GetGhostCount
SetGhostDispersal / GetGhostDispersal
SetHaloWidth / GetHaloWidth
SetChromaticAberration / GetChromaticAberration
SetDistortionEnabled / GetDistortionEnabled
```

## 已知限制

1. **需 Bloom backend 支持**：`SupportsLensFlare = SupportsBloom`（复用 bright/composite shader）
2. **GhostCount 上限硬定 8**：shader 静态 for 循环；改大需重编 shader
3. **Ghost 朝中心反投**：固定算法，无法朝光源主方向（需 camera matrix → 留 Phase E.8 候选）
4. **Direction 偏移不可调**：Halo 总是径向；非径向方向需新 uniform
5. **Legacy 后端 no-op**：`SupportsLensFlare = false`，Lua API 全链路静默
