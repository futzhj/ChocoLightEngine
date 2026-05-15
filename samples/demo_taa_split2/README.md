# demo_taa_split2 — Phase F.0.10.2 True Physical Split-Screen with Independent TAA

> 真物理 split-screen demo: 同一帧内左半屏 player 1, 右半屏 player 2, 双 TAA instance
> **各自独立 history**, 真正的同帧双 TAA. 与 F.0.10.1 `demo_taa_split` 的 timeline cycle
> 演示**本质不同** — F.0.10.1 切换 active 是时间轴上前后帧切换, F.0.10.2 是同一帧内
> 两半屏并行渲染 + 两半屏并行 TAA.

---

## 核心价值

| 维度 | F.0.10.1 `demo_taa_split` | F.0.10.2 `demo_taa_split2` |
|------|---------------------------|-----------------------------|
| TAA instance 切换方式 | 用户按键 (timeline) | 同帧自动 (per-region) |
| 同帧渲染视角数 | 1 | **2 (左右半屏)** |
| 同帧 TAA process 数 | 1 | **2 (region 限定)** |
| 真分屏视觉效果 | ❌ | ✅ |
| 用到的新 API | 0 | `HDR.SetAutoTAA` / `Gfx.SetViewport` / `TAA.Process(region)` |

---

## 实现要点 (F.0.10.2 三步 API)

```lua
-- 一次性初始化
HDR.SetAutoTAA(false)                     -- 关 EndScene 自动 TAA, 让 demo 手动控时序
local p1 = TAA.CreateInstance()           -- 创建 player 1 TAA instance
local p2 = TAA.CreateInstance()           -- 创建 player 2 TAA instance
TAA.SetActiveInstance(p1); TAA.Enable(W, H)  -- p1 history RT (full size)
TAA.SetActiveInstance(p2); TAA.Enable(W, H)  -- p2 history RT (full size)

-- 每帧
HDR.BeginScene()                          -- 全屏清屏 HDR fbo

-- Player 1 (左半屏)
Gfx.SetViewport(0, 0, W/2, H)
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
Gfx.SetCamera(player1_eye, player1_at)
-- ... draw scene ...
TAA.Process(0, 0, W/2, H)                  -- scissor 限定 p1 history 仅写左半

-- Player 2 (右半屏)
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
Gfx.SetCamera(player2_eye, player2_at)
-- ... draw scene ...
TAA.Process(W/2, 0, W/2, H)                -- scissor 限定 p2 history 仅写右半

Gfx.SetViewport(0, 0, W, H)               -- 复位全屏
HDR.EndScene()                            -- bloom + tonemap 全屏 (auto-TAA 已 off, 不重复 TAA)
```

---

## Player Profile

| Player | TAA Instance | Sharpen | Mode | 演示重点 |
|--------|-------------|---------|------|---------|
| 1 (LEFT) | `p1_id` | 1.2 | RCAS (F.0.12 robust contrast-adaptive) | 强锐化 + noise/edge 保护 |
| 2 (RIGHT) | `p2_id` | 0 | Lanczos 25-tap halfRes (F.0.14) | 高画质上采样 (-55% blur vs bilinear) |

---

## 控制

- **R** : 重置两 instance history (Disable + Enable, 用于 stabilize 重测)
- **ESC** : 退出

---

## 技术约束 (本 demo 不解决的问题, 留 F.0.10.3)

1. **TAA neighborhood 跨 region 边界采样**: F.0.10.2 用 scissor 限制写, 但 shader 邻域采 sceneTex
   会跨边界读取另一半内容 (~1px 锯齿). 完整方案需 shader 加 uvOffset/uvScale uniform, 留 F.0.10.3.

2. **History reproject 跨 region 边界**: 当 prevUV 越过 region 边界时, 读到 stale data. 适用场景:
   静态视角 / 慢速运动. 高速运动可能在边界出现 ghost 残影 (~1-2px).

3. **Bloom / SSR 仍全屏处理**: HDR.EndScene 内的 Bloom / LensDirt / SSR / MotionBlur 仍按全屏跑,
   即两半的 bloom 混合 (在 HDR fbo 上是全屏的). 视觉上 acceptable, 真物理分屏 bloom 留 F.0.10.4.

---

## 与 F.0.10.1 demo_taa_split 的关系

- F.0.10.1: 验证 multi-instance API 自身正确 (创建/销毁/切换/参数独立性)
- F.0.10.2: 验证 multi-instance + region API **同帧组合**, 真分屏

两 demo 都应保留, 配合使用:
- 先跑 demo_taa_split 验证 instance API 基本流程
- 再跑 demo_taa_split2 看真分屏视觉效果

---

## CI / Headless 验证

CI smoke 只跑 `scripts/smoke/*.lua` (TAA 41 fn / HDR 22 fn), demo 不在 smoke 路径.
本 demo 在 headless 模式下自动走 API 探针路径 (验证 HDR.SetAutoTAA round-trip + TAA.Process headless 错误处理),
不开窗口 → 不阻塞 CI.
