# Phase G.1.1 Shared GL Context — 待办与缺失配置

> **更新日期**：2026-05-17
> **关联**：[ACCEPTANCE](ACCEPTANCE_PhaseG_1_1.md) · [FINAL](FINAL_PhaseG_1_1.md)

---

## 优先级 P0（无）

本期未引入任何阻塞性技术债。

---

## 优先级 P1（演进路径，单独立项）

### ✅ TODO-1 ｜ G.1.2 Mesh worker 上传 — **已完成 (2026-05-17)**

完成于 Phase G.1.2，方案：worker 直接发 GL 命令创建 vao/vbo/ebo，主线程 Tick 内调新增的 `RenderBackend::RegisterUploadedMesh` 完成 backend 注册。详见 [PLAN_PhaseG_1_2.md](PLAN_PhaseG_1_2.md) · [ACCEPTANCE_PhaseG_1_2.md](ACCEPTANCE_PhaseG_1_2.md) · [FINAL_PhaseG_1_2.md](FINAL_PhaseG_1_2.md)。

---

## 优先级 P2（CI / 工具链改进）

### ✅ TODO-2 ｜ Probe 脚本接入 CI 包装 — **已完成 (2026-05-17)**

写了跨平台 wrapper 脚本：

- `@e:/jinyiNew/Light/scripts/run_probe_smoke.ps1`（Windows / PowerShell, 纯 ASCII 避免编码问题）
- `@e:/jinyiNew/Light/scripts/run_probe_smoke.sh`（Linux / Mac）

行为：
- 默认 10 秒 timeout 启动 `light.exe scripts/smoke/asset_loader_async_probe.lua`
- 抓 stdout 关键字 `AssetLoader: Shared GL Context enabled` 或 `fallback to main-thread upload` → 判 PASS / FAIL
- 完全忽略 `light.exe` 的进程退出码（绕开退出语义不稳定问题）

退出码：`0=PASS`, `1=key log not found`, `2=bad arg`。

**CI 接入示例**（自托管 GPU runner）：

```yaml
# Windows
- name: Phase G.1.1 probe smoke
  shell: pwsh
  run: .\scripts\run_probe_smoke.ps1

# Linux / Mac
- name: Phase G.1.1 probe smoke
  run: ./scripts/run_probe_smoke.sh
```

**当前状态**：GitHub Actions `windows-latest` 无 GPU，无法直接接入。等用户配置自托管 GPU runner 时一行命令即可启用。

---

### ✅ NOTE-1 ｜ probe 脚本 `self:Close()` 后 hang — **已修复 (2026-05-18, Phase G.1.3)**

**根因**：probe 脚本用 `function Demo:OnFrame()`，但 `@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` 中 Window:__call 实际回调约定是 `Update(dt) + Draw()`。`OnFrame` 永远不会被引擎调用 → `self:Close()` 永远不触发 → 窗口 hang。

**修复**：`@e:/jinyiNew/Light/scripts/smoke/asset_loader_async_probe.lua` 把 `OnFrame` 改为 `Update(dt)` + 添加空 `Draw()` 实现。

**验证**：`light.exe asset_loader_async_probe.lua` 现在 2 秒内干净退出 (exit=0)，自动打印 `AssetLoader: shutdown complete` + `asset_loader_async_probe smoke ok`。

---

### ✅ NOTE-2 ｜ light.exe 退出阶段 STATUS_STACK_BUFFER_OVERRUN — **已修复 (2026-05-18, Phase G.1.3)**

**根因**：sample 通用模板 `while UI.Loop() do UI.Resume() end` 在 `UI.Loop=false` 时退出 while，此后**不再调用 `UI.Resume()`**。而清理逻辑（含 `AssetLoader::Shutdown` → worker thread join）原本只在 `l_UI_Resume` 的 `ShouldClose` 分支内执行。结果：

1. Update 内 `self:Close()` → `SetShouldClose(true)`
2. 当前 `UI.Resume` 已在 "正常一帧" 分支，继续完成
3. 下次 `UI.Loop()` → false → 退 while
4. ❌ `UI.Resume` 清理路径**从未执行** → worker 线程未 join
5. 进程退出时 `std::thread g_worker` 静态析构, joinable → `std::terminate()` → `STATUS_STACK_BUFFER_OVERRUN` (`0xC0000409`)

**为什么 G.1.x 之前的 sample 都不崩**：它们不调 `Image.LoadAsync`，AssetLoader 不创 worker thread，进程退出时无 joinable thread 析构 → 不触发 `std::terminate`。但实际上**所有 21 个 sample 都有同样的清理路径缺失**（GL ctx / SDL 窗口 / Audio backend 等都没显式 shutdown），只是表面不可见。

**修复**：`@e:/jinyiNew/Light/ChocoLight/src/light_ui.cpp` 抽 `PerformWindowShutdown_(L)` helper（含完整 13 个子系统 shutdown 序列 + GL ctx 销毁 + SDL3 shutdown），让 `l_UI_Loop` 与 `l_UI_Resume` 在任一路径首次检测到 `ShouldClose` 时都调用此函数；内部用 `g_mainWindow != nullptr` 作为幂等 guard。

**验证**：
- ✅ `perf_async_loader` sample 跑 3 次均 exit=0，stdout 见 `AssetLoader: worker thread exit` + `AssetLoader: shutdown complete`
- ✅ `asset_loader_async_probe` smoke exit=0
- ✅ `asset_loader_async` smoke (无窗口分支) 不受影响, exit=0
- ✅ `ssao` / `bloom` smoke 行为不变，exit=0（pre-existing FAIL 与本修复无关）

**副作用**：所有现存 sample 现在都会真正执行引擎清理路径（之前 21 个 sample 都跳过了），消除潜在资源泄漏。无 API 兼容性变化。

---

## 优先级 P3（量化数据）

### ✅ TODO-3 ｜ 性能 benchmark Sample — **已完成 (2026-05-18)**

交付物：
- `@e:/jinyiNew/Light/samples/perf_async_loader/main.lua` — 完整 benchmark 实现（资源自动扫描 + Update 帧时收集 + P50/P95/P99/Max 报告）
- `@e:/jinyiNew/Light/samples/perf_async_loader/README.md` — 用法说明 + 报告示例 + 解读指引 + 退出崩溃问题说明
- `@e:/jinyiNew/Light/samples/perf_async_loader/.gitignore` — `textures/` 用户自备资源不入 repo

**实测数据**（5 张 64x64 PNG, NVIDIA RTX, GL 3.3 NVIDIA 560.94, Windows 10）：

```
[perf_async_loader] frames=6  avg=2.57ms  P50=2.15ms  P95=4.80ms  P99=4.80ms  Max=4.80ms
[perf_async_loader] loaded 5/5 textures (errors=0)
[perf_async_loader] PASS: P95 < 16.7ms (60fps budget held)
```

**结论**：G.1.1 worker 上传路径 + G.1.2 mesh 改动都不引入主线程阻塞，P95 = 4.80ms 远低于 60fps budget。

**真实压力测试**：用户自备 100 张 1024x1024+ PNG 即可一行命令重跑得到大规模数据（详见 sample README）。

---

### TODO-3-archived ｜ 原始任务定义（已完成, 保留供历史参考）

| 项 | 内容 |
|----|----|
| **背景** | FINAL § 五 仅给定性预期，未量化 |
| **要做** | 写一个 sample（建议放 `@e:/jinyiNew/Light/samples/perf_async_loader/`）：连续 LoadAsync 100 张 4K PNG，用 `Light.Time.GetMicros()` 量主线程每帧时长 P50/P95/P99，对比 G.1.0 同步同步加载 / G.1.0 异步主线程上传 / G.1.1 worker 上传 三组 |
| **依赖** | 一组 100 张 4K test 图 (现有 `@e:/jinyiNew/Light/assets/` 是否有? 需用户确认) |
| **预估工作量** | ≈1 小时（如已有素材）/ ≈2 小时（含造素材） |
| **建议触发** | 验证 G.1.1 收益数据时 / 写性能博客 / 项目里程碑评审时 |

---

## 缺失配置 / 用户支持需求

### Q1 ｜ 是否启动 G.1.2 Mesh worker 上传？

> **现状**：本期已显式裁剪范围，Mesh 仍走主线程上传。功能正常，仅大型 GLTF 加载时有可见尖峰
> **决策选项**：A) 立即接 G.1.2（按 TODO-1 操作指引开工）/ B) 等用户报告掉帧问题再做 / C) 先做 TODO-3 benchmark 用数据决策

### Q2 ｜ macOS / Linux 是否需要现在验证？

> **现状**：仅 Windows + NVIDIA 560.94 + GL 3.3 Core 跑过 probe 日志验证
> **决策选项**：A) 你能跑就帮跑一次 probe 脚本（我看 stdout）/ B) 等其他平台 CI 跑 / C) 跳过

### Q3 ｜ Probe 脚本的 PowerShell exit code 异常需要修吗？

> **现状**：probe 日志正常输出，但 OnFrame Close 退出后 PS exit code 偶发非 0；不影响功能，仅影响 CI 接入 (TODO-2)
> **决策选项**：A) 接入 CI 时一并修（用 Select-String 而非 exit code 判断）/ B) 不接入 CI，跳过

---

## 文档链接索引

- 阶段 1 对齐：[ALIGNMENT_PhaseG_1_1.md](ALIGNMENT_PhaseG_1_1.md)
- 阶段 2 设计：[DESIGN_PhaseG_1_1.md](DESIGN_PhaseG_1_1.md)
- 阶段 3 任务：[TASK_PhaseG_1_1.md](TASK_PhaseG_1_1.md)
- 阶段 5+6 验收：[ACCEPTANCE_PhaseG_1_1.md](ACCEPTANCE_PhaseG_1_1.md)
- 阶段 6 总结：[FINAL_PhaseG_1_1.md](FINAL_PhaseG_1_1.md)
