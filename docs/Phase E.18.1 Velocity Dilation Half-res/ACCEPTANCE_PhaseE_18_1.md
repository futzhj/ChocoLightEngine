# Phase E.18.1 Velocity Dilation Half-Resolution — ACCEPTANCE

> 6A 工作流 · 阶段 6 · Assess
> 基线：Phase E.18 commit `8b7c25b`

---

## 1. 实施完成度

| 任务 | 文件 | 行数变更 | 状态 |
|------|------|---------|------|
| T1 backend 接口扩 sw/sh | `@e:/jinyiNew/Light/ChocoLight/include/render_backend.h` | +14 / -8 | ✅ |
| T2 GL33 impl 用 sw/sh | `@e:/jinyiNew/Light/ChocoLight/src/render_gl33.cpp` | +10 / -5 | ✅ |
| T3 HDRRenderer 集成 | `@e:/jinyiNew/Light/ChocoLight/include/hdr_renderer.h` | +10 / 0 | ✅ |
| | `@e:/jinyiNew/Light/ChocoLight/src/hdr_renderer.cpp` | +90 / -8 | ✅ |
| T4 Lua 绑定 | `@e:/jinyiNew/Light/ChocoLight/src/light_graphics.cpp` | +30 / 0 | ✅ |
| T5 smoke / demo / docs | `@e:/jinyiNew/Light/scripts/smoke/hdr.lua` | +50 / -1 | ✅ |
| | `@e:/jinyiNew/Light/samples/demo_ssr/main.lua` | +14 / -2 | ✅ |
| | `@e:/jinyiNew/Light/docs/api/Light_Graphics.md` | +96 / -1 | ✅ |
| T6 6A 三件套 | `ACCEPTANCE_PhaseE_18_1.md` / `FINAL_PhaseE_18_1.md` / `TODO_PhaseE_18_1.md` | 新增 | ✅ |
| T7 commit+push+CI | git + GitHub Actions | — | ✅ |

---

## 2. 决策对齐核对（ALIGNMENT 10 决策矩阵）

| # | 决策点 | 已落实位置 | 状态 |
|---|--------|----------|------|
| 1 | 半分辨率公式 `((W+1)/2, (H+1)/2)` | `hdr_renderer.cpp::ComputeDilationStorageSize` | ✅ |
| 2 | uTexel 策略 `1.0/(sw, sh)` 半分辨率纹素间距 | `render_gl33.cpp::DrawVelocityDilate` | ✅ |
| 3 | 过滤模式 `GL_LINEAR + GL_CLAMP_TO_EDGE` | `render_gl33.cpp::CreateVelocityDilateRT`（沿用 Phase E.18） | ✅ |
| 4 | API 命名 `SetVelocityDilationHalfRes` / `GetVelocityDilationHalfRes` | `hdr_renderer.h` + `light_graphics.cpp` | ✅ |
| 5 | 默认值 `false`（保 Phase E.18 行为） | `hdr_renderer.cpp::State::dilationHalfRes = false` | ✅ |
| 6 | 切换时立即重建（双 RT 同步） | `SetVelocityDilationHalfRes` → `ReleaseDilationRT` + `RebuildDilationRT` | ✅ |
| 7 | backend 接口扩 sw/sh | `render_backend.h` 2 接口签名变更 | ✅ |
| 8 | Consumer 路径零改动 | SSR/MotionBlur 未修改（单点采 dilatedTex，硬件 bilinear 上采） | ✅ |
| 9 | 生效条件：仅 dilation pass 启用时 | dilation OFF → dilatedFbo 未创建 → state 保存但无效 | ✅ |
| 10 | 零回归保障：halfRes=false 行为 = Phase E.18 | sw=w, sh=h 时 backend 与 Phase E.18 路径完全等价 | ✅ |

---

## 3. 验收 checklist

### T1 编译通过
- [x] `render_backend.h` 2 个接口签名扩 sw/sh，默认实现保持 safe
- [x] 注释明确 w/h 是 logical（未来 sanity check）、sw/sh 是 storage

### T2 GL33 运行时
- [x] `CreateVelocityDilateRT` 内 `glTexImage2D(GL_RG16F, sw, sh)`
- [x] `DrawVelocityDilate` 内 `glViewport(0, 0, sw, sh)` + `uTexel = 1/(sw, sh)`
- [x] FBO 完整性检查 + log 同时输出 sw/sh 和 logical w/h

### T3 HDRRenderer
- [x] State 增 `dilationHalfRes = false`
- [x] `ComputeDilationStorageSize` 与 motion blur `ComputeStorageSize` 同模式
- [x] CreateRT 内透传 dsw/dsh 到双 CreateVelocityDilateRT 调用
- [x] EndScene 内透传 dsw/dsh 到双 DrawVelocityDilate 调用
- [x] `ReleaseDilationRT` 仅释放 dilation 部分（不动 HDR 主 FBO）
- [x] `RebuildDilationRT` 仅在 HDR Enable + backend 支持 + rawVelocity 存在时重建
- [x] `SetVelocityDilationHalfRes(bool)` no-op 短路 + 已 Enable 立即重建
- [x] `GetVelocityDilationHalfRes() → bool`

### T4 Lua 绑定
- [x] `l_HDR_SetVelocityDilationHalfRes` 严格 boolean 检查 → nil + err
- [x] `l_HDR_GetVelocityDilationHalfRes` 返 boolean
- [x] `hdr_funcs[]` 注册 2 项（Phase E.18.1 标记）

### T5 docs / smoke / demo
- [x] `hdr.lua` smoke 加 §9 round-trip + type-error + no-op 测试（4 个 pass）
- [x] `hdr.lua` 函数表加 2 项；末尾计数 18 functions / "Phase E.3 + E.14 + E.18.1"
- [x] `demo_ssr/main.lua` 加 `]` 快捷键 + HUD 显示 halfRes=ON/OFF
- [x] `Light_Graphics.md` 新增 `HDR.SetVelocityDilationHalfRes` / `GetVelocityDilationHalfRes` 段（+性能/VRAM 表 + 适用场景表）
- [x] API 速查表加一行 E.18.1

### T6 6A 文档
- [x] ALIGNMENT / DESIGN / TASK 已完成
- [x] ACCEPTANCE / FINAL / TODO 本次完成

### T7 CI
- [x] GitHub Actions 6/6 平台 success
- [x] CI 状态回填 ACCEPTANCE + FINAL + TODO

---

## 4. 关键技术洞察

### 4.1 uTexel 策略选 `1/(sw, sh)` 的理由

- 物理直觉：uTexel 就是 dilated tex 的纹素间距，应跟随 viewport
- max-filter 自动鲁棒：邻域物理覆盖在 raw space 由 3 raw px 扩到 6 raw px，max() 操作只会扩大覆盖、不引入伪信号
- 实现简洁：DrawVelocityDilate 只需 sw/sh 一个尺寸，不用同时传 logical fullW/fullH
- 与 Phase E.18 等价回退：halfRes=false 时 sw=w, sh=h → uTexel=1/(W,H) 与 Phase E.18 完全一致

### 4.2 与 Phase E.17 motion blur halfRes 的对比

| 维度 | Phase E.17 (motion blur halfRes) | Phase E.18.1 (dilation halfRes) |
|------|----------------------------------|--------------------------------|
| RT 角色 | output (写入端) | output (写入端) |
| 半分辨率公式 | `((W+1)/2, (H+1)/2)` | `((W+1)/2, (H+1)/2)` ✓ 同 |
| viewport | half-res | half-res ✓ 同 |
| uTexel | full-res `1/(W,H)`（reconstruction filter 步长） | **half-res `1/(sw,sh)`**（dilation 邻域定义）★ |
| consumer 上采 | tonemap blit bilinear | shader texture() bilinear ✓ 同 |
| 默认值 | false | false ✓ 同 |
| 切换重建 | 立即 Resize | 立即 ReleaseDilationRT + RebuildDilationRT |

> uTexel 不同的原因：motion blur 的 reconstruction filter 步长跟屏幕坐标走（与 output RT 尺寸无关），dilation 的 max-filter 邻域定义跟随 output texel 间距。

### 4.3 三层 silent fallback 保留

1. backend 不支持 dilation pass → `SupportsVelocityDilation()=false` → CreateRT 不创建 dilated RT → halfRes 无影响
2. dilation OFF（用户主动） → CreateRT 跳过 dilated 创建 → halfRes 无影响
3. CreateVelocityDilateRT 失败 → dilatedFbo=0 → EndScene 跳过 DrawVelocityDilate → consumer fallback raw velocity + inline 9-tap

### 4.4 RebuildDilationRT 的独立逻辑

- `ReleaseDilationRT` / `RebuildDilationRT` 仅处理 dilation RT，避免重建整个 HDR FBO（含 scene/normal/velocity MRT，代价更大）
- 切换 halfRes 时只重建 2 张 dilated RT（combined + 可选 camera-only），无需重置 velocity history
- 同 Phase E.17 motion blur halfRes 重建模式

---

## 5. 性能预算（理论，1080p）

### VRAM 节省

| 资源 | full-res | half-res | 节省 |
|------|----------|----------|------|
| dilatedVelocityTex | 8 MB | 2 MB | 6 MB |
| dilatedCameraVelocityTex | 8 MB | 2 MB | 6 MB |
| **总计** | **16 MB** | **4 MB** | **12 MB (-75%)** |

### GPU 时间（dilation pass 自身）

| 配置 | 像素数 | fetch | 估算 ms (RTX 3060) |
|------|--------|-------|---------------------|
| full-res dilation | 1920×1080 = 2.07 M | 9 fetch/px | 0.10 ms |
| half-res dilation | 960×540 = 0.52 M | 9 fetch/px | 0.025 ms |
| **节省** | **-75%** | 0 | **-0.075 ms** |

### 总管线收益（SSR + Motion Blur N=8 同开）

| 模式 | dilation pass | consumer fetch | 总 | 估算 ms |
|------|---------------|----------------|----|---------| 
| Phase E.18 (full-res) | 0.10 | 0.30 | 0.40 | 0.40 ms |
| Phase E.18.1 (half-res) | 0.025 | 0.30 | 0.325 | 0.33 ms |
| **节省** | -75% | 0% | -19% | **-0.075 ms** |

> Consumer fetch 数不变（仍单点采 dilatedTex），主要收益在 dilation pass 自身 + VRAM。

---

## 6. 已知限制

1. **极宽窄物体高速运动可能丢失**：half-res 下 1-2 像素物体 motion blur 弱化（与 Phase E.17 motion blur halfRes 同妥协）；用户可关 halfRes
2. **single-consumer 无收益**：仅 SSR Temporal 或仅 Motion Blur 时 dilation pass 本身 cost 大于 inline 9-tap；用户应自行判断
3. **未做真机 GPU profile 实测**：性能数据基于理论，建议后续用 Tracy/RGP 实测
4. **dilation halfRes 与 motion blur halfRes 解耦**：互不影响，但用户可能误解组合关系 → 文档已明确

---

## 7. CI 状态

| 平台 | 状态 | 状态详情 |
|------|------|------|
| build-windows | ✅ success | runtime smoke 25 PASS (hdr.lua 18 fn) + Phase E.16/17/18 零回归 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [`25901596673`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25901596673)
Commit hash: `254984f`
Total duration: **6 min**
Date: 2026-05-15 05:14 UTC → 05:20 UTC
