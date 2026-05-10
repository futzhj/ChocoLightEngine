# TODO — Phase AW.x（待办与建议）

> Phase AW.x 主体已完整交付（详见 `ACCEPTANCE_PhaseAWx.md` / `FINAL_PhaseAWx.md`）。本文件列出**不阻塞合并**但建议跟进的事项。

---

## 一、强烈建议跟进（P0）

### 1.1 真机性能数字收集

**为什么**：sample 已就绪但实际 GPU 性能数字仍未在桌面 GPU 上测过。Phase AW + AW.x 的"30x 提升"是理论估算，需要真实数字背书。

**建议操作**：

```powershell
# 在桌面 GPU 机器（Windows / Linux / macOS）
cd <ChocoLight 根目录>
.\samples\demo_skinning_perf\setup.ps1                                   # 下载默认资产
.\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua  # 启动并截图 baseline 表
```

**记录建议**：把 baseline 输出粘贴到本文件 §五"真机数据收集"段。

**优先级**：用户行为；非引擎层工程。

---

## 二、建议优化（P1）

### 2.1 大模型资产真机验证

**当前**：默认 setup 下载 `RiggedSimple.glb`（24 顶点 / 2 关节）— 太小，GPU 收益不明显。

**建议**：用 `CesiumMan.glb` 或自有 5000+ 顶点角色，更能体现 Phase AW 的真实价值。

**操作**：编辑 `setup.ps1` / `setup.sh` URL 为 `CesiumMan/glTF-Binary/CesiumMan.glb`，或直接手动下载到 `samples/demo_skinning_perf/assets/character.glb`。

---

### 2.2 sample 自动 frame timing CSV 输出

**为什么**：当前 baseline 输出在控制台，无法自动比对多次运行的差异。

**建议**：在 `R` 重新 baseline 时同时把每帧 ms 写入 `samples/demo_skinning_perf/results/<timestamp>.csv`。

**收益**：用户可用 Excel / pandas 做趋势对比。

**优先级**：低（多次手动 R 也能定性看到稳定性）。

---

### 2.3 OSD 加 frame histogram

**为什么**：avg/min/max 看不出长尾分布。如果 GPU 偶尔有 spike（30+ ms），avg 数字仍很好看但用户感觉卡顿。

**建议**：OSD 加一个 64-bucket histogram 显示帧时间分布。

**优先级**：低（baseline 表的 max 已能定位最坏情况）。

---

## 三、文档维护（P2）

### 3.1 同步 `samples/README.md` 跨平台支持矩阵

**当前**：`samples/README.md` 末尾的"跨平台说明"表没有 demo_skinning_perf 这一行。

**建议**：增加一行说明：
| 模块 | Windows | macOS | Linux | Android | iOS | Web |
| Skinning Perf | ✅ | ✅ | ✅ | ⚠️ 控制方式不同 | ⚠️ | ⚠️ Web GPU 默认 cpu |

**优先级**：低（README 表面信息）。

### 3.2 Phase AW.x ACCEPTANCE 与 Phase AW ACCEPTANCE 交叉引用

**当前**：两阶段 ACCEPTANCE 各自完整，但 AW 的 ACCEPTANCE 没有引用 AW.x 的 sample 验证。

**建议**：在 `docs/Phase AW GPU Skinning/ACCEPTANCE_PhaseAW.md` §A 段补一句"真机验证由 Phase AW.x 交付（见 docs/Phase AW.x/）"。

**优先级**：低（追求文档自洽性）。

---

## 四、需用户决策的事项

无 — 当前实现完全自洽。

---

## 五、真机数据收集（占位）

> 本节预留给用户在桌面 GPU 机器上实际运行 demo 后填充。

### 5.1 我的设备 1（用户编辑）

```
日期: <YYYY-MM-DD>
设备: <CPU / GPU / OS>
Backend: <GL33Core 等>
Asset: <character.glb 顶点数 / 关节数>

baseline 输出:
==== Phase AW Skinning Performance Baseline ====
... (粘贴控制台输出)
================================================
```

### 5.2 设备 2 / 3 / ... （按需添加）

---

## 六、操作指引

### 立即可做（5 分钟）

```powershell
# 收集第一组数字
cd e:\jinyiNew\Light
.\samples\demo_skinning_perf\setup.ps1
.\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua
# 把控制台 baseline 表粘贴到本文件 §5.1
```

### 短期（1-2 小时）

实现 §2.2 CSV 输出 + §2.3 histogram，用于多机器对比。

---

## 七、TODO 跟踪

完成事项时：
1. 把对应章节标题加 ✅ 前缀
2. 在文末"完成日志"添加日期 + 描述

### 完成日志

（首次完成 TODO 项时在此添加）
