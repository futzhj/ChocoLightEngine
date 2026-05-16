# Phase F.0.10.10 — Quad-Split Linkage Demo FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (lua-only, 零 C++ 改动, 零 API 增量, 零 smoke 改动)

---

## 1. 完成工作

新增 `samples/demo_quad_split/`:
- `main.lua` — 460 LOC callback model
- `README.md` — 用法文档
- `luts/{warm_red,cool_blue}.cube` — 复用自 demo_multi_hdr_pip

闭环 F.0.10.9.x multi-instance 系列:
- F.0.10.2 TAA multi-instance + region API (15h)
- F.0.10.3 Bloom/SSR/MB region API (3h)
- F.0.10.6 per-region tonemap (4h)
- F.0.10.8.x LUT 子生态 (12h, 6 sub-phase)
- F.0.10.9 multi-HDR 主体 (25h)
- F.0.10.9.{1,2,3,x.1} 状态隔离 + LUT 同步 + Window:__call 暴露 + demo migration (8h)
- **F.0.10.10 (本)** — 集大成 demo (~3h)

## 2. 核心创新: 4 HDR × 4 TAA 同帧

```
┌──────────────┬──────────────┐
│  TL  HDR=0   │  TR  HDR=1   │
│  TAA=0       │  TAA=1       │
│  warm + ACES │  cool + Unch │
│  + RCAS      │  + Lanczos   │
├──────────────┼──────────────┤
│  BL  HDR=2   │  BR  HDR=3   │
│  TAA=2       │  TAA=3       │
│  no-LUT      │  cool-tint   │
│  Reinhard    │  ACES + AF   │
└──────────────┴──────────────┘
```

每 quad 独立: HDR fbo (640×360) + Bloom pyramid + dilation RT + TAA history.

## 3. 关键技术验证 (代码层确认)

`HDRRenderer::SetActiveInstance(i)` 切换后:
- `HDR.GetFBO() / GetSceneTexture()` 返回 instance i 的 RT
- `Bloom/SSR/MB.Process(rgn)` 内部用 `GetFBO()+GetSceneTexture()` → 自动作用于 instance i
- `HDR.Tonemap(rgn, params)` 也读 active instance 的 sceneTex

**这是 multi-instance HDR 的核心价值** — 全局单例后处理通过 active instance
间接路由到正确 fbo, 调用方零负担 (不需传 fbo handle).

## 4. 验证

### 4.1 真 GL 模式 (Windows 本地 3 秒启动测试)

```
==== ChocoLight Phase F.0.10.10 Quad-Split Linkage Demo (callback-model) ====
[demo_quad_split] HDR.IsSupported = true
[demo_quad_split] TAA.IsSupported = true
[demo_quad_split] OnOpen: setup 4 HDR + 4 TAA instance, LUTs, postfx
HDRRenderer::Enable: HDR RT created (640x360, fbo=1, tex=2)         [instance 0]
HDRRenderer::CreateInstance: 创建 instance id=1 (count=2)
HDRRenderer::Enable: HDR RT created (640x360, fbo=9, tex=13)        [instance 1]
HDRRenderer::CreateInstance: 创建 instance id=2 (count=3)
HDRRenderer::Enable: HDR RT created (640x360, fbo=12, tex=19)       [instance 2]
HDRRenderer::CreateInstance: 创建 instance id=3 (count=4)
HDRRenderer::Enable: HDR RT created (640x360, fbo=15, tex=25)       [instance 3]
[demo_quad_split] 4 HDR instance ready: ids=[0, 1, 2, 3], each 640x360
[demo_quad_split] LUT loaded: warm=31, cool=32
TAARenderer::CreateInstance: 创建 instance id=1 (count=2)
TAARenderer::CreateInstance: 创建 instance id=2 (count=3)
TAARenderer::CreateInstance: 创建 instance id=3 (count=4)
TAARenderer::Enable: TAA history RT created (640x360, RGBA16F × 2)  [instance 0]
TAARenderer::Enable: TAA history RT created (640x360, RGBA16F × 2)  [instance 1]
TAARenderer::SetHalfResHistory: ON, history RT = 320x180 (TR Lanczos)
TAARenderer::Enable: TAA history RT created (640x360, RGBA16F × 2)  [instance 2]
TAARenderer::Enable: TAA history RT created (640x360, RGBA16F × 2)  [instance 3]
[demo_quad_split] 4 TAA instance ready: ids=[0, 1, 2, 3]
[demo_quad_split] OnOpen: setup ok, entering render loop
Window opened: 1280x720 'Phase F.0.10.10 - Quad-Split Linkage Demo (4 HDR x 4 TAA)'
```

✅ 4 HDR instance + 4 TAA instance + Bloom + SSR + MB 全部 Enable
✅ 进入 render loop 无 Lua / GL error
✅ MAX_INSTANCES = 4 上限正好用满

### 4.2 8 smoke 零回归

```
PASS hdr  ·  PASS bloom  ·  PASS ssr  ·  PASS auto_exposure
PASS lens_fx  ·  PASS motion_blur  ·  PASS taa  ·  PASS lighting2d
```

### 4.3 Headless API probe (CI 兼容)

无 GL ctx 时跑 6 个探针, 全 PASS:
1. HDR multi-instance Create/Destroy round-trip
2. TAA multi-instance Create/Destroy round-trip
3. HDR.BeginScene/EndScene headless silent no-op
4. HDR.Tonemap headless 退化
5. Bloom/SSR/MB.Process headless 退化
6. MAX_INSTANCES = 4 enforced (4th create fail)

## 5. VRAM 预算 (1280×720, 4 quad × 640×360)

| 资源 | 单 instance | × 4 instance | 备注 |
|------|------------|--------------|------|
| HDR sceneTex (RGBA16F) | 1.85 MB | 7.4 MB | 4× |
| Bloom pyramid (5 level) | ~2 MB | 2 MB | 全局单例 (640×360) |
| Velocity dilation (RG16F × 2) | 0.93 MB | 3.7 MB | 4× per HDR instance |
| TAA history (RGBA16F × 2) | 3.7 MB | 14.8 MB | 4× (TR halfRes 减 75% → 实际 ~12 MB) |
| MotionBlur RT | 1.85 MB | 1.85 MB | 全局单例 |
| **总计** | ~10 MB | **~30 MB** | 与 demo_taa_split2 (单 instance 1280×540 ~25 MB) 同量级 |

## 6. 控制

- **1 / 2 / 3 / 4** : 重置对应 quad 的 TAA history (Disable + Enable + re-apply profile)
- **L** : 全局 LUT toggle (TL warm / TR / BR cool 一起切)
- **ESC** : 退出

## 7. 文件变更

| 文件 | 变更 |
|------|------|
| `samples/demo_quad_split/main.lua` | 新建 (460 LOC) |
| `samples/demo_quad_split/README.md` | 新建 (~120 行) |
| `samples/demo_quad_split/luts/warm_red.cube` | 复制自 demo_multi_hdr_pip |
| `samples/demo_quad_split/luts/cool_blue.cube` | 复制自 demo_multi_hdr_pip |
| `docs/Phase F.0.10.10 quad-split linkage demo/PLAN_PhaseF_0_10_10.md` | 新建 |
| `docs/Phase F.0.10.10 quad-split linkage demo/FINAL_PhaseF_0_10_10.md` | 本文 |
| `docs/Phase F.0.10.10 quad-split linkage demo/TODO_PhaseF_0_10_10.md` | 后续接力 |

**零 C++ 改动 / 零 Lua API 增量 / 零 smoke 改动**.

## 8. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN §1 任务范围 + 成功标准 |
| **Architect** | PLAN §2.1 代码层验证 + §2.2 帧流程 + §2.3 4 profile 表 |
| **Atomize** | PLAN §3 任务清单 |
| **Approve** | 用户隐式确认 (ask_user_question) |
| **Automate** | demo 460 LOC 实现 (~3h) |
| **Assess** | 本 FINAL + 真 GL 启动 + 8 smoke + headless probe |

## 9. F.0.10.x 系列里程碑

| Phase | 功能 | LOC | Lua API |
|-------|------|-----|---------|
| F.0.10.0 | TAA + Bloom/SSR region API | ~6h | +14 |
| F.0.10.1 | TAA multi-instance + split demo | ~5h | +5 |
| F.0.10.2 | TAA region API | ~3h | +1 |
| F.0.10.3 | Bloom/SSR/MB region API | ~3h | +6 |
| F.0.10.4 | postfx region 化 | ~2h | +0 |
| F.0.10.5 | uvBounds (split-screen 边界) | ~2h | +0 |
| F.0.10.6 | per-region tonemap | ~4h | +5 |
| F.0.10.7 | demo_taa_split2 | ~4h | +0 |
| F.0.10.8.{0..6} | LUT 子生态 | ~12h | +12 |
| F.0.10.9.{0..3,x.1} | multi-HDR + 状态隔离 + demo migration | ~32h | +5 |
| **F.0.10.10 (本)** | **集大成 quad-split demo** | **~3h** | **+0** |
| **总计** | **multi-instance + region 完整链** | **~76h** | **+48 (32→80)** |

## 10. 下一步候选

- F.0.10.9.x.2 Bloom/SSR/MB pyramid 多 instance (低优, ~6h)
- F.0.10.9.x.3 GetState/Clone 易用性 (中优, ~1.5h)
- F.0.11 demo 截图/录屏 (高优, ~3h, 可对比 4 quad 视觉差异)
- F.1 TAAU DLSS-like (~10-15h, 大版本里程碑)
