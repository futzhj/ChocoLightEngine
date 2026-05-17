# Phase F.2 渲染架构补齐 — FINAL (交付总结) 文档

> **阶段**：6A Workflow — 阶段 6 Approve / Final
> **创建日期**：2026-05-17
> **基线变更**：F.0.11.6 + F.1.1 → **F.2.0 (渲染架构补齐完结)**

---

## 1. 交付清单

### 修改文件 (10)

| 文件 | 性质 | 改动概要 |
|------|------|---------|
| ChocoLight/src/hdr_renderer.cpp | G1 P0 修复 | OnTAAURenderScaleChanged + OnTAAUDisabled 各加 8 行下游 OnHDRResized |
| ChocoLight/include/lit_batch_renderer.h | G3 接口 | +12 行 (OnHDREnabled/Disabled/Resized 声明) |
| ChocoLight/src/lit_batch_renderer.cpp | G3 实现 | +5 行 stub |
| ChocoLight/include/ssao_renderer.h | G2.1 | +9 行 multi-instance 6 fn |
| ChocoLight/src/ssao_renderer.cpp | G2.1 | +90 行 (state[4] + 6 fn 实现) |
| ChocoLight/include/lens_flare_renderer.h | G2.2 | +9 行 |
| ChocoLight/src/lens_flare_renderer.cpp | G2.2 | +85 行 |
| ChocoLight/include/lens_dirt_renderer.h | G2.3 | +12 行 |
| ChocoLight/src/lens_dirt_renderer.cpp | G2.3 | +75 行 |
| ChocoLight/include/streak_renderer.h | G2.4 | +9 行 |
| ChocoLight/src/streak_renderer.cpp | G2.4 | +75 行 |
| ChocoLight/include/auto_exposure_renderer.h | G2.5 | +9 行 |
| ChocoLight/src/auto_exposure_renderer.cpp | G2.5 | +85 行 |
| ChocoLight/src/light_graphics.cpp | Lua binding | +155 行 (5 × 6 fn × ~5 行 + funcs[] 行) |

### 新增文件 (8)

| 文件 | 用途 |
|------|------|
| docs/Phase F.2 渲染架构补齐/ALIGNMENT_PhaseF_2.md | 6A 阶段 1 |
| docs/Phase F.2 渲染架构补齐/CONSENSUS_PhaseF_2.md | 6A 阶段 1.5 |
| docs/Phase F.2 渲染架构补齐/DESIGN_PhaseF_2.md | 6A 阶段 2 |
| docs/Phase F.2 渲染架构补齐/TASK_PhaseF_2.md | 6A 阶段 3 |
| docs/Phase F.2 渲染架构补齐/ACCEPTANCE_PhaseF_2.md | 6A 阶段 5 |
| docs/Phase F.2 渲染架构补齐/FINAL_PhaseF_2.md | 6A 阶段 6 |
| docs/Phase F.2 渲染架构补齐/TODO_PhaseF_2.md | 后续 TODO |
| scripts/smoke/phase_f2_multi_instance.lua | 多实例 smoke |

## 2. 解决的问题

### G1 (P0 正确性) — TAAU 切换下游静默失效
**修复前**: 启用 TAAU (renderScale=0.667) 时, sceneTex=1280×720 但 Bloom pyramid/SSAO/MotionBlur 仍是 1920×1080, 比例错位 + 性能浪费.

**修复后**: TAAU 切换主动通知 8 个下游模块 OnHDRResized(renderW, renderH); 切回 F.0 时镜像调用恢复 outputW × outputH.

### G2 (P1 一致性) — 后处理多实例覆盖不全
**修复前**: 仅 5/9 后处理 (HDR/TAA/Bloom/SSR/MotionBlur) 支持 multi-instance, split-screen 4 player 后 5 类效果全屏共享参数.

**修复后**: 全 10/10 后处理 (含新加 SSAO/LensFlare/LensDirt/Streak/AutoExposure) 都支持 4 instance + Clone, 30 个新 Lua API.

### G3 (P1 一致性) — LitBatch 接口缺位
**修复前**: 9 个后处理都有 OnHDREnabled/Disabled/Resized, LitBatch 缺这 3 个 (内部无 RT 但接口不一致).

**修复后**: LitBatch 加 stub 三件套, 接口对齐.

## 3. 关键 Lua API 新增

```lua
-- 5 模块通用模板:
local id = Light.Graphics.<Module>.CreateInstance()        -- 返 [1, 3] 或 0
Light.Graphics.<Module>.SetActiveInstance(id)               -- 切换 active
Light.Graphics.<Module>.GetActiveInstance()                 -- 当前 active
Light.Graphics.<Module>.GetInstanceCount()                  -- 已分配数 [1, 4]
local cloneId = Light.Graphics.<Module>.CloneInstance(srcId) -- 克隆参数 (不含 RT)
Light.Graphics.<Module>.DestroyInstance(id)                 -- 销毁 user instance

-- <Module> ∈ { SSAO, LensFlare, LensDirt, Streak, AutoExposure }
```

## 4. Smoke 测试结果

```
[OK] Phase F.2 multi-instance smoke 全部通过 (30 binding + 6 行为)
[OK] taa.lua: 56/53 fn 全部通过
[OK] hdr.lua: 57 fn 全部通过
[OK] bloom.lua: E.4 + F.0.10.3 + F.0.10.9.x.2/x.3/x.4 全部通过
[OK] ssao.lua: E.8 全部通过
[OK] auto_exposure.lua: E.5 全部通过
[OK] lens_fx.lua: E.6 全部通过
[OK] lens_flare.lua: E.7 全部通过
```

## 5. Lua 用法示例 (split-screen 4 player AE)

```lua
local AE = Light.Graphics.AutoExposure
-- 创建 3 个 user instance (已有 default = 0)
local p2 = AE.CreateInstance()    -- player 2
local p3 = AE.CreateInstance()    -- player 3
local p4 = AE.CreateInstance()    -- player 4

-- 各自不同的曝光配置
AE.SetActiveInstance(0)  AE.SetTargetEV(0.0)   -- player 1
AE.SetActiveInstance(p2) AE.SetTargetEV(-0.5)  -- player 2 暗一点
AE.SetActiveInstance(p3) AE.SetTargetEV(0.5)   -- player 3 亮一点
AE.SetActiveInstance(p4) AE.SetSpeedUp(5.0)    -- player 4 适应快

-- 渲染时配合 HDR.SetActiveInstance 同步切换
```

## 6. 渲染管线方向状态总览

| 类别 | 状态 |
|------|------|
| 多实例后处理 | ✅ 10/10 全覆盖 (HDR/TAA/Bloom/SSR/MotionBlur/SSAO/LensFlare/LensDirt/Streak/AutoExposure) |
| TAAU + Mipmap LOD Bias + Multi-HDR TAAU | ✅ F.1.0/F.1.0.1/F.1.1 |
| 截图 (PNG/HDR/EXR) + 录屏 (PNG seq/MP4) | ✅ F.0.11.x |
| TAAU 切换下游 RT 同步 | ✅ F.2 (本期) |
| 后处理接口一致性 (LitBatch) | ✅ F.2 (本期) |
| 已知可选项 | F.1.2 Velocity Nearest (按需启用) / F.0.11.6 worker thread (待真机) |

## 7. 下一步

按用户指令: 转入**非渲染基础架构**:
1. 异步资源加载 (Async Asset)
2. 内存与显存管理 (VRAM Profiling)
3. 多线程与逻辑/渲染解耦 (Tick vs Render)
4. Lua API 健壮性 + Lua 脚本热重载

详见 [docs/HANDOFF_REMAINING_TASKS.md](../HANDOFF_REMAINING_TASKS.md) §2.
