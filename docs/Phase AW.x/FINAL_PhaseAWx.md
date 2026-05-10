# FINAL — Phase AW.x（GPU Skinning 真机验证工具链 总结）

**完成日期**: 2026-05-10
**总耗时**: 约 1 个工作单元（6A Stage 1-6 一次会话内完成）
**最终 commit**: `368f68a`
**CI**: T1+T2 ✅ 6/6 平台 / **smoke +11 PASS** / 0 FAIL（累计 170 → 181）

---

## 一、项目目标回顾

> **背景**：Phase AW 已完整交付 GPU Skinning 调度逻辑（zero-break API + Silent fallback + 6 平台 CI 全绿），但 **GPU 路径在真实 GPU 上的端到端运行未做过实测**（CI 是 headless 环境）。
>
> **目标**：提供开箱即用的真机性能验证工具链，让用户在 5 分钟内得到具体的 CPU vs GPU 性能对比数字，量化 Phase AW 的实际收益。

---

## 二、6A 工作流执行轨迹

| 阶段 | 产物 | 关键决策 |
|------|------|---------|
| **1 Align** | `ALIGNMENT_PhaseAWx.md` | 修正"`Light.Util.FrameTime` API 新建" → "复用既有 `Light.Time.GetTicksNS`"（勘察发现）|
| **2 Architect** | `CONSENSUS_PhaseAWx.md` + `DESIGN_PhaseAWx.md` | AQ1=补 GetBackendName / AQ2=A+C 组合（setup 脚本下载）/ AQ3=headless 退出 0 |
| **3 Atomize** | `TASK_PhaseAWx.md` | 5 个独立可验证任务 + DAG（T1 → T2/T3 → T4/T5）|
| **4 Approve** | （Stage 4 检查清单内联）| 5 维度全过 |
| **5 Automate** | T1-T5 5 次 commit | 关键发现：T1 因 `GetName()` 已存在大幅缩减（4 文件改 → 1 文件改） |
| **6 Assess** | `ACCEPTANCE_PhaseAWx.md` + `FINAL_PhaseAWx.md` + `TODO_PhaseAWx.md` | smoke +11 PASS / 0 FAIL；可合并 |

---

## 三、技术成果

### 1. C++ Lua API（`light_graphics.cpp` +20 行）

```cpp
// 永不 raise / 永不返回 nil；headless 安全
static int l_Graphics_GetBackendName(lua_State* L) {
    if (!g_render) { lua_pushliteral(L, "None"); return 1; }
    const char* name = g_render->GetName();
    lua_pushstring(L, (name && *name) ? name : "Unknown");
    return 1;
}
// 注册到 graphics_funcs[]:
// { "GetBackendName", l_Graphics_GetBackendName },
```

> **意外收获**：勘察发现 `RenderBackend::GetName()` 早已是 pure virtual，且 `GL33Backend` / `LegacyGLBackend` 都已实现（返回 `"GL33Core"` / `"LegacyGL"`）。T1 实际只需补 Lua binding 一步。

### 2. smoke 验证（`scripts/smoke/graphics.lua` +85 行）

11 个 CHECK 完整覆盖 API 表面 + 错误处理：
- API 注册存在性
- pcall 安全（不 raise）
- 返回类型 / 非空 / 白名单
- 多次调用稳定性

### 3. Lua sample（`samples/demo_skinning_perf/main.lua` ~370 行）

单文件 OOP Window，含 5 个内部段：
- **FrameStat**: ring buffer，60-frame avg/min/max/reset，纯 Lua + GetTicksNS
- **AssetLoader**: 5 路径候选 + 资产缺失友好提示 + setup 脚本指引
- **Baseline**: 30 帧预热 + 60 帧测量 + 模式实际生效检测
- **OSD**: 双 panel（左上状态 + 右下 baseline + 错误条）color-coded
- **Game**: OnOpen/OnKey/Update/Draw 完整生命周期

### 4. setup 脚本（`setup.ps1` + `setup.sh`）

- Khronos `RiggedSimple.glb` 公开 CDN 下载
- Windows: `Invoke-WebRequest`；Unix: `curl` / `wget` fallback
- `-Force` / `-f` 重新下载选项
- 失败时打印手动下载链接（3 个推荐资产）
- `assets/` 加入 `.gitignore`，不污染 git 仓库

### 5. 文档（4 处同步）

| 文件 | 改动 |
|------|------|
| `samples/README.md` | 表格新增 `demo_skinning_perf/` 一行（Phase AW.x 标识）|
| `docs/api/Light_Animation.md` | Phase AW 章节末尾追加"真机验证 GPU Skinning 收益"段（33 行）|
| `docs/Phase AW GPU Skinning/TODO_PhaseAW.md` | §1.1 §1.2 §3.3 标记 ✅ + 完成日志区新行 |
| `samples/demo_skinning_perf/README.md` | sample 子文档（quickstart + keys + troubleshooting + license）|

---

## 四、量化指标

| 维度 | 数值 |
|------|------|
| 代码增量（含文档）| ~1500 行 |
| 新文件数 | 5 (sample 4 + smoke 1) |
| 修改文件数 | 4 (light_graphics.cpp / build-templates.yml / samples/README.md / Light_Animation.md / TODO_PhaseAW.md) |
| commit 数 | 5 (T1-T5) + 1 (Stage 1-3 docs) + 1 (Stage 6 docs，本次) |
| smoke 净增 PASS | +11 (170 → 181) |
| smoke FAIL | 0 |
| 6 平台 CI 通过率 | 6/6 ✅ |
| C++ API 破坏 | 0 |
| Phase AW 既有测试退化 | 0 |
| TODO 完成项 | 3/8 (P0 全部完成) |

---

## 五、核心设计亮点

### 1. **零 C++ 模块新增**

最初 ALIGNMENT 中"补 `Light.Util.FrameTime` API"在 Stage 1 勘察时被否决 — 项目已有 `Light.Time.GetTicksNS()`。最终 C++ 改动仅 +20 行 Lua binding。

### 2. **借力既有 OOP Window 范式**

100% 复用 `samples/perf_benchmark/main.lua` 的 `Light(Light.UI.Window):New()` + `OnOpen/OnKey/Update/Draw` 模式。无需引入新的 Lua 框架抽象。

### 3. **资产策略 A+C 组合**

不在 git 仓库放二进制 + 提供一键 setup 脚本 + README 提供高级用户的扩展资源指引。同时满足"开箱即用"和"仓库轻量"。

### 4. **Headless CI 友好**

所有可能在 headless 环境失败的路径都有 fallback：
- `g_render = nullptr` → `GetBackendName()` 返回 `"None"`
- `Light.UI.Window` 不可用 → sample 打印提示退出 0
- 资产缺失 → 打印候选路径 + setup 指引退出 0
- DrawSkinnedMesh 异常 → OSD 红色错误条，不退出

### 5. **TODO 倒推驱动设计**

整个 Phase AW.x 直接源于 `TODO_PhaseAW.md` 中 P0 强烈建议项 §1.1 §1.2 §3.3。三个 P0 全部一次性闭环，证明 6A 工作流的 TODO 文档不是死文档而是实际驱动后续阶段的工件。

---

## 六、风险与缓解

| 风险 | 概率 | 实际影响 | 缓解 |
|------|------|---------|------|
| Khronos repo URL 变化 | 低 | 中 | README 提供 3 个备选资产 + 手动下载提示 |
| Web Safari WebGL2 兼容 | （Phase AW Q7 范围）| n/a | 不在本阶段范围 |
| 不同 GPU 性能差异巨大 | 高 | 低 | sample 输出 backend 名称 + 不预设期望值 |
| baseline OS 后台干扰 | 中 | 低 | R 键重新 baseline + min/max 显式报告波动 |
| OOP Window 在某平台 fail | 低 | 低 | sample 检测后退出 0（与 demo_animation 一致）|

---

## 七、与既有阶段的关系

| 关联 | 说明 |
|------|------|
| **驱动来源** Phase AW TODO | §1.1 / §1.2 / §3.3 P0 全部在本阶段闭环 |
| **依赖** Phase AS | 复用 `Light.Graphics.Print` / `SetColor` |
| **依赖** Phase AV | 复用 `LoadSkinnedGLTF` / `NewAnimator` / `AddState` / `Play` |
| **依赖** Phase AW | 复用 `SetSkinningMode` / `GetSkinningMode` / `DrawSkinnedMesh` 已有 API |
| **不影响** Phase AS / AV / AW 既有测试 | 既有 smoke 0 退化，新增 11 PASS 累加 |

---

## 八、交付清单

| 类型 | 文件 | 状态 |
|------|------|------|
| 设计文档 | `docs/Phase AW.x/ALIGNMENT_PhaseAWx.md` | ✅ |
| 设计文档 | `docs/Phase AW.x/CONSENSUS_PhaseAWx.md` | ✅ |
| 设计文档 | `docs/Phase AW.x/DESIGN_PhaseAWx.md` | ✅ |
| 设计文档 | `docs/Phase AW.x/TASK_PhaseAWx.md` | ✅ |
| 验收文档 | `docs/Phase AW.x/ACCEPTANCE_PhaseAWx.md` | ✅ |
| 总结文档 | `docs/Phase AW.x/FINAL_PhaseAWx.md`（本文件）| ✅ |
| TODO | `docs/Phase AW.x/TODO_PhaseAWx.md` | ✅ |
| C++ Lua API | `ChocoLight/src/light_graphics.cpp` | ✅ |
| smoke | `scripts/smoke/graphics.lua` | ✅ |
| CI workflow | `.github/workflows/build-templates.yml` | ✅ |
| sample 主文件 | `samples/demo_skinning_perf/main.lua` | ✅ |
| sample setup | `samples/demo_skinning_perf/setup.ps1` | ✅ |
| sample setup | `samples/demo_skinning_perf/setup.sh` | ✅ |
| sample gitignore | `samples/demo_skinning_perf/.gitignore` | ✅ |
| sample 子文档 | `samples/demo_skinning_perf/README.md` | ✅ |
| 主文档 | `samples/README.md` | ✅ |
| API 文档 | `docs/api/Light_Animation.md` | ✅ |
| Phase AW TODO 标记 | `docs/Phase AW GPU Skinning/TODO_PhaseAW.md` | ✅ |

---

## 九、最终状态

**Phase AW.x GPU Skinning 真机验证工具链已全部完成，6 平台 CI 全绿，可合并主线。**

**用户下一步行动建议**（按 ROI 排序）：

1. **立即可做**（~5 分钟）：在桌面 GPU 机器上运行 `samples\demo_skinning_perf\setup.ps1` 然后 `light.exe samples\demo_skinning_perf\main.lua`，得到第一手的 CPU vs GPU 性能数字
2. **根据数字决策**：如果 GPU 路径数字 ≥ 5x 提升 → Phase AW 价值已证；< 2x 提升 → 触发 Phase AW.y profile（增量 UBO / 多 mesh 共享）
3. **可选扩展**：填用更大模型（CesiumMan / 自有角色）测试真实场景；记录到 `docs/Phase AW.x/TODO_PhaseAWx.md` 的"真机数据收集"段
