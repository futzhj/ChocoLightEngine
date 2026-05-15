# Phase F.0.5 TAA Half-Res History — PLAN

> 6A 工作流 · 阶段 1 (Align) + 阶段 2 (Architect) + 阶段 3 (Atomize) 合并
> 基线：Phase F.0 + F.0.1 + F.0.2 + F.0.3 + F.0.4 (commit `15b0db7` + `e429022`)

---

## 1. Align — 对齐阶段

### 1.1 项目上下文

- **TAA 主管线已上线**：F.0 (jitter + reproject + clip + blend) → F.0.1 (sharpening) → F.0.2 (YCoCg AABB) → F.0.3 (variance) → F.0.4 (anti-flicker)
- **当前 history RT 实现**: `RGBA16F × 2 ping-pong`，与 sceneTex 同尺寸 (full-res)
- **接口契约**: `RenderBackend::CreateTAAHistoryRT(w, h, ...)` 创建 + `DeleteTAAHistoryRT` 释放
- **VRAM 占用 (1080p)**: `2 × 1920 × 1080 × 8 (RGBA16F = 4 chan × 2B) = 33.2 MB`
- **VRAM 占用 (4K)**: `2 × 3840 × 2160 × 8 = 132.7 MB` ← 移动端紧张

### 1.2 任务边界 (CONSENSUS)

| 范围 | 是否 |
|------|------|
| 增加 `Light.Graphics.TAA.SetHalfResHistory(bool)` / `GetHalfResHistory()` | ✅ |
| TAA history RT 半分辨率 (w/2, h/2) RGBA16F × 2，VRAM -75% | ✅ |
| 默认 OFF，零回归 (老用户走 full-res 路径完全不变) | ✅ |
| 切换 halfRes 时立即重建 RT + invalidate hasHistory (避免分辨率不匹配 reproject) | ✅ |
| `BlitTAAToHDR` 扩展支持 src→dst stretch + GL_LINEAR (上采样) | ✅ |
| 提供 fragment shader 上采样 (`DrawTAASharpenPass` 走 sample，自然支持) | ✅ |
| 实现独立 downsample pass (写回 history 时降分辨率) | ❌ — 直接 TAA pass 在 half-res 渲染 |
| 实现 TAAU (上采样到更高分辨率) | ❌ — 留给 Phase F.1 |
| 实现 motion-adaptive halfRes 切换 | ❌ — 静态用户控制 |

### 1.3 决策矩阵 (8/8 自动决策)

| # | 决策点 | 选择 | 理由 |
|---|--------|------|------|
| D1 | 实现方案 | **TAA pass 在 half-res 渲染** | UE4 早期 TAA 标准，零 downsample pass，shader 不变 |
| D2 | 邻域 clip 精度 | **接受 box-filtered** | sceneTex sample 用 GL_LINEAR 自动 box filter，firefly 反而被预压制 |
| D3 | 默认值 | **`false` (full-res)** | 零回归，与 F.0/0.1/0.2/0.3/0.4 行为一致 |
| D4 | 上采样滤镜 | **GL_LINEAR (bilinear)** | shader sample 自动支持；BlitTAAToHDR 改 GL_LINEAR；性能 ≈ free |
| D5 | 切换时机 | **立即重建 RT + invalidate hasHistory** | 避免分辨率不匹配的 history reproject 导致花屏 |
| D6 | half-res 比例 | **1/2 each axis (1/4 像素)** | UE4/UE5 标准；VRAM -75% / 性能 +25% |
| D7 | API 名 | **SetHalfResHistory** | 与 `MotionBlur.SetHalfRes` 对齐 (项目已有同名约定) |
| D8 | 接口扩展 | **BlitTAAToHDR(srcW, srcH, dstW=0, dstH=0)** | 默认参数 = src 同 dst (向后兼容)；非零 dst 走 stretch + LINEAR |

### 1.4 假设 / 不确定性

| # | 假设 | 验证方式 |
|---|------|---------|
| A1 | sceneTex 用 GL_LINEAR sampler (TAA shader 邻域 fetch 自动 box filter) | 已确认: HDR sceneTex 创建时 GL_LINEAR (Phase E) |
| A2 | `glBlitFramebuffer` 支持 stretch + LINEAR 跨 RGBA16F | 已确认: GL3.3+ 标准支持 |
| A3 | `DrawTAASharpenPass` 上采样 history (half-res) → sceneTex (full-res) 不需修改 | 已确认: viewport 用 dstFbo 尺寸, srcTex sample GL_LINEAR 自动上采样 |
| A4 | velocity buffer / dilatedVelocity 用 full-res grid 是否影响 half-res TAA pass | 不影响: shader 内 vUV [0,1] 归一化, sample 用 GL_LINEAR |

---

## 2. Architect — 架构设计

### 2.1 数据流图

```
┌────────────────────────────────────────────────────────────────┐
│ Phase F.0.5 OFF (default, full-res):                           │
│                                                                │
│   sceneTex(full)  ─┐                                           │
│                    ├─→ TAA pass → history[writeIdx] (full-res)│
│   history[read]   ─┘   viewport (full-w, full-h)              │
│                            │                                   │
│                            └→ BlitTAAToHDR/Sharpen → sceneTex │
└────────────────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ Phase F.0.5 ON (halfResHistory=true):                          │
│                                                                │
│   sceneTex(full)  ─┐ GL_LINEAR sample (auto box filter)        │
│                    ├─→ TAA pass → history[writeIdx] (HALF-res)│
│   history[read]   ─┘   viewport (half-w, half-h) = (w/2, h/2) │
│                            │                                   │
│                            └→ BlitTAAToHDR/Sharpen           │
│                               (stretch upsample GL_LINEAR)   │
│                               → sceneTex (full-res)           │
└────────────────────────────────────────────────────────────────┘
```

### 2.2 接口契约变化

#### `render_backend.h::BlitTAAToHDR` 扩展（向后兼容）

```cpp
// 老签名 (Phase F.0):
//   BlitTAAToHDR(srcTex, dstFbo, w, h)
//     → src 与 dst 同尺寸, GL_NEAREST (full-res blit)
//
// 新签名 (Phase F.0.5):
//   BlitTAAToHDR(srcTex, dstFbo, srcW, srcH, dstW=0, dstH=0)
//     → dstW=0 || dstH=0: 退化为 src 同 dst (老行为, 兼容)
//     → 非零: stretch blit src(srcW,srcH) → dst(dstW,dstH), GL_LINEAR
virtual void BlitTAAToHDR(uint32_t srcTex, uint32_t dstFbo,
                          int srcW, int srcH,
                          int dstW = 0, int dstH = 0) {}
```

#### `taa_renderer.h` 新增

```cpp
namespace TAARenderer {
    // Phase F.0.5: 切换 history RT 是否半分辨率
    // 默认 false (零回归); true 时 history RT 尺寸 (w/2, h/2), VRAM -75%
    void SetHalfResHistory(bool on);
    bool GetHalfResHistory();
}
```

#### `light_graphics.cpp` 新增 Lua API

```lua
Light.Graphics.TAA.SetHalfResHistory(bool) -- F.0.5
Light.Graphics.TAA.GetHalfResHistory() → bool
```

TAA 子表 21 → 23 fn。

### 2.3 状态机

```
            ┌─ enabled=false ─┐
SetHalfRes  │                 │  (no rebuild, only state)
   ↓        │                 │
[新值 = 旧值] → no-op
[新值 ≠ 旧值] → 修改 g.halfResHistory
              ↓
        [enabled?]
         ├─ no  → 仅修改 state
         └─ yes → ReleaseRT + AllocateRT(w/2, h/2 or w, h)
                  + g.hasHistory = false (invalidate)
```

### 2.4 模块依赖图

```
┌──────────────────┐
│  light_graphics  │  Lua API: SetHalfResHistory + GetHalfResHistory
│  (taa_funcs)     │
└────────┬─────────┘
         ↓
┌──────────────────┐
│  TAARenderer     │  state.halfResHistory + Set/Get + historyW/H()
│                  │  Process: viewport (hw, hh) instead of (w, h)
└────────┬─────────┘
         ↓
┌──────────────────┐
│  RenderBackend   │  CreateTAAHistoryRT(w/2, h/2) — 接口零变动
│                  │  BlitTAAToHDR(srcW, srcH, dstW, dstH) — 加 dst 默认参数
└──────────────────┘
```

### 2.5 异常处理

| 场景 | 处理 |
|------|------|
| `SetHalfResHistory(true)` 但 `enabled=false` | 仅修改 state，下次 Enable 自动用 half-res |
| Enable 时 AllocateRT(w/2, h/2) 失败 | 走原有 fallback 路径，enabled=false |
| Resize(w, h) 时 halfResHistory=true | 内部传 (w/2, h/2) 给 AllocateRT |
| 切换 halfResHistory 时 `g.hasHistory = false` | 下一帧 TAA 会写 history 但不 read，避免分辨率不匹配 reproject |

---

## 3. Atomize — 原子化

### 3.1 任务依赖图

```
T1 (shader/backend program)
  - 无新 shader (TAA shader 不变)
  - BlitTAAToHDR 扩展 stretch + LINEAR
       ↓
T2 (backend impl)
  - render_gl33.cpp BlitTAAToHDR 加 srcW/srcH/dstW/dstH 实现
  - render_backend.h 扩展默认参数
       ↓
T3 (TAARenderer)
  - taa_renderer.h: 加 SetHalfResHistory / GetHalfResHistory + state
  - taa_renderer.cpp: 加 halfResHistory + historyW_/historyH_ 内部辅助
  - Process 内 DrawTAAPass 用 (hw, hh)，BlitTAAToHDR 用 (hw, hh, w, h)
  - SetHalfResHistory 实现重建 RT + invalidate
       ↓
T4 (Lua + smoke + demo + docs)
  - light_graphics.cpp: l_TAA_SetHalfResHistory / l_TAA_GetHalfResHistory
  - taa_funcs[] 21 → 23
  - taa.lua: surface 23 fn + halfRes round-trip + type-error
  - demo_ssr/main.lua: 加 X 键切换 halfResHistory + HUD 显示
  - Light_Graphics.md: 速查表 21 → 23 + SetHalfResHistory 完整文档段
       ↓
T5 (6A 文档)
  - ACCEPTANCE / FINAL / TODO
       ↓
T6 (commit + push + CI)
```

### 3.2 原子任务详述

#### T1 (Shader / Backend prog) — 极小

- **新 shader**: 无 (TAA shader 不变, sharpen shader 不变)
- **`BlitTAAToHDR` 实现改造**: GL33 后端 `glBlitFramebuffer` 加 GL_LINEAR + 支持非零 dst stretch
- **接口契约**: `render_backend.h` 加 dstW/dstH 默认参数 = 0

#### T2 (Backend impl)

- `render_gl33.cpp::BlitTAAToHDR(srcTex, dstFbo, srcW, srcH, dstW=0, dstH=0)`：
  - 若 `dstW <= 0 || dstH <= 0`: dstW = srcW, dstH = srcH (老行为)
  - 若 srcW != dstW || srcH != dstH: GL_LINEAR (stretch); 否则 GL_NEAREST (1:1 老行为)
  - `glBlitFramebuffer(0, 0, srcW, srcH, 0, 0, dstW, dstH, GL_COLOR_BUFFER_BIT, filter)`

#### T3 (TAARenderer)

- `taa_renderer.h`：
  - 加 `void SetHalfResHistory(bool on)` / `bool GetHalfResHistory()` 声明
- `taa_renderer.cpp`：
  - state 加 `bool halfResHistory = false`
  - 内部辅助 `historyW_()` / `historyH_()`：halfResHistory 时返回 max(w/2, 1)，否则 w
  - `Enable(w, h)`：内部传 historyW/H 给 AllocateRT
  - `Process`：
    - `DrawTAAPass(..., historyW, historyH, ...)` 改为 history 尺寸
    - `BlitTAAToHDR(..., historyW, historyH, width, height)` (sharpness=0 路径)
    - `DrawTAASharpenPass(..., width, height, ...)` 不变 (走 fragment shader 上采样)
  - `SetHalfResHistory(bool)`：
    - 若 on == g.halfResHistory: no-op
    - 修改 state
    - if g.enabled: ReleaseRT + AllocateRT(historyW, historyH) + g.hasHistory = false
  - `Resize(w, h)`：内部 historyW/H 自动适配

#### T4 (Lua + smoke + demo + docs)

- `light_graphics.cpp`：
  - `l_TAA_SetHalfResHistory`：`luaL_checktype(L, 1, LUA_TBOOLEAN)`，调用 `SetHalfResHistory(lua_toboolean)`，push true
  - `l_TAA_GetHalfResHistory`：`lua_pushboolean(L, GetHalfResHistory())`
  - `taa_funcs[]` 21 → 23
- `scripts/smoke/taa.lua`：
  - `fn_names` 加 `SetHalfResHistory` / `GetHalfResHistory`
  - 默认值 = false 验证
  - round-trip true/false
  - type-error: SetHalfResHistory("true") raise / nil raise
  - 切换时不影响其他状态 (alpha / clipMode / antiFlicker / sharpness)
  - 末尾计数 23 / 23
- `samples/demo_ssr/main.lua`：
  - X 键切换 halfResHistory + 打印
  - HUD 加 `halfRes=ON/OFF` 字段
- `docs/api/Light_Graphics.md`：
  - 速查表 21 → 23 行
  - 新增 SetHalfResHistory / GetHalfResHistory 完整文档段（性能 / VRAM 计算 / 推荐场景 / 与 F.0.1 sharp 协同）

#### T5 (6A 文档)

- `ACCEPTANCE_PhaseF_0_5.md` / `FINAL_PhaseF_0_5.md` / `TODO_PhaseF_0_5.md`

#### T6 (CI)

- commit + push origin main
- `gh run view` 监控 6/6 平台 success
- 回填 ACCEPTANCE / FINAL / TODO

---

## 4. 性能预期

| 指标 | full-res (default) | half-res (F.0.5 ON) | 增量 |
|------|---------------------|----------------------|------|
| VRAM @ 1080p | 33.2 MB (2 RT) | 8.3 MB | **-74.9%** |
| VRAM @ 4K | 132.7 MB | 33.2 MB | **-74.9%** |
| TAA pass 像素数 | 全分辨率 (W×H) | 1/4 (W/2 × H/2) | **-75%** |
| TAA pass 时间 | 0.10 ms (estimate) | ~0.04 ms | -60% |
| BlitTAAToHDR 时间 | <0.01 ms | ~0.02 ms (stretch + LINEAR) | +0.01 ms |
| Sharpen pass | 0.03 ms (full-res) | 0.03 ms (full-res, 不变) | 0 |
| **总体 TAA 开销** | **~0.13 ms** | **~0.09 ms** | **-30%** |

### 视觉影响

- **history bilinear 上采样**: 引入 ~1px 模糊；F.0.1 sharpening 默认 0.5 完全弥补
- **邻域 clip 精度**: sceneTex GL_LINEAR sample → 9-tap 邻域看到的是 box-filtered 邻域，dynamic range 略小，但 firefly 被预压制
- **静止图像**: 视觉差异不可察觉（特别 sharpness > 0 时）
- **快速运动**: trail 略宽（half-res clip 比全分辨率宽容），但仍优于无 TAA

---

## 5. 风险评估

| 风险 | 严重度 | 缓解措施 |
|------|--------|---------|
| 切换 halfRes 时 history 分辨率不匹配 → 花屏 1 帧 | 中 | `g.hasHistory = false` invalidate，下一帧从干净状态重建 history |
| Resize 与 halfRes 切换交互 | 低 | Resize 内部走 Enable，自动按当前 halfRes state 重建 |
| `glBlitFramebuffer` stretch + RGBA16F 兼容性 | 低 | GL3.3 标准支持，已被各平台 RT format 测试覆盖 |
| 整数除法 `w/2` 在奇数 w 时 round down | 低 | `max(w/2, 1)` 防止 0 |
| 视觉退化超出预期 | 中 | 默认 OFF + 用户主动 opt-in；F.0.1 sharpening 弥补模糊 |

---

## 6. 验收标准

| 项 | 标准 |
|----|------|
| Lua API | `Light.Graphics.TAA.SetHalfResHistory` / `GetHalfResHistory` 存在且类型为 function；taa_funcs[] 增到 23 |
| 默认值 | `GetHalfResHistory()` 返 false |
| Round-trip | true/false 切换正确反映 |
| Type-error | SetHalfResHistory("foo") raise (与 SetNeighborhoodClip 同模式) |
| 状态独立 | 切换 halfRes 不影响 alpha / clipMode / sharpness 等其他参数 |
| 与现有功能正交 | F.0.1+0.2+0.3+0.4 + halfRes 五启共存测试通过 |
| Backend 安全 | 切换 halfRes 时 `g.hasHistory = false` 重置 |
| CI | 6/6 平台 success + Windows runtime smoke 23/23 fn |
| 文档 | speedhash 表 + 完整 API 文档段 + 性能 / VRAM 数据表 |

---

## 7. 实施顺序

1. **T1+T2** (backend `BlitTAAToHDR` 扩展) ← 起点，下游依赖
2. **T3** (TAARenderer state + Set/Get + Process viewport) ← 主功能
3. **T4** (Lua + smoke + demo + docs)
4. **T5** (6A 文档三件套)
5. **T6** (commit + push + CI)

预估累计：~3-4h
