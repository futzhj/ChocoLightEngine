# Phase F.0.10.9.3 — Demo Callback-Model Migration FINAL

> 6A · ASSESS 收尾
> 状态: ✅ 完成 (11 / 11 demo 全部迁完)

---

## 1. 完成工作

把 11 个 `samples/demo_*/main.lua` 从坏的 immediate-mode UI API (`win:BeginFrame() / EndFrame() / IsOpen() / IsKeyPressed() / DrawText() / PollEvents()` 这些**根本不存在**的 API) 全部迁到正确的 OOP callback model:

```lua
local Demo = Light(Light.UI.Window):New()
function Demo:OnOpen() ... end
function Demo:Update(dt) ... end
function Demo:Draw() ... end
function Demo:OnKey(key, scancode, action, mods) ... end
Demo:Open(W, H, title)
while Light.UI.Loop() do Light.UI.Resume() end
```

## 2. 11 个 demo 迁移清单

| # | demo | 原 LOC | 新 LOC | 状态 |
|---|------|--------|--------|------|
| 1 | demo_hdr | 303 | 178 | ✅ HDR + ACES tonemap, H/Z/X/C/V/T/B/R/ESC |
| 2 | demo_bloom | 262 | 152 | ✅ Bloom 后处理, B/1-8/R/ESC |
| 3 | demo_auto_exposure | 275 | 156 | ✅ AE eye adaptation, A/D/1-6/R/ESC |
| 4 | demo_ssao | 323 | 207 | ✅ SSAO 3D PBR scene, F/1-8/B/K/R/ESC |
| 5 | demo_lens_flare | 251 | 175 | ✅ LensFlare ghost+halo, F/1-9/0/D/T/R/ESC |
| 6 | demo_lens_fx | 223 | 154 | ✅ LensDirt + Streak, L/K/1-8/H/V/G/R/ESC |
| 7 | demo_morph_target | 315 | 197 | ✅ glTF morph weights, 1-8/Up/Down/Q/W/C/G/N/Space/ESC |
| 8 | demo_ssr | 694 | 326 | ✅ SSR + Velocity + MB + TAA 综合沙盒, 全键位 |
| 9 | demo_taa_compare | 478 | 222 | ✅ TAA 8-preset 切换, 1-8/R/ESC |
| 10 | demo_taa_split | 435 | 218 | ✅ TAA 4-instance split, 0/1/2/3/R/C/ESC |
| 11 | demo_taa_split2 | 858 | 366 | ✅ True split-screen multi-pass, R/ESC |
| **Total** | | **4417** | **2351** | **-47% LOC (helper inline)** |

## 3. 关键映射表 (旧 → 新)

| 旧 (immediate, 不存在) | 新 (callback, 实际可用) |
|----------------------|----------------------|
| `Window.Open(W, H, t)` 返回 win | `Demo:Open(W, H, t)` (OnOpen 内 init) |
| `while win:IsOpen() do ... end` | `while Light.UI.Loop() do Light.UI.Resume() end` |
| `win:PollEvents()` | (auto, 由 Light.UI.Resume 调) |
| `win:BeginFrame(r, g, b, a)` | (auto, Window:__call 内调 g_render->BeginFrame) |
| `win:EndFrame()` | (auto, Window:__call 内调 g_render->EndFrame + SwapBuffers) |
| `win:IsKeyPressed('escape')` + `keyTap()` 防长按 | `Demo:OnKey(key, sc, action, mods)` (action=1 仅按下, 自带"防长按") |
| `win:DrawText(x, y, s, r, g, b, a)` | `Gfx.SetColor(r,g,b,a); Gfx.Print(s, x, y, 0)` |
| `win:Close()` | `self:Close()` |

## 4. GLFW 键码常用 (`platform_window_sdl3.cpp:35-80`)

```
ESC=256  ENTER=257  TAB=258  SPACE=32
'A'=65 ... 'Z'=90 (大写 GLFW)
'0'=48 ... '9'=57
F1=290 ... F12=301
LEFT=263 RIGHT=262 UP=265 DOWN=264
- (MINUS)=45  = (EQUAL)=61
[ (LBRACKET)=91  ] (RBRACKET)=93
, (COMMA)=44  . (PERIOD)=46  ; (SEMICOLON)=59
\ (BACKSLASH)=92  / (SLASH)=47
```

字母键: `key == string.byte('H')` 简化 (避免硬记数字).

## 5. 验证

### 5.1 11 个 demo 真 GL 启动测试 (本地 IDE)

`light.exe samples\<demo>\main.lua` 跑 2 秒后 PowerShell 强制 kill:

| Demo | 状态 |
|------|------|
| demo_hdr | RUN (GUI 启动 + 进入 render loop) |
| demo_bloom | RUN |
| demo_auto_exposure | RUN |
| demo_ssao | RUN |
| demo_lens_flare | RUN |
| demo_lens_fx | RUN |
| demo_morph_target | Exit normally (no .glb asset, prints "demo_morph_target ok (no asset)") |
| demo_ssr | RUN |
| demo_taa_compare | RUN |
| demo_taa_split | RUN |
| demo_taa_split2 | RUN |

10/11 真 GL 启动成功 + 1/11 优雅退出 (资产缺失). **全 11 个均无 Lua error**.

### 5.2 8 smoke 零回归

```
PASS hdr  ·  PASS bloom  ·  PASS ssr  ·  PASS auto_exposure
PASS lens_fx  ·  PASS motion_blur  ·  PASS taa  ·  PASS lighting2d
```

## 6. 关键 lessons

### 6.1 老 demo 全部"靠 fallback 苟活"

老的 11 个 demo `Window.Open(W, H, t)` 在 light.exe 中**实际抛 "bad argument #1 (table expected)"** (因为 `Window.Open` 是 OOP method 必须用冒号 `Window:Open`), 然后 `pcall` 捕获 → fallback 走 headless API probe 路径退出. 用户从来没在真 GL 模式跑过这些 demo.

### 6.2 callback model 比 immediate 简洁很多

新 demo 平均 -47% LOC (4417 → 2351). 主要省掉:
- `keyTap()` cooldown 表 (callback 模型 OnKey 自带 KeyDown 触发, 无需防长按)
- `lastTime / dt` 手动计算 (Update(dt) 直接接收)
- `win:BeginFrame()` / `win:EndFrame()` (Window:__call 自动)
- `win:PollEvents()` (Light.UI.Resume 自动)

### 6.3 Lua linter 误报 mesh:Draw

Lua linter 不识别 `Gfx.Mesh.New()` 返回值的 OOP method `mesh:Draw(0)`. 所有 demo 都有此 warning, 不影响真机运行 (老 demo 也都用同样写法). 忽略.

## 7. 文件变更

| 文件 | 变更 |
|------|------|
| `samples/demo_hdr/main.lua` | 重写 callback model (-125 LOC) |
| `samples/demo_bloom/main.lua` | 重写 callback model (-110 LOC) |
| `samples/demo_auto_exposure/main.lua` | 重写 callback model (-119 LOC) |
| `samples/demo_ssao/main.lua` | 重写 callback model (-116 LOC) |
| `samples/demo_lens_flare/main.lua` | 重写 callback model (-76 LOC) |
| `samples/demo_lens_fx/main.lua` | 重写 callback model (-69 LOC) |
| `samples/demo_morph_target/main.lua` | 重写 callback model (-118 LOC) |
| `samples/demo_ssr/main.lua` | 重写 callback model (-368 LOC, 大量重复键位 dispatch table) |
| `samples/demo_taa_compare/main.lua` | 重写 callback model (-256 LOC) |
| `samples/demo_taa_split/main.lua` | 重写 callback model (-217 LOC) |
| `samples/demo_taa_split2/main.lua` | 重写 callback model (-492 LOC) |
| `docs/Phase F.0.10.9.3 demo callback migration/PLAN_PhaseF_0_10_9_3.md` | 新建 PLAN |
| `docs/Phase F.0.10.9.3 demo callback migration/FINAL_PhaseF_0_10_9_3.md` | 本文 |
| `docs/Phase F.0.10.9.3 demo callback migration/TODO_PhaseF_0_10_9_3.md` | 后续接力 |

## 8. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN 11 demo 清单 + 1 关键决策 (用户确认 scope=全 11 个) |
| **Architect** | PLAN §3 通用迁移模板 + §4 旧→新映射表 + §5 GLFW 键码表 |
| **Atomize** | TODO 11 个 sub-task (1 demo 1 task) |
| **Approve** | 用户隐式确认 (ask_user_question) |
| **Automate** | 11 demo 顺序迁移 (~3h, ~250-400 LOC/demo, 平均 ~25min/demo) |
| **Assess** | 本 FINAL + 11 demo 真 GL 启动测试 + 8 smoke 零回归 |

## 9. C++ 改动

无. 本 phase 全是 Lua 代码迁移 (samples/demo_*/main.lua).

## 10. Lua API 总数

不变. 仍是 F.0.10.9.2 的 80 fn.

---

## 11. 下一步候选

- F.0.10.9.x.2 Bloom/SSR/MB pyramid 多 instance (低优, ~6h)
- F.0.10.9.x.3 GetState/Clone (中优, ~1h)
- F.0.10.10 multi-HDR + multi-TAA + multi-Bloom 联动 demo (~4h)
- F.1 TAAU DLSS-like (~10-15h)
- F.2 PBR Material system
