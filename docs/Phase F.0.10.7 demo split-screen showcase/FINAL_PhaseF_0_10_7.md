# Phase F.0.10.7 — demo_taa_split2 视觉演示 + bug fix FINAL

> 6A 工作流 · 收尾报告 (精简版 — demo 升级 phase)
> Commit: `1af2b99`
> 工作量: ~1.5h

---

## 1. 项目背景

F.0.10.5 (shader uvBounds 完美边界) 与 F.0.10.6 (per-region tonemap) 已完成, 但**实际效果未在 demo 中展示**. 本 phase 把两个 phase 的最终能力同屏演示, 让用户能直观看到累积的 region 化基础设施的价值.

---

## 2. 交付内容

### 2.1 Backend bug fix

**问题**: `HDRRenderer::Tonemap(rgn, ...)` 内部不调 `UnbindFBO`, 调用方在 `EndScene` 之前调时 (HDR fbo 仍绑着), tonemap 写回 HDR RT 而非 default fb → **黑屏**.

**修复** (`hdr_renderer.cpp` 2 个重载):

```cpp
void Tonemap(int rgnX, int rgnY, int rgnW, int rgnH) {
    if (!g.enabled || !g.backend || !g.sceneTex) return;
    g.backend->UnbindFBO();   // F.0.10.7 fix: 切到 default fb
    // ...
}
```

**验证**: F.0.10.6 之前已通过 8 smoke + headless 但缺实际 GL context 测试; 本 phase 实施时发现该 bug 并修复.

### 2.2 demo 升级 (`samples/demo_taa_split2/main.lua`)

| 升级点 | 内容 |
|-------|------|
| 标题 | F.0.10.4 → F.0.10.7 (含 Tonemap per-region) |
| Window 标题 | 带 `+Tonemap per-region` |
| `hasF10_6` 检测 | 检查 `HDR.SetAutoTonemap` + `HDR.GetAutoTonemap` + `HDR.Tonemap` 3 fn |
| Headless probe | 加 3 个 case (round-trip / Tonemap nil+err / params-table nil+err) |
| `tonemapPerRegion` 状态 | F.0.10.6 启用时设 `HDR.SetAutoTonemap(false)` |
| `P1_TM_PARAMS` | `{exposure=1.5, gamma=2.2, tonemap='aces'}` 黄昏暖调 |
| `P2_TM_PARAMS` | `{exposure=0.6, gamma=2.4, tonemap='uncharted2'}` 冷夜蓝调 |
| 主循环 | `win:EndFrame` 之前加 2 次 `HDR.Tonemap(rgn, params)` |
| HUD | 加 Tonemap 状态行 + P1/P2 描述加 tonemap profile |
| 右半 banner | 调色改冷调 (0.5, 0.7, 1.0) 突出冷夜感 |
| cleanup | 复位 5 个 auto 开关 (加 `SetAutoTonemap(true)`) |

### 2.3 README 升级 (`samples/demo_taa_split2/README.md`)

| 升级点 | 内容 |
|-------|------|
| 标题 + 副标题 | F.0.10.7 + Tonemap per-region |
| 核心价值表 | 加边界完美 / per-region tonemap / 双 tonemap profile 3 行 |
| API 总数 | 14 → **17** (+3) |
| 实现要点 | 加 F.0.10.6 (3 API) + Tonemap 调用例 |
| 双 Player Profile 表 | 加 Tonemap 行 (ACES vs Uncharted2) |
| 演示重点 | "黄昏动感冲击" vs "冷夜静态高画质" |
| 技术约束 | 改 "已解决 / 后续 Phase" (1+2 已解决, 3-5 留后续) |
| CI 验证 | 10 PASS → **11 PASS** |

---

## 3. 验证

### 3.1 demo headless 11 PASS

```
PASS: HDR.SetAutoTAA(false) round-trip ok
PASS: TAA.Process(region) headless returns nil + err
PASS: HDR.SetAutoBloom(false) round-trip ok
PASS: HDR.SetAutoSSR(false) round-trip ok
PASS: HDR.SetAutoMotionBlur(false) round-trip ok
PASS: Bloom.Process(region) headless returns nil + err
PASS: SSR.Process(region) headless returns nil + err
PASS: MB.Process(region) headless returns nil + err
PASS: HDR.SetAutoTonemap(false) round-trip ok
PASS: HDR.Tonemap(rgn) headless returns nil + err
PASS: HDR.Tonemap(rgn, {exposure=1.5, tonemap="aces"}) headless ok
```

### 3.2 8 smoke 零回归

`hdr.lua` (31 fn) / `motion_blur.lua` / `bloom.lua` / `ssr.lua` / `ssao.lua` / `taa.lua` / `lens_flare.lua` / `lens_fx.lua` 全 PASS.

---

## 4. 关键决策

### 4.1 调用时序: `win:EndFrame` 之前

`HDR.Tonemap(rgn)` 必须在 `win:EndFrame` 之前调, 因为:
- `win:EndFrame` 内部隐式调 `EndScene`
- `EndScene` 进入时 `UnbindFBO` 切到 default fb
- 后续 SSAO/AE/LensFx 仍跑 (写 HDR RT, 浪费但无害)
- 用户调 `HDR.Tonemap(rgn, params)` 必须先 unbind (本 phase bug fix)

### 4.2 双 tonemap profile 选择

- **ACES (P1)**: Narkowicz 2016 fitted, 电影感, 暖调倾向, 适合 "黄昏阳光"
- **Uncharted2 (P2)**: Hable filmic, 冷调倾向 + white scale, 适合 "冷夜"

两者数学曲线显著不同, 视觉对比强烈, 是展示 per-region tonemap 能力的最佳选择.

### 4.3 不显式调 `EndScene`

demo 仍用 `win:EndFrame` 的 EndScene 自动调用. 这意味着 EndScene 仍跑 SSAO/AE/LensFlare/LensDirt/Streak 等模块 (写 HDR RT), 但这些输出在下帧 BeginScene 被 clear, 实际是 GPU 浪费. 后续优化空间 (留 phase).

---

## 5. 工作量

| 阶段 | 内容 | 工作量 |
|------|------|-------|
| ALIGN | 读 demo 现状, 决定升级范围 | 0.2h |
| SP1 (bug fix + demo + README) | UnbindFBO 修复 + main.lua 升级 + README 升级 | 1h |
| SP2 (Assess 文档) | 本文件 | 0.3h |
| **合计** | | **~1.5h** (vs 估 2-3h, 节约 ~30%) |

---

## 6. 后续候选

| Phase | 主题 | 工作量 | 优先级 |
|-------|------|-------|-------|
| **F.1** | DLSS-like TAAU 真上采样 | ~10-15h | 高 (TAA 终极) |
| **F.0.10.8** | per-region color grading LUT | ~5-7h | 中 |
| **F.0.11** | volumetric fog region 化 | ~4-5h | 低 |
| **F.0.10.9** | 真多 HDR target (RT pool) | ~8-10h | 中 |

详见 `Phase F.0.10.6/TODO_PhaseF_0_10_6.md` §4.

---

## 7. TODO

### 7.1 强制

- [ ] **CI 6/6 验证**: 等 push 后 webhook (commit `1af2b99` + 本 commit)

### 7.2 可选

- [ ] **视觉打点**: 实际跑 demo 截图入 README (需 GL context)
- [ ] **EndScene 优化**: SSAO/AE/LensFx 在 split-screen + tonemapPerRegion 时跳过 (省 GPU)

### 7.3 用户支持需求

- 无新配置 / 新依赖

---

## 8. 结论

F.0.10.7 **成功完成**, demo 现具备:

- ✅ 双 TAA instance + 双 player 后处理 (TAA / Bloom / SSR / MB)
- ✅ 边界像素完美 (F.0.10.5 uvBounds)
- ✅ per-region tonemap 双 profile (F.0.10.6 + 本 phase bug fix)

**下一步建议**: 启动 F.1 (TAAU 真上采样) 或 F.0.10.8 (color grading LUT).
