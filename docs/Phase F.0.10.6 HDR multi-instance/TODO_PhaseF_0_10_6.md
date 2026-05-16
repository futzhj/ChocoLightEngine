# Phase F.0.10.6 — HDR multi-instance TODO

> 6A 工作流 · 阶段 6 (Assess) · 待办事项
> 关联: `ACCEPTANCE_PhaseF_0_10_6.md` / `FINAL_PhaseF_0_10_6.md`

---

## 1. 强制 (必须解决)

### 1.1 ⏳ CI 6/6 验证

**状态**: 待 push (commit `b9afe74` + SP3 docs commit) GitHub Actions 完成所有平台 build

**期望平台**:
- ubuntu-latest (Linux GL 3.3)
- macos-latest (macOS GL 3.3 / Metal)
- windows-latest (Windows GL 3.3)
- emscripten (Web GLES 3.0)
- iOS (mobile GLES 3.0)
- Android (mobile GLES 3.0)

**用户操作**: 等待 5-10 分钟后, 检查 https://github.com/futzhj/ChocoLightEngine/actions

**异常处理**:
- 不动 shader, 理论上 6 平台均通过 (`DrawTonemapRegion` 仅多 scissor 一步)
- 若 macOS Metal 转 GLSL 失败 → 检查 transpile 时是否丢 uniform (不应该, 因为没新 uniform)

---

## 2. 可选 (建议但非阻塞)

### 2.1 demo 视觉验证 (推荐)

**目标**: 实际跑 demo 演示 split-screen 中 P1 黄昏 vs P2 冷夜 tonemap 差异

**实施方式**:
- 选项 A: 升级 `demo_taa_split2/main.lua` 加 per-region tonemap 调用 (改 ~30 行)
- 选项 B: 新建 `demo_tonemap_split2/main.lua` 专门演示

**示例 Lua 代码**:
```lua
local W, H = SCREEN_W, SCREEN_H
HDR.SetAutoTonemap(false)
HDR.SetAutoBloom(false)
HDR.SetAutoTAA(false)

-- 主循环 ...
HDR.BeginScene()
-- ...渲染 P1 (左半) + Bloom.Process(rgn=left) + TAA.Process(rgn=left)...
-- ...渲染 P2 (右半) + Bloom.Process(rgn=right) + TAA.Process(rgn=right)...
HDR.EndScene()

-- per-region tonemap (核心)
HDR.Tonemap(0,    0, W/2, H, {exposure=1.5, gamma=2.2, tonemap="aces"})        -- P1 黄昏
HDR.Tonemap(W/2,  0, W/2, H, {exposure=0.6, gamma=2.4, tonemap="uncharted2"})  -- P2 冷夜
```

**预期**:
- 左右两半亮度 / 色调显著不同 (验证 region 化生效)
- 中间分割线无色彩串扰 (与 F.0.10.5 边界完美)

**工作量**: ~1.5h

### 2.2 perf benchmark

**目标**: 验证 region tonemap 比 fullscreen 慢 < 5%

**工具**: RenderDoc + frame timer

**测试**:
- 单 viewport baseline (`HDR.SetAutoTonemap(true)`, fullscreen path)
- 双 region (`HDR.SetAutoTonemap(false)` + 2x `HDR.Tonemap(rgn)`)

**预期**: 双 region 比 fullscreen 慢 ~10-15% (双 pass 而非单 pass), 仍可接受

### 2.3 真多 HDR target 评估

**目标**: 评估是否需要 per-region 独立 HDR sceneTex

**典型场景**:
- A. 当前: 共享 sceneTex (P1 P2 看同一场景) → 已支持
- B. 多 HDR target: 每 region 独立 sceneTex (P1 看场景1, P2 看场景2, 不同模型) → 未支持

**B 的工作量**: 8-10h (RT pool + EndScene 多次 + Bloom/SSR 全 region 改)

**优先级**: 评估必要性后决定是否启动 phase

---

## 3. 用户支持需求

### 3.1 配置 / 环境

- **无新配置**: F.0.10.6 是渐进 API 增量, 不引入新依赖
- **现有 demo 兼容**: demo_taa_split2 自动受益 (新 API 可选, 不调即不变)

### 3.2 文档使用指引

| 角色 | 推荐阅读 |
|------|---------|
| 业务开发者 (写 split-screen) | `FINAL_PhaseF_0_10_6.md` §2.3 + TODO §2.1 demo 示例 |
| 引擎开发者 (扩展 LUT/grading) | `DESIGN_PhaseF_0_10_6.md` + `FINAL_PhaseF_0_10_6.md` §3 |

### 3.3 调试 / 排查

如 split-screen tonemap 出现问题:

```lua
-- 1. 验证 autoTonemap 是否正确关闭
print("autoTonemap =", HDR.GetAutoTonemap())   -- 应该 false

-- 2. 验证 Tonemap 调用返回值
local ok, err = HDR.Tonemap(x, y, w, h, params)
if not ok then print("Tonemap failed:", err) end

-- 3. 检查 region 是否覆盖整屏
-- 例: split-screen 左右 → region1=(0,0,W/2,H) region2=(W/2,0,W/2,H)
-- 中间不能漏一行 (W 是奇数时注意)
```

或 backend 端验证 (临时):
```cpp
// 在 DrawTonemapRegion 加 log
CC::Log(CC::LOG_INFO, "Tonemap region: (%d,%d, %dx%d) exp=%.2f gamma=%.2f mode=%d",
        rgnX, rgnY, rgnW, rgnH, exposure, gamma, tonemapMode);
```

---

## 4. 后续 Phase 候选

详见 `FINAL_PhaseF_0_10_6.md` §7.

| Phase | 主题 | 工作量 | 优先级 |
|-------|------|-------|-------|
| **F.0.10.7** (建议) | demo_tonemap_split2 视觉演示 | ~2-3h | 中 |
| **F.0.10.8** | per-region color grading LUT | ~5-7h | 中 |
| **F.0.11** | volumetric fog region 化 | ~4-5h | 低 |
| **F.1** | DLSS-like TAAU | ~10-15h | 高 (TAA 终极) |
| **F.0.10.9** (备选) | 真多 HDR target (RT pool) | ~8-10h | 中 |

---

## 5. 文档导航

| 文档 | 用途 |
|------|------|
| `ALIGNMENT_PhaseF_0_10_6.md` | 需求 / 假设 / scope |
| `DESIGN_PhaseF_0_10_6.md` | 接口契约 + 数据流 |
| `TASK_PhaseF_0_10_6.md` | 15 原子任务 + 依赖图 |
| `ACCEPTANCE_PhaseF_0_10_6.md` | 验收 + 风险矩阵 + 工作量 |
| `FINAL_PhaseF_0_10_6.md` | 项目总结 + 关键决策 + 模板复用 |
| **`TODO_PhaseF_0_10_6.md`** | 本文件 (强制 + 可选 + 用户支持) |
