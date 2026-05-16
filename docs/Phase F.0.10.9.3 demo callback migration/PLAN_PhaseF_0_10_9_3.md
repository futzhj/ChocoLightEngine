# Phase F.0.10.9.3 — Demo Callback-Model Migration (11 个) PLAN

> 6A · ALIGN + ARCHITECT + ATOMIZE 极简版本 (高同质化任务)

---

## 1. 问题陈述

11 个 `samples/demo_*/main.lua` 用错的 immediate-mode UI API (`win:BeginFrame() / EndFrame() / IsOpen() / IsKeyPressed() / DrawText() / PollEvents()`) — 这些 API **根本不存在** (light_ui.cpp 无对应实现). 真 GL 模式跑不起来, 全靠 `pcall(Window.Open(...))` fallback 到 headless API probe 路径苟活 CI.

正确 OOP callback model (验证可用 demo: `demo_2d_lighting`, `demo_ecs_render`, `demo_multi_hdr_pip`):

```lua
local Game = Light(Light.UI.Window):New()
function Game:OnOpen()  ... end
function Game:Update(dt) ... end
function Game:Draw() ... end
function Game:OnKey(key, scancode, action, mods) ... end
Game:Open(W, H, title)
while Light.UI.Loop() do Light.UI.Resume() end
```

## 2. 待迁 demo 清单

| # | demo | LOC | 复杂度 | 特殊路径 |
|---|------|-----|--------|----------|
| 1 | demo_hdr | ~303 | 简 | 单 win, HDR + Bloom |
| 2 | demo_bloom | ~? | 简 | 单 win, HDR + Bloom |
| 3 | demo_auto_exposure | ~? | 简 | 单 win, HDR + AE |
| 4 | demo_lens_flare | ~? | 中 | HDR + LensFlare |
| 5 | demo_lens_fx | ~? | 中 | HDR + Dirt+Streak+Flare 三合一 |
| 6 | demo_morph_target | ~? | 中 | 16 matches (最多输入), 模型动画 |
| 7 | demo_ssao | ~? | 简 | HDR + SSAO |
| 8 | demo_ssr | ~? | 中 | HDR + SSR + multiple toggle |
| 9 | demo_taa_compare | ~467 | 中 | TAA 8-preset 切换 |
| 10 | demo_taa_split | ~423 | 中 | TAA multi-instance 切 |
| 11 | demo_taa_split2 | ~828 | 高 | F.0.10.7 split-screen, multi-pass tonemap, per-region |

## 3. 迁移模板 (统一格式)

```lua
-- 0. require + 检测 (沿用)
local UI = ... ; local Gfx = ... ; local Time = ...
if not Gfx then ... return end
if not UI or not UI.Window then [headless probe] return end

-- 1. 全局常量 + 几何 (沿用)
local WIN_W, WIN_H = ...
local function buildXxx() ... end

-- 2. Demo 类
local Demo = Light(Light.UI.Window):New()

-- 3. 局部状态 (从 main scope 提到 file-level upvalue, OnKey/Update/Draw 共享)
local g_xxx = ...
local g_initOk = false

-- 4. OnOpen (替代旧 Window.Open 后的初始化代码)
function Demo:OnOpen()
    -- 建 mesh / Enable HDR / 加载 LUT / 设置相机 / ...
    g_initOk = true
end

-- 5. Update (替代 dt 计算 + 角度积分)
function Demo:Update(dt)
    if dt > 0.1 then dt = 0.1 end
    -- 时间积分
end

-- 6. Draw (替代 win:BeginFrame ... drawScene ... win:EndFrame)
function Demo:Draw()
    if not g_initOk then return end
    -- HDR.BeginScene 已被 Window:__call 自动调
    -- 渲染 + HUD (用 Gfx.Print 替代 win:DrawText)
end

-- 7. OnKey (替代 keyTap + win:IsKeyPressed)
function Demo:OnKey(key, scancode, action, mods)
    if action ~= 1 then return end   -- 仅按下
    if key == 256 then self:Close() end   -- ESC
    -- 字母键 = 65 + ('A' offset), e.g. H=72, Z=90, X=88, C=67, V=86, T=84, B=66, R=82
end

-- 8. Cleanup (替代旧 cleanup 块)
local function cleanup_demo()
    if not g_initOk then return end
    -- Disable HDR / DeleteLUT / Mesh:Delete
end

-- 9. 主循环
Demo:Open(WIN_W, WIN_H, title)
while Light.UI.Loop() do Light.UI.Resume() end
cleanup_demo()
print('demo_xxx ok')
```

## 4. 关键映射表

| 旧 (immediate) | 新 (callback) |
|----------------|---------------|
| `win = Window.Open(W, H, t)` | `Demo:Open(W, H, t)` (OnOpen 内 init) |
| `while win:IsOpen() do ... end` | `while Light.UI.Loop() do Light.UI.Resume() end` |
| `win:PollEvents()` | (auto, 由 Light.UI.Resume 调) |
| `win:BeginFrame(r, g, b, a)` | (auto, Window:__call 内调 g_render->BeginFrame) |
| `win:EndFrame()` | (auto, Window:__call 内调 g_render->EndFrame + SwapBuffers) |
| `win:IsKeyPressed('escape')` | `function Demo:OnKey(key, sc, action, mods) if action==1 and key==256 then self:Close() end end` |
| `win:DrawText(x, y, s, r, g, b, a)` | `Gfx.SetColor(r, g, b, a); Gfx.Print(s, x, y, 0)` |
| `win:Close()` | `self:Close()` |

## 5. GLFW 键码常用值 (见 platform_window_sdl3.cpp:35-80)

```
ESCAPE=256  ENTER=257  TAB=258  SPACE=32
'A'=65 ... 'Z'=90 (大写 GLFW)
'0'=48 ... '9'=57
F1=290 ... F12=301
LEFT=263 RIGHT=262 UP=265 DOWN=264
LEFT_BRACKET=91 RIGHT_BRACKET=93
COMMA=44 PERIOD=46 SLASH=47
```

字母键: 用 `string.byte('H')` 简化 (不依赖记忆), 例: `if key == string.byte('H') then ...`.

## 6. 工作量估算

- 简单 demo (单 win, ~300 LOC): ~25min × 7 = ~3h
- 中复杂 demo (multi-pass, ~400-500 LOC): ~40min × 3 = ~2h
- 高复杂 demo (taa_split2 split-screen multi-instance ~830 LOC): ~1h × 1 = ~1h
- 总计 ~6h

## 7. 验证策略

每 demo 迁完跑 headless probe 验证 (CI 兼容路径不应回归):

```bash
.\lumen-master\build\src\light\Release\light.exe (Resolve-Path "samples\demo_xxx\main.lua")
# 期望: 退出 0, 输出 "demo_xxx ok"
```

最后跑 8 相关 smoke 零回归:
- `hdr` `bloom` `ssr` `auto_exposure` `lens_fx` `motion_blur` `taa` `lighting2d`

CI 6/6 全绿才算完.

## 8. 风险

| 风险 | 概率 | 缓解 |
|------|------|------|
| 某 demo input 模型特殊 (e.g. multi-key combo) | 中 | OnKey + global state 模拟; 测试本地 GL 行为 |
| `Gfx.Print` 与 `win:DrawText` 行为不同 (字号/字体) | 低 | 接受差异, demo 是技术演示不追求 pixel match |
| Window:__call 自动 BeginFrame/BeginScene 与 demo 期望冲突 | 中 | 复杂 demo (taa_split2) 用 SetActiveInstance + 手动 BeginScene/EndScene 解决 |
| 全部一次性迁完风险大 | 中 | 分批: 先简单 5 个 → headless smoke → commit; 再中 3 个 → 再高 1 个; 最后 commit |

## 9. 不做 (Out of scope)

- 修复 `Gfx.Print` 字号 / 字体 / 多行布局 (HUD 视觉差异接受)
- 重新设计 demo 视觉 (保持原始 demo 的功能 + UI)
- 修 demo 中的算法 bug (仅迁框架)
- 写新 demo (本 phase 仅迁老的)

## 10. 提交策略

- 1 commit "Phase F.0.10.9.3: migrate 11 demos to OOP callback model" (11 demo + PLAN/FINAL/TODO + smoke)
- CI 6/6 → ASSESS

---

## 11. 6A 决策矩阵 (10/10 自动决策)

| # | 决策 | 选择 |
|---|------|------|
| 1 | scope | 全 11 个 (用户已确认) |
| 2 | 迁移模式 | callback model (Light(Light.UI.Window):New()) |
| 3 | helper 提取 | 不做 (一次性迁完, 不引入 _shared/) |
| 4 | input keytap 模拟 | OnKey + cooldown table 模拟原行为 |
| 5 | HUD 文本 | Gfx.Print (替代 win:DrawText) |
| 6 | dt 来源 | Update(dt) 直接接收 (Window:__call 已计算) |
| 7 | cleanup 调用点 | 主循环退出后, 全局函数 (因无 OnClose 回调) |
| 8 | demo 内 BeginScene/EndScene 控制 | 一般 demo 不调 (Window:__call 自动); split2 例外 (multi-instance 手动) |
| 9 | 错误兼容 (旧 Window.Open API 也保留) | 不保留, 直接 callback model |
| 10 | 提交粒度 | 1 commit (避免 CI run 浪费) |
