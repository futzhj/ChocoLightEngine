# Phase F.0.14 TAA Lanczos-2 Upscaler — ACCEPTANCE

> 6A 工作流 · 阶段 4+6 合并
> 关联：`PLAN_PhaseF_0_14.md` / `FINAL_PhaseF_0_14.md` / `TODO_PhaseF_0_14.md`
> 基线：F.0 + F.0.1~0.9 + F.0.12 + F.0.13 (commit `3923584`)

---

## 1. 任务完整性

| 维度 | 实际 | 状态 |
|------|------|------|
| Backend virtual DrawTAALanczosPass | render_backend.h 加默认 no-op + doxygen | ✅ |
| GLES 3.0 shader FS_LANCZOS_UPSCALE | 5x5 25-tap unrolled + sinc(x)*sinc(x/2) + wsum 归一化 + HDR safe | ✅ |
| GL 3.3 shader FS_LANCZOS_UPSCALE | 与 GLES3 等价 (#version + 无 precision) | ✅ |
| GL33Backend program/uniform | programLanczosUpscale + locLanczos_InputTex/Texel | ✅ |
| GL33Backend program load | tonemap init 阶段构建, 失败 fallback 走 BlitTAAToHDR | ✅ |
| GL33Backend cleanup | glDeleteProgram 释放 | ✅ |
| GL33Backend DrawTAALanczosPass override | 复用 vaoTonemap, FBO 绑定 + viewport + uniform 上传 | ✅ |
| TAARenderer parseUpscaleMode_ | "lanczos" → 2 + case-insensitive | ✅ |
| TAARenderer GetUpscaleMode | 三 mode 字符串映射 (0=bilinear/1=bicubic/2=lanczos) | ✅ |
| TAARenderer Process upscaleMode==2 | 走 DrawTAALanczosPass | ✅ |
| smoke (taa.lua) | F.0.14 完整测试段 (~5 PASS) + 十一启共存 | ✅ |
| demo (demo_ssr) | P 键三 mode 轮转 | ✅ |
| API doc | 速查表更新 + 完整段加 lanczos 说明 | ✅ |
| Lua 语法验证 | lightc -p taa.lua && demo_ssr Exit 0 | ✅ |

---

## 2. 决策矩阵对齐验证（6/6）

| # | 决策 | 实现确认 |
|---|------|---------|
| D1 API surface = 复用 SetUpscaleMode | parseUpscaleMode_ 加 "lanczos" 分支, Lua API 不变 | ✅ |
| D2 shader 实现 = 单 pass 25-tap | 5x5 unrolled, 不需 intermediate RT | ✅ |
| D3 sinc 实现 = 直接 sin/π | abs<1e-5 提前返 1.0 + abs>=2 提前返 0 | ✅ |
| D4 边界处理 = wsum 归一化 | sum / max(wsum, 1e-4) 防 0/0 | ✅ |
| D5 默认值 = "bilinear" | 零回归, parseUpscaleMode_ 默认走 bilinear | ✅ |
| D6 mode 编码 = 0=bilinear/1=bicubic/2=lanczos | 升序扩展, 不破坏 F.0.9 现有 | ✅ |

---

## 3. 验收清单

### 功能
- [x] `SetUpscaleMode("lanczos")` round-trip
- [x] `SetUpscaleMode("LANCZOS")` 大小写不敏感
- [x] `SetUpscaleMode("LanCzOs")` 大小写不敏感
- [x] 未识别字符串保持当前 state (与 F.0.9 一致)
- [x] bilinear → bicubic → lanczos → bilinear 三 mode 轮转
- [x] F.0.1+F.0.2+F.0.3+F.0.4+F.0.5+F.0.6+F.0.8+F.0.9+F.0.12+F.0.13+F.0.14 十一启共存

### CI (已验证)
- [x] runtime smoke 35/35 fn (Lua API 不变) + lanczos 测试段
- [x] GitHub Actions 6/6 平台 success ([run 25936869113](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113))

---

## 4. CI 状态（已验证）

| 平台 | 状态 |
|------|------|
| build-windows | ✅ success |
| build-linux | ✅ success |
| build-macos | ✅ success |
| build-android | ✅ success |
| build-ios | ✅ success |
| build-web | ✅ success |

GitHub Run ID: [`25936869113`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25936869113) (修复合集: c5264f2)
F.0.14 原始 commit: `0776e8f` (Lua 白名单漏加 'lanczos', fix `c5264f2`)
Date: 2026-05-15 19:23 UTC

---

## 5. 关键技术决策回顾

### 5.1 为什么用单 pass 25-tap 而非 separable 2-pass

- separable Lanczos-2 = 5+5 fetch + 中间 RT (节省 fetch 但增加 IO + RT 创建复杂度)
- 单 pass 25-tap = 25 fetch + 1 pass (简单，与 F.0.9/F.0.12 同构)
- 25-tap 在桌面 GPU ~0.04 ms 完全可接受；mobile 不推荐用 lanczos

### 5.2 wsum 归一化必要性

Lanczos-2 理论上 sum_w 接近 1，但数值上 ~0.99-1.01，不归一化会出现微弱亮度漂移。Catmull-Rom 不需要因为权重和恒等于 1（多项式精确）。

### 5.3 sinc(0) = 1 处理

`abs(x) < 1e-5` 提前返 1.0，避免 sin(0)/0 NaN。

### 5.4 HDR safe (max(0))

Lanczos kernel 含负 lobe，可能产生 ringing 黑斑（特别 HDR 范围）。`max(rgb, 0)` 截断负值，与 F.0.9 Catmull-Rom 同处理。
