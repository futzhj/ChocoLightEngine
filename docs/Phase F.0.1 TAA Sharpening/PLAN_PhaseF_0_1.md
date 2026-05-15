# Phase F.0.1 TAA Sharpening — PLAN (Align + Architect + Atomize 三合一)

> 6A 工作流 · 阶段 1-3 · Phase F.0 TAA 主管线的视觉补偿优化
> 基线: Phase F.0 commit `6e6ab03` (6/6 CI PASS)
> 目标: TAA 后 unsharp mask 锐化补偿（弥补 sub-pixel 平均带来的高频损失）

---

## §1 Align — 对齐阶段

### 1.1 项目上下文

**Phase F.0 现状**：
- TAA 主管线已交付（13 Lua API, 6/6 CI PASS, history × 2 RGBA16F ping-pong）
- 算法：sub-pixel jittered projection + Halton-2,3 8-sample + 9-tap AABB clip + `mix(cur, hist, α=0.92)`
- **视觉副作用**（业界已知）：alpha 累积本质是高频信息**低通滤波**，输出比 cur 略模糊 5-10%
- **业界标准补偿**：post-TAA unsharp mask sharpening（UE5 / FSR2 / Unity HDRP 通用）

**业界算法横向对比**：

| 方案 | fetch | ALU | 优点 | 缺点 |
|------|-------|-----|------|------|
| 4-tap unsharp mask (上下左右) | 5 | 低 | 简单, 业界主流 | 对角线频率失真 |
| 5-tap CAS (AMD FidelityFX) | 9 | 中 | 各向同性, 质量高 | fetch 翻倍 |
| Laplacian 3×3 kernel | 9 | 中 | 各向同性 | 等同 5-tap CAS |
| Catmull-Rom 2-tap | 5 | 高 | 抗 ringing | 算法复杂 |

**选定方案**：**4-tap unsharp mask**（最简单、最便宜、最业界主流）。

### 1.2 需求规格

| 项 | 值 |
|----|---|
| Phase ID | F.0.1 |
| 范围 | 仅扩展 Phase F.0 TAARenderer + backend，**不破坏** F.0 API |
| 算法 | 4-tap unsharp mask: `sharpened = max(0, c + (c - avg4) × sharpness)` |
| 集成方式 | **替换** TAA 末尾的 `BlitTAAToHDR` 为 `DrawTAASharpenPass`（in-place, 零额外 RT） |
| Lua API 新增 | 2 个 (`SetSharpness` / `GetSharpness`) |
| backend 接口新增 | 2 个 (`SupportsTAASharpen` 复用 `SupportsTAA` / `DrawTAASharpenPass`) |
| 默认值 | `sharpness = 0.5` (中等强度, 视觉差异可感知) |
| clamp | `[0, 2]` |
| sharpness=0 行为 | 等价 BlitTAAToHDR (纯 blit, 不做 sharpen ALU) |
| VRAM 增量 | **0**（in-place 写回 sceneTex, 不开新 RT） |
| GPU 时长增量 | ~0.03 ms @ 1080p (4 fetch + 几个 ALU) |
| 兼容性 | 默认 TAA OFF → sharpness 状态零影响; TAA ON 默认 sharpness=0.5 适度锐化 |

### 1.3 决策矩阵（6 个关键决策，全主动决策）

| # | 决策点 | 候选 | 选定 | 理由 |
|---|--------|------|------|------|
| 1 | 锐化算法 | 4-tap unsharp mask / 5-tap CAS / Laplacian / Catmull-Rom | **4-tap unsharp mask** | 业界最常用, fetch 最少 |
| 2 | 集成方式 | A.替换 BlitTAAToHDR (in-place) / B.独立 post-pass | **A. 替换** | 零额外 RT + 零额外 blit + 替换语义干净 |
| 3 | 锐化作用对象 | A.cur 锐化后再 blend / B.TAA 输出锐化 | **B. TAA 输出** | A 会被 history blend 稀释; B 是业界标准 |
| 4 | sharpness 默认值 | 0.0 (off by default) / 0.3 (UE5 保守) / 0.5 (中等) / 0.8 (激进) | **0.5** | 中等强度, 视觉差异可感知, 不易 ringing |
| 5 | clamp 范围 | `[0, 1]` / `[0, 2]` / `[0, 4]` | **`[0, 2]`** | 与 MotionBlur strength 一致; > 2 易 ringing |
| 6 | sharpness=0 优化 | A.shader 内分支跳过 ALU / B.CPU 端切回 BlitTAAToHDR | **B. CPU 端切回** | 零 shader 分支开销; 1 行 C++ 判断 |

### 1.4 需要用户拍板的决策

**无**。6 个决策全部可基于业界标准 + Phase F.0 一致性主动决策。

---

## §2 Architect — 架构阶段

### 2.1 数据流

```
[Phase F.0 baseline]
TAA blend: cur HDR + history[readIdx] + velocity
   ↓
   → DrawTAAPass → history[writeIdx]
   ↓
   → BlitTAAToHDR(history[writeIdx], hdrFbo, w, h)   ← Phase F.0
   ↓
sceneTex (HDR FBO) ← TAA 输出 (sub-pixel 模糊)

[Phase F.0.1 改造]
TAA blend: 同 F.0
   ↓
   → DrawTAAPass → history[writeIdx]
   ↓
   → if sharpness > 0:
        DrawTAASharpenPass(history[writeIdx], hdrFbo, w, h, sharpness)   ← Phase F.0.1
     else:
        BlitTAAToHDR(history[writeIdx], hdrFbo, w, h)   ← F.0 fallback
   ↓
sceneTex (HDR FBO) ← sharpened TAA 输出 (高频补偿)
```

### 2.2 Shader 设计

**SHARPEN_FS (GLES 3.0 + GL 3.3 两份)**：

```glsl
#version 330 core / #version 300 es
in vec2 vUv;
out vec4 fragColor;
uniform sampler2D uInputTex;    // TAA blend 输出 (history[writeIdx])
uniform vec2 uTexelSize;        // 1.0 / (width, height)
uniform float uSharpness;       // [0, 2]

void main() {
    vec3 c = texture(uInputTex, vUv).rgb;
    // 4-tap unsharp mask: 仅上下左右, 对角线舍弃
    vec3 n = texture(uInputTex, vUv + vec2(0.0,  uTexelSize.y)).rgb;
    vec3 s = texture(uInputTex, vUv - vec2(0.0,  uTexelSize.y)).rgb;
    vec3 e = texture(uInputTex, vUv + vec2(uTexelSize.x, 0.0)).rgb;
    vec3 w = texture(uInputTex, vUv - vec2(uTexelSize.x, 0.0)).rgb;
    vec3 avg4 = (n + s + e + w) * 0.25;
    vec3 sharpened = c + (c - avg4) * uSharpness;
    // 防 ringing: max(0) 防黑色负值; HDR 模式不 clamp 上限 (允许超亮锐化)
    fragColor = vec4(max(sharpened, vec3(0.0)), 1.0);
}
```

### 2.3 Backend 接口契约

**新增 1 个接口**（仅 GL3.3 backend 实现）：

```cpp
// @brief  TAA Sharpen pass: 复用 vaoTonemap, 写到 dstFbo (覆盖 sceneTex slot)
// @param  srcTex      TAA blend 输出 (history[writeIdx])
// @param  dstFbo      HDR FBO (写到 sceneTex slot)
// @param  width/height  RT 尺寸
// @param  sharpness   [0, 2] 锐化强度
virtual void DrawTAASharpenPass(uint32_t srcTex, uint32_t dstFbo,
                                int width, int height, float sharpness) = 0;
```

**`SupportsTAA()` 复用**（Phase F.0 已实现）—— sharpen 与 TAA 同后端能力位，不新增 query。

### 2.4 TAARenderer 状态扩展

```cpp
struct TAARenderer::State {
    // Phase F.0 已有字段...
    float sharpness = 0.5f;   // Phase F.0.1 新增, [0, 2]
};
```

**`Process()` 改造**（仅末尾 1 个分支）：

```cpp
// Phase F.0.1: sharpness > 0 时调 Sharpen pass; == 0 时纯 blit (零 ALU 开销)
if (g.sharpness > 0.0f && g.backend->SupportsTAA()) {
    g.backend->DrawTAASharpenPass(historyWrite, hdrFbo, g.width, g.height, g.sharpness);
} else {
    g.backend->BlitTAAToHDR(historyWrite, hdrFbo, g.width, g.height);
}
```

### 2.5 Lua API 扩展

```lua
-- Phase F.0.1 新增 2 函数
Light.Graphics.TAA.SetSharpness(s)   -- clamp [0, 2], 默认 0.5
Light.Graphics.TAA.GetSharpness()    -- returns number

-- 完整 TAA 子表函数数: 13 + 2 = 15
```

---

## §3 Atomize — 任务拆分

### T1 — backend SHARPEN shader + DrawTAASharpenPass 实现

**输入**: Phase F.0 backend (taa_renderer.h 接口 + render_gl33.cpp DrawTAAPass)
**输出**:
- GLES 3.0 `FS_SHARPEN_SOURCE` 嵌入字符串
- GL 3.3 `FS_SHARPEN_SOURCE` 嵌入字符串
- `programSharpen` + 3 个 uniform locations (`locSharpenInputTex` / `locSharpenTexelSize` / `locSharpenSharpness`)
- Init: 编译 program + 缓存 location + 默认绑 sampler slot 0
- Shutdown: 清理 program + 复位 location 为 -1
- `RenderBackend::DrawTAASharpenPass(srcTex, dstFbo, w, h, sharpness)` 接口声明
- `RenderGL33::DrawTAASharpenPass` 实现（复用 vaoTonemap + 设 texel size + glDrawArrays）

**估时**: 30 分钟

### T2 — taa_renderer 集成 sharpness

**输入**: Phase F.0 taa_renderer.{h,cpp}
**输出**:
- `State::sharpness = 0.5f` 字段
- `SetSharpness(float)` + `GetSharpness() → float` 公共接口（含 clamp）
- `Process()` 末尾 if 分支 (sharpness > 0 走 SharpenPass, 否则纯 Blit)

**估时**: 15 分钟

### T3 — Lua API + smoke + demo + docs

**输入**: Phase F.0 light_graphics.cpp (taa_funcs 13 函数) / scripts/smoke/taa.lua / samples/demo_ssr/main.lua / docs/api/Light_Graphics.md
**输出**:
- 2 个 wrapper 函数 (`l_TAA_SetSharpness` / `l_TAA_GetSharpness`)
- `taa_funcs[]` 数组加 2 行
- smoke 加 §11 sharpness round-trip + clamp + 默认值检查
- demo_ssr 加 'H' 键 ±0.1 调节 sharpness + HUD 显示
- `Light_Graphics.md` TAA 章节增加 `SetSharpness/GetSharpness` 文档段（含取值建议表）

**估时**: 25 分钟

### T4 — 6A 文档三件套

**输入**: PLAN（本文件）
**输出**:
- `ACCEPTANCE_PhaseF_0_1.md` (验收 + 决策对齐 + 技术洞察)
- `FINAL_PhaseF_0_1.md` (一句话总结 + 数据流 + Phase F.0 关系 + 风险)
- `TODO_PhaseF_0_1.md` (必做 + 候选 + CI 回填)

**估时**: 20 分钟

### T5 — commit + push + CI

**输入**: 完整修改
**输出**:
- git add + commit + push
- CI 6/6 平台 success 验证
- 三份 6A 文档 CI 状态回填

**估时**: 15 分钟（含 CI 等待）

**总估时**: ~1h45m

### 任务依赖图

```
T1 (backend)
   ↓
T2 (taa_renderer 集成)
   ↓
T3 (Lua API + smoke + demo + docs)
   ↓
T4 (6A 文档)
   ↓
T5 (commit + CI)
```

无并行任务，严格串行。

---

## §4 Approve — 检查清单

### 4.1 完整性
- [x] 决策矩阵 6/6 全主动决策
- [x] 算法、集成方式、shader、backend 接口、Lua API 全部明确
- [x] 数据流图 + 任务拆分 + 估时

### 4.2 与 Phase F.0 一致性
- [x] 复用 backend `SupportsTAA()` query
- [x] 复用 `vaoTonemap` + `LoadProgramAndUniform` 模式
- [x] 复用 Phase F.0 子表 (`Light.Graphics.TAA.*`) 命名空间
- [x] 替换 `BlitTAAToHDR` 而非新增 RT (零 VRAM 增量)

### 4.3 风险与缓解

| 风险 | 缓解 |
|------|------|
| sharpness 过高产生 ringing | 默认 0.5 保守; 文档警告 > 1.5 易 ringing |
| HDR mode 锐化产生超亮 firefly | `max(0)` 防负值; 不 clamp 上限保留 HDR; F.0.4 anti-flicker 独立处理 |
| GLES 2 / WebGL1 不支持 | `SupportsTAA()` = false 自动 fallback |
| sharpness=0 时仍跑 sharpen shader | CPU 端 if 分支跳过, 走纯 blit 路径 |

### 4.4 验收标准

1. **功能**: TAA enable + sharpness=0.5 时输出比 sharpness=0 (baseline) 边缘更锐
2. **Lua API**: `SetSharpness(2.5)` clamp 到 2.0; `SetSharpness(-1)` clamp 到 0.0
3. **smoke**: 13 函数 + 2 新增 = 15 函数 surface check; round-trip; clamp
4. **CI**: 6/6 平台 success
5. **回归**: Phase F.0 13 函数行为零变化; Phase E.x 全模块零回归

---

## §5 实施开始

按 T1 → T2 → T3 → T4 → T5 顺序执行。无中断条件触发即顺序完成。
