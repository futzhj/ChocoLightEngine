# Phase F.1 TAAU — CONSENSUS 文档

> **阶段**：6A Workflow — 阶段 1 Align（对齐）→ 锁定共识
> **目标**：ALIGNMENT 决策 Q1-Q8 → 用户拍板 → 锁定方案
> **基线**：ALIGNMENT_PhaseF_1.md (v0.1, 2026-05-17)
> **拍板日期**：2026-05-17
> **下一步**：阶段 3 Atomize → TASK_PhaseF_1.md

---

## 1. 拍板结果

用户回复：**"好的启动"** → 默认接受全推荐 A 组合。

| 决策点 | 锁定方案 |
|---|---|
| Q1 单 pass vs 分离两 pass | **A — 单 pass TAA-Upsample (UE4 风格)** |
| Q2 renderScale 范围 | **A — [0.5, 1.0]** |
| Q3 UpscalePreset | **A — 4 档暴露 (performance/balanced/quality/native)，与 renderScale 双向同步** |
| Q4 默认 TAAUEnabled | **A — false (兼容优先)** |
| Q5 HalfResHistory 冲突仲裁 | **A — 启用 TAAU 时强制关闭 HalfResHistory + log warning** |
| Q6 F.0.9/F.0.14 Upscale 在 TAAU 路径 | **A — dead code (不混合，保留 API)** |
| Q7 Mipmap LOD bias | **B — 留 F.1.1 (本次不动 mesh shader)** |
| Q8 Resize 路径 | **A — outputResize 触发全重建** |

---

## 2. 实施阶段范围（F.1.0 单 Instance 主路径）

### 范围内
- Backend 接口扩展（`DrawTAAPass` 增 4 参 + 2 个新 OutputSceneTex 函数）
- HDRRenderer 双尺寸路径（**仅 default instance id=0**）
- TAARenderer State + 8 新 Lua API（3 setter + 3 getter + 2 query）
- FS_TAA shader 增 4 uniform，main 函数 cur 邻域步长改用 `uRenderTexel`
- `samples/demo_taau` 新建（4 档预设切换 + HUD）
- Smoke 增 6-8 检查点
- 7 件套文档（ALIGNMENT/CONSENSUS 已完成；DESIGN 已起草；TASK/ACCEPTANCE/FINAL/TODO 待写）

### 范围外（推迟）
- F.1.0.1 Multi-instance × TAAU（user instance 1..3）→ 独立增量 PR
- F.1.1 Mipmap LOD bias 调整
- F.1.2 Velocity nearest sample
- F.2+ FSR2 / DLSS NN 风格

---

## 3. 关键不变量（实施时不可破坏）

| 不变量 | 说明 |
|---|---|
| **F.0 行为零回归** | `taauEnabled=false` 时所有 demo 视觉 + 性能完全等同 main HEAD |
| **velocity 是 UV-space delta** | render-res 与 output-res shader 共用同一份 velocity 纹理无需缩放 |
| **history 始终在 output-res** | 不论 renderScale 多少 |
| **jitter 按 render-res pixel** | NDC 偏移 = `jitter * 2.0 / renderRes` |
| **sceneTex 双角色由 helper 单一入口** | `HDRRenderer::GetSceneTexForOutput()` 是唯一入口，sharpen/tonemap 必经 |
| **Q5 仲裁自动执行** | `SetTAAUEnabled(true)` 时若 halfResHistory=true 自动关闭 + warning |
| **Q4 默认值** | `taauEnabled=false`, `renderScale=1.0`, `upscalePreset="native"` |

---

## 4. 文件改动总览（最终清单）

| 文件 | 改动类型 | 改动量预估 |
|---|---|---|
| `ChocoLight/include/render_backend.h` | 修改 | ~30 行（DrawTAAPass 签名扩展 + 2 新虚函数） |
| `ChocoLight/src/render_gl33.cpp` | 修改 | ~150 行（FS_TAA shader 扩展 + DrawTAAPass impl + OutputSceneTex impl + 4 GLint loc + buildProgram 内 glGetUniformLocation） |
| `ChocoLight/include/hdr_renderer.h` | 修改 | ~10 行（3 新声明） |
| `ChocoLight/src/hdr_renderer.cpp` | 修改 | ~120 行（State +3 字段 + OnTAAURenderScaleChanged + OnTAAUDisabled + GetSceneTexForOutput + EndScene 内 sharpen/tonemap 改写） |
| `ChocoLight/include/taa_renderer.h` | 修改 | ~30 行（8 新公开 API 声明） |
| `ChocoLight/src/taa_renderer.cpp` | 修改 | ~180 行（State +5 字段 + ApplyJitter render-res + Process 双路径 + SetTAAUEnabled 含 Q5 仲裁 + SetRenderScale + SetUpscalePreset + 双向同步 + Get Resolution × 2） |
| `ChocoLight/src/light_graphics.cpp` | 修改 | ~80 行（8 Lua bridge + taa_funcs[] 注册） |
| `scripts/smoke/taa.lua` | 修改 | +60 行（6-8 检查点） |
| `samples/demo_taau/main.lua` | 新增 | ~250 行 |
| `samples/demo_taau/README.md` | 新增 | ~50 行 |
| `docs/Phase F.1 TAAU/CONSENSUS_PhaseF_1.md` | 新增 | 当前文件 |
| `docs/Phase F.1 TAAU/TASK_PhaseF_1.md` | 新增 | 待写 |
| `docs/Phase F.1 TAAU/ACCEPTANCE_PhaseF_1.md` | 新增 | 验收（实施完写） |
| `docs/Phase F.1 TAAU/FINAL_PhaseF_1.md` | 新增 | 实施记录（实施完写） |
| `docs/api/Light_Graphics.md` | 修改 | TAA 子表 +8 函数 |
| `CHANGELOG.md` | 修改 | Phase F.1 入口 |

**总代码改动估算**：~700 行（含 demo + smoke + docs metadata）

---

## 5. Commit 拆分计划

为方便 review 和回退，拆 **5 个 commit**：

```
Commit 1: Phase F.1.0 — Backend interface + FS_TAA shader F.1 uniforms
  - render_backend.h: DrawTAAPass 签名扩展, CreateOutputSceneTex/DeleteOutputSceneTex 虚函数
  - render_gl33.cpp: FS_TAA shader 增 4 uniform (GLES3 + GL33), DrawTAAPass impl, OutputSceneTex impl

Commit 2: Phase F.1.0 — HDRRenderer 双尺寸路径
  - hdr_renderer.h: OnTAAURenderScaleChanged / OnTAAUDisabled / GetSceneTexForOutput
  - hdr_renderer.cpp: State +3, RT 重建路径, EndScene sharpen/tonemap 走 helper

Commit 3: Phase F.1.0 — TAARenderer F.1 API + Process 双路径
  - taa_renderer.h: 8 新 API
  - taa_renderer.cpp: State +5, ApplyJitter render-res NDC, Process 双路径, Q5 仲裁

Commit 4: Phase F.1.0 — Lua bridge + smoke 增量
  - light_graphics.cpp: 8 bridge + 注册
  - scripts/smoke/taa.lua: +6-8 检查点

Commit 5: Phase F.1.0 — demo_taau + 文档完结
  - samples/demo_taau/{main.lua, README.md}
  - docs/Phase F.1 TAAU/{TASK, ACCEPTANCE, FINAL, TODO}.md
  - docs/api/Light_Graphics.md, CHANGELOG.md
```

---

## 6. 验收门槛

- ✅ Release 构建通过（CI 6/6 platforms green）
- ✅ Smoke 含 F.1 6-8 检查点全过
- ✅ `demo_ssr` / `demo_taa_split2` 默认运行画面 / 性能与 main HEAD 一致（视觉 0 回归）
- ✅ `demo_taau` 真机 4 档 (perf/bal/qual/native) 切换无 crash，HUD 正确显示
- ✅ Q5 仲裁可观察（启用 TAAU 时 HalfResHistory 自动关闭 + log warning）

性能实际收益由用户在真机做对比测试（CI 不参与），目标：
- renderScale=0.667 (Balanced): GPU 时间 -30%~40%
- renderScale=0.5 (Performance): GPU 时间 -45%~60%

---

## 7. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 拍板 — 全推荐 A 组合，F.1.0 单 instance 主路径 |
