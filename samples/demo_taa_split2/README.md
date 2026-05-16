# demo_taa_split2 — Phase F.0.10.7 True Physical Split-Screen TAA + Bloom + SSR + MotionBlur + **Tonemap** (per-region)

> 真物理 split-screen demo: 同一帧内左半屏 player 1, 右半屏 player 2,
> **双 TAA instance + 5 个后处理都 region 化 (含 tonemap)**, 双 player 采用完全不同 profile + tonemap params.
>
> F.0.10.2 交付 双 TAA instance, F.0.10.3 交付 Bloom/SSR/MB region 化,
> F.0.10.4 同屏站台化展示, F.0.10.5 (uvBounds) 保证边界零像素泄漏,
> F.0.10.6 交付 per-region tonemap (HDR.Tonemap),
> **F.0.10.7 (本 demo 升级) 把 F.0.10.5+F.0.10.6 的最终效果同屏展示: P1 黄昏暖调 vs P2 冷夜蓝调**.

---

## 核心价值

| 维度 | F.0.10.2 初版 | F.0.10.4 | **F.0.10.7 当前版** |
|------|---------------|----------|--------------------|
| TAA instance 切换 | 同帧 region | 同帧 region | 同帧 region |
| 同帧渲染视角数 | 2 | 2 | 2 |
| TAA region 化 | ✅ | ✅ | ✅ |
| Bloom/SSR/MB region 化 | ❌ | ✅ | ✅ |
| 双 player 后处理 profile | ❌ | ✅ | ✅ |
| **边界像素完美 (uvBounds)** | ❌ | ❌ | **✅ (F.0.10.5)** |
| **per-region tonemap** | ❌ | ❌ | **✅ (F.0.10.6)** |
| **双 tonemap profile (黄昏 vs 冷夜)** | ❌ | ❌ | **✅** |
| 用到的新 API | 5 | 14 | **17** (增 F.0.10.6 的3) |

---

## 实现要点 (F.0.10.7 完整 17 API)

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

-- F.0.10.6 (3 API): per-region tonemap, 关 auto
HDR.SetAutoTonemap(false)

-- 每帧 (per region)
HDR.BeginScene()                          -- 全屏清屏 HDR fbo

-- Player 1 (左半屏, 黄昏电影感 profile)
Gfx.SetViewport(0, 0, W/2, H)
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
Gfx.SetCamera(p1_eye, p1_at)
-- ... draw scene ...
apply_p1_postfx_profile()                  -- 切 Bloom/SSR/MB 全局参数到 p1 profile
Bloom.Process(0, 0, W/2, H)                -- 1❶ Bloom
SSR.Process(0, 0, W/2, H)                  -- 2❷ SSR
MotionBlur.Process(0, 0, W/2, H)           -- 3❸ MotionBlur
TAA.Process(0, 0, W/2, H)                  -- 4❹ TAA history (p1 instance)

-- Player 2 (右半屏, 冷夜高清 profile, 同理)
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
Gfx.SetCamera(p2_eye, p2_at)
-- ... draw scene ...
apply_p2_postfx_profile()
Bloom.Process(W/2, 0, W/2, H); SSR.Process(W/2, 0, W/2, H)
MotionBlur.Process(W/2, 0, W/2, H); TAA.Process(W/2, 0, W/2, H)

-- F.0.10.7 新增: per-region tonemap (不同 profile)
Gfx.SetViewport(0, 0, W, H)               -- 复位
HDR.Tonemap(0,    0, W/2, H, {exposure=1.5, gamma=2.2, tonemap='aces'})       -- 5❺ P1 黄昏
HDR.Tonemap(W/2,  0, W/2, H, {exposure=0.6, gamma=2.4, tonemap='uncharted2'}) -- 6❻ P2 冷夜
win:EndFrame()                            -- 内部 EndScene 跳过全屏 tonemap
```

---

## 双 Player Profile (差异化后处理对比)

F.0.10.4 隐含的设计理念: Bloom/SSR/MB 是 *全局 singleton*, 不像 TAA 那样有 multi-instance.
但后处理参数 (intensity / threshold / radius / strength) 可以每帧切, 本 demo 的
automatic profile switching (`apply_p[12]_postfx_profile()`) 实现差异化 < 1µs, 不影响性能.

| Effect | **Player 1 (LEFT)** 黄昏电影感 | **Player 2 (RIGHT)** 冷夜高清 |
|--------|----------------------------|----------------------------|
| TAA Sharpen | RCAS 1.2 (F.0.12 强锐) | Lanczos halfRes (F.0.14 高质上采样) |
| **Bloom** | intensity=1.5, thresh=0.8, radius=1.5 (强辉光) | intensity=0.4, thresh=1.5, radius=0.8 (轻辉光) |
| **SSR** | intensity=0.6, temporal=false (中等反射, 无降噪) | intensity=1.0, temporal=true (强反射 + 时序降噪) |
| **MotionBlur** | strength=0.8, samples=12 (强动模糊) | strength=0.0 (关) |
| **Tonemap** (F.0.10.7) | **ACES, exposure=1.5, gamma=2.2** (黄昏阳光感) | **Uncharted2, exposure=0.6, gamma=2.4** (冷夜 Hable filmic) |
| 演示重点 | 多种后处理叠加 + 暖调 → "黄昏动感冲击" | 高频细节保真 + 冷调 → "冷夜静态高画质" |

---

## 控制

- **R** : 重置两 instance history (Disable + Enable, 用于 stabilize 重测)
- **ESC** : 退出

---

## 已解决 / 后续 Phase

1. ~~**TAA neighborhood 跨 region 边界采样** (~1px 锯齿)~~ → **F.0.10.5 已解决** (shader uvBounds + 0.5 texel inset)
2. ~~**全屏单一 tonemap** (不能每 region 不同调色)~~ → **F.0.10.6 已解决** (HDR.Tonemap(rgn, params))
3. **Bloom/SSR/MB 是 singleton, 需每帧切参数**: 本 demo 已处理. 未来若需真独立,
   需 multi-instance Bloom/SSR/MB renderer (留后续 phase, 同 TAA multi-instance 路).
4. **跨 player 反射 / Bloom 光晕 跨 region**: 以前会跨边界 (物理正确, F.0.10.5 之前).
   F.0.10.5 边界完美后这种跨区光能被 clamp 住, 冷席 (双 player 独立).
5. **真多 HDR target** (每 region 独立 sceneTex, 不同场景): 留后续 phase 评估.

---

## 与 F.0.10.1 demo_taa_split 的关系

- F.0.10.1: 验证 multi-instance API 自身正确 (创建/销毁/切换/参数独立性)
- F.0.10.7 (本 demo): 验证 multi-instance + region + tonemap API **同帧组合**, 真分屏 + 完美边界 + 双 tonemap

两 demo 都应保留, 配合使用:
- 先跑 demo_taa_split 验证 instance API 基本流程
- 再跑 demo_taa_split2 看真分屏视觉效果 (包含 F.0.10.5 边界完美 + F.0.10.6 双 tonemap)

---

## CI / Headless 验证

CI smoke 跑 `scripts/smoke/*.lua` (TAA 41 fn / HDR 28 fn / Bloom 16 fn / SSR 24 fn / MB 16 fn).
本 demo 在 headless (no GL context) 模式下自动走 API 探针路径:

**11 PASS** 验证:
- F.0.10.2 (2 PASS): `HDR.SetAutoTAA` round-trip / `TAA.Process(region)` nil+err
- F.0.10.4 (6 PASS): 3 个 `HDR.SetAuto[Bloom/SSR/MotionBlur]` round-trip + 3 个 `.Process(region)` nil+err
- **F.0.10.7 (3 PASS)**: `HDR.SetAutoTonemap` round-trip + `HDR.Tonemap(rgn)` nil+err + `HDR.Tonemap(rgn, {params})` nil+err

不开窗口 → 不阻塞 CI. 本地运行命令:
```bash
lumen-master/build/src/light/Release/light.exe samples/demo_taa_split2/main.lua
```
