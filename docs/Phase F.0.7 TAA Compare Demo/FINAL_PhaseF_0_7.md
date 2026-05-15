# Phase F.0.7 TAA Compare Demo — FINAL

> 6A 工作流 · 阶段 6 (Assess) · 项目总结报告
> 关联：`PLAN_PhaseF_0_7.md` / `ACCEPTANCE_PhaseF_0_7.md` / `TODO_PhaseF_0_7.md`

---

## 1. 交付物总览

| 交付物 | 文件 | 行数 |
|--------|------|------|
| Demo 主文件 | `samples/demo_taa_compare/main.lua` | ~480 |
| Demo README | `samples/demo_taa_compare/README.md` | ~75 |
| 6A 文档 | `docs/Phase F.0.7 TAA Compare Demo/{PLAN, ACCEPTANCE, FINAL, TODO}` | ~600 |

**累计**：纯 demo + 文档 ~1155 行；**零代码改动**（无 .h / .cpp / .lua API / shader 修改）

---

## 2. 核心方案

### 2.1 Preset 切换替代真 split-screen

**约束**：TAARenderer 是 namespace 单实例模式，无法支持双 viewport 双实例。

**替代方案**：单一 viewport，按数字键 1-8 切换 8 个 preset；HUD 显示 history stabilization 进度条提示用户等 30 帧再公平对比。

**优势**：
- 渐进式 8 preset（OFF → F.0 → ... → ALL）让用户看到每个 Phase 的边际贡献
- 比 split-screen 双对比更全面（双对比只能看 2 个 preset）
- 实现工作量 2h vs 真 split-screen 6h+

### 2.2 高对比测试场景

| 元素 | 触发的 TAA 弱点 | 验证的 Phase |
|------|---------------|------------|
| 中央旋转金色 cube (HDR 高光) | firefly | F.0.4 anti-flicker |
| 8 根 4cm 薄棒 (1px 厚度) | aliasing | F.0 jitter + reproject |
| 薄棒公转 60°/s | trail / ghosting | F.0.2 YCoCg / F.0.3 variance |
| 多色彩虹薄棒 | 色彩边缘 clip 误判 | F.0.2 YCoCg |
| 静态相机 | 隔离测试运动物体处理 | 全部 Phase |

### 2.3 HUD 设计原则

```
[Preset 5/8] F.0.3 variance clip                              ← 大字突出 preset
Algorithm: mean +/- gamma*sigma (Salvi 2016 / UE5 default)    ← 教学价值
TAA: ON | alpha=0.92 | clip=ON/variance(g=1.0) | jitter=ON    ← 实时参数
Sharpness: 0.5 (sharpen pass) | AntiFlicker: OFF | HalfRes: OFF
History: stabilizing [============-----------] 12/30 frames    ← 防误判!
Keys: 1-8=preset | R=reset history | ESC=exit
Tip: 切 preset 后等 history STABLE 再观察画质 (~30 frames)
```

**关键创新**：history stabilization 进度条 — 不让用户误以为新 preset 画质差（其实是历史 ghosting 还没消散）。

---

## 3. 与现有 demo 的对比

| Demo | 范围 | TAA 控制深度 |
|------|------|-------------|
| `demo_ssr` | 综合 (SSR + Bloom + MotionBlur + TAA + ...) | 仅 Y 键 toggle + J/H/G/X 控制 4 个参数 |
| `demo_hdr` | HDR + Tonemap | TAA 不是主角 |
| **`demo_taa_compare`** (新) | **专攻 TAA preset 对比** | **8 preset 一键切换 + 渐进式叠加** |

→ 三个 demo 互补：用户先用 `demo_ssr` 看综合管线，再用 `demo_taa_compare` 专门研究 TAA。

---

## 4. 渐进式 8 Preset 设计的教学价值

```
Preset 1 (OFF):
  画面: 严重 aliasing + firefly + 1px shimmer
  教训: 无 TAA 时 PBR 渲染的局限

Preset 2 (F.0 base):
  画面: aliasing 消失 + 但有 trail/ghosting
  教训: TAA 基本原理 (jitter + reproject + RGB clip)

Preset 3 (F.0.1 sharpening):
  画面: 边缘锐度回升
  教训: TAA 模糊的代价 + sharpening 弥补

Preset 4 (F.0.2 YCoCg):
  画面: 薄棒交叠区 ghosting 减少
  教训: 色彩空间对 clip 鲁棒性的影响

Preset 5 (F.0.3 variance):
  画面: trail 进一步收紧
  教训: AABB → 概率分布 clip 的 single-outlier 鲁棒性

Preset 6 (F.0.4 anti-flicker):
  画面: 金色 cube 高光 firefly 几乎消失
  教训: HDR luma 加权对 firefly 的压制

Preset 7 (F.0.5 half-res):
  画面: 几乎与 preset 6 等同; HUD 显示 VRAM -75%
  教训: 性能优化 ≠ 画质牺牲 (有 sharpness 弥补)

Preset 8 (ALL):
  画面: 完整管线最佳画质
  教训: sharpness=0.8 弥补 halfRes 模糊; 移动 4K 推荐
```

**这是 ChocoLight TAA 系列的"hello world"教学工具**。

---

## 5. CI 状态 (✅ 全部通过)

| 平台 | 状态 | 备注 |
|------|------|------|
| build-windows | ✅ success | lightc -p 自动覆盖 demo lua |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [25930638608](https://github.com/futzhj/ChocoLightEngine/actions/runs/25930638608)
Commit hash: `a858a29`
Date: `2026-05-15`
Total duration: `9m41s` (17:02:31 → 17:12:12 UTC)

---

## 6. 工程反思

### 做得好的地方

1. **零代码改动**：纯 demo Phase，无 .h / .cpp / Lua API / shader 修改，零回归风险
2. **务实的 split-screen 替代方案**：TAA 单实例约束下，preset 切换 + stabilization 进度条 = 等效对比工具
3. **渐进式 8 preset 设计**：让用户看到每个 Phase 边际贡献，教学价值远超双对比
4. **stabilization 进度条创新**：避免用户切 preset 立即看画面误判（history 还在 alpha 收敛）
5. **场景精心设计**：中央 cube + 8 薄棒 + 黑底，每个元素都触发特定 TAA 弱点（教科书级测试场景）
6. **决策矩阵全自动**：5/5 决策点基于 TAA 单实例约束 + 业界实践，零用户拍板

### 可改进点

1. **不是真 split-screen**：未来 Phase F.0.10 可考虑 TAARenderer 双实例化（~6h）支持真左右对比
2. **桌面看不出 VRAM 节省**：preset 7/8 的 -75% VRAM 仅 4K 显著，1080p 用户感知弱
3. **薄棒 4cm 厚度折衷**：1px 测试在桌面屏看不清，4cm 是显示与测试的权衡
4. **缺 frame-by-frame 截图**：用户需自行录屏对比；留 Phase F.0.11

### 工程经验

1. **demo Phase 工作量评估**：场景搭建占 50% / preset 切换逻辑 20% / HUD 20% / 文档 10%
2. **单实例约束下的 A/B 工具设计**：进度条 / preset 渐进 / 教学文字描述比强行双 viewport 更有效
3. **HUD 信息密度平衡**：6 行 HUD 是上限（preset 名 + 算法 + TAA 参数 × 2 + 进度 + 帮助），更多就 overload
4. **ASCII 硬约束**：ChocoLight bitmap font 限制；γ → g / σ → sigma fallback 是必要的工程妥协

---

## 7. Phase F.0 系列累计 (F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 + F.0.5 + F.0.7)

| Phase | 功能 | Lua API | shader / demo |
|-------|------|---------|--------------|
| F.0 | TAA 主管线 | 13 fn | FS_TAA shader |
| F.0.1 | 4-tap sharpening | +2 (15) | +FS_SHARPEN shader |
| F.0.4 | Karis anti-flicker | +2 (17) | 改 FS_TAA blend |
| F.0.2 | YCoCg AABB clip | +2 (19) | 改 FS_TAA clip |
| F.0.3 | YCoCg variance clip | +2 (21) | 改 FS_TAA clip |
| F.0.5 | Half-res TAA history | +2 (23) | shader 不变 |
| **F.0.7** | **Demo 对比工具** | **+0 (23)** | **新 demo, 零代码改动** |

**Phase F.0 系列**：23 fn Lua API / 2 shader / 1 backend pass / 1 backend 接口扩展 / 3 demos (`demo_ssr` 集成 / `demo_taa_compare` 专攻)

---

## 8. Phase F.0.x 后续路线

### 短期

1. **Phase F.0.6** — 5-tap CAS sharpening（替代 4-tap, AMD FSR2 算法, 4h）
2. **Phase F.0.8** — Motion-adaptive variance γ（基于 velocity 长度动态调整, 3h）

### 中期

3. **Phase F.0.9** — Custom upsampler (bicubic / Lanczos)：替代 bilinear 上采样
4. **Phase F.0.10** — TAARenderer 多实例化 + 真 split-screen demo（6h+）
5. **Phase F.0.11** — Demo 截图 / 录屏功能（3h）

### 长期

6. **Phase F.1** — DLSS-like TAAU (upscale 1/2 → 2× 输出)
7. **Phase F.2** — Bloom + TAA sharp HDR 联动
