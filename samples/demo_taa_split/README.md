# Phase F.0.10.1 — TAA Multi-Instance Split Demo

演示 ChocoLight Phase F.0.10 引入的多实例 TAA API：4 个独立 instance 各自持有 history RT + 14 sub-phase 参数。

## 核心价值 (vs `demo_taa_compare`)

| 维度 | `demo_taa_compare` (Phase F.0.7, 单实例) | **`demo_taa_split` (Phase F.0.10.1, 多实例)** |
|------|---------------------------------------|------------------------------------------------|
| 状态切换 | 切 preset = 改 `g` 参数 | 切 instance = 切 active 索引 |
| history 持续性 | **每次切 preset 都重置 history** | **各 instance 独立累积, 切换无 reset** |
| 稳定时间 | 30 帧 stabilize (TAA blend α=0.92 收敛) | 0 帧 (history 一直在累积) |
| 适用场景 | 学习 / 单玩家调试 | split-screen / 双窗编辑器 / VR 双眼独立 TAA |

## 4 个 Instance Profile

| ID | 名称 | 参数 | 重点 Phase |
|----|------|------|-----------|
| 0  | Default (F.0 baseline) | ycocg + sharp=0.5 + AF | F.0 / F.0.2 / F.0.4 |
| 1  | Lanczos Upscale | sharp=0 + halfRes + lanczos 25-tap | **F.0.14** |
| 2  | RCAS Strong Sharpen | sharp=1.5 + RCAS (FSR2 robust) | **F.0.12** |
| 3  | Motion Adaptive All | variance + motionAdaptive γ + sharpness | **F.0.8 / F.0.13** |

## 控制

- `0/1/2/3` — 切到 active instance 0/1/2/3 (history 不重置)
- `R` — Disable + Enable 当前 instance (彻底重置 history)
- `C` — 销毁所有 user instance (1/2/3), 重建并 re-apply profile
- `ESC` — 退出

## 实现要点

```lua
local TAA = Light.Graphics.TAA

-- 启动: 创建 3 个 user instance
local id1 = TAA.CreateInstance()    -- → 1
local id2 = TAA.CreateInstance()    -- → 2
local id3 = TAA.CreateInstance()    -- → 3

-- 各自配置 (独立 Enable + 独立参数)
TAA.SetActiveInstance(id1); TAA.Enable(W, H); TAA.SetUpscaleMode('lanczos'); TAA.SetHalfResHistory(true)
TAA.SetActiveInstance(id2); TAA.Enable(W, H); TAA.SetSharpenMode('rcas');    TAA.SetSharpness(1.5)
TAA.SetActiveInstance(id3); TAA.Enable(W, H); TAA.SetMotionAdaptive(true);   TAA.SetMotionAdaptiveSharpness(true)

-- 每帧: 切 active instance + Process (history 在 instance 内累积)
TAA.SetActiveInstance(0); TAA.ApplyJitter()
-- ... draw scene ...
-- TAA.Process(hdrFbo, hdrTex)   -- 引擎 HDR 链路内部调用

-- 收尾
TAA.SetActiveInstance(0)
TAA.DestroyInstance(id1)
TAA.DestroyInstance(id2)
TAA.DestroyInstance(id3)
```

## 局限

本 demo 仅展示 instance API 行为正确性 (切换/创建/销毁), 不实现真物理 split-screen (左右分屏并行渲染需要 backend FBO + viewport 改动)。
真 split-screen 需要在 ChocoLight 暴露 `Graphics.SetViewport()` Lua API + 解决 HDR 单帧多次 Process 问题, 属 F.0.10.2 范围。

## CI / 验证

- Headless 模式 (`light.exe samples/demo_taa_split/main.lua`, 无 Window): 仅验证 `CreateInstance / GetInstanceCount / DestroyInstance` 路径, 不渲染
- GUI 模式 (用户手动启动): 完整 4-instance 演示 + HUD
