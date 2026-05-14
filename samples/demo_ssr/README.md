# demo_ssr — Phase E.9 + E.10 + E.11 + E.12 Screen-Space Reflection (含 Bilateral Blur + Temporal)

ChocoLight Phase E.9 SSR + Phase E.10 SSR Blur + Phase E.11 Bilateral + Phase E.12 Temporal demo，
演示金属反射场景，含可选 Gaussian / Bilateral 模糊 × TAA-style 时序累积降噪。

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
   ├── SSR raw          (Phase E.9 — ray march + Halton-2,3 jitter★)
   ├── SSR Temporal ★    (Phase E.12 — reverse-reproject + neighborhood clip)
   ├── SSR Blur ★       (Phase E.10 — 5-tap separable; Phase E.11 位可选 Bilateral)
   │  └─ mode: BlurEnabled=true × BilateralEnabled 选 Gaussian / Bilateral
   ├── SSR Composite    (additive 入 HDR color)
   ├── LensFlare
   ├── AutoExposure
   ├── Bloom
   └── Tonemap → 显示
```

Phase E.12 插入点详解:
- **SSR raw 中的 jitter** 仅在 TemporalEnabled=true 时启用（8 帧 Halton 循环）
- **SSR Temporal** 输出为下帧的 history 输入, 也是本帧 Blur 的输入
- TemporalEnabled=false 时跳过中间两点, 行为完全等同 Phase E.11

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
| `V`         | 切换 Bilateral on/off  *(Phase E.11)*  | —                   |
| `,` / `.`   | BlurDepthSigma -/+（步长 25） *(E.11)*| [50, 500]           |
| `T`         | 切换 Temporal on/off   *(Phase E.12)*  | —                   |
| `U` / `I`   | TemporalAlpha -/+（步长 0.02） *(E.12)*| [0.5, 0.99]         |
| `N`         | 切换 RejectionMode 0/1 *(Phase E.12)*  | —                   |
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
BilateralEnabled = true Phase E.11 默认开（高质量默认，BlurEnabled=true 时生效）
BlurDepthSigma = 200    Phase E.11　bilateral 深度权重 σ [50, 500]
TemporalEnabled = true  Phase E.12 默认开（TAA-style 业界标准）
TemporalAlpha = 0.9     Phase E.12 history 混合权重 [0.5, 0.99]
RejectionMode = 1       Phase E.12 1=neighborhood AABB clip (默认), 0=current-depth threshold
```

## 性能调优建议

- **高端 PC**：默认 64 步 + Blur on (radius=2.0) + Temporal on，~3.5 ms（1080p）
- **中端 GPU**：32 步 + Blur on (radius=1.5) + Temporal on，~2.5 ms
- **低端 / 移动**：16 步 + Blur off + Temporal off + MaxDistance 调到 20，~1 ms
- **Blur 额外成本**：~0.3 ms（1080p, half-res H+V pass + composite读取）
- **Temporal 额外成本**：~0.2-0.4 ms（1080p, full-screen reproject + 9-tap clip）

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
- **Phase E.11 Bilateral**：仅 depth-aware （未采用 normal-aware）；跨超大深度跨 (>uDepthSigma 上限) 可能权重过低导致突变
- **Phase E.12 Temporal**：仅 reverse-reproject from depth（无 G-buffer velocity）→ **动态物体反射可能 ghost**
  - 静态几何 + 镜头转动：reproject 准确，无 ghost
  - 底下的几何移动（如车辆 / 角色）：反射中会拖影，需 Phase E.13+ 引入 G-buffer velocity
  - 首帧强制输出 cur，避免 1-frame 黑帧
  - Resize 后从头累积，需约 8 帧 (140ms @ 60fps) 收敛主体
- 半透明物体不写 depth/normal，因此不会被 SSR 反射
- 后端不支持 G-buffer MRT 时 silent skip + 首次 warn（不崩溃）

## 相关文档

- Phase E.9 设计文档：`docs/Phase E.9 SSR/`
- Phase E.10 设计文档：`docs/Phase E.10 SSR Blur/`
- Phase E.11 设计文档：`docs/Phase E.11 Bilateral SSR Blur/`
- **Phase E.12 设计文档**：`docs/Phase E.12 Temporal SSR/`
- API 参考：`docs/API_REFERENCE.md` → Light.Graphics.SSR
- smoke 测试：`scripts/smoke/ssr.lua`
