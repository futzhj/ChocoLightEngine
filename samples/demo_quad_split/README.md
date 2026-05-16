# demo_quad_split — Phase F.0.10.10 Quad-Split Linkage Demo

> 真物理 4-screen split-screen, 4 个 HDR instance × 4 个 TAA instance,
> 每 quad 各自独立 LUT / exposure / tonemap / sharpen profile,
> 集 F.0.10.9.x multi-instance 系列之大成.

## Quad 布局 (1280×720, 各 quad 640×360)

```
┌──────────────┬──────────────┐
│  TL (Q0)     │  TR (Q1)     │
│  黄昏暖调      │  冷夜冷调      │
│ ACES + warm  │ Uncharted2   │
│ + RCAS sharp │ + Lanczos    │
├──────────────┼──────────────┤
│  BL (Q2)     │  BR (Q3)     │
│  中性复古      │  赛博青蓝      │
│ Reinhard,    │ ACES + cool  │
│ no LUT       │ + AntiFlick  │
└──────────────┴──────────────┘
```

每个 quad 是独立的 HDR instance (各自 fbo + sceneTex + bloom pyramid + dilation RT + history),
渲染时通过 `HDR.SetActiveInstance(i)` 切换, `Bloom.Process` / `SSR.Process` /
`MB.Process` / `TAA.Process` 自动作用于 active instance 的 RT (内部用
`HDRRenderer::GetFBO()` + `GetSceneTexture()`).

## 关键 API 用法

### 1. 创建 4 个 HDR instance (3 user + 1 default)

```lua
HDR.Enable(640, 360)                    -- instance 0 (default)
for i = 1, 3 do
    local id = HDR.CreateInstance()     -- 1, 2, 3
    HDR.SetActiveInstance(id)
    HDR.Enable(640, 360)                -- 各自独立 fbo
end
```

`MAX_INSTANCES = 4` (default + 3 user) 正好满足 4-quad split-screen 需求.

### 2. 创建 4 个 TAA instance (history 隔离)

```lua
for i = 1, 3 do TAA.CreateInstance() end -- 1, 2, 3 (default 0 已存在)
-- per-instance setup (clipMode/sharpness/halfResHistory 全独立)
```

### 3. 关闭所有 auto* (手动控时序)

```lua
for i = 0, 3 do
    HDR.SetActiveInstance(i)
    HDR.SetAutoTonemap(false)           -- 每 instance 独立
end
HDR.SetAutoBloom(false)                 -- 全局
HDR.SetAutoSSR(false)
HDR.SetAutoMotionBlur(false)
HDR.SetAutoTAA(false)
```

### 4. 帧流程 — 每 quad 渲染 + 后处理

```lua
for i = 0, 3 do
    apply_postfx_profile(i)             -- Bloom/SSR/MB 全局参数 (每帧切)
    HDR.SetActiveInstance(hdr_ids[i])
    HDR.BeginScene()                    -- bind instance i 的 fbo + clear
    Gfx.SetViewport(0, 0, 640, 360)
    SetCamera(camera_i); drawScene()
    Bloom.Process(0, 0, 640, 360)       -- 自动作用于 active fbo
    SSR.Process(0, 0, 640, 360)
    MB.Process(0, 0, 640, 360)
    TAA.SetActiveInstance(taa_ids[i])
    TAA.ApplyJitter(); TAA.Process(0, 0, 640, 360)
    HDR.EndScene()                      -- unbind
end
```

### 5. Tonemap pass — 4 instance → default fb 的 4 quad

```lua
Gfx.SetViewport(0, 0, 1280, 720)
for i = 0, 3 do
    HDR.SetActiveInstance(hdr_ids[i])
    HDR.Tonemap(quad_x, quad_y, 640, 360, {
        exposure = 1.5, gamma = 2.2,
        tonemap = 'aces', lut = warmLut, lutStrength = 0.85
    })
end
```

## 4 Quad Profile 对比

| Quad | LUT | Exposure | Tonemap | Sharpen | Bloom | SSR | MB |
|------|-----|----------|---------|---------|-------|-----|-----|
| TL | warm | 1.5 | ACES | RCAS 1.2 | strong | off | strong |
| TR | cool | 0.6 | Uncharted2 | Lanczos halfRes | light | temporal | off |
| BL | none | 1.0 | Reinhard | unsharp 0.5 | mid | mid | mid |
| BR | cool 50% | 1.2 | ACES | RCAS 1.5 | mid | mid | mid |

## 控制

- **1 / 2 / 3 / 4** : 重置对应 quad 的 TAA history (Disable + Enable + re-apply profile)
- **L** : 全局 LUT toggle (TL warm + TR/BR cool 一起切)
- **ESC** : 退出

## 验证

### Headless API probe (CI 兼容)

无 GL ctx 时跑 6 个探针:
1. HDR multi-instance Create/Destroy round-trip
2. TAA multi-instance Create/Destroy round-trip
3. HDR.BeginScene/EndScene headless silent no-op
4. HDR.Tonemap headless 退化 (nil + err)
5. Bloom/SSR/MB.Process headless 退化
6. MAX_INSTANCES = 4 enforced (第 4 个 user create 失败)

### 真 GL 模式 (Windows 本地)

```
==== ChocoLight Phase F.0.10.10 Quad-Split Linkage Demo (callback-model) ====
[demo_quad_split] 4 HDR instance ready: ids=[0, 1, 2, 3], each 640x360
[demo_quad_split] LUT loaded: warm=31, cool=32
[demo_quad_split] 4 TAA instance ready: ids=[0, 1, 2, 3]
Window opened: 1280x720 'Phase F.0.10.10 - Quad-Split Linkage Demo (4 HDR x 4 TAA)'
```

每个 HDR instance 都有独立 RT (5-level bloom pyramid × 4 + dilation RT × 8),
4 TAA instance 各自 RGBA16F × 2 history (instance 1 还启用 halfRes = 320×180).

## Lua API 依赖矩阵

| API | 来源 phase | 用途 |
|-----|-----------|------|
| `HDR.CreateInstance / SetActiveInstance / DestroyInstance / GetInstanceCount` | F.0.10.9 | 多 instance 主体 |
| `HDR.BeginScene / EndScene` | F.0.10.9.2 | 手动 fbo bind/unbind |
| `HDR.SetAuto{Tonemap, Bloom, SSR, MotionBlur, TAA}` | F.0.10.{6,3,2} | 关 auto* 让用户手动 .Process |
| `HDR.Tonemap(rgn, params)` | F.0.10.6 + F.0.10.8 | per-region tonemap + LUT |
| `HDR.LoadCubeLUT / SetGradingLUT / DeleteLUT3D` | F.0.10.8.{1,0} | 调色 LUT |
| `Bloom.Process(rgn) / SSR.Process(rgn) / MB.Process(rgn)` | F.0.10.3 | per-region 后处理 |
| `TAA.CreateInstance / SetActiveInstance / Process(rgn)` | F.0.10.2 | 多 instance TAA history |
| `TAA.SetUpscaleMode("lanczos")` | F.0.14 | 高画质上采 (TR quad) |
| `TAA.SetSharpenMode('rcas')` | F.0.12 | 强锐化 (TL/BR quad) |
| `Gfx.SetViewport(x, y, w, h)` | F.0.10.2 | quad 裁剪 |

## 与之前 demo 关系

- `demo_taa_split2`: 单 HDR instance + 2 TAA instance, 2-screen split
- `demo_multi_hdr_pip`: 2 HDR instance (主屏 + PIP), 单 TAA instance
- `demo_quad_split` (本): **4 HDR + 4 TAA**, 真 4-screen, 完整 multi-instance 系列闭环
