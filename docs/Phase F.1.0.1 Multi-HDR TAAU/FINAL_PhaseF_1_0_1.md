# Phase F.1.0.1 Multi-HDR × TAAU — FINAL 文档（实施记录）

> **阶段**：6A Workflow — 阶段 4 Approve / Apply（实施）
> **基线**：PLAN_PhaseF_1_0_1.md
> **实施日期**：2026-05-17
> **完成度**：F.1.0.1 全量交付 (移除 F.1.0 的 g_active==0 限制)

---

## 1. 实施时间线

| 任务 | 实际产出 | 耗时 |
|---|---|---|
| T1 | 移除 SetTAAUEnabled / SetRenderScale 中 `g_active != 0` 限制 + 头部注释更新 | ~15 min |
| T2 | scripts/smoke/taa.lua 章节 12 (3 子检查: state 隔离 / SetTAAUEnabled 放开 / Clone 保留 scale) | ~30 min |
| T3 | demo_multi_hdr_pip 增 PIP TAA instance 创建 + T 键切 PIP TAAU + HUD + cleanup | ~30 min |
| T4 | 构建 + 4 demo 零回归验证 + 文档收尾 | ~30 min |

**总计**: ~2 小时 (与 PLAN 估时一致)

---

## 2. 文件改动清单

| 文件 | 改动类型 | 改动量 |
|---|---|---|
| `ChocoLight/src/taa_renderer.cpp` | 修改 | -7 行 (移除 g_active != 0 检查 + 注释) +5 行 (Phase F.1.0.1 注释) |
| `ChocoLight/include/taa_renderer.h` | 修改 | 头部注释 2 处更新 |
| `scripts/smoke/taa.lua` | 修改 | +75 行 (章节 12, 3 子检查点) |
| `samples/demo_multi_hdr_pip/main.lua` | 修改 | +50 行 (TAA 加载 + PIP TAA instance setup + T 键 + HUD + cleanup) |
| `docs/Phase F.1.0.1 Multi-HDR TAAU/PLAN_PhaseF_1_0_1.md` | 新建 | ~120 行 |
| `docs/Phase F.1.0.1 Multi-HDR TAAU/ACCEPTANCE_PhaseF_1_0_1.md` | 新建 | ~110 行 |
| `docs/Phase F.1.0.1 Multi-HDR TAAU/FINAL_PhaseF_1_0_1.md` | 新建 | 本文 |
| `docs/HANDOFF_REMAINING_TASKS.md` | 修改 | F.1.0.1 状态更新 |

**总改动**: 4 文件代码修改 + 3 文档新建 + 1 索引更新

---

## 3. 关键实现细节

### 3.1 限制移除 (taa_renderer.cpp)

**SetTAAUEnabled** (旧):
```cpp
if (g_active != 0) {
    CC::Log(CC::LOG_WARN, "TAARenderer::SetTAAUEnabled: F.1.0 仅 default instance 支持 ...");
    return false;
}
```

**SetTAAUEnabled** (新):
```cpp
// Phase F.1.0.1: 移除 F.1.0 的 `g_active != 0` 限制, 让 user instance 也能启用 TAAU.
//   调用方需保证 HDR g_active 与 TAA g_active 一致 (否则 GetSceneFboForOutput 返错 fbo).
//   典型用法: HDR.SetActiveInstance(pipId); TAA.SetActiveInstance(pipId); TAA.SetTAAUEnabled(true)
```

**SetRenderScale** 同步放开: `if (g.taauEnabled && g.enabled && g_active == 0)` → `if (g.taauEnabled && g.enabled)`

### 3.2 Clone 行为 (taa_renderer.cpp)

**老**: 强制清 `taauEnabled` + 显式说 "F.1.0 限制" 不让继承 (但 renderScale / upscalePreset 已经在保留)
**新**: 注释更新表明 "新 instance 走自己 Enable + SetTAAUEnabled, 但 scale/preset 已经传过来用户期待生效"

### 3.3 demo_multi_hdr_pip 改动模式

```lua
-- 顶部: 检测 TAA + TAAU 可用性
local TAA = Gfx.TAA
local hasTAAU = type(TAA) == 'table' and type(TAA.SetTAAUEnabled) == 'function'
                                     and type(TAA.SetUpscalePreset) == 'function'

-- OnOpen 末尾: 创建 PIP TAA instance, 默认 OFF
if hasTAAU and TAA.IsSupported() then
    g_pipTaaId = TAA.CreateInstance() or 0
    if g_pipTaaId > 0 then
        TAA.SetActiveInstance(g_pipTaaId)
        TAA.Enable(PIP_W, PIP_H)
        TAA.SetActiveInstance(0)
    end
end

-- T 键: 同步切 HDR + TAA active 到 PIP, SetTAAUEnabled, 切回 default
elseif key == 84 then
    if g_pipTaaId > 0 and hasTAAU then
        g_pipTaauOn = not g_pipTaauOn
        HDR.SetActiveInstance(g_pipId); TAA.SetActiveInstance(g_pipTaaId)
        if g_pipTaauOn then TAA.SetUpscalePreset('balanced') end
        TAA.SetTAAUEnabled(g_pipTaauOn)
        HDR.SetActiveInstance(0); TAA.SetActiveInstance(0)
    end
end

-- cleanup: PIP TAA instance 必须在 PIP HDR 销毁前释放
if g_pipTaaId > 0 and hasTAAU then
    TAA.SetActiveInstance(g_pipTaaId)
    if g_pipTaauOn then TAA.SetTAAUEnabled(false) end
    TAA.Disable()
    TAA.SetActiveInstance(0)
    TAA.DestroyInstance(g_pipTaaId)
end
```

---

## 4. 测试覆盖

### 4.1 Smoke (3 新检查点全过)
- ✅ user instance × TAAU state 隔离 (default=balanced, user instance=performance 互不影响)
- ✅ user instance SetTAAUEnabled 不再被拒绝 (返 boolean 而非 false 加 warning)
- ✅ Clone 保留 renderScale + upscalePreset, 清 taauEnabled

### 4.2 Zero-Regression (4 demo)
- ✅ demo_ssr (F.0.x SSR 路径)
- ✅ demo_taa_split2 (multi-instance TAA + region split-screen 路径)
- ✅ demo_taau (F.1.0 default instance TAAU 路径)
- ✅ demo_multi_hdr_pip (multi-HDR-instance 路径 + 新增 PIP × TAAU)

### 4.3 真机视觉 (待用户)
- ⏳ demo_multi_hdr_pip: 按 T 切 PIP TAAU, HUD 显示 "PIP TAAU: ON (balanced, render 320x180 -> output 480x270)"
- ⏳ PIP 视觉对比 OFF/ON 应可见 TAA 时序累积效应

---

## 5. 已知 / 留观察问题

### 5.1 设计层
- **HDR g_active 与 TAA g_active 不同步时静默错误**: 设计上不引入强制检查 (用户可能临时切, 见 ACCEPTANCE §3.2). 头部注释 + PLAN 文档明示约定.
- **OnTAAURenderScaleChanged 在 g_active 切换中的安全性**: HDRRenderer 内部 ReleaseRT/CreateRT 都作用于 g_active 槽, 调用方在 SetActiveInstance 后再调 SetTAAUEnabled, 路径正确.

### 5.2 待 F.1.1 / F.1.2 (与 F.1.0 路线图一致)
- F.1.1 Mipmap LOD bias (TAAU 视觉接近 native 关键)
- F.1.2 Velocity nearest-filter (按需启用)

### 5.3 性能
- multi-HDR × TAAU 性能收益依赖每 instance 独立的 raster + 后处理负担; PIP 480×270 → 0.667 scale = 320×180 = 缩到 ~44% 像素, raster 时间应有显著降低. **真机验证由用户执行**.

---

## 6. 文档版本

| 版本 | 日期 | 修订 |
|---|---|---|
| v1.0 | 2026-05-17 | 实施完结 — F.1.0.1 全量代码 + 文档交付 |
