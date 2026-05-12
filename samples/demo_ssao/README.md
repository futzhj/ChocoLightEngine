# demo_ssao — Phase E.8 SSAO (Screen-Space Ambient Occlusion)

3D PBR 场景 + 屏幕空间环境光遮蔽演示。代码生成 cube + plane mesh，旋转相机环视，F 键 toggle SSAO 对比开关效果。

## 运行

```powershell
# 编译后:
./light.exe samples/demo_ssao/main.lua
```

## 控制

| 按键 | 功能 |
|------|------|
| `F` | 切换 SSAO on/off（最重要 — 对比 AO 效果）|
| `1` / `2` | Radius −/+ （步长 0.1, 范围 [0.05, 5.0]）|
| `3` / `4` | Bias −/+ （步长 0.005, 范围 [0, 0.2]）|
| `5` / `6` | Intensity −/+ （步长 0.1, 范围 [0, 4.0]）|
| `7` / `8` | Power −/+ （步长 0.25, 范围 [0.5, 8.0]）|
| `B` | 切换 BlurEnabled（双边滤波 on/off）|
| `K` | 切换 KernelSize（8 / 16, 性能↔质量）|
| `R` | reset 所有参数到默认 |
| `ESC` | 退出 |

## 默认参数

| 参数 | 默认 | 范围 | 含义 |
|------|------|------|------|
| **Radius** | 0.5 | [0.05, 5.0] | 采样半径（view space 单位）|
| **Bias** | 0.025 | [0, 0.2] | 防自遮蔽偏移 |
| **Intensity** | 1.0 | [0, 4.0] | AO 强度乘子 |
| **KernelSize** | 16 | {8, 16} | 采样数 |
| **Power** | 2.0 | [0.5, 8.0] | AO 对比度幂 |
| **BlurEnabled** | true | bool | 双边滤波开关 |

## 期望视觉

- **SSAO OFF**：cube 之间的阴影完全靠 PBR direction light 计算；角落 / 物体接触面有刺眼亮区，缺少自然过渡
- **SSAO ON**：cube 接触地面 / 相邻 cube 之间出现自然柔和阴影；角落变暗有体积感
- **Intensity = 0**：等于 OFF
- **Intensity = 2-4**：AO 过强，整体偏暗
- **BlurEnabled OFF**：AO 有 noise 颗粒感（适合 stylized 风格）
- **BlurEnabled ON**：AO 平滑（标准游戏画面）

## 管线流程

```
Lua mesh:Draw 写 HDR color + depth (renderbuffer)
              ↓
HDR.EndScene:
   Bloom.Process
   AE.Process
   LensDirt.Process
   Streak.Process
   SSAO.Process:
     0. glBlitFramebuffer (HDR depth RB -> SSAO depth tex, 旁路)
     1. DrawSSAO: depth + noise + kernel -> AO raw (R16F, 1/2 res)
     2. DrawSSAOBlur 水平 + 垂直 (depth-aware bilateral, 5-tap)
     3. DrawSSAOComposite: HDR *= mix(1, ao, intensity)
   LensFlare.Process
   Tonemap → backbuffer
```

## API 入口汇总

**Light.Graphics.SSAO (19 fn)**:
```
Enable(w, h) / Disable / IsEnabled / IsSupported / Resize(w, h)
SetAutoEnable / GetAutoEnable
SetRadius / GetRadius
SetBias / GetBias
SetIntensity / GetIntensity
SetKernelSize / GetKernelSize           -- 仅接受 {8, 16}
SetPower / GetPower
SetBlurEnabled / GetBlurEnabled
```

## Phase E.8 双 RT 旁路策略（设计决策）

**用户选择 2026-05-12**：HDR RT 保持不变（`GL_DEPTH_COMPONENT24 renderbuffer`）；SSAO 模块自管独立 `depth texture` + 小 FBO，每帧 `glBlitFramebuffer` 从 HDR FBO 复制 depth 进去。

**优点**：
- HDR RT 代码零改动，所有现有 demo 行为完全不变
- `glBlitFramebuffer` 是 GPU 原生操作，~0.1 ms 全标紧 blit 开销
- 用户 API 完全透明：`SSAO.Enable(w,h)` 一键即用，无需包住 3D 绘制段

## 已知限制

1. **2D 场景 SSAO 无效**：所有像素 z=0，AO 输出全 1（按设计；用户已确认 SSAO 仅 3D 适用）
2. **Normal 重建依赖 ddx/ddy**：极端角度有精度问题；若出现可视化问题考虑 G-buffer normal 路径（留 Phase E.9 候选）
3. **KernelSize 仅 8 / 16**：shader 静态 for 循环；改大需重编 shader
4. **Composite 顺序在 LensFlare 之后**：与"理想 AO 在 Bloom 之前"略不同；对最终效果有轻微影响（可接受）
5. **Legacy 后端 no-op**：`SupportsSSAO() = false`，Lua API 全链路静默
