# Phase F.1 TAAU Demo — 渲染分辨率与输出分辨率解耦

> **Phase**: F.1.0
> **核心特性**: TAAU = TAA + Upsampling (DLSS / FSR2 风格)
> **目标**: 在低分辨率渲染场景, 利用 TAA 时序累积 + 高分辨率 history 上采样, 拿到 "近似 native" 画质 + 30~60% 性能收益

## 启动

将 `Light.dll` / `light.exe` / `lightw.exe` / `lua51.dll` / `lib/` 放入本目录后运行:

```bash
./light.exe main.lua
```

## 操作键位

### TAAU 开关与预设
| 键 | 作用 |
|---|---|
| **Y**  | 切 TAAU (默认 OFF — F.0 行为零回归) |
| **1**  | Performance preset (renderScale = 0.5) |
| **2**  | Balanced preset (renderScale = 0.667) — **推荐** |
| **3**  | Quality preset (renderScale = 0.75) |
| **4**  | Native preset (renderScale = 1.0) — 等同 F.0 |
| **-/=** | 自定义 renderScale ±0.05 (与 preset 自动同步, 容差 0.01 找最近) |

### TAA 基础
| 键 | 作用 |
|---|---|
| **T**  | 切 TAA (基础, 必须 ON 才能见到 TAAU 效果) |
| **J**  | 切 Jitter |
| **H**  | Sharpness 循环 (0 / 0.5 / 1.0 / 1.5 / 2.0) |
| **Z**  | SharpenMode 循环 (unsharp / cas / rcas) |
| **X**  | 切 HalfResHistory (Q5 仲裁: 与 TAAU 互斥, 演示自动关闭逻辑) |
| **B**  | 切 AutoMipBias (Phase F.1.1: 自动 LOD bias 让纹理在低 scale 下保持锐度) |
| **N**  | 切 DRS 总开关 (Phase F.1.4: 帧率自适应, 监控帧时间自动跳档) |
| **F**  | 循环切目标 FPS (Phase F.1.4: 60 → 72 → 120 → 144 → 60) |
| **G**  | 切 DRS 决策源 (Phase F.1.5: prefer GPU/CPU; GPU 时间更精确, CPU 用作 fallback) |
| **E**  | 截图为 OpenEXR (Phase F.0.11.5: half-float HDR + ZIP 压缩, 影视后期工业格式) |
| **M**  | 切 MP4 录屏 (Phase F.0.11.6: H.264 30 fps + 5 Mbps; 需要 lib/ 内有 FFmpeg DLL) |

### 通用
| 键 | 作用 |
|---|---|
| **R**   | reset 全部为默认 (TAAU=off, scale=1.0, sharpness=0.5, mode=unsharp, jitter=on, halfRes=off) |
| **ESC** | 退出 |

## HUD 字段

```
HDR: ON   TAA: ON   Jitter: ON
TAAU: ON   preset=balanced   scale=0.667
Render: 854x480   Output: 1280x720   (ratio 67%)
Sharpen: 0.50   mode=unsharp   HalfResHistory=OFF
Jitter: frame=3  jx=-0.250  jy=0.111 (in render pixel)
```

- **Render**: raster + 后处理 (Bloom/SSAO/SSR/MotionBlur) 工作分辨率
- **Output**: history + sharpen + tonemap 工作分辨率 (== 窗口尺寸)
- **ratio**: render/output × 100, 业界等同 "Quality Mode %"
- **Jitter pixel space**: TAAU ON 时 jitter 单位 = render pixel (NDC offset 自动放大让多帧覆盖更稀疏的 sub-pixel 网格)

## 真机性能预期 (供用户验证)

| Preset | RenderScale | 1080p output GPU 时间 | 视觉评价 |
|---|---|---|---|
| Performance | 0.5 (960×540) | -45%~60% | 明显 ghost, 适合极限低端 |
| Balanced | 0.667 (1280×720) | -30%~40% | 中度 ghost, 大众甜点 |
| Quality | 0.75 (1440×810) | -15%~25% | 轻 ghost, 接近 native |
| Native | 1.0 (1920×1080) | 0% (baseline) | 等同 F.0 TAA |

实际数值因 GPU 而异 (NV / AMD / Intel / Mali / Adreno 分布广)。

## 已知限制 (F.1.0)

1. **仅 default instance (id=0) 支持** — user instance (1..3) 启用 TAAU 会被拒绝 (warning log)
2. **HalfResHistory 互斥** — 启用 TAAU 时自动关闭 HalfResHistory (Q5 仲裁, log warning)
3. **Mipmap LOD bias 不调整** — 留 Phase F.1.1; 在 1.0 scale 以外纹理可能略糊 (FSR2 标准做法 `bias = log2(scale) - 0.7` 留增量)
4. **Velocity sampling bilinear** — render-res velocity → output-res shader, 1-pixel 误差由 history clip 吸收

## Phase F.1.0.1 / F.1.1 进展 (2026-05-17)

- ✅ **F.1.0.1**: 移除单 instance 限制, user instance (1..3) 也能启用 TAAU
- ✅ **F.1.1**: Mipmap LOD bias 自动调整 (`bias = log2(scale) - 0.7`); 默认 ON
  - 0.667 scale → bias = -1.285 (远处纹理保持锐度)
  - 0.5   scale → bias = -1.7
  - 1.0   scale → bias = 0 (零回归, 等同 F.0)

## 关联文档

- [ALIGNMENT_PhaseF_1.md](../../docs/Phase F.1 TAAU/ALIGNMENT_PhaseF_1.md)
- [DESIGN_PhaseF_1.md](../../docs/Phase F.1 TAAU/DESIGN_PhaseF_1.md)
- [CONSENSUS_PhaseF_1.md](../../docs/Phase F.1 TAAU/CONSENSUS_PhaseF_1.md)
