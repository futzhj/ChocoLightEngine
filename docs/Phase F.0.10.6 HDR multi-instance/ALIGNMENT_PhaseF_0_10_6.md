# Phase F.0.10.6 — HDR multi-instance per-region tonemap ALIGNMENT 对齐

> 6A 工作流 · 阶段 1 (Align) · 需求理解与对齐
> 启动: 2026-05-16
> 关联前置 phase: F.0.10.5 (Shader uvBounds), F.0.10.3 (Bloom/SSR/MB region)

---

## 1. 项目上下文

### 1.1 当前架构状态

| 组件 | 当前能力 | 限制 |
|------|---------|------|
| HDR FBO + sceneTex | 全局 1 个 FBO/tex (RGBA16F) | 多 region 共享同一 HDR target |
| Bloom / SSR / MotionBlur / TAA | F.0.10.3 + F.0.10.5 已支持 region | 物理 split-screen 边界完美 |
| Tonemap | `DrawTonemapFullscreen(hdrTex, exposure, gamma, mode)` 全屏 1 pass | 全局 1 组 (exposure, gamma, mode) |
| Lua API | `HDR.SetExposure / SetGamma / SetTonemapper` (全局 single-set) | 不能 per-region 不同曝光 |

### 1.2 需求来源

F.0.10.5 FINAL 文档 §7 后续候选:
> **F.0.10.6** (HDR multi-instance): 每个 region 独立 HDR target / tonemap params

实际典型用例:
- Split-screen 中 P1 黄昏暖调 (exposure=1.5, ACES) vs P2 冷夜蓝调 (exposure=0.6, Uncharted2)
- 编辑器中 viewport A 默认 vs viewport B 调试 Linear (无 tonemap)
- VR 多视图: 左右眼可能需要微调曝光补偿

---

## 2. 范围对齐

### 2.1 IN scope

- 新增 backend `DrawTonemapRegion(...)`: 与 fullscreen 同 shader, 加 scissor 限定写区域
- 新增 HDR `SetAutoTonemap(bool)` 开关 (默认 true 零回归; false 时 EndScene 不自动跑)
- 新增 HDR `Tonemap(rgnX, rgnY, rgnW, rgnH, exposure, gamma, mode)` C++ API
- 新增 Lua `HDR.Tonemap(rgnX, rgnY, rgnW, rgnH [, params_table])` (params 可选, 不传用全局)
- 新增 Lua `HDR.SetAutoTonemap / GetAutoTonemap`
- smoke `hdr.lua` 加 case 验证 round-trip + headless tonemap 退化

### 2.2 OUT of scope

- ❌ **不实现**: 多 HDR FBO / sceneTex (per-region 独立 HDR target). 当前 sceneTex 是 RGBA16F 全屏共享, region 化只在 *输出阶段* (tonemap) 区分参数, 不复制 HDR 内容. 真正多 HDR target 涉及 RT pool, 工作量数倍, 留后续 phase
- ❌ **不实现**: per-region color grading LUT / film grain (这是 Post-FX, 与 tonemap 解耦)
- ❌ **不改**: tonemap shader (复用现有 4 operator + uniform)
- ❌ **不改**: demo 视觉验证 (留用户手动跑 / 后续 phase 加 demo_tonemap_split2)

### 2.3 兼容性要求

- ✅ 老调用 `HDR.SetExposure(v)` 不变, 全屏 tonemap 老路径零回归
- ✅ `SetAutoTonemap(false)` 时, EndScene 不调 fullscreen, 但 sceneTex 仍存在, 用户必须自己调 `HDR.Tonemap(rgn)` 至少一次, 否则屏幕黑屏 (与 SetAutoBloom 同语义)
- ✅ Lua API 数 +3 (`HDR.Tonemap` + `HDR.SetAutoTonemap` + `HDR.GetAutoTonemap`) → 总 57 fn

---

## 3. 关键假设

### 3.1 已确认 (基于代码探查)

- ✅ Backend 已有 `programTonemap` + 4 个 uniform location (`locTonemap_HDRTex/Exposure/Gamma/Mode`) — 复用即可
- ✅ Backend 已有 scissor 模式 (Phase F.0.10.3 引入), `DrawTonemapRegion` 直接套
- ✅ HDRRenderer 已有 `g.autoTAA / g.autoBloom / g.autoSSR / g.autoMotionBlur` 模式, 加 `g.autoTonemap` 同模式
- ✅ Tonemap pass 写 default framebuffer (不是 HDR fbo), region 化的写区是 backbuffer 的 split-screen viewport

### 3.2 待澄清 (主动决策)

| 决策点 | 选项 | 选 | 理由 |
|--------|------|---|------|
| **uvBounds 是否需要** | A. 加 B. 不加 | **B** | Tonemap 是单点采 (`texture(uHDRTex, vUV)`), 无邻域采样, 不会跨 region 泄漏 |
| **params_table 字段名** | A. `mode` (与 C++ enum 对齐) B. `tonemap` (与 `SetTonemapper` 对齐) | **B** | 与 Lua `HDR.SetTonemapper(name)` 一致, 用 string name |
| **params 字段类型** | A. 全 number/string B. 接受 int mode (0..3) 或 string name | **B** | 灵活, 与 `SetTonemapper` 现有 string-name 模式一致 |
| **未传 params 行为** | A. 用全局 (g.exposure / g.gamma / g.tonemap) B. 必传 | **A** | 与 fullscreen 一致, 简化 Lua 调用 |
| **autoTonemap 默认值** | A. true (零回归) B. false (强制用户改) | **A** | 零回归, 与 autoTAA/autoBloom/autoSSR/autoMotionBlur 同 |
| **AutoExposure (AE) 与 region 关系** | A. region 模式仍走 AE 全局 B. region 模式忽略 AE | **A** | AE 是 hdr 整体亮度自适应, region 只是输出 tonemap, 用户传的 exposure 仍叠 AE 倍率 |

---

## 4. 验收标准

### 4.1 功能验收

- [ ] `HDR.SetAutoTonemap(false)` round-trip (set/get)
- [ ] `HDR.Tonemap(rgnX, rgnY, rgnW, rgnH)` headless 不崩 (HDR 未启用时 silent skip)
- [ ] `HDR.Tonemap(rgnX, rgnY, rgnW, rgnH, {exposure=1.5, gamma=2.4, tonemap="uncharted2"})` 接受 params
- [ ] 全屏老路径 (auto=true, 不调 region API) 视觉与 F.0.10.5 完全一致

### 4.2 测试验收

- [ ] 8 smoke 全过 (hdr/motion_blur/bloom/ssr/ssao/taa/lens_flare/lens_fx)
- [ ] hdr.lua 加 case 验证: SetAutoTonemap round-trip + Tonemap headless

### 4.3 集成验收

- [ ] CI 6/6 success (含 Web GLES + iOS GLES)
- [ ] 编译 Release 通过, 零 warning

---

## 5. 风险评估

| 风险 | 概率 | 影响 | 缓解 |
|-----|------|------|------|
| AE (AutoExposure) 与 per-region exposure 互动复杂 | 中 | 视觉不符预期 | 文档明确: per-region exposure 是 *叠加* AE 倍率, 不是替代 |
| auto=false 用户忘调 region tonemap → 黑屏 | 低 | 用户排查时间 | TODO 文档 + Lua 错误日志 |
| Backend `DrawTonemapRegion` 与 `DrawTonemapFullscreen` 代码重复 | 低 | 维护成本 | 复用 helper, 全屏版调 region 版传 (0,0,0,0) |
| CI 平台差异 (Web GLES tonemap shader 不变) | 极低 | N/A | 不改 shader |

---

## 6. 工作量预估

| 阶段 | 估算 |
|------|------|
| ALIGN / DESIGN / TASK 文档 | 0.5h |
| Sub-Phase 1: backend `DrawTonemapRegion` + HDRRenderer `Tonemap(rgn)` + autoTonemap | 1.5h |
| Sub-Phase 2: Lua API + smoke test 加 case | 1h |
| Sub-Phase 3: 6A Assess (ACCEPTANCE/FINAL/TODO) | 0.5h |
| **合计** | **~3.5h** |

低于初步估的 6-8h, 因为复用了 F.0.10.3 (region scissor 模式) + F.0.10.5 (auto-* 开关模式) + 不动 shader + 不改 demo (留后续 phase).

---

## 7. 共识声明

确认此 ALIGNMENT 文档已对齐, scope 清晰, 假设已澄清, 进入 DESIGN 阶段.
