# ALIGNMENT — Phase AW.x（GPU Skinning 真机验证工具链）

> **6A 工作流 Stage 1 — Align**：模糊需求 → 精确规范

**生成时间**: 2026-05-10
**前置阶段**: Phase AW（已完成 ✅，6 平台 CI 全绿，170 PASS / 0 FAIL）

---

## 1. 项目和任务特性规范

### 1.1 项目现状（基于实际代码勘察）

| 维度 | 现状 |
|------|------|
| 引擎 | ChocoLight Engine (Lua 5.1 + SDL3 + 多 Backend 渲染) |
| 跨平台 | Windows / Linux / macOS / Android / iOS / Web 6 平台 |
| 已交付阶段 | Phase A（Sprite Batcher）/ AR（Timer）/ AS（PBR Material）/ AU（physics3d）/ AV（骨骼动画）/ AV.x（procedural 内省 API）/ **AW（GPU Skinning 调度）** |
| 6A 工作流 | 全阶段标准化（Align→Architect→Atomize→Approve→Automate→Assess） |

### 1.2 与 Phase AW.x 直接相关的现有能力

| 已有能力 | 文件 / API | 适用性 |
|---------|------------|--------|
| `Light.Time` 完整模块 | `ChocoLight/src/light_time.cpp` | ✅ **不需新建** — 已含 `GetTicksNS()`、`GetPerformanceCounter/Frequency()`、`Delay/DelayNS/DelayPrecise` |
| `Light.Animation.SetSkinningMode("auto"/"cpu"/"gpu")` | Phase AW T5 | ✅ 调度入口已就绪 |
| `Light.Animation.GetSkinningMode()` | Phase AW T5 | ✅ 返回当前实际生效路径 |
| OOP Window 框架范式 | `samples/perf_benchmark/main.lua` | ✅ 可参考 (`Light(Light.UI.Window):New()` + `OnOpen`/`OnKey`/`Update`/`Draw`) |
| Console-mode 动画演示 | `samples/demo_animation/main.lua` | ⚠️ 不能改造为 frame timing demo（无 Window 渲染循环） |
| `Anim.LoadSkinnedGLTF` | Phase AV | ✅ 真机有 glTF 资产时可用 |
| Procedural skeleton/clip | Phase AV.x `NewEmptySkeleton` / `NewEmptyClip` | ⚠️ **没有 procedural skinned mesh 入口** — 资产缺失时无法 procedural 兜底 |
| `Light.Graphics.GetBackendName` | ⚠️ **不存在**（`perf_benchmark` 防御性 nil-check） | 不影响 Phase AW.x（用 `GetSkinningMode` 替代） |

### 1.3 Phase AW TODO 关联

| TODO 项 | 优先级 | 本阶段处理 |
|---------|--------|-----------|
| §1.1 真机性能 baseline 测量 | P0 | ✅ 本阶段交付 |
| §1.2 GPU vs CPU 数值/视觉一致性验证 | P0 | ✅ 本阶段交付（视觉方案 B） |
| §2.1 关节上限 64→128 | P1 | ❌ 不在范围（独立任务） |
| §2.2 Web GPU 默认值校验 | P1 | ❌ 不在范围（依赖浏览器矩阵测试） |
| §2.3 jointMats 增量上传 | P1 | ❌ 不在范围（依赖 profile 数据） |
| §2.4 多 mesh 共享 animator | P1 | ❌ 不在范围 |
| §3.3 demo skinning mode 示例 | P2 | ✅ 本阶段交付（合并入 §1.1 demo） |

---

## 2. 原始需求

> 用户对话原文："**继续，6A**"（在 Phase AW 已完成、`TODO_PhaseAW.md` 中选择"P0: 真机性能 + 视觉验证"工作流方向之后）。
>
> 需求选项卡描述：**"按 TODO_PhaseAW.md 推进未完事项：补 Light.Util.FrameTime API + frame timing demo + GPU/CPU 视觉对比 helper，让 Phase AW 收益可量化"**

**澄清点**：选项卡描述中的"`Light.Util.FrameTime` API"在项目上下文勘察后**修正为**：直接使用已存在的 `Light.Time.GetTicksNS()` — 无需新建模块。

---

## 3. 边界确认

### 3.1 范围内（IN）

| # | 交付物 |
|---|-------|
| 1 | **新建 `samples/demo_skinning_perf/main.lua`** — 完整 OOP Window demo，参考 `perf_benchmark` 风格 |
| 2 | demo 内置 frame timing helper（rolling avg / min / max，纯 Lua 实现，基于 `Light.Time.GetTicksNS`）|
| 3 | demo 支持运行时 G/C/A 键切换 GPU/CPU/AUTO 模式 + 屏上实时显示当前模式 + 当前帧 ms |
| 4 | demo 资产缺失时 graceful fallback（提示资产路径 + 退出，不崩） |
| 5 | demo 启动时打印一次 baseline（CPU/GPU 各 60 帧 avg），便于 CI / 终端用户复制粘贴 |
| 6 | **新建 `docs/api/Light_Animation.md` 中"如何在真机验证 GPU skinning 收益"段** — 引用新 sample，给出操作步骤 |
| 7 | **更新 `samples/README.md`** — 登记新 sample |
| 8 | **更新 `TODO_PhaseAW.md`** — 标记 §1.1 §1.2 §3.3 已完成 |

### 3.2 范围外（OUT）

| # | 项 | 理由 |
|---|---|------|
| 1 | 不修改 `light_animation.cpp` C++ 实现 | Phase AW 已完整交付；本阶段纯工具链 |
| 2 | 不修改 GL33Backend / shader | 同上 |
| 3 | 不引入新 C++ Lua API（如 `Light.Util.FrameTime`）| `Light.Time.GetTicksNS()` 已够用 |
| 4 | 不改 `samples/demo_animation/main.lua` | 它是 console-mode 测试，作用不同；保持纯净 |
| 5 | 不实现 `Light.Graphics.GetBackendName` | 与 Phase AW.x 主线无关；如需要可独立小任务 |
| 6 | 不做 GPU vs CPU 像素级数值对比（方案 A）| CONSENSUS Q6 已决策走视觉对比（方案 B）|
| 7 | 不做关节上限提升 / Web GPU 默认值切换 / 增量 UBO | Phase AW TODO §2.x 范围 |
| 8 | 不做 smoke 段补充 | 现有 `[15]` 段已覆盖 API 表面；frame timing demo 是 sample 而非 smoke |

### 3.3 不阻塞但建议跟进（FOLLOW-UP）

| # | 项 | 建议时机 |
|---|---|---------|
| 1 | `Light.Graphics.GetBackendName` 实现 | 独立小任务（10 分钟） |
| 2 | `Anim.NewProceduralSkinnedMesh(N_verts, N_joints)` API | 当用户有"无资产 demo"需求时 |
| 3 | CI runner 加 GPU smoke（如 GitHub-hosted Linux + Mesa）| 工程量较大 |

---

## 4. 需求理解

### 4.1 核心目的

让用户/QA 能**在 5 分钟内**得到一个具体数字，回答"Phase AW GPU skinning 在我的真机上提升了多少？"

### 4.2 用户使用流程（预期）

```powershell
# 用户在桌面 GPU 机器（有 GL3.3 上下文）
cd e:\jinyiNew\Light
.\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua

# 期望输出（控制台 + 屏上 OSD）：
# ==== Skinning Perf Demo ====
# Backend supports GPU skinning: true
# Loading samples/demo_skinning_perf/assets/character.glb ... 5234 verts, 28 joints
# Calibrating CPU baseline (60 frames)... avg=1.482ms, min=1.31ms, max=1.94ms
# Calibrating GPU baseline (60 frames)... avg=0.063ms, min=0.05ms, max=0.11ms
# Speedup: 23.5x
#
# Keys: G=GPU / C=CPU / A=AUTO / R=re-baseline / ESC=quit
# (屏上 OSD 实时显示: Mode=GPU, frame=0.06ms (60fps cap)
```

### 4.3 资产策略

- **资产存在** → 完整 baseline + 交互 demo
- **资产缺失** → 友好提示 + 退出码 0（不崩；与 demo_animation 一致）

### 4.4 跨平台一致性

- demo 主要用于桌面机器测速（移动端 / Web 用户群体单独适配）
- 但代码必须**编译通过 6 平台**（纯 Lua sample 自动满足）
- 退出码与 demo_animation 一致（headless / 资产缺失都返回 0）

---

## 5. 疑问澄清（智能决策）

按 6A 规范，对所有歧义点：**优先基于现有项目内容自决** + **关键决策主动询问**。

### 5.1 自主决策项（基于现有项目模式）

#### **DQ1**：sample 入口位置 → **新建独立 `samples/demo_skinning_perf/`**

理由：
- `samples/demo_animation` 是 console-mode 验证（无 Window，跑完即退），改造为 OOP Window 会破坏其原始用途
- 新建独立 sample 与 `perf_benchmark` 风格平行，命名清晰（`demo_*` = 功能演示，`perf_*` 已被 Phase A 占用所以选 `demo_skinning_perf`）

#### **DQ2**：视觉对比方式 → **时间切片 + OSD 显示**

理由：
- 简单清晰：3 秒 GPU / 3 秒 CPU 自动切换，OSD 显示当前模式 + 实时 frame ms
- 同帧分屏需要双 viewport / 双 framebuffer 改造，工程量过大且超出 sample 范围
- 用户也可手动按 G/C/A 键切换

#### **DQ3**：资产缺失策略 → **友好提示 + 退出码 0**

理由：
- 与 `demo_animation` 保持一致（资产缺失打印路径 + `print("xxx ok")`）
- 不引入 procedural skinned mesh API（超出范围；详见 OUT-3）
- CI 必然资产缺失 — 退出码 0 才能跟现有 6 平台 build 流水线一致

#### **DQ4**：frame timing 实现位置 → **纯 Lua 写在 sample 内**

理由：
- `Light.Time.GetTicksNS()` 已是 ns 精度，纯 Lua wrap 一个 ring buffer 即可
- 单个 sample 内的 helper 不值得抽到引擎层
- 后续如有多 sample 需要可再抽到 `samples/_lib/`（但当前只有一个 sample 用）

#### **DQ5**：baseline 帧数 → **60 帧 + 1 秒预热**

理由：
- 60 帧约 1 秒，足够稳定（首帧 GPU mesh 上传 + shader JIT 在预热期完成）
- 太少（<30）抖动大；太多（>120）用户等候时间长
- baseline 完成后才进入交互模式（避免用户预期混乱）

#### **DQ6**：是否在 demo 启动时调用 `SetSkinningMode("gpu")` 强开 → **是**

理由：
- demo 目的是测试 GPU 路径；如果 AUTO 在某平台 fallback 到 CPU，用户不会知道是 fallback
- 启动时检查 `g_render:SupportsGPUSkinning()` 等价物（通过 `SetSkinningMode("gpu")` + `GetSkinningMode()` 检查实际生效）
- 不支持时打印 warning 但仍跑 baseline（会得到 CPU 数据 — 也有意义，证明 fallback 正常）

#### **DQ7**：OSD 显示如何实现 → **`Light.Graphics.Print`**

理由：
- 与 perf_benchmark 一致（line 106 `Light.Graphics.Print("...", nil, 20, 20, 0)`）
- 默认字体；左上角

#### **DQ8**：3D 视角 / 摄像机 → **固定固定固定（最小改动）**

理由：
- demo 重点是**性能测试**而非渲染美学
- 用 Phase AS / AV 已有的 3D 设置 default（unchanged）
- 模型应该自然出现在视野内（用 LoadSkinnedGLTF 解出的 bbox 自动 fit）

---

### 5.2 主动询问项（关键决策需要用户确认）

#### **AQ1**：是否同时实现 `Light.Graphics.GetBackendName()` 作为附赠？

- 选项 A：是 — 顺手补一个简单查询（10 分钟工作量），让 demo 显示 "Backend: GL33 + GPU Skinning Supported"
- 选项 B：否 — 严格按范围，本阶段只做 sample；`GetBackendName` 留作独立小任务
- **倾向**：选项 A（与 Phase AW.x 主题"真机验证"高度相关；用户复制粘贴 demo 输出时希望看到 backend 信息）

#### **AQ2**：是否在 sample 中放置一个示例 glTF 资产？

- 选项 A：放置一个轻量 glTF 资产（如 fox.glb < 1MB）于 `samples/demo_skinning_perf/assets/character.glb`，commit 到 git
- 选项 B：不放置 — 用户自备资产，demo 检测多个候选路径
- 选项 C：在 README 提供 glTF-Sample-Models 下载链接 + 命令行 wget
- **倾向**：选项 C（不增加 git 仓库体积；用户 5 分钟内可下载资产；保持 cross-platform 中立）

#### **AQ3**：sample 是否应支持 headless mode（CI 跑得通）？

- 选项 A：是 — 启动时检测无 Window 上下文则跳过 baseline + 退出码 0
- 选项 B：否 — sample 仅本地真机；CI 不跑（`samples/` 目录大多数 demo 都不在 CI 中）
- **倾向**：选项 A（与 demo_animation 一致：资产缺失 / headless 都退出码 0；保护 CI 流水线）

---

## 6. 后续阶段预览

| 阶段 | 产物 | 预计文件 |
|------|------|---------|
| Stage 2 Architect | `CONSENSUS_PhaseAWx.md` + `DESIGN_PhaseAWx.md` | demo 整体架构 + 模块依赖 + frame timing helper 数据流 |
| Stage 3 Atomize | `TASK_PhaseAWx.md` | 4-6 个原子任务（demo 主体 / frame timing helper / OSD / readme / API doc 段 / TODO 同步） |
| Stage 4 Approve | （审批清单）| 由用户审核后进入实施 |
| Stage 5 Automate | `ACCEPTANCE_PhaseAWx.md` 增量更新 | 每任务 commit + push CI |
| Stage 6 Assess | `FINAL_PhaseAWx.md` + `TODO_PhaseAWx.md` | 总结 + 后续待办 |

---

## 7. 等待用户决策

请确认 §5.2 中的 **AQ1 / AQ2 / AQ3** 三个关键决策；之后进入 Stage 2 Architect 生成 CONSENSUS + DESIGN。
