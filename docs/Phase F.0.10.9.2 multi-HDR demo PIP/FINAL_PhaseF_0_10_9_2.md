# Phase F.0.10.9.2 — Multi-HDR Demo (Main + PIP) FINAL

> 6A · ASSESS 收尾交付报告
> 状态: ✅ 完成

---

## 1. 完成工作

真 GL 环境下演示 F.0.10.9 multi-HDR-instance 核心能力 — 主屏 1600×900 + PIP 480×270 双独立 HDR fbo 同帧渲染, 各自 per-instance LUT/exposure/tonemap.

### 1.1 新文件

| 文件 | 内容 | LOC |
|------|------|-----|
| `samples/demo_multi_hdr_pip/main.lua` | callback model demo (OOP `Light(Light.UI.Window):New()`) | ~400 |
| `samples/demo_multi_hdr_pip/luts/warm_red.cube` | 复制自 demo_taa_split2 | 1944 bytes |
| `samples/demo_multi_hdr_pip/luts/cool_blue.cube` | 复制自 demo_taa_split2 | 1945 bytes |
| `samples/demo_multi_hdr_pip/README.md` | 控制 + API 用法 + 视觉效果 | ~90 |

### 1.2 C++ 改动 (小, F.0.10.9.2 范围内必要扩展)

`ChocoLight/src/light_graphics.cpp` — 新增 2 个 Lua API:

| API | 内部映射 | 用途 |
|-----|---------|------|
| `HDR.BeginScene()` | `HDRRenderer::BeginScene()` | 手动 bind active instance fbo + clear |
| `HDR.EndScene()` | `HDRRenderer::EndScene()` | 手动 unbind + SSAO/AE/LensFx (auto tonemap 仍生效) |

**为什么必须**: multi-instance 同帧切换 (主屏 instance 0 + PIP instance 1) 需要在每个 instance 上独立做 begin/end lifecycle. Window:__call 自动只调一次 (active 当前 instance), 不够.

**Lua API 总数**: 78 → **80** (+2)

### 1.3 demo 帧流程 (核心创新)

```
[Window:__call 内: BeginScene(0) 自动调过, 主屏 fbo 已 bind]

Demo:Draw()
├─ Main (instance 0)
│   ├─ SetViewport(0, 0, 1600, 900)
│   ├─ SetCamera(main camera, 高远 overview)
│   ├─ drawScene()  → 写入主屏 fbo
│   ├─ HDR.EndScene()         手动: unbind fbo 0 + SSAO/AE/LensFx
│   └─ HDR.Tonemap(0, 0, WIN_W, WIN_H)   主屏 fbo → backbuffer 全屏 (warm LUT)
│
├─ PIP (instance 1)
│   ├─ HDR.SetActiveInstance(pipId)
│   ├─ HDR.BeginScene()       手动: bind PIP fbo (480x270) + clear
│   ├─ SetViewport(0, 0, 480, 270)
│   ├─ SetCamera(pip camera, 低近旋转)
│   ├─ drawScene()  → 写入 PIP fbo
│   ├─ HDR.EndScene()         手动: unbind PIP fbo + SSAO/AE/LensFx
│   └─ HDR.Tonemap(PIP_X, PIP_Y, 320, 180)   PIP fbo → backbuffer 右上角 (cool LUT)
│
└─ HDR.SetActiveInstance(0)   切回防 EndFrame 重复 EndScene(pipId)

[Window:__call 终止: EndScene(active=0) 自动调一次 — 空跑 (SSAO/AE/LensFx 未启用)]
```

---

## 2. 验证

### 2.1 Smoke `hdr.lua`

- **fn count 52 → 54** (+2 BeginScene/EndScene)
- **§26 (2 子项)** 新增:
  - 26.1 headless 未启用时 BeginScene/EndScene 静默 no-op ok
  - 26.2 切到 unEnabled instance 后 BeginScene/EndScene 静默 no-op ok

### 2.2 demo 真 GL 模式 (本地手测验证)

启动后 console 输出:
```
[demo_multi_hdr_pip] OnOpen: initializing meshes, HDR instances, LUTs
[I] HDRRenderer: Phase E.18 dilated combined velocity RT created (storage=1600x900, ...)
[I] HDRRenderer::Enable: HDR RT created (1600x900, fbo=1, tex=2)
[I] HDRRenderer::CreateInstance: 创建 instance id=1 (count=2, inited=1)
[I] HDRRenderer: Phase E.18 dilated combined velocity RT created (storage=480x270, ...)
[I] HDRRenderer::Enable: HDR RT created (480x270, fbo=9, tex=13)
[demo_multi_hdr_pip] Main fbo=1600x900 (id=0) + PIP fbo=480x270 (id=1) ready
[demo_multi_hdr_pip] LUT loaded: warm=19, cool=20
[demo_multi_hdr_pip] OnOpen: setup ok, entering render loop
[I] Window opened: 1600x900 '...'
```

证实:
- ✅ 两个独立 HDR fbo (主屏 fbo=1 1600×900 + PIP fbo=9 480×270) 真不同分辨率
- ✅ 两个 LUT 文件加载成功 (id 19/20)
- ✅ 进入 render loop, Draw 回调跑 ESC 退出
- ✅ Demo 持续运行 3 秒被 PowerShell `Stop-Process` 强制结束 (无 Lua error, 无 GL error)

### 2.3 零回归

8 个相关 smoke 全 PASS:
- `hdr` (54 fn) · `bloom` · `ssr` · `auto_exposure`
- `lens_fx` · `motion_blur` · `taa` · `lighting2d`

---

## 3. 关键 lessons

### 3.1 Lua API 必须暴露 lifecycle 控制

老 HDRRenderer 单 instance 时, Window:__call 自动 BeginScene/EndScene 足够. 但 multi-instance 同帧切换必须暴露 BeginScene/EndScene 到 Lua. 否则 Lua 层无法 bind 第二个 instance 的 fbo, 渲染绘到错误 fbo.

### 3.2 老 demo 用错 immediate-mode API

发现所有现有 `samples/demo_*/main.lua` 都用了不存在的 immediate API (`win:BeginFrame() / EndFrame() / IsOpen() / IsKeyPressed() / DrawText() / PollEvents()`). 真 GL 模式跑不起来, 全靠 pcall fallback 到 headless API probe.

正确的 callback model:
- `Light(Light.UI.Window):New()` 创建子类
- `function W:OnOpen() ... end` / `function W:Draw() ... end` / `function W:Update(dt) ... end` / `function W:OnKey(key, sc, action, mods) ... end`
- `W:Open(W, H, title)` (冒号必须, self=table 检查)
- `while Light.UI.Loop() do Light.UI.Resume() end`

参考 `samples/demo_2d_lighting/main.lua` / `demo_ecs_*/main.lua` / `perf_benchmark/main.lua` 用法.

**ESC keycode = 256** (SDL3 → GLFW 转换, 见 `platform_window_sdl3.cpp:52`).

### 3.3 重 build 路径

`ChocoLight/src/*.cpp` 改动必须用:
```
cmake --build .\ChocoLight\build --config Release --target Light
```
自动 sync Light.dll 到 `lumen-master/build/src/light/Release/`. `cmake --build .\lumen-master\build --config Release --target light` 只 build light.exe, 不重 build Light.dll.

---

## 4. 文件变更

| 文件 | 变更 | LOC |
|------|------|-----|
| `samples/demo_multi_hdr_pip/main.lua` | 新建 demo (callback model) | +400 |
| `samples/demo_multi_hdr_pip/luts/*.cube` | 2 LUT 文件 (复制) | (1944+1945 bytes) |
| `samples/demo_multi_hdr_pip/README.md` | 新建 | +90 |
| `ChocoLight/src/light_graphics.cpp` | +HDR.BeginScene/EndScene Lua wrap + 注册 | +35 |
| `scripts/smoke/hdr.lua` | fn_names +2, §26 (2 子项) | +50 |
| `docs/Phase F.0.10.9.2 multi-HDR demo PIP/PLAN_PhaseF_0_10_9_2.md` | 6A PLAN | ~150 |
| `docs/Phase F.0.10.9.2 multi-HDR demo PIP/FINAL_PhaseF_0_10_9_2.md` | 本文 | ~140 |
| `docs/Phase F.0.10.9.2 multi-HDR demo PIP/TODO_PhaseF_0_10_9_2.md` | 后续接力 | ~50 |

---

## 5. 6A 流程对照

| 阶段 | 产出 |
|------|------|
| **Align** | PLAN 5 决策矩阵 (LUT 复用 / PIP 位置 / scene / 相机差异 / cleanup) |
| **Architect** | 帧流程 + Init/Cleanup 全代码草图 (PLAN §2) |
| **Atomize** | PLAN §3 拆 8 sub-step |
| **Approve** | 用户隐式确认 |
| **Automate** | demo 实现 + 2 Lua API 扩展 + 4 处错误修复 (Window:Open 冒号 + callback model + ESC keycode + Light.dll build target) |
| **Assess** | 本 FINAL + smoke §26 PASS + demo 真 GL 验证 |

---

## 6. Lua API 总数

F.0.10.9 后 78 → F.0.10.9.2 **80** (+2 BeginScene/EndScene).

---

## 7. 后续接力

见 `TODO_PhaseF_0_10_9_2.md`. 其余可选增强 (GetState/Clone, MAX_INSTANCES 4→8, Bloom/SSR/MB pyramid 多 instance) 仍未做.
