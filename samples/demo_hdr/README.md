# demo_hdr — HDR + ACES Tonemapping 可视对比 demo

**Phase E.3** 配套示例。演示 ChocoLight 引擎的 HDR 离屏渲染管线 + ACES filmic tonemap 对高亮场景的压缩能力。

## 运行

```bash
light.exe samples/demo_hdr/main.lua
```

无需额外资源；所有内容由引擎自带基元绘制。

## 演示内容

- **灰度梯度条**：10 个矩形，亮度从 `0.2` 线性递增到 `3.8`。值 > 1.0 的部分在 LDR 管线下被硬件 clamp 为白色，无法区分；HDR 管线下 ACES 曲线保留高亮细节。
- **彩色梯度条**：3 组 RGB 色调（暖红 / 荧光绿 / 深蓝）分别乘上 10 个亮度系数，演示 ACES 对彩色高亮的压缩。
- **OSD**：屏顶实时显示 HDR 状态 / Exposure / Gamma / Scene texture id / Backend 名。

## 操作

| 键 | 功能 |
|----|------|
| `H` | 切换 HDR 启用 / 禁用 |
| `Z` / `X` | Exposure -/+ 0.1（范围 0.1 ~ 5.0） |
| `C` / `V` | Gamma -/+ 0.1（范围 1.0 ~ 3.0） |
| `R` | 重置 Exposure=1.0, Gamma=2.2 |
| `ESC` | 退出 |

## 预期视觉

- **HDR OFF**：亮度 > 1.0 的 6 个灰块全部饱和为白；彩色块亦同（亮度 > 1.0 后色相信息丢失）。
- **HDR ON**：全部 10 个灰块呈现连续渐变；彩色块在 `exposure≈1.0` 下保持色相，随亮度增加逐渐压缩趋近白色但仍可区分相邻档位。
- **Exposure 下调** 可"找回"饱和区域细节（如从白色 clip 恢复出颜色梯度）。
- **Gamma 提高** 整体画面变暗；降低（接近 1.0）画面变亮。

## 后端支持

| 后端 | HDR 支持 |
|------|----------|
| GL33（OpenGL 3.3 Core） | ✅ RGBA16F RT + ACES shader |
| Legacy（OpenGL 2.x fixed function） | ❌ 无 float RT 支持，`HDR.IsSupported() == false` |

Legacy 后端下 demo 会打印提示并留在 LDR 模式运行。

## 关联文档

- 设计：`docs/Phase E 渲染管线升级/DESIGN_PhaseE_3.md`
- API 设计：`Light.Graphics.HDR.*` 10 个函数（见 `main.lua` 顶部注释）
- 内部实现：`ChocoLight/src/hdr_renderer.cpp` + `ChocoLight/src/render_gl33.cpp` 的 `tonemapFS_GLSL33` shader
