# Phase F.0.10 TAA Multi-Instance — TODO

> 6A 工作流 · 阶段 6 · TODO 收尾

---

## 1. 必做

| 任务 | 状态 |
|------|------|
| commit + push 代码到 main | ✅ 完成 (f2ab9cf) |
| 监控 GitHub Actions CI 6/6 success | ✅ 完成 ([25938518533](https://github.com/futzhj/ChocoLightEngine/actions/runs/25938518533)) |
| CI 状态回填 ACCEPTANCE / FINAL / TODO | ✅ 完成 |
| 修复 CreateInstance headless smoke 兼容 | ✅ 完成 (51dbb4e) |

---

## 2. 用户指引

### 2.1 单 instance (老用户, 零改动)

老代码 100% 兼容, 不需调用任何 instance API：

```lua
TAA.Enable(W, H)              -- 默认作用于 g_states[0] (default instance)
TAA.SetSharpness(0.5)         -- 默认作用于 g_states[0]
TAA.Process(hdrFbo, hdrTex)   -- 默认作用于 g_states[0]
```

### 2.2 多 instance (split-screen / 多视口)

```lua
local TAA = Light.Graphics.TAA

-- 创建 2 个 user instance (id1=1, id2=2)
local id1 = TAA.CreateInstance()
local id2 = TAA.CreateInstance()

-- 各自配置 (player1 sharp, player2 soft)
TAA.SetActiveInstance(id1)
TAA.Enable(W/2, H)
TAA.SetSharpness(1.2); TAA.SetSharpenMode("rcas")

TAA.SetActiveInstance(id2)
TAA.Enable(W/2, H)
TAA.SetSharpness(0.4); TAA.SetClipMode("variance")

-- 每帧分别 ApplyJitter + Process
TAA.SetActiveInstance(id1); TAA.ApplyJitter()
    win:SetViewport(0, 0, W/2, H); -- draw scene 1
    TAA.Process(hdrFbo, hdrTex)
TAA.SetActiveInstance(id2); TAA.ApplyJitter()
    win:SetViewport(W/2, 0, W/2, H); -- draw scene 2
    TAA.Process(hdrFbo, hdrTex)

-- 收尾
TAA.SetActiveInstance(0)
TAA.DestroyInstance(id1)
TAA.DestroyInstance(id2)
```

### 2.3 常见错误避免

| 错误 | 处理 |
|------|------|
| 忘记 SetActiveInstance 直接 SetSharpness | 默认作用于 g_active (上次切换的 instance), 可能不是预期 |
| DestroyInstance(0) | 拒绝, 返 nil + err (default 不可销毁) |
| 销毁 active 后继续 SetSharpness | 不会崩 (自动切回 default 0), 但参数写入 default 可能非预期 |
| 5 次以上 CreateInstance | 第 4 次开始返 0 (MAX_INSTANCES=4), 需升级常数重编译 |
| Lua 用浮点 id (e.g. 1.5) | lua_tointeger 截断为 1, 不报错; 推荐用整数 |

### 2.4 demo_ssr C 键演示

按 **C 键** 触发 4-state lifecycle 循环演示：

| 按键次数 | 操作 | 状态 |
|----------|------|------|
| 1 | 创建 3 个 user instance, 各赋差异化参数 | count=4, active=0 |
| 2 | active 0 → 1 (sharpness=0.3, clip=rgb, sharpen=unsharp) | active=1 |
| 3 | active 1 → 2 (sharpness=1.5, clip=ycocg, sharpen=cas) | active=2 |
| 4 | active 2 → 3 (sharpness=0.8, clip=variance, sharpen=rcas) | active=3 |
| 5 | 销毁所有 user instance, 回 default | count=1, active=0 |

---

## 3. CI 回填

GitHub Run ID: [`25938518533`](https://github.com/futzhj/ChocoLightEngine/actions/runs/25938518533) / Fix commit: `51dbb4e` / Date: 2026-05-15 19:55 UTC / Result: **6/6 success**

---

## 4. 候选 Phase

- **F.0.10.1 — demo_taa_split 多实例演示** ✅ **已完成**
  - 新建 `samples/demo_taa_split/main.lua` (~340 行) + README.md
  - 4 个 Instance Profile (default + 3 user, 各赋差异化参数突出 F.0.12 / F.0.14 / F.0.8+0.13)
  - 0/1/2/3 键: 切 active instance (history 各自累积, 切换 0 stabilize 时间)
  - R 键: 重置当前 instance history; C 键: 销毁/重建 user instance
  - HUD: active instance + profile 描述 + 实际 GetXxx 参数对比
  - **注意**: 仅展示 instance API 行为, 未实现真物理 split-screen (左右分屏并行渲染需要 backend FBO + Lua SetViewport API, 属 F.0.10.2 范围)
- **F.0.10.2 — 真物理 split-screen** (后续候选, 4-6h)
  - 暴露 `Graphics.SetViewport(x, y, w, h)` Lua API
  - HDR/TAA 链路支持单帧多次 Process + viewport 切换
  - 左半屏 instance 1 / 右半屏 instance 2 真正同帧并行
- F.0.11 — Demo 截图 / 录屏 (3h)
- F.0.15 — TAA-driven CAS strength scaling (2h)
- F.1 — DLSS-like TAAU

---

## 5. 总结

Phase F.0.10 实施完整，**无阻塞性遗留**。主要交付：

- **multi-instance 重构**: State 数组化 + macro 透明展开, 146 处 `g.X` 零改动
- **5 fn instance API**: Create / Destroy / SetActive / GetActive / GetInstanceCount
- **完整防御性**: 非法 id / 未分配 / Destroy(0) 全部 nil+err + 销毁 active 自动切回 default
- **smoke 16 PASS**: 覆盖 round-trip / 槽满 / type-error / 参数独立性 / 销毁 active / 槽位复用
- **demo_ssr C 键 lifecycle**: 4-state 循环演示
- **零回归**: default instance 行为完全等价 F.0 ~ F.0.14
- **API 35 → 40 fn**: Phase F.0 系列累计交付

**下一步**：T6 commit + push + CI 验证 6/6 平台 success。
