# Phase F.0.10.2 — TODO 与用户指引

> 6A 工作流 · 阶段 6 (Assess) · TODO
> 关联: FINAL_PhaseF_0_10_2.md

---

## 1. 用户立即可用 (无任何 TODO)

Phase F.0.10.2 已完成, 用户**立即可用**以下能力:

### 1.1 API 完整可用 (45 fn, +5 新增)

| 新 API | 用途 |
|--------|------|
| `Light.Graphics.SetViewport(x, y, w, h)` | 限制 raster 子矩形 (split-screen / picture-in-picture 必备) |
| `Light.Graphics.GetViewport()` | 查当前 viewport (返 4 integer) |
| `Light.Graphics.HDR.SetAutoTAA(bool)` | 控 EndScene 是否自动 TAA (默认 true 零回归) |
| `Light.Graphics.HDR.GetAutoTAA()` | 查当前 autoTAA 状态 |
| `Light.Graphics.TAA.Process()` / `TAA.Process(x, y, w, h)` | 手动 TAA 触发 (可选 region) |

### 1.2 Demo 可用 (4 个 TAA demo)

| Demo | 路径 | 价值 |
|------|-----|------|
| `samples/demo_taa_compare/` | F.0 ~ F.0.14 8 个 preset 对比 | 单 instance 不同参数视觉对比 |
| `samples/demo_ssr/` (按 C 键) | F.0.10 multi-instance timeline cycle | 4 instance 创建/切换/销毁验证 |
| `samples/demo_taa_split/` | F.0.10.1 4 instance profile timeline | multi-instance API 流程演示 |
| `samples/demo_taa_split2/` | **F.0.10.2 真物理 split-screen** | 同帧双 player + 双 TAA instance + region |

---

## 2. 用户操作指引

### 2.1 启用真物理 split-screen 的最小代码

```lua
-- 一次性初始化
local HDR, TAA, Gfx = Light.Graphics.HDR, Light.Graphics.TAA, Light.Graphics
HDR.Enable(W, H)
HDR.SetAutoTAA(false)           -- 关 EndScene 自动 TAA
local p1 = TAA.CreateInstance() -- player 1 instance
local p2 = TAA.CreateInstance() -- player 2 instance
TAA.SetActiveInstance(p1); TAA.Enable(W, H)  -- p1 history
TAA.SetActiveInstance(p2); TAA.Enable(W, H)  -- p2 history

-- 每帧
HDR.BeginScene()

-- Left half (player 1)
Gfx.SetViewport(0, 0, W/2, H)
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
Gfx.SetCamera(...)
-- draw scene from player 1 perspective
TAA.Process(0, 0, W/2, H)

-- Right half (player 2)
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
Gfx.SetCamera(...)
-- draw scene from player 2 perspective
TAA.Process(W/2, 0, W/2, H)

Gfx.SetViewport(0, 0, W, H)
HDR.EndScene()
```

### 2.2 配置每 player 不同 TAA 风格

```lua
-- 切到 p1, 设强 sharpen + RCAS
TAA.SetActiveInstance(p1)
TAA.SetSharpness(1.5)
TAA.SetSharpenMode('rcas')

-- 切到 p2, 设柔和 + Lanczos halfRes
TAA.SetActiveInstance(p2)
TAA.SetSharpness(0)
TAA.SetHalfResHistory(true)
TAA.SetUpscaleMode('lanczos')
```

### 2.3 4-player split-screen (2x2 grid, 用户可立即扩展)

```lua
local W, H = 1920, 1080
local p1 = TAA.CreateInstance()  -- top-left
local p2 = TAA.CreateInstance()  -- top-right
local p3 = TAA.CreateInstance()  -- bottom-left
local p4 = TAA.CreateInstance()  -- bottom-right

-- 注意: 4 instance × 2 history × 1920×1080×8B = ~132MB VRAM, 1080p 可承受

-- 每帧 4 个 region:
local regions = {
    {p1, 0,     H/2, W/2, H/2}, -- top-left  (GL origin 左下, y=H/2 = 上半)
    {p2, W/2,   H/2, W/2, H/2}, -- top-right
    {p3, 0,     0,   W/2, H/2}, -- bottom-left
    {p4, W/2,   0,   W/2, H/2}, -- bottom-right
}
for _, r in ipairs(regions) do
    local id, x, y, rw, rh = r[1], r[2], r[3], r[4], r[5]
    Gfx.SetViewport(x, y, rw, rh)
    TAA.SetActiveInstance(id); TAA.ApplyJitter()
    Gfx.SetCamera(...)  -- 对应 player 相机
    -- draw scene
    TAA.Process(x, y, rw, rh)
end
```

---

## 3. 常见错误避免

### 3.1 忘记关 autoTAA → 双 TAA process

```lua
-- ❌ 错误: autoTAA 默认 true, EndScene 仍会调一次 TAA
TAA.Process(0, 0, W/2, H)
HDR.EndScene()  -- 这里又跑 TAARenderer::Process(...) 全屏, history 被覆盖

-- ✅ 正确
HDR.SetAutoTAA(false)
TAA.Process(0, 0, W/2, H)
HDR.EndScene()  -- 跳过 TAA, 用户的 region process 生效
```

### 3.2 没切 ApplyJitter → 双 instance jitter 错误

```lua
-- ❌ 错误: 只 SetActiveInstance, 没 ApplyJitter, 用上一个 instance 的 jitter
TAA.SetActiveInstance(p2)
-- 直接 raster: 用 p1 的 jitter !
Gfx.SetCamera(...)
draw_player2()
TAA.Process(W/2, 0, W/2, H)

-- ✅ 正确: SetActiveInstance + ApplyJitter
TAA.SetActiveInstance(p2)
TAA.ApplyJitter()           -- 重置为 p2 自己的 halton 偏移
Gfx.SetCamera(...)
draw_player2()
TAA.Process(W/2, 0, W/2, H)
```

### 3.3 忘记复位 viewport → HDR.EndScene tonemap 区域错

```lua
-- ❌ 错误: tonemap 只写右半屏, 左半屏黑
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.Process(W/2, 0, W/2, H)
HDR.EndScene()  -- viewport 仍是右半, tonemap 输出右半, 左半保持上帧画面

-- ✅ 正确: 复位 viewport 为全屏
Gfx.SetViewport(W/2, 0, W/2, H); TAA.Process(W/2, 0, W/2, H)
Gfx.SetViewport(0, 0, W, H)     -- 复位
HDR.EndScene()
```

### 3.4 region (w, h) 负数 / 0 拒绝

```lua
-- ❌ 错误: TAA.Process 抛 nil + err "w/h must be >= 0"
TAA.Process(0, 0, -100, 100)

-- ✅ 正确: 全屏走 0/0/0/0 (或不传)
TAA.Process()  -- 等价 TAA.Process(0, 0, 0, 0)
TAA.Process(0, 0, W/2, H)
```

---

## 4. 待办事项 (用户层面, 无引擎 TODO)

### 4.1 配置环境

- [ ] 确保使用 Lua 5.1 (Light 引擎运行时, `//` 整除不支持, 用 `math.floor`)
- [ ] 跑 `samples/demo_taa_split2/main.lua` 验证 GL context 工作
- [ ] 启用 HDR 之前确认 `HDR.IsSupported()` 返 true (GL33 backend, 不支持 Legacy)

### 4.2 集成到自己项目

- [ ] 复制 demo_taa_split2/main.lua 的核心循环结构
- [ ] 调整 player 相机参数 (eye/at) 适配自己场景
- [ ] 选择每 player 的 TAA profile (sharpness / sharpenMode / upscaleMode)

### 4.3 已知约束 (设计上不解决, 留 F.0.10.3)

- [ ] **~1px 边界锯齿**: TAA 邻域 clip 跨 region 边界采到另一半内容. 默认场景肉眼难辨. 极致场景留 F.0.10.3 shader uvOffset 方案.
- [ ] **Bloom / SSR 全屏耦合**: HDR.EndScene 内 Bloom 仍按全屏跑, 两半 bloom 在 HDR fbo 上混合. 真物理分屏 bloom 留 F.0.10.4 HDR 多实例化方案.

---

## 5. 引擎方 TODO (留作未来 phase)

| 项 | Phase | 优先级 | 工作量 |
|----|-------|--------|-------|
| Shader uvOffset/uvScale (彻底解决边界锯齿) | F.0.10.3 | 🟡 低 | 3-4h |
| Bloom / SSR / MotionBlur region 化 | F.0.10.3 | 🔴 中 | 6-8h |
| HDR fbo 多实例化 (per-player HDR) | F.0.10.4 | 🔴 中 | 8-12h |
| 4-player demo (2x2 grid) | (无独立 Phase, 用户可立即写) | 🟢 低 | 1-2h |

---

## 6. CI 状态回填 (待 Phase 3/4 完成后更新本表)

| Commit | CI Run | 状态 |
|--------|--------|-----|
| a94322f (Phase 1 fix) | 25941161759 | ✅ 6/6 success |
| 57d78ae (Phase 2) | 25942163582 | ✅ 6/6 success |
| fa29d75 (Phase 3) | 25942469141 | ⏳ in_progress |
| 66ee607 (Phase 4) | 25942649835 | ⏳ queued |

(本文档 Phase 3/4 通过后回填; 若某一步 failure, 留下 fix commit hash + 修复说明)
