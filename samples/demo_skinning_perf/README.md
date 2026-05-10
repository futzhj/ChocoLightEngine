# demo_skinning_perf — Phase AW GPU Skinning 真机性能测试

> **一句话**：启动后自动跑 60 帧 CPU + 60 帧 GPU baseline，打印 speedup 数字；按 `G/C/A` 运行时切换模式，OSD 实时显示 frame ms。

---

## 快速开始

### Windows

```powershell
cd <ChocoLight 根目录>
.\samples\demo_skinning_perf\setup.ps1                                   # 下载默认资产 (~80KB)
.\Light-0.2.3\windows-x64\light.exe samples\demo_skinning_perf\main.lua  # 启动
```

### Linux / macOS

```bash
cd <ChocoLight 根目录>
chmod +x samples/demo_skinning_perf/setup.sh
./samples/demo_skinning_perf/setup.sh
./Light-0.2.3/<platform>/light samples/demo_skinning_perf/main.lua
```

---

## 键盘控制

| 键 | 行为 |
|----|------|
| `G` | 切换到 GPU 模式 (`Anim.SetSkinningMode("gpu")`) |
| `C` | 切换到 CPU 模式 |
| `A` | 切换到 AUTO 模式（按平台 + backend 推断） |
| `R` | 重新跑 baseline (60 帧 CPU + 60 帧 GPU) |
| `ESC` | 退出 |

---

## 期望输出

启动后控制台会打印类似：

```
[demo_skinning_perf] 加载 samples/demo_skinning_perf/assets/character.glb ...
[demo_skinning_perf] 校准 CPU baseline (60 帧 + 30 预热) ...
[demo_skinning_perf] 校准 GPU baseline (60 帧 + 30 预热) ...

==== Phase AW Skinning Performance Baseline ====
Backend       : GL33Core
Asset         : samples/demo_skinning_perf/assets/character.glb
Mesh          : 24 vertices, 36 indices
GPU support   : true

  CPU: avg=0.082ms  min=0.071ms  max=0.108ms  (n=60, actual=cpu)
  GPU: avg=0.012ms  min=0.009ms  max=0.024ms  (n=60, actual=gpu)
Speedup       : 6.8x (CPU avg / GPU avg)
================================================

Entering interactive mode. Keys: G=GPU C=CPU A=AUTO R=re-baseline ESC=quit
```

> 不同设备 / 顶点数 / 关节数会有显著差异。典型 5000 顶点角色模型在桌面 GL3.3 上约 **20-30x** 提升。

---

## 屏上 OSD

- **左上 panel**：当前 backend / mode / frame 平均/min/max / FPS
- **右下 panel**：CPU baseline / GPU baseline / 当前 speedup
- **底部错误条**（如有）：DrawSkinnedMesh 异常 + 累计计数

---

## 自定义资产

任何 glTF 2.0 skinned mesh 都可以作为测试资产，放置到：

```
samples/demo_skinning_perf/assets/character.glb
```

如果该路径不存在，sample 会按以下优先级查找：

1. `samples/demo_skinning_perf/assets/character.glb`（setup 脚本的目标）
2. `samples/demo_animation/assets/character.glb`
3. `samples/demo_animation/character.glb`
4. `assets/character.glb`
5. `Light-0.2.3/assets/character.glb`

资产完全缺失时会打印提示后退出码 0（CI 友好）。

---

## 推荐资产源

| 资产 | 顶点数 | 关节数 | 适用 |
|------|-------|-------|------|
| [RiggedSimple](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedSimple) | ~24 | 2 | 默认（setup 脚本下载） |
| [RiggedFigure](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/RiggedFigure) | ~370 | 19 | 中等复杂度 |
| [CesiumMan](https://github.com/KhronosGroup/glTF-Sample-Models/tree/master/2.0/CesiumMan) | ~1700 | 19 | 真实人物 |

> 顶点数越多，GPU 路径的相对优势越大（CPU 路径每帧重传顶点开销与顶点数正相关）。

---

## 故障排查

### 问题 1：`GPU support: false`

**原因**：当前 backend 不支持 GPU skinning，常见情况：
- 在 headless / 远程桌面环境下 backend 退化为 LegacyGL
- backend 是 LegacyGL（GL 1.x 路径，不支持 UBO/shader）
- shader 编译失败（应用日志会有警告）

**应对**：sample 仍会跑 baseline（两条路径都返回 CPU 数据，方便诊断）。

### 问题 2：`下载失败`

**原因**：网络不通 / Khronos repo URL 变化 / 防火墙。

**应对**：手动下载任意 glTF 2.0 skinned mesh 到 `assets/character.glb`，sample 会自动检测。

### 问题 3：Window 创建失败 / 启动即退

**原因**：纯 console 环境无 GUI 上下文（如 SSH 会话）。

**应对**：sample 自动 print 提示并退出码 0，不算错误。需要在桌面环境运行。

### 问题 4：DrawSkinnedMesh 抛异常

**原因**：mesh / animator userdata 在某些场景被回收。

**应对**：OSD 底部会显示 `ERROR x<N>: <msg>`，便于定位。多数情况不影响下一帧。

---

## 实现细节

详见：

- `main.lua` — 单文件 sample（含 FrameStat / AssetLoader / Baseline / OSD / Game）
- `docs/api/Light_Animation.md` — `Anim.SetSkinningMode` / `GetSkinningMode` API 文档
- `docs/Phase AW GPU Skinning/` — Phase AW 6A 全流程文档
- `docs/Phase AW.x/` — 本 sample 的 6A 文档（设计 / 任务拆分 / 验收）

---

## 许可

本 sample 代码遵循 ChocoLight 引擎许可。下载的 Khronos `RiggedSimple.glb` 资产为 **CC0 / Royalty-Free**。
