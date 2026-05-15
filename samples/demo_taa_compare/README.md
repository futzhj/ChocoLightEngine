# demo_taa_compare — Phase F.0.7 TAA Compare Demo

一键切换 8 个 TAA 预设配置，直观对比 6 个 Phase 的画质差异。

## 启动

```pwsh
cd samples\demo_taa_compare
..\..\Light\light.exe main.lua
```

## 8 个预设（数字键 1-8）

| Key | Preset 名 | 启用特性 | 算法说明 |
|-----|-----------|---------|---------|
| **1** | TAA OFF (baseline) | 无 TAA | 看原始 aliasing / firefly / 1px shimmer |
| **2** | F.0 base | jitter + reproject + RGB AABB | Phase F.0 原始管线 |
| **3** | F.0.1 sharpening | + sharpness=0.5 | 4-tap unsharp mask |
| **4** | F.0.2 YCoCg clip | + YCoCg AABB | 色彩边缘鲁棒 |
| **5** | F.0.3 variance | + variance(γ=1.0) | mean ± γσ (Salvi 2016 / UE5) |
| **6** | F.0.4 anti-flicker | + Karis luma | 压制 HDR firefly |
| **7** | F.0.5 half-res | + history (W/2, H/2) | VRAM -75% / TAA pass -75% pixels |
| **8** | ALL (推荐) | sharpness=0.8 + 全开 | 移动 4K 推荐配置 |

**渐进式叠加**：每个 preset 在前一个基础上加一个特性，让你清晰看到每个 Phase 的边际贡献。

## 控制键

| 键 | 功能 |
|----|------|
| **1-8** | 切换 8 个 preset |
| **R** | 重置 history（Disable + Enable，重新 stabilize） |
| **ESC** | 退出 |

## 场景设计

- **中央旋转金色 cube**（HDR 高光，自转 30°/s）→ 触发 firefly + ghosting
- **8 根围绕薄棒**（4cm 厚度 × 1.2m 高，公转 60°/s，彩虹色）→ 触发 1px aliasing + trail
- **黑色地面**（10×10 plane）→ 反衬 HDR 高光

## 推荐观察顺序

1. **启动 → preset 1 (OFF)**：观察金色 cube 边缘 + 薄棒的 raw aliasing 与 firefly
2. **切到 preset 2 (F.0 base)**：jitter 立即消除大部分 aliasing；但 trail / ghosting 明显
3. **切到 preset 3 (F.0.1 sharpening)**：边缘锐度回升（弥补 TAA 模糊）
4. **切到 preset 4 (F.0.2 YCoCg)**：薄棒交叠区 ghosting 减少
5. **切到 preset 5 (F.0.3 variance)**：trail 进一步收紧（clip 更严）
6. **切到 preset 6 (F.0.4 anti-flicker)**：金色 cube 高光 firefly 几乎消除
7. **切到 preset 7 (F.0.5 half-res)**：肉眼几乎不可辨；HUD 显示 VRAM -75%
8. **切到 preset 8 (ALL)**：完整管线，sharpness=0.8 弥补 half-res 模糊

## HUD 字段说明

```
[Preset 5/8] F.0.3 variance clip               ← 当前 preset 索引 + 名
Algorithm: mean +/- gamma*sigma (...)          ← 算法说明
TAA: ON | alpha=0.92 | clip=ON/variance(g=1.0) | jitter=ON
Sharpness: 0.5 (sharpen pass) | AntiFlicker: OFF | HalfRes: OFF
History: stabilizing [============------------------] 12/30 frames | TAA frame=4521
Keys: 1-8=preset | R=reset history | ESC=exit
Tip: 切 preset 后等 history STABLE 再观察画质 (~30 frames)
```

**注意**：TAA 是连续帧累积（alpha=0.92 收敛 ~30 帧），切 preset 后 HUD 显示 stabilization 进度条。完全稳定（30/30）后再观察画质对比才公平。

## 技术说明

- 单一 TAARenderer 全局实例（不支持双 viewport 双实例对比）
- preset 切换：Disable + Enable（preset 1 → enable preset N）或仅参数变更
- history 在 preset 切换时被自然替换（alpha 收敛），无需手动 clear
- HUD 用 `win:DrawText` 显示，ASCII-only（γ → "g" 兼容字体限制）

## 与 demo_ssr 的区别

- demo_ssr：综合 demo，TAA 只是 SSR 的 partner，无法专门切 TAA preset
- demo_taa_compare：专攻 TAA，1 键切 preset 直观对比 6 个 Phase

## 故障排查

| 现象 | 原因 / 处理 |
|------|------------|
| `[demo_taa_compare] need HDR + TAA subtables` | Phase E.3 + F.0 未启用，检查构建 |
| `Mesh.New not available` | Gfx.Mesh 模块缺失，检查 graphics.cpp 注册 |
| 切 preset 后画质看起来一样 | 等 history STABLE（HUD 显示 30/30）再观察 |
| HUD 不显示 | `win.DrawText` 为 nil，检查 Window OOP 接口版本 |
