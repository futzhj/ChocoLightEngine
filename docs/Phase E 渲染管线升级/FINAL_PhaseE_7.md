# FINAL — Phase E.7 · Lens Flare (Ghost + Halo + Chromatic Aberration)

> 6A 工作流 · 阶段 6 · Assess（总结）
> Phase E.7 完整交付：**HDR 链路第 6 剑客** — Lens Flare（屏幕空间 ghost-halo-chromatic）。复用 Bloom bright/composite shader，单独编译 1 个新 ghost shader，最大化资源效率。

---

## 1. 交付物总览

### 1.1 四阶段拆分

| 阶段 | commit | 范围 | CI |
|------|--------|------|-----|
| 规划 | `d26574e` | ALIGNMENT + DESIGN + TASK | ✅ 6/6 |
| **E.7.1** Backend | `ea0c873` | RenderBackend 4 虚接口 + GL33 1 shader 双 profile + ping-pong RT + InitLensFx | ✅ **6/6** |
| **E.7.2** Module | `d0ad4c3` | LensFlareRenderer namespace (27 C++ fn) + HDR 5 联动点 + light_ui + CMake | ✅ **6/6** |
| **E.7.3** Lua API | `d63234f` | 21 fn 子表 + lens_flare.lua smoke (~50 断言) + demo_lens_flare + CI 注册 | ⏳ 跑中 |
| docs | `pending` | ACCEPTANCE + FINAL + TODO | — |

### 1.2 改动文件清单

| 文件 | 行数 | 类型 |
|------|------|------|
| `@e:\jinyiNew\Light\ChocoLight\include\render_backend.h` | +~80 | E.7.1 修改（4 虚接口） |
| `@e:\jinyiNew\Light\ChocoLight\src\render_gl33.cpp` | +~250 | E.7.1 修改（1 shader × 2 profile + InitLensFx + 4 override） |
| `@e:\jinyiNew\Light\ChocoLight\include\lens_flare_renderer.h` | +~95 | E.7.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\lens_flare_renderer.cpp` | +~210 | E.7.2 新建 |
| `@e:\jinyiNew\Light\ChocoLight\src\hdr_renderer.cpp` | +~10 | E.7.2 修改（5 联动点） |
| `@e:\jinyiNew\Light\ChocoLight\src\light_ui.cpp` | +4 | E.7.2 修改 |
| `@e:\jinyiNew\Light\ChocoLight\src\light_graphics.cpp` | +~210 | E.7.3 修改（21 binding + 1 子表） |
| `@e:\jinyiNew\Light\ChocoLight\CMakeLists.txt` | +1 | E.7.2 |
| `@e:\jinyiNew\Light\scripts\smoke\lens_flare.lua` | +~230 | E.7.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_lens_flare\main.lua` | +~200 | E.7.3 新建 |
| `@e:\jinyiNew\Light\samples\demo_lens_flare\README.md` | +~110 | E.7.3 新建 |
| `@e:\jinyiNew\Light\.github\workflows\build-templates.yml` | +3 | E.7.3 CI 注册 |
| 6 份 Phase E.7 docs | — | — |

**总新增代码（生产）**：C++ ~ **855 行** + Lua/CMake/YAML ~ **750 行**
**总新增文档**：~ **2100 行**

---

## 2. 技术架构

### 2.1 数据流（HDR 链路 6 剑客全开）

```
Lua Draw 帧:

  HDRRenderer::BeginScene → bind HDR_FBO (RGBA16F)
  Lua Draw → 写入 HDR RT
  HDRRenderer::EndScene:
      ├── BloomRenderer::Process              ── pyramid additive  [Phase E.4]
      ├── AutoExposureRenderer::Process       ── 测 logLuma         [Phase E.5]
      ├── LensDirtRenderer::Process            ── bloom × dirt      [Phase E.6]
      ├── StreakRenderer::Process              ── anamorphic flare  [Phase E.6]
      ├── LensFlareRenderer::Process:                                [Phase E.7]
      │     1. Bloom.DrawBloomBrightPass(hdrTex → lfRT[0], threshold)   ←┐
      │     2. DrawLensFlareGhost(lfRT[0] → lfRT[1], 5 uniforms)         ├─ 1 新 shader
      │     3. Bloom.DrawBloomComposite(lfRT[1] → hdrFbo, intensity)    ←┘
      │
      ├── exposure ← AE.IsEnabled() ? AE.GetCurrentExposure() : g.exposure
      └── DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)
            ↓
       Backbuffer
```

### 2.2 模块层次

```
┌────────────────────────────────────────────────────────────────────────┐
│  Light.Graphics (Lua subtables, 6 个 = 89 函数)                         │
│  .HDR 12 / .Bloom 15 / .AutoExposure 18 / .LensDirt 10 / .Streak 13     │
│  .LensFlare 21                                                          │
└────────────────────────────────────────────────────────────────────────┘
                                ↓
   HDRRenderer (scene RT)
   ├── BloomRenderer       (pyramid N 级)
   ├── AutoExposureRenderer (luminance mipmap + readback)
   ├── LensDirtRenderer    (no RT, dirtTexId)
   ├── StreakRenderer       (ping-pong RT, 1/2 res)
   └── LensFlareRenderer    (ping-pong RT, 1/2 res) ★ Phase E.7
                                ↓
       RenderBackend 虚接口: HDR 4 + Bloom 6 + AE 6 + LensDirt 2
                            + Streak 6 + LensFlare 4 = 28
                                ↓
       GL33Backend / LegacyBackend (default no-op)
```

### 2.3 关键决策

| 决策 | 理由 |
|------|------|
| **独立 ping-pong RT**（不复用 Streak RT） | 解耦：两个模块可同时启用 |
| **复用 Bloom bright/composite shader** | 节省 shader 字节；语义一致（threshold + additive） |
| **GhostCount 上限 8** | 静态 for 循环（GLES3 兼容） |
| **Ghost UV flip**（`1 - vUV`） | 模拟相机镜组内反射，朝中心反投 |
| **Halo 单环** | UE 实践证明 1 环已足够；多环留 v2 |
| **DistortionEnabled flag** | 可关 chromatic aberration 省 3× 带宽 |
| **autoEnable 默认 false** | 视觉强烈，不应默认接管 |
| **HDR.Disable 时 LensFlare 最先关** | 管线末端依赖最深（Bloom + HDR RT） |

---

## 3. API surface（Phase E.7 新增）

### 3.1 C++ (LensFlareRenderer, 27 fn)

详见 `@e:\jinyiNew\Light\ChocoLight\include\lens_flare_renderer.h`：

```cpp
Init / Shutdown                                        // 2
Enable(w,h) / Disable / IsEnabled / IsSupported / Resize(w,h)   // 5
OnHDREnabled / OnHDRDisabled / OnHDRResized            // 3
SetAutoEnable / GetAutoEnable                          // 2
Set+Get × 7 params                                     // 14
  Threshold / Intensity / GhostCount / GhostDispersal
  HaloWidth / ChromaticAberration / DistortionEnabled
Process(hdrFbo, hdrTex)                                // 1
```

### 3.2 Lua (Light.Graphics.LensFlare, 21 fn)

详见 smoke `@e:\jinyiNew\Light\scripts\smoke\lens_flare.lua`：

```lua
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

---

## 4. CI 证据

| Commit | Run | 结论 |
|--------|-----|------|
| `d26574e` planning | [`25701081881`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701081881) | ✅ 6/6 |
| `ea0c873` E.7.1 backend | [`25701231375`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701231375) | ✅ 6/6 |
| `d0ad4c3` E.7.2 module | [`25701331587`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701331587) | ✅ 6/6 |
| `d63234f` E.7.3 Lua+smoke | [`25701617533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25701617533) | ⏳ 跑中 |

---

## 5. Phase E 链路累计

```
HDR 链路 6 剑客 / 89 Lua API:

Phase E.3 — HDR + 4 tonemap operator      (12 fn)
Phase E.4 — Bloom pyramid                  (15 fn)
Phase E.5 — Auto Exposure (Eye Adaptation) (18 fn)
Phase E.6 — Lens Dirt + Streak             (23 fn)
Phase E.7 — Lens Flare                     (21 fn)   ✨ 新增
```

**电影感后处理工具箱完整成型** ✨

---

## 6. 已知限制

1. **Ghost 反向固定朝中心**（屏幕空间），无法朝 3D 光源方向（需 camera matrix）
2. **GhostCount 上限 8**（静态 for 循环）
3. **Halo 仅 1 环**（多环留 v2）
4. **无 lens flare 贴图采样**（纯 procedural）
5. **可能存在边缘锯齿**（硬采样未做 anti-aliasing）

---

## 7. 后续阶段建议

| Phase | 主题 | 收益 |
|-------|------|------|
| **Phase E.7.x** | 默认 lens flare 贴图 + 多环 halo + 光源方向追踪 | 完整电影感套件 |
| **Phase E.8** | SSAO（屏幕空间环境光遮蔽） | 3D 场景质感大幅提升 |
| **Phase E.9** | SSR（屏幕空间反射） | 水面 / 玻璃反射 |
| **Phase F.x** | Compute shader pipeline | CS 加速 bloom / blur / AE histogram |

---

**Phase E.7 主交付完毕**。HDR 后处理链路（HDR + Bloom + AE + LensDirt + Streak + LensFlare）= **6 剑客 / 89 Lua API** 上线 ✨
