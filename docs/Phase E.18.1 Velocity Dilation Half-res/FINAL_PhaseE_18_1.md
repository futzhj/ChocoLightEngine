# Phase E.18.1 Velocity Dilation Half-Resolution — FINAL

> 6A 工作流 · 阶段 6 · Assess · 项目总结
> 基线：Phase E.18 commit `8b7c25b`

---

## 1. 项目概述

将 Phase E.18 的独立 dilation pass RT 改为**半分辨率**：

- VRAM 节省 **75%**（1080p：dilated RT 16MB → 4MB）
- dilation pass 自身性能 **+4×**（fragment count -75%）
- 视觉差异：邻域物理覆盖由 3 raw px 扩到 6 raw px（max-filter 自动鲁棒，几乎无视觉差异）
- **零 API 破坏性变更**：新增 1 对 `HDR.SetVelocityDilationHalfRes` / `GetVelocityDilationHalfRes` Lua API
- **默认 OFF**（halfRes=false 时行为完全等价 Phase E.18）

---

## 2. 代码 / 文档统计

### 代码改动

| 类别 | 文件 | 行数（+ / -） |
|------|------|--------------|
| C++ Header | `include/render_backend.h` | +14 / -8 |
| C++ Header | `include/hdr_renderer.h` | +10 / 0 |
| C++ Source | `src/render_gl33.cpp` | +10 / -5 |
| C++ Source | `src/hdr_renderer.cpp` | +90 / -8 |
| C++ Source | `src/light_graphics.cpp` | +30 / 0 |
| Lua smoke | `scripts/smoke/hdr.lua` | +50 / -1 |
| Lua demo | `samples/demo_ssr/main.lua` | +14 / -2 |
| Markdown | `docs/api/Light_Graphics.md` | +96 / -1 |
| **代码合计** | — | **+314 / -25** |

### 文档新增

| 文件 | 行数 |
|------|------|
| `docs/Phase E.18.1/ALIGNMENT_PhaseE_18_1.md` | ~190 |
| `docs/Phase E.18.1/DESIGN_PhaseE_18_1.md` | ~260 |
| `docs/Phase E.18.1/TASK_PhaseE_18_1.md` | ~170 |
| `docs/Phase E.18.1/ACCEPTANCE_PhaseE_18_1.md` | ~170 |
| `docs/Phase E.18.1/FINAL_PhaseE_18_1.md` | 本文件 |
| `docs/Phase E.18.1/TODO_PhaseE_18_1.md` | 待补 |
| **文档合计** | **~990** |

---

## 3. 关键技术亮点

### 3.1 接口扩展最小化

`render_backend.h` 仅扩 2 个接口（CreateVelocityDilateRT + DrawVelocityDilate）加 sw/sh 参数。新增 1 对 Lua API。其他接口（含 6 个 Phase E.18 dilation 接口中其余 4 个）零改动。

### 3.2 uTexel 物理直觉一致

`uTexel = 1.0 / vec2(sw, sh)` 跟随实际 viewport，shader 内 9-tap 在 raw velocity space 邻域物理覆盖：

- full-res: 3 raw 像素
- half-res: 6 raw 像素

max-filter 算法本身对覆盖扩大有自然鲁棒性（取最长 velocity 永远等于或大于子集最大值），故 visual no-op 安全保障。

### 3.3 HDRRenderer 内独立 Release/Rebuild

`ReleaseDilationRT` / `RebuildDilationRT` 仅处理 dilation 部分，避免切换 halfRes 时重建整个 HDR FBO（含 scene/normal/velocity MRT，重建代价更高）。与 Phase E.17 motion blur halfRes 重建模式一致。

### 3.4 三层 silent fallback 保留

1. backend 不支持 dilation → halfRes 无影响
2. dilation OFF → dilatedFbo 未创建 → halfRes 无影响
3. CreateVelocityDilateRT 失败 → consumer fallback raw + inline 9-tap

业务层完全零感知，与 Phase E.18 的容错策略一致。

### 3.5 默认 OFF 零回归保障

`dilationHalfRes=false` 时 sw=w, sh=h → backend 路径与 Phase E.18 完全等价；连 log 输出格式都保持兼容（仅多了 "halfRes=OFF" 标记）。

---

## 4. Phase E 系列累计（HDR 链路）

| Phase | 模块 | Lua API 数 | 主要交付 |
|-------|------|------------|---------|
| E.3 | HDR + 4 tonemap | 12 | ACES / Reinhard / Uncharted2 / Linear |
| E.4 | Bloom pyramid | 15 | 6-mip ping-pong |
| E.5 | Auto Exposure | 18 | Eye Adaptation + 8-bit lum mip |
| E.6 | Lens Dirt + Streak | 23 | Anamorphic flare |
| E.7 | Lens Flare | 21 | Ghost + Halo + CA |
| E.8 | SSAO | 19 | 16-sample hemisphere + bilateral blur |
| E.9~E.12 | SSR | 22 | reflect + blur + temporal |
| E.13~E.14 | Velocity buffer | — | RG16F/RG8 + dilation + reproject |
| E.15 | Motion Blur | 11 | velocity-driven per-pixel |
| E.16 | Camera-only Motion Blur | 2 | mode 0/1/2 + dual MRT |
| E.17 | Half-res Motion Blur | 2 | VRAM -75% / perf ~4× |
| E.18 | Velocity Dilation Pass | 0 | shared 9-tap, ~50% fetch save |
| **E.18.1** | **Velocity Dilation Half-res** | **2** | **VRAM -75% / dilation perf +4×** |
| **累计** | **HDR 12+ 剑客** | **147** | **后处理管线持续优化** |

---

## 5. Phase E.17 / E.18 / E.18.1 三阶段对比

```
E.17 motion blur halfRes
   - 谁半分辨率: motionBlurTex
   - 收益: VRAM -75% (motion blur 自身) + perf ~4× (motion blur shader)
   - 视觉: 1px 轻微上采模糊
   ↓
E.18 dilation pass 抽出
   - 谁创建: dilatedVelocityTex / dilatedCameraVelocityTex (full-res)
   - 收益: 多 consumer 场景 velocity fetch -50% (避免重复 9-tap)
   - 视觉: 零差异（算法等价）
   ↓
★ E.18.1 dilation pass halfRes ★
   - 谁半分辨率: dilatedVelocityTex + dilatedCameraVelocityTex
   - 收益: VRAM -75% (dilation 自身) + perf +4× (dilation pass)
   - 视觉: max-filter 邻域物理覆盖扩 2× (自动鲁棒)
```

三阶段正交、独立，可任意组合。例如：
- 移动端配置：HDR + dilation + motion blur halfRes + dilation halfRes（全开省）
- 桌面 4K 配置：HDR + dilation + dilation halfRes（dilation 受益最大，motion blur 可保 full-res）

---

## 6. Lua API 累积（Phase E.18.1 新增 2 个）

新增：
- `Light.Graphics.HDR.SetVelocityDilationHalfRes(bool) → bool / nil+err`
- `Light.Graphics.HDR.GetVelocityDilationHalfRes() → bool`

行为：
- 默认 false（Phase E.18 行为完全保留）
- 仅在 dilation pass 启用时生效（SetVelocityDilation(true) + backend 支持）
- 切换时立即重建 dilated RT（已 Enable 时）

---

## 7. 已知限制 / 未来方向

### 短期可选项

1. **真机 GPU profile 实测**：当前性能数据基于理论（fragment count × fetch），建议用 Tracy / RGP / RenderDoc 实测
2. **dilation halfRes 自动判定**：根据分辨率自动决定（4K 默认开、720p 默认关）

### 长期可选项

3. **E.18.2 运行时智能 single-consumer skip**：仅 SSR 或仅 Motion Blur 启用时自动跳过 dilation pass
4. **E.18.3 接入 TAA 主管线**：dilation 输出可服务于未来 TAA
5. **E.18.4 细粒度 toggle**：SSR/MB 独立控制 dilation 启用

---

## 8. CI 状态

| 平台 | 状态 | 备注 |
|------|------|------|
| build-windows | ✅ success | runtime smoke 25 PASS (hdr.lua 18 fn) + 17 phase 零回归 |
| build-linux | ✅ success | 纯构建 |
| build-macos | ✅ success | 纯构建 |
| build-android | ✅ success | 纯构建 |
| build-ios | ✅ success | 纯构建 |
| build-web | ✅ success | Emscripten WASM |

GitHub Run ID: [`25901596673`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25901596673)
Commit hash: `254984f`
Date: 2026-05-15 05:14 UTC → 05:20 UTC
Total duration: **6 min** (6/6 平台并行)

---

## 9. 工程反思

### 做得好的地方

- **接口扩展最小化**：仅扩 2 个 backend 接口的 sw/sh 参数；不动 dilation 算法 / shader / consumer 路径
- **完整三层 silent fallback 保留**：从 Phase E.18 继承的容错保护未破坏
- **默认 false 零回归保障**：halfRes=false 时与 Phase E.18 完全等价
- **6A 文档完备**：~990 行覆盖 align→architect→atomize→approve→automate→assess
- **与 Phase E.17 motion blur halfRes 路径一致**：用户认知一致（key `[`=MB halfRes, `]`=dilation halfRes）

### 可改进点

- **uTexel 不同于 motion blur halfRes**：motion blur halfRes 保 full-res uTexel，dilation halfRes 用 half-res uTexel；虽各自合理但用户文档需明确说明
- **未做单 consumer 自动判断**：用户需手动判断 halfRes 是否开启（建议留 E.18.2 实现）

### 经验沉淀

- **半分辨率优化的 RT/shader 关系分两类**：reconstruction-style（如 motion blur）用 full-res uTexel；filter-style（如 dilation）用 half-res uTexel
- **接口扩参数比加新接口更友好**：CreateVelocityDilateRT 加 sw/sh 比新增 CreateVelocityDilateRTHalfRes 更易维护
- **状态机的 no-op 短路重要**：SetVelocityDilationHalfRes 在同值切换时跳过重建，避免无意义 GPU 状态变化
