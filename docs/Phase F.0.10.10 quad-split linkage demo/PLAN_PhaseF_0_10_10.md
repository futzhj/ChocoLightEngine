# Phase F.0.10.10 — Quad-Split Linkage Demo PLAN

> 6A · ALIGN + ARCHITECT + ATOMIZE
> 目标: 真物理 4-screen split-screen, 4 HDR × 4 TAA instance, 集 F.0.10.9.x 之大成

## 1. 对齐 (ALIGN)

### 1.1 任务范围
- 新增 `samples/demo_quad_split/main.lua` (~450 LOC, callback model)
- 复用现有 LUT 文件 (warm_red.cube + cool_blue.cube), 不新增资产
- **零 C++ 改动, 零 Lua API 增量** (仍 80 fn)
- 闭环 F.0.10.9.x multi-instance 系列, 视觉对比清晰

### 1.2 成功标准
- 真 GL 模式 4 HDR + 4 TAA instance 同时存在并各自渲染
- 4 quad 视觉差异极明显 (LUT × exposure × tonemap × sharpen 全独立)
- TAA history 切 quad 不污染 (与 demo_taa_compare 切 preset 重置 history 不同)
- Headless API probe 在 CI 上跑通
- 8 smoke 零回归

## 2. 架构 (ARCHITECT)

### 2.1 关键技术验证 (代码层确认)

**HDR multi-instance** (`hdr_renderer.cpp:116-122`):
- `static State g_states[MAX_INSTANCES = 4];`
- `#define g g_states[g_active]` 让所有 namespace fn 自动作用于 active instance
- `SetActiveInstance(i)` → 后续 `HDR.Tonemap` / `HDR.SetGradingLUT` / `HDR.SetExposure` 等都作用于 instance i

**Bloom/SSR/MB Process(rgn) 内部读 active HDR fbo+sceneTex** (`light_graphics.cpp:2566-2575, 3548-3559, 3751-3762`):
```cpp
const uint32_t fbo = HDRRenderer::GetFBO();         // 来自 active instance
const uint32_t tex = HDRRenderer::GetSceneTexture(); // 来自 active instance
BloomRenderer::Process(fbo, tex, rgnX, rgnY, rgnW, rgnH);
```

**这就是 F.0.10.10 demo 的核心设计支点** — 切 active HDR instance 后, 全局
单例的 Bloom/SSR/MB 自动作用于该 instance 的 fbo, 无需额外参数.

**TAA multi-instance**: 与 HDR 同模式, `TAA.SetActiveInstance(i)` 切 history RT.

**MAX_INSTANCES = 4** 正好够 4-quad demo (default 0 + 3 user).

### 2.2 帧流程

```
Window:__call (auto BeginScene(0))   [active=0 自动 bind, 但下面会 SetActive 切走]

Demo:Draw()
├─ for i = 0..3:                     [4 quad scene 渲染]
│  ├─ apply_postfx_profile(i)        [Bloom/SSR/MB 全局参数 (per-frame switch)]
│  ├─ HDR.SetActiveInstance(hdr_ids[i])
│  ├─ HDR.BeginScene()               [bind instance i 的 fbo + clear]
│  ├─ Gfx.SetViewport(0, 0, 640, 360)
│  ├─ SetCamera(camera_i); drawScene()
│  ├─ Bloom.Process(0, 0, 640, 360)  [作用于 instance i 的 fbo]
│  ├─ SSR.Process(0, 0, 640, 360)
│  ├─ MB.Process(0, 0, 640, 360)
│  ├─ TAA.SetActiveInstance(taa_ids[i])
│  ├─ TAA.ApplyJitter(); TAA.Process(0, 0, 640, 360)  [写到 instance i 的 sceneTex]
│  └─ HDR.EndScene()                 [unbind]
│
├─ Gfx.SetViewport(0, 0, 1280, 720)  [复位全屏 viewport]
├─ for i = 0..3:                     [Tonemap pass]
│  ├─ HDR.SetActiveInstance(hdr_ids[i])
│  └─ HDR.Tonemap(quad_x, quad_y, 640, 360, params_i)
│                                     [instance i 的 sceneTex → default fb 的 quad i]
├─ HDR.SetActiveInstance(0); TAA.SetActiveInstance(0)
└─ HUD on default fb

Window:__call (auto EndScene(0))     [active=0 已 unbind, 残留 SSAO/AE/LensFx no-op]
```

### 2.3 4 quad profile (差异显著)

| Quad | 位置 | LUT | Exp | Tonemap | TAA Sharpen | Bloom | SSR | MB |
|------|------|-----|-----|---------|-------------|-------|-----|-----|
| TL Q0 | (0, 360) | warm | 1.5 | aces | RCAS 1.2 | 1.5 / th=0.8 | 0.4 | 0.8 / 12 |
| TR Q1 | (640, 360) | cool | 0.6 | uncharted2 | Lanczos halfRes | 0.4 / th=1.5 | 1.0 temporal | 0.0 |
| BL Q2 | (0, 0) | none | 1.0 | reinhard | unsharp 0.5 | 0.8 / th=1.0 | 0.7 | 0.3 / 8 |
| BR Q3 | (640, 0) | cool 50% | 1.2 | aces | RCAS 1.5 + variance | 1.2 / th=0.9 | 0.6 | 0.5 / 10 |

## 3. 原子化 (ATOMIZE)

### 3.1 任务清单
- [x] 探 HDR multi-instance + Bloom/SSR/MB.Process 读 active fbo (代码验证)
- [x] 创建 `samples/demo_quad_split/` 目录 + 复制 luts
- [x] 写 main.lua (~450 LOC):
  - [x] safe_require + API 检查 + headless probe
  - [x] 几何 (cube + plane) + 4 BAR_COLORS + 4 CAMERAS + 4 QUAD_RECTS
  - [x] OnOpen: 4 HDR instance Enable + 4 TAA instance setup + LUT load + auto* 关
  - [x] Update: 角度更新
  - [x] Draw: render_quad(i) × 4 + Tonemap × 4 + HUD
  - [x] OnKey: 1-4=reset history, L=LUT toggle, ESC
  - [x] cleanup_demo: 反向 Disable + Destroy
- [x] README.md
- [ ] 真 GL 启动测试 (3 秒 RUN, 验证 4 instance Enable 成功)
- [ ] 8 smoke 零回归
- [ ] FINAL/TODO + commit + CI 6/6

## 4. 风险

| 风险 | 缓解 |
|------|------|
| 4 HDR instance VRAM 4× | 各 instance 是 quad-sized (640×360), 总 VRAM 与单 instance WIN 尺寸 (1280×720) 相当 |
| Bloom/SSR/MB 全局参数每帧切换性能 | <1us, 与 demo_taa_split2 同模式, 已验证可接受 |
| `MAX_INSTANCES = 4` 限制 | 正好 4-quad, 无空间留扩展 (后续如需 6-quad 需提 MAX_INSTANCES) |
| TAA jitter 4 instance 各自序列 | 各 instance frameCounter 独立, 自动同步 |

## 5. 提交策略

- 1 commit: demo + README + PLAN/FINAL/TODO
- 不动 C++ / 不动 smoke (零 API 变更)
- CI 风险极低 (lua-only)
