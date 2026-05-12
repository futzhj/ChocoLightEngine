# demo_ssr — Phase E.9 + E.10 Screen-Space Reflection (含 Blur)

ChocoLight Phase E.9 SSR (屏幕空间反射) + Phase E.10 SSR Blur demo，演示金属反射场景，
含可选 half-res Gaussian 模糊。

## 场景

代码生成 3D mesh：

- **plane**：10×10 的灰色地面，朝上法线（反射面）
- **7 个 cube**：红/绿/蓝/黄/青/紫/橙等鲜艳颜色，位于地面之上不同高度

相机以 8 单位半径围绕场景缓慢旋转（0.25 rad/s），俯角 3.5 单位高度，
使地面反射方向能命中周围 cube。

## 渲染管线（Phase E.x 完整后处理链）

```
3D PBR Mesh (写入 HDR color + G-buffer normal RG16F MRT)
   ↓
HDR Pipeline EndScene:
   ├── LensDirt
   ├── Streak
   ├── SSAO              (基于 G-buffer normal)
   ├── SSR raw          (Phase E.9 — ray march 写入 reflection RT)
   ├── SSR Blur ★       (Phase E.10 — 可选 half-res 5-tap Gaussian, H + V)
   ├── SSR Composite    (additive 入 HDR color)
   ├── LensFlare
   ├── AutoExposure
   ├── Bloom
   └── Tonemap → 显示
```

## 操作

| 按键        | 作用                                  | 范围                |
|-------------|---------------------------------------|---------------------|
| `F`         | 切换 SSR on/off                       | —                   |
| `1` / `2`   | MaxSteps -/+（步长 8）                | [8, 128]            |
| `3` / `4`   | StepSize -/+（步长 0.05）             | [0.01, 1.0]         |
| `5` / `6`   | Thickness -/+（步长 0.1）             | [0.01, 5.0]         |
| `7` / `8`   | Intensity -/+（步长 0.1）             | [0.0, 2.0]          |
| `-` / `=`   | MaxDistance -/+（步长 10）            | [1, 1000]           |
| `[` / `]`   | EdgeFade -/+（步长 0.05）             | [0.0, 0.5]          |
| `B`         | 切换 SSR Blur on/off   *(Phase E.10)*  | —                   |
| `9` / `0`   | BlurRadius -/+（步长 0.25） *(E.10)* | [0.5, 4.0]          |
| `R`         | reset 所有参数到默认                  | —                   |
| `ESC`       | 退出                                  | —                   |

## 默认参数（高质量方案）

```
MaxSteps    = 64        ray march 步数
StepSize    = 0.1       每步 view-space 单位
Thickness   = 0.5       depth 命中容差
MaxDistance = 50.0      ray march 距离上限
Intensity   = 0.7       composite 强度
EdgeFade    = 0.1       屏幕边缘 fade 区域宽度
BlurEnabled = false     Phase E.10 默认关（保持向后兼容）
BlurRadius  = 1.5       Phase E.10　Gaussian 半径 [0.5, 4.0]
```

## 性能调优建议

- **高端 PC**：默认 64 步 + Blur on (radius=2.0)，~3.3 ms（1080p）
- **中端 GPU**：32 步 + Blur on (radius=1.5)，~2.3 ms
- **低端 / 移动**：16 步 + Blur off + MaxDistance 调到 20，~1 ms
- **Blur 额外成本**：~0.3 ms（1080p, half-res H+V pass + composite读取）

## 运行

```bash
light samples/demo_ssr/main.lua
```

headless（无窗口/无 GL）下：自动 fallback 到 API probe，
打印参数默认值 + ReflectionTexId(=0)，正常退出。

## 已知限制

- SSR 反射依赖屏幕已渲染的几何体（屏外物体不反射）
- 自反射剔除阈值 `dot(viewN, viewV) < 0.05` 时跳过
- **Phase E.10 Blur**：统一模糊半径（未采用 PBR roughness-aware blur）；half-res 上采样有少量边缘锁步闪烁
- 半透明物体不写 depth/normal，因此不会被 SSR 反射
- 后端不支持 G-buffer MRT 时 silent skip + 首次 warn（不崩溃）

## 相关文档

- Phase E.9 设计文档：`docs/Phase E.9 SSR/`
- **Phase E.10 设计文档**：`docs/Phase E.10 SSR Blur/`
- API 参考：`docs/API_REFERENCE.md` → Light.Graphics.SSR
- smoke 测试：`scripts/smoke/ssr.lua`
