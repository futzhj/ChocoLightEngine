# Phase F.1 TAAU — TASK 文档（原子化任务拆分）

> **阶段**：6A Workflow — 阶段 3 Atomize（原子化）
> **目标**：CONSENSUS → 可独立提交的最小任务单元
> **基线**：CONSENSUS_PhaseF_1.md (v1.0, 2026-05-17, 全 A)
> **下一步**：阶段 4 Approve（实施 + commit）

---

## 任务 T1 — Backend 接口扩展 + FS_TAA Shader F.1 uniforms

### T1.1 render_backend.h 接口扩展
- 修改 `DrawTAAPass` 虚函数签名，新增 `int renderW, int renderH, int outputW, int outputH, int taauEnabled` 参数（在保持向后兼容的前提下，全部默认参数；旧签名调用方不需立即修改）
- 新增 2 虚函数（默认 no-op）：
  - `virtual bool CreateOutputSceneTex(int w, int h, uint32_t* outFbo, uint32_t* outTex)`
  - `virtual void DeleteOutputSceneTex(uint32_t fbo, uint32_t tex)`

### T1.2 FS_TAA shader 扩展（GLES3 + GL3.3）
- 新增 4 uniform：`uRenderTexel`, `uOutputTexel`, `uRenderToOutputRatio`, `uTaauEnabled`
- main 函数中 cur 邻域采样步长由 `uTexel` 改为局部变量 `neighborStep = uTaauEnabled==1 ? uRenderTexel : uTexel`
- variance/ycocg/rgb clip 全 9-tap 采样均改用 `neighborStep`
- 完全保留 F.0 行为：`uTaauEnabled=0` 时 `neighborStep == uTexel`，逐字节等同 main HEAD

### T1.3 render_gl33.cpp DrawTAAPass + OutputSceneTex 实现
- buildProgram 后 `glGetUniformLocation` 4 个新 loc 并初始化（缓存到 GLint）
- DrawTAAPass impl：bind 3 纹理 + 设置 14 个 F.0 uniforms + 4 个 F.1 uniforms + 1 个 uTaauEnabled，glViewport 用 outputW/H，scissor region
- CreateOutputSceneTex / DeleteOutputSceneTex impl：RGBA16F + linear filter + clamp-to-edge

**验收**: `cmake --build` 通过；不动现有调用方（向后兼容默认参数）。

---

## 任务 T2 — HDRRenderer 双尺寸路径

### T2.1 hdr_renderer.h
- 声明 3 个新接口：
  - `void OnTAAURenderScaleChanged(int renderW, int renderH, int outputW, int outputH)`
  - `void OnTAAUDisabled()`
  - `uint32_t GetSceneTexForOutput()`

### T2.2 hdr_renderer.cpp State 字段
- 新增 `int renderW, renderH`（taau 关闭时 = width/height）
- 新增 `bool taauActive`（默认 false）
- 新增 `uint32_t outputSceneFbo, outputSceneTex`（仅 TAAU active 时分配）

### T2.3 RT 重建路径
- `OnTAAURenderScaleChanged`：DestroyRT → 用 (renderW, renderH) 调 CreateRT → 调 backend->CreateOutputSceneTex(outputW, outputH)
- `OnTAAUDisabled`：DestroyOutputSceneTex → DestroyRT → 用 (width, height) 调 CreateRT → renderW/H = 0

### T2.4 EndScene sharpen/tonemap 改写
- 关键位置：定位 EndScene 中 sharpen pass、tonemap pass 的所有 `g.sceneTex` 引用，逐个判断属于 render-res 阶段（不动）还是 output-res 阶段（改用 `GetSceneTexForOutput()`）
- 注意 viewport：tonemap 必须 `glViewport(0, 0, outputW, outputH)`，避免在 render-res viewport 下 sample output-res 纹理

### T2.5 Multi-Instance 协调
- F.1.0 仅 `g_states[0]` 走双尺寸路径；user instance 检查 `taauActive=false`（保兼容）
- `OnTAAURenderScaleChanged` 内部 `g_active` 暂存 + 锁定到 0，结束后还原

**验收**: `taauActive=false` 时画面与 main HEAD 完全一致（demo_ssr / demo_taa_split2 走查）。

---

## 任务 T3 — TAARenderer F.1 API + Process 双路径

### T3.1 taa_renderer.h 公开 API
新增 8 函数声明：
- `SetTAAUEnabled(bool) / GetTAAUEnabled()`
- `SetRenderScale(float) / GetRenderScale()`
- `SetUpscalePreset(const char*) / GetUpscalePreset()`
- `GetRenderResolution(int*, int*)`
- `GetOutputResolution(int*, int*)`

### T3.2 State 字段
- `bool taauEnabled = false`
- `float renderScale = 1.0f`
- `int upscalePreset = 3` (3=native)
- `int renderW = 0, renderH = 0`

### T3.3 ApplyJitter 改写
- 计算 ndcOff 时分母由 `g.width / g.height` 改为：
  - `taauEnabled && renderW > 0 ? renderW : g.width`（H 同理）
- 保证 F.0 行为零回归

### T3.4 SetTAAUEnabled 实现（含 Q5 仲裁）
- flag==当前值时短路返回
- flag=true 时：halfResHistory 强制关 + warning；renderW/H 重算；调 `HDRRenderer::OnTAAURenderScaleChanged`
- flag=false 时：renderW/H 归零；调 `HDRRenderer::OnTAAUDisabled`
- 重置 `hasHistory=false`，`historyIdx=0`，`frameCounter=0`

### T3.5 SetRenderScale 实现
- clamp [0.5, 1.0] + log info
- 仅 taauEnabled=true 时触发 HDR 重建（避免 F.0 用户误调改尺寸）
- 重建后 `hasHistory=false`
- 同步更新 upscalePreset（容差 0.01 匹配预设档）

### T3.6 SetUpscalePreset 实现
- 4 档字符串映射：performance=0.5 / balanced=0.667 / quality=0.75 / native=1.0
- 未知字符串：warning + 不生效
- 内部调 `SetRenderScale` 完成实际尺寸更新

### T3.7 Process 双路径
- 取 sceneTex（来自 hdrTex 入参，已在 HDR.EndScene 内是 render-res 当 taauActive）
- 取 historyTex / velocityTex
- 调 backend->DrawTAAPass，传 (renderW, renderH, outputW, outputH, taauEnabled)
- 调 BlitTAAToHDR 时目标 FBO 在 outputSceneFbo（taauActive）or 原 hdrFbo（F.0）

### T3.8 CloneInstance 兼容
- F.1 新字段全部复制；newInstance 不继承 hasHistory（保持现有逻辑）

**验收**: smoke 增量通过；taauEnabled 切换不 crash；jitter 量级正确（renderScale=0.667 时 ndcOff 比 1.0 时大 ~50%）。

---

## 任务 T4 — Lua bridge + Smoke

### T4.1 light_graphics.cpp 8 Lua 函数
- l_TAA_SetTAAUEnabled / GetTAAUEnabled
- l_TAA_SetRenderScale / GetRenderScale
- l_TAA_SetUpscalePreset / GetUpscalePreset
- l_TAA_GetRenderResolution / GetOutputResolution

### T4.2 taa_funcs[] 注册
- 添加 8 个 entry 到 lua_register table

### T4.3 scripts/smoke/taa.lua 增量
- 6-8 检查点：surface / defaults / clamp / preset-scale-bidi-sync / Q5 arbitration / resolution query

**验收**: `light.exe scripts/smoke/taa.lua` 全过。

---

## 任务 T5 — demo_taau + 文档收尾

### T5.1 demo_taau
- main.lua（按 ALIGNMENT §3.4 + DESIGN §7.2 设计）
- README.md（键位 + renderScale 说明）

### T5.2 文档收尾
- ACCEPTANCE_PhaseF_1.md（验收清单填实际测试结果）
- FINAL_PhaseF_1.md（实施记录、commit 哈希、问题与修复）
- TODO_PhaseF_1.md（剩余可选增量：F.1.0.1 / F.1.1 / F.1.2）
- docs/api/Light_Graphics.md（TAA 子表 +8 函数）
- CHANGELOG.md（Phase F.1 入口）

**验收**: 文档齐全，demo 真机可跑。

---

## 实施顺序

```
T1 → T2 → T3 → T4 → 构建+smoke 验证 → T5 demo + 文档
```

**估时**: 3-5 工时（不含真机性能验收 + 用户回看修订）。

---

## 风险记录

| 触发条件 | 检测点 | 缓解 |
|---|---|---|
| EndScene 内 sceneTex 引用漏改 | demo 黑屏 / 错误纹理 | T2.4 grep 全文 + 单步 review |
| Multi-instance 错误激活 TAAU | user instance 1..3 启用时 HDR 重建破坏 default | T3.4 检查 g_active==0；非 0 拒绝 + warning |
| F.0 demo 视觉回归 | demo_ssr 跑出来不对 | T2 完成后立即跑 demo_ssr 走查 |
| jitter NDC 折算错 | 画面抖动量级不对 | T3.3 单元测试: renderScale 0.5 时 ndcOff 是 1.0 时的 2× |

---

## 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 5 任务原子化 |
