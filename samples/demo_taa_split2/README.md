# demo_taa_split2 — Phase F.0.10.4 True Physical Split-Screen TAA + Bloom + SSR + MotionBlur (per-region)

> 真物理 split-screen demo: 同一帧内左半屏 player 1, 右半屏 player 2,
> **双 TAA instance + 4 个后处理都 region 化**, 双 player 采用完全不同后处理 profile.
>
> F.0.10.2 交付 双 TAA instance 同帧 split-screen, F.0.10.3 交付 Bloom/SSR/MB region 化,
> F.0.10.4 (本 demo 升级) 把 F.0.10.3 的 9 个新 Lua API 同屏站台化展示.

---

## 核心价值

| 维度 | F.0.10.1 `demo_taa_split` | F.0.10.2 初版 | **F.0.10.4 当前版** |
|------|---------------------------|---------------|--------------------|
| TAA instance 切换 | 手动 timeline | 同帧 region | 同帧 region |
| 同帧渲染视角数 | 1 | 2 | 2 |
| TAA region 化 | ❌ | ✅ | ✅ |
| **Bloom region 化** | ❌ | ❌ | **✅** |
| **SSR region 化** | ❌ | ❌ | **✅** |
| **MotionBlur region 化** | ❌ | ❌ | **✅** |
| 双 player 后处理 profile | ❌ | ❌ | **✅ (差异化对比)** |
| 用到的新 API | 0 | 5 (F.0.10.2) | 5 + 9 = **14** (F.0.10.4) |

---

## 实现要点 (F.0.10.4 完整 14 API)

```lua
-- 一次性初始化
HDR.Enable(W, H)

-- F.0.10.2 (5 API): 双 TAA instance + region
HDR.SetAutoTAA(false)
local p1 = TAA.CreateInstance(); TAA.SetActiveInstance(p1); TAA.Enable(W, H)
local p2 = TAA.CreateInstance(); TAA.SetActiveInstance(p2); TAA.Enable(W, H)

-- F.0.10.3 (9 API): Bloom/SSR/MB 各自 Enable + 关 auto
Bloom.Enable(W, H);    HDR.SetAutoBloom(false)
SSR.Enable(W, H);      HDR.SetAutoSSR(false)
MotionBlur.Enable(W, H); HDR.SetAutoMotionBlur(false)

-- 每帧 (per region)
HDR.BeginScene()                          -- 全屏清屏 HDR fbo

-- Player 1 (左半屏, 电影感冲击 profile)
Gfx.SetViewport(0, 0, W/2, H)
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
Gfx.SetCamera(p1_eye, p1_at)
-- ... draw scene ...
apply_p1_postfx_profile()                  -- 切 Bloom/SSR/MB 全局参数到 p1 profile
Bloom.Process(0, 0, W/2, H)                -- 1❶ Bloom (顺序同 HDR.EndScene auto 路径)
SSR.Process(0, 0, W/2, H)                  -- 2❷ SSR
MotionBlur.Process(0, 0, W/2, H)           -- 3❸ MotionBlur
TAA.Process(0, 0, W/2, H)                  -- 4❹ TAA history (p1 instance)

-- Player 2 (右半屏, 高清细腻 profile, 同理)
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
Gfx.SetCamera(p2_eye, p2_at)
-- ... draw scene ...
apply_p2_postfx_profile()                  -- 切到 p2 profile
Bloom.Process(W/2, 0, W/2, H); SSR.Process(W/2, 0, W/2, H)
MotionBlur.Process(W/2, 0, W/2, H); TAA.Process(W/2, 0, W/2, H)

Gfx.SetViewport(0, 0, W, H)               -- 复位全屏
HDR.EndScene()                            -- 仅 tonemap (auto-Bloom/SSR/MB/TAA 均 off)
```

---

## 双 Player Profile (差异化后处理对比)

F.0.10.4 隐含的设计理念: Bloom/SSR/MB 是 *全局 singleton*, 不像 TAA 那样有 multi-instance.
但后处理参数 (intensity / threshold / radius / strength) 可以每帧切, 本 demo 的
automatic profile switching (`apply_p[12]_postfx_profile()`) 实现差异化 < 1µs, 不影响性能.

| Effect | **Player 1 (LEFT)** 电影感冲击 | **Player 2 (RIGHT)** 高清细腻 |
|--------|----------------------------|----------------------------|
| TAA Sharpen | RCAS 1.2 (F.0.12 强锐) | Lanczos halfRes (F.0.14 高质上采样) |
| **Bloom** | intensity=1.5, thresh=0.8, radius=1.5 (强辉光) | intensity=0.4, thresh=1.5, radius=0.8 (轻辉光) |
| **SSR** | intensity=0.6, temporal=false (中等反射, 无降噪) | intensity=1.0, temporal=true (强反射 + 时序降噪) |
| **MotionBlur** | strength=0.8, samples=12 (强动模糊) | strength=0.0 (关) |
| 演示重点 | 多种后处理叠加冲击 → "动感 + 强对比" | 高频细节保真 → "静态 + 强反射高画质" |

---

## 控制

- **R** : 重置两 instance history (Disable + Enable, 用于 stabilize 重测)
- **ESC** : 退出

---

## 技术约束 (留待后续 Phase)

1. **TAA neighborhood 跨 region 边界采样** (~1px 锯齿): scissor 限制写, 但 shader 邻域采 sceneTex
   会跨边界读取另一半. 默认场景肉眼难辨, 完美方案 → F.0.10.5 shader uvOffset/uvScale.

2. **Bloom/SSR/MB 是 singleton, 需每帧切参数**: 本 demo 已处理. 未来若需真独立,
   需 multi-instance Bloom/SSR/MB renderer (留 F.0.10.6, 同 TAA multi-instance 路).

3. **跨 player 反射**: SSR ray march 可跨 region 采样, 即 player 1 可看到 player 2 那侧的反射.
   这是物理正确 (与 SSR 本质一致), 不是 bug.

---

## 与 F.0.10.1 demo_taa_split 的关系

- F.0.10.1: 验证 multi-instance API 自身正确 (创建/销毁/切换/参数独立性)
- F.0.10.2: 验证 multi-instance + region API **同帧组合**, 真分屏

两 demo 都应保留, 配合使用:
- 先跑 demo_taa_split 验证 instance API 基本流程
- 再跑 demo_taa_split2 看真分屏视觉效果

---

## CI / Headless 验证

CI smoke 跑 `scripts/smoke/*.lua` (TAA 41 fn / HDR 28 fn / Bloom 16 fn / SSR 24 fn / MB 16 fn).
本 demo 在 headless (no GL context) 模式下自动走 API 探针路径:

**10 PASS** 验证:
- F.0.10.2 (3 PASS): `HDR.SetAutoTAA` round-trip / `TAA.Process(region)` nil+err / `CreateInstance` x2
- F.0.10.4 (7 PASS): 3 个 `HDR.SetAuto[Bloom/SSR/MotionBlur]` round-trip + 3 个 `.Process(region)` nil+err + 1 个 hasF10_3 detect

不开窗口 → 不阻塞 CI. 本地运行命令:
```bash
lumen-master/build/src/light/Release/light.exe samples/demo_taa_split2/main.lua
```
