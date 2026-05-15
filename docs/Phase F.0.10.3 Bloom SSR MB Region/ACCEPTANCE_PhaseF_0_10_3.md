# Phase F.0.10.3 — Bloom/SSR/MotionBlur Region ACCEPTANCE 验收

> 6A 工作流 · 阶段 6 (Assess) · ACCEPTANCE
> 关联: ALIGNMENT / DESIGN / TASK PhaseF_0_10_3
> 总工作量: 4 个 sub-phase, 实际累计 ~6h (DESIGN 估 9-12h, 因复用 F.0.10.2 scissor 路径模板压缩)

---

## 1. 任务完整性 (Sub-Phase 矩阵)

| Sub-Phase | 目标 | Commit | 本地 smoke | 备注 |
|-----------|------|--------|------------|------|
| 阶段 1-4 (6A) | ALIGN/ARCH/ATOMIZE/APPROVE 文档 | a0e2579 | n/a | 3 docs (~44 KB) 共识/架构/任务拆分 |
| Sub-Phase 1 — MotionBlur region + 6 HDR.SetAutoXxx | MB.Process(rgn) + 3 对 HDR Auto 开关 | 6f83d95 | 5/5 PASS | HDR auto-Bloom/SSR pre-implement (sub-phase 2/3 留 .Process) |
| Sub-Phase 2 — Bloom region + Bloom.Process | 4 个 backend pass + mip 链 region 缩放 | 761b914 | 7/7 PASS | 复用 Bloom shader 的 LensFlare/Streak 零回归 |
| Sub-Phase 3 — SSR region + SSR.Process | 5 个 backend pass (含 blur 缩半) | c4b64ff | 8/8 PASS | 复用 BlitHDRDepthToSSAO 的 SSAO 零回归 |
| Sub-Phase 4 — 6A Assess 收尾 | ACCEPTANCE / FINAL / TODO | (本 commit) | n/a | CI 待 push 后回填 |

---

## 2. 设计决策回顾 (vs DESIGN / TASK)

### 2.1 关键路径选型

| 项 | DESIGN 决策 | 实际 | 偏差说明 |
|----|------------|------|---------|
| 总体路径 | F.0.10.2 scissor 路径模板 | ✅ 一致 | 5 个 Bloom / 5 个 SSR / 1 个 MB pass 全 conditional scissor |
| Shader 改动 | 完全不动 | ✅ 完全不动 | GL_SCISSOR_TEST + glBlitFramebuffer src/dst rect 替代 |
| Backend 接口 | 加默认 0 region 参数 (零回归) | ✅ 全部 10 个虚接口 | BlitHDRDepthToSSAO / DrawSSR / DrawSSRComposite / DrawSSRBlur / DrawSSRTemporal / DrawBloomBrightPass / DrawBloomDownsample / DrawBloomUpsample / DrawBloomComposite / DrawMotionBlur |
| Renderer Process | 加 region overload + 老 Process 转发 | ✅ 3 个 module | MotionBlurRenderer / BloomRenderer / SSRRenderer |
| HDR 联动开关 | 3 对 SetAutoXxx/GetAutoXxx | ✅ 3 对 (合并 F.0.10.2 SetAutoTAA 同模式) | EndScene 3 处 gate |
| Lua API 新增 | +9 fn (3 Process + 6 SetAuto/GetAuto) | ✅ +9 fn | hdr.lua 22→28, motion_blur.lua 15→16, bloom.lua 15→16, ssr.lua 23→24 |
| Bloom mip 链 region | 每级 >>1 缩半 / <<i 翻倍 | ✅ 实现 | downsample 递推 >>1; upsample 按 (i-1) 反算 (不递推避免误差); std::max(1, ...) 兜底 |
| SSR blur 缩半 | caller 负责 full→half | ✅ 实现 | Process 入口算 blurRgn = (x/2, y/2, max(1, w/2), max(1, h/2)) |
| glBlitFramebuffer | sub-rect blit (受 scissor 不影响) | ✅ 2 处 | BlitHDRDepthToSSAO + DrawSSRComposite ① pass |

### 2.2 与 F.0.10.2 (TAA) 工艺对照

| 维度 | F.0.10.2 (TAA) | F.0.10.3 (Bloom/SSR/MB) |
|------|----------------|------------------------|
| Backend pass 改造 | 7 个 (TAA + variants) | 10 个 (Bloom 4 + SSR 5 + MB 1) |
| Renderer Process overload | 1 个 (TAARenderer) | 3 个 (MB/Bloom/SSR) |
| HDR Auto 开关 | 1 对 (SetAutoTAA) | 3 对 (SetAuto Bloom/SSR/MB) |
| Lua API 新增 | +5 fn (含 SetViewport) | +9 fn |
| 新增 smoke | taa.lua +X (region 段) | 3 文件各 +6 PASS (defense) + hdr.lua +12 PASS |
| 实际工作量 | ~6h | ~6h (复用模板高度成熟) |

---

## 3. 接口契约 (新增 10 fn)

### 3.1 三个 .Process(region) Lua API (同模式)

```lua
-- MotionBlur / Bloom / SSR 三个 module 同接口签名
Light.Graphics.MotionBlur.Process(x, y, w, h)
Light.Graphics.Bloom.Process(x, y, w, h)
Light.Graphics.SSR.Process(x, y, w, h)
-- @param x,y integer? region 左下角 (默认 0 = 全屏老路径)
-- @param w,h integer? region 尺寸 (默认 0 = 全屏老路径)
-- @return boolean true 成功; nil + err string = HDR 未启 / 参数非法
-- @note 内部用 HDRRenderer::GetFBO() + GetSceneTexture() 作为目标
--       headless / HDR-off 静默 no-op (返 nil + err string)

-- 防御性: 0/4 args (只传 2/3 个拒绝), w/h>=0 (负值拒绝), 类型错抛 luaL_error
```

### 3.2 HDR.SetAuto[Bloom/SSR/MotionBlur] (3 对开关)

```lua
-- 与 SetAutoTAA 同模式: 默认 true = 老 EndScene 自动调对应 Renderer::Process (零回归)
-- 设 false = 用户负责手动调 Bloom.Process / SSR.Process / MotionBlur.Process 控时序
HDR.SetAutoBloom(on)       -- @return true; bad-arg 返 nil + err
HDR.GetAutoBloom()         -- @return boolean (default true)
HDR.SetAutoSSR(on)
HDR.GetAutoSSR()
HDR.SetAutoMotionBlur(on)
HDR.GetAutoMotionBlur()
```

### 3.3 典型 split-screen 用法

```lua
local W, H = 1920, 1080
-- 1. 设置: HDR + 3 个后处理 Enable, 但关 auto-EndScene
HDR.Enable(W, H)
Bloom.Enable(W, H);    HDR.SetAutoBloom(false)
SSR.Enable(W, H);      HDR.SetAutoSSR(false)
MotionBlur.Enable(W, H); HDR.SetAutoMotionBlur(false)
HDR.SetAutoTAA(false)   -- F.0.10.2

-- 2. 渲染循环 (每帧)
HDR.BeginScene()
  -- Player 1 (左半屏)
  Graphics.SetViewport(0, 0, W/2, H);
  draw_player1_scene()
  Bloom.Process(0, 0, W/2, H)
  SSR.Process(0, 0, W/2, H)
  MotionBlur.Process(0, 0, W/2, H)
  TAA.SetActiveInstance(1); TAA.Process(0, 0, W/2, H)

  -- Player 2 (右半屏)
  Graphics.SetViewport(W/2, 0, W/2, H);
  draw_player2_scene()
  Bloom.Process(W/2, 0, W/2, H)
  SSR.Process(W/2, 0, W/2, H)
  MotionBlur.Process(W/2, 0, W/2, H)
  TAA.SetActiveInstance(2); TAA.Process(W/2, 0, W/2, H)
HDR.EndScene()
```

---

## 4. 本地验证清单

### 4.1 编译 (Release, Windows x64)
```
cmake --build build --config Release --target Light  # ChocoLight/build
# 输出: build/bin/Release/Light.dll
# sync 到 lumen-master/build/src/light/Release/Light.dll
```
✅ 三个 sub-phase 各自编译通过, 无 warning 引入

### 4.2 Smoke 全 PASS (8/8)
```
foreach $f in (hdr, motion_blur, bloom, ssr, ssao, taa, lens_flare, lens_fx) {
  light.exe scripts/smoke/$f.lua  # 全 exit 0
}
```

| smoke | 新增 PASS | 关键点 |
|-------|----------|--------|
| `hdr.lua` | +12 | SetAuto Bloom/SSR/MB × 4 PASS (default true, round-trip, bad-arg, idempotent); 28 fn surface |
| `motion_blur.lua` | +10 | Process defense × 6 + SetAutoMotionBlur × 4; 16 fn surface |
| `bloom.lua` | +7 | Process defense × 6 + fn_names; 16 fn surface |
| `ssr.lua` | +7 | Process defense × 6 + fn_names; 24 fn surface |
| `ssao.lua` | 0 (零回归) | 复用 BlitHDRDepthToSSAO 接口零行为变更 |
| `taa.lua` | 0 (零回归) | F.0.10.2 已 region 化, 不动 |
| `lens_flare.lua` | 0 (零回归) | 复用 Bloom shader, 不传 region 走默认 0 = 全屏 |
| `lens_fx.lua` | 0 (零回归) | 复用 Bloom shader, 同 lens_flare |

### 4.3 累计 PASS 增量 (36 PASS)
- hdr.lua: +12
- motion_blur.lua: +10
- bloom.lua: +7
- ssr.lua: +7

---

## 5. 风险矩阵 (DESIGN 风险 → 实际)

| Risk ID | DESIGN 风险描述 | 实际状态 | 处理 |
|---------|---------------|---------|------|
| R-1 | scissor 跨 mip 缩半误差 (Bloom) | 已规避 | upsample 按 (i-1) 反算, 不递推; std::max(1, ...) clamp 1×1 兜底 |
| R-2 | SSR blur 缩半 region 边界泄漏 | 已规避 | caller 用 (x/2, y/2, max(1, w/2), max(1, h/2)) 与 backend CreateSSRBlurRT 同模式 |
| R-3 | glBlitFramebuffer 不受 scissor 影响 | 已规避 | BlitHDRDepthToSSAO + DrawSSRComposite ① 用 src/dst rect 显式控制 |
| R-4 | ray march 跨 region 采样 | 接受 (物理正确) | 反射借邻区合理, 与 SSR 屏幕空间本质一致 |
| R-5 | 复用 Bloom shader 的 LensFlare/Streak 回归 | 已规避 | 4 个 Bloom 接口加默认 0 region 参数, 老调用零改动 |
| R-6 | 复用 BlitHDRDepthToSSAO 的 SSAO 回归 | 已规避 | SSAO 不传参 = 老行为, smoke 通过验证 |
| R-7 | ~1px 边界采样泄漏 (邻 region 颜色串色) | 接受 (与 F.0.10.2 TAA 同等级) | 默认场景肉眼难辨; 完美方案需 shader uvOffset/uvScale (留 F.0.10.5+) |

---

## 6. 累计 Lua API 演进

| Phase | 累计 fn | 增量 |
|-------|--------|------|
| F.0.10 (TAA multi-instance) | 41 | +6 (CreateInstance + variants) |
| F.0.10.2 (真物理 split-screen TAA) | 45 | +4 (SetViewport/GetViewport + SetAutoTAA + GetAutoTAA + TAA.Process) |
| F.0.10.3 sub-phase 1 (MB + 6 HDR Auto) | 52 | +7 (MB.Process + 3 对 HDR SetAuto/GetAuto) |
| F.0.10.3 sub-phase 2 (Bloom region) | 53 | +1 (Bloom.Process) |
| F.0.10.3 sub-phase 3 (SSR region) | 54 | +1 (SSR.Process) |
| **累计 F.0.10.3** | **54** | **+9 vs F.0.10.2** |

---

## 7. 待办 (CI 回填)
- [ ] push 后等 CI 跑全 smoke (build-templates.yml 已含 motion_blur / bloom / ssr / taa / hdr)
- [ ] 在 ACCEPTANCE 表格回填实际 CI run id

---

> 验收结论: **3 个 sub-phase 全部完成 + 3 个新 .Process(region) Lua API + 3 对 HDR SetAuto/GetAuto + 10 个 backend 接口 region 化 + 36 PASS smoke 增量**. 零回归 (8 smoke 全过, 含 SSAO/LensFlare/LensFX 复用接口), 等待 CI 验证.
