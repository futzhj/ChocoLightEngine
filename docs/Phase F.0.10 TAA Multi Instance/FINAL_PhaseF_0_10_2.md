# Phase F.0.10.2 — 真物理 Split-Screen FINAL 总结

> 6A 工作流 · 阶段 6 (Assess) · FINAL
> 关联: PLAN / DESIGN / ACCEPTANCE
> 提交日期: 2026-05-15 ~ 16
> 总工作量: 4 sub-phase, ~6h (低于原估 8-12h, 因走 scissor 路径)

---

## 1. 交付物清单

### 1.1 引擎代码 (C++)

| 文件 | 变更 | 行数 |
|------|------|------|
| `ChocoLight/include/render_backend.h` | 加 region 参数到 7 个 TAA pass 虚接口 | +14 |
| `ChocoLight/src/render_gl33.cpp` | GL33Backend 7 个 TAA pass 实现 scissor + dst rect | +84 |
| `ChocoLight/src/render_legacy.cpp` | GetViewport (Phase 1) | +9 |
| `ChocoLight/include/taa_renderer.h` | 加 Process region overload 声明 | +12 |
| `ChocoLight/src/taa_renderer.cpp` | Process(fbo,tex) 转发到 Process(fbo,tex,rgnX,Y,W,H) | +33 |
| `ChocoLight/include/hdr_renderer.h` | 加 SetAutoTAA / GetAutoTAA 声明 | +14 |
| `ChocoLight/src/hdr_renderer.cpp` | autoTAA 状态字段 + impl + EndScene 条件调用 | +20 |
| `ChocoLight/src/light_graphics.cpp` | l_SetViewport / l_GetViewport / l_HDR_SetAutoTAA / l_HDR_GetAutoTAA / l_TAA_Process 5 个 Lua binding | +130 |

总计 C++: **~316 行新增**, 4 commit (Phase 1 + 2 + 3 各一次, Phase 1 fix 一次).

### 1.2 Smoke 测试

| 文件 | 增量 |
|------|------|
| `scripts/smoke/taa.lua` | +6 PASS (Phase F.0.10.2 Process 区段 10.1–10.6) + Process 加入 fn_names |
| `scripts/smoke/hdr.lua` | +4 PASS (Phase F.0.10.2 SetAutoTAA 区段 11.1–11.4) + 2 fn 加入 fn_names |
| `scripts/smoke/graphics.lua` | Phase 1 已加 SetViewport / GetViewport 段 |

### 1.3 Demo

| 文件 | 新建 |
|------|-----|
| `samples/demo_taa_split2/main.lua` | 真物理 split-screen demo (双 player 同帧, 双 TAA instance) ~400 行 |
| `samples/demo_taa_split2/README.md` | 设计说明 + 对比 F.0.10.1 + 已知约束 |

### 1.4 6A 文档

| 文件 | 内容 |
|------|-----|
| `docs/.../PLAN_PhaseF_0_10_2.md` | 阶段 1 Align + 3 备选路径决策点 |
| `docs/.../DESIGN_PhaseF_0_10_2.md` | 阶段 2 Architect + 4 sub-phase 详设 + Mermaid 依赖图 |
| `docs/.../ACCEPTANCE_PhaseF_0_10_2.md` | 阶段 6 Assess + 验收清单 + 决策回顾 |
| `docs/.../FINAL_PhaseF_0_10_2.md` | (本文档) 阶段 6 Assess + 交付总结 |
| `docs/.../TODO_PhaseF_0_10_2.md` | 待办事项 + 用户指引 |

---

## 2. 核心方案 (3 行总结)

> 真物理 split-screen = 同帧内 `glViewport(left)` + draw + `TAA.Process(0, 0, W/2, H)` (scissor 限定 left history)
> 然后 `glViewport(right)` + draw + `TAA.Process(W/2, 0, W/2, H)` (scissor 限定 right history)
> 全程 shader 零改动, HDR fbo 单实例, 每 instance 全 size history.

---

## 3. Lua API 增量演示 (3 步)

```lua
-- 一次性初始化
HDR.SetAutoTAA(false)                    -- 关 EndScene 自动 TAA (零回归默认 true)
local p1 = TAA.CreateInstance()          -- player 1 TAA (id=1)
local p2 = TAA.CreateInstance()          -- player 2 TAA (id=2)
TAA.SetActiveInstance(p1); TAA.Enable(W, H)  -- p1 history (全 size)
TAA.SetActiveInstance(p2); TAA.Enable(W, H)  -- p2 history (全 size)

-- 每帧
HDR.BeginScene()  -- 单 HDR fbo 全屏清屏

-- Player 1 区域
Gfx.SetViewport(0, 0, W/2, H)
TAA.SetActiveInstance(p1); TAA.ApplyJitter()
Gfx.SetCamera(...)
-- draw scene
TAA.Process(0, 0, W/2, H)   -- scissor 限 p1 history 仅写左半

-- Player 2 区域
Gfx.SetViewport(W/2, 0, W/2, H)
TAA.SetActiveInstance(p2); TAA.ApplyJitter()
Gfx.SetCamera(...)
-- draw scene
TAA.Process(W/2, 0, W/2, H) -- scissor 限 p2 history 仅写右半

Gfx.SetViewport(0, 0, W, H) -- 复位
HDR.EndScene()              -- bloom + tonemap 全屏 (auto-TAA 已 off)
```

---

## 4. CI 状态

| Commit | CI Run | 状态 | 时间 |
|--------|--------|------|------|
| 525cbaa (Phase 1 init) | 25940796470 | ❌ failure (smoke 顺序问题) | 9m35s |
| a94322f (Phase 1 fix) | 25941161759 | ✅ 6/6 success | 8m20s |
| 57d78ae (Phase 2) | 25942163582 | ✅ 6/6 success | 8m26s |
| fa29d75 (Phase 3) | 25942469141 | ✅ 6/6 success | ~12m |
| 66ee607 (Phase 4) | 25942649835 | ✅ 6/6 success | ~10m |
| 37605f1 (Phase 5 docs) | 25942801442 | ⏳ in_progress | (docs-only, 预期 success) |

**4/4 sub-phase 6/6 平台全 success** — windows/macos/ios/android/web/linux 全部通过.

---

## 5. 工程反思

### 5.1 做对的事

1. **Align 阶段提出 3 路径**: PLAN 明确列了 路径 A/B/C 工作量与风险, 让用户能根据预算选择.
2. **Architect 阶段重新评估实现**: DESIGN 原本想走 shader uvOffset, 实施 Phase 2 时发现 scissor 路径简单 4 倍, 主动改方案.
3. **Atomize 阶段切 4 sub-phase**: 每个 sub-phase 单独 commit + CI 验证, 任何一个挂掉只回滚一步.
4. **零回归刚性约束**: 7 个 backend pass 默认 0 region 参数, 老调用方零改动. demo_taa_compare / demo_taa_split 完全不受影响.

### 5.2 走过的弯路

1. **Phase 1 fix**: `l_SetViewport` 在 g_render=null 时直接 return, 跳过了 luaL_checkinteger 类型检查. 修复后 CI 通过.
2. **TAA shader 改 vs 不改的取舍**: Phase 2 一开始打算改 FS_TAA_SOURCE 加 uvOffset uniform, 但实施前重新评估, 改走 scissor + 全 size history 路径. 节省 3-4h.
3. **demo_taa_split2 lua 5.1 兼容性**: `WIN_W // 2` 在 lua 5.1 不支持, 改用 `math.floor`. 1 行修复.
4. **demo_taa_split2 headless 分支**: 原本只在 `if not UI.Window then` 跑 API 探针, 但 Window.Open 失败时 pcall 接住后直接退出, 不跑 API 探针. 改成两种失败路径都跑 API 探针.

### 5.3 留给未来 (F.0.10.3+)

| 项 | 优先级 | 工作量 | 价值 |
|----|-------|-------|-----|
| Shader uvOffset/uvScale (彻底解决 ~1px 边界锯齿) | 🟡 低 | 3-4h | 视觉极致 + history per region 节 VRAM |
| Bloom / SSR / MotionBlur region 化 (真物理分屏后处理) | 🔴 中 | 6-8h | 真正完整 split-screen 后处理 |
| HDR fbo 多实例化 (per-player HDR) | 🔴 中 | 8-12h | 彻底分离 player VRAM (4 player support) |
| 4-player split-screen demo (2x2 grid) | 🟢 低 | 1-2h | 直接复用现有 API, 仅 demo 改动 |

---

## 6. 累计 Phase F.0 系列 API 总览

| Phase | Lua fn | 增量 | 累计 |
|-------|--------|------|-----|
| F.0 ~ F.0.14 (无 F.0.10) | 35 | — | 35 |
| F.0.10 (multi-instance) | +5 | CreateInstance, DestroyInstance, SetActiveInstance, GetActiveInstance, GetInstanceCount | 40 |
| F.0.10.2 (split-screen) | +5 | Graphics.SetViewport / GetViewport, HDR.SetAutoTAA / GetAutoTAA, TAA.Process | **45** |

Demo 累计: demo_taa_compare + demo_ssr (C 键 cycle) + demo_taa_split (F.0.10.1) + demo_taa_split2 (F.0.10.2) = **4 个**.

---

## 7. 完成签字

| 维度 | 状态 |
|------|-----|
| 代码 | ✅ 4 commit pushed (525cbaa→a94322f→57d78ae→fa29d75→66ee607) |
| smoke | ✅ TAA 41/41 + HDR 22/22 + Graphics 19/0 本地 windows headless |
| demo | ✅ samples/demo_taa_split2 (headless API probe 验证 PASS) |
| 6A 文档 | ✅ PLAN / DESIGN / ACCEPTANCE / FINAL / TODO (5 文档) |
| CI | ✅ Phase 1-4 全 6/6 success (Phase 5 docs in_progress) |

**Phase F.0.10.2 真物理 Split-Screen 交付完成. CI 4/4 sub-phase × 6 平台 全部 success.**
